#include "backend/services/StageService.h"

#include "backend/services/SerialBus.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

namespace backend::services {

using stage::Direction;
using stage::MoveState;
using stage::StageError;
using stage::StageStatus;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kRetainedOperations = 32;

bool terminal(StageService::OperationState s)
{
    return s != StageService::OperationState::Queued && s != StageService::OperationState::Running;
}

bool isWhole(double um)
{
    return std::isfinite(um) && std::abs(um - std::round(um)) <= 1e-6;
}

std::uint16_t randomToken()
{
    std::random_device rd;
    std::uniform_int_distribution<int> dist(1, 0xFFFF);
    return static_cast<std::uint16_t>(dist(rd));
}

// Widest travel around the zero: the stage's half span less a margin.
double capFor(const StageConfig& cfg)
{
    return cfg.reference.expectedSpanUm / 2.0 - cfg.reference.softLimitMarginUm;
}

struct Envelope {
    double min{0.0};
    double max{0.0};
};

// ADR 0013 A3: +/-default (or +/-cap once the operator declared mid-travel),
// intersected with the window the first zero of this power-up set.
Envelope envelopeOf(const StageReferenceRecord& r, const StageConfig& cfg)
{
    const double base = r.midTravelDeclared ? capFor(cfg) : cfg.envelope.defaultUm;
    // Edges round inward to whole micrometres (targets are whole um), with a
    // small epsilon so an exact edge such as -1000 is kept.
    return {std::ceil(std::max(-base, r.windowMinUm) - 1e-6), std::floor(std::min(base, r.windowMaxUm) + 1e-6)};
}

// Errors that mean the open-loop counter may no longer match the stage.
bool losesReference(StageError e)
{
    switch (e) {
    case StageError::LimitSwitch:
    case StageError::EmergencyStop:
    case StageError::DriverAlarm:
    case StageError::LostAck:
    case StageError::Timeout:
    case StageError::Protocol:
    case StageError::Transport: return true;
    default: return false;
    }
}

} // namespace

const char* toString(StageService::OperationState state)
{
    switch (state) {
    case StageService::OperationState::Queued: return "queued";
    case StageService::OperationState::Running: return "running";
    case StageService::OperationState::Completed: return "completed";
    case StageService::OperationState::Failed: return "failed";
    case StageService::OperationState::Cancelled: return "cancelled";
    case StageService::OperationState::TimedOut: return "timed out";
    }
    return "unknown";
}

// --- reference store ---------------------------------------------------------

FileStageReferenceStore::FileStageReferenceStore(std::string path) : path_(std::move(path)) {}

std::optional<StageReferenceRecord> FileStageReferenceStore::load()
{
    std::ifstream in(path_);
    if (!in) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(in);
        StageReferenceRecord r;
        r.controllerSerial = j.at("controller_serial").get<std::string>();
        r.token = j.at("token").get<std::uint16_t>();
        r.midTravelDeclared = j.at("mid_travel_declared").get<bool>();
        r.windowMinUm = j.at("window_min_um").get<double>();
        r.windowMaxUm = j.at("window_max_um").get<double>();
        r.zeroValid = j.at("zero_valid").get<bool>();
        r.frameUncertain = j.at("frame_uncertain").get<bool>();
        // A record from the Home era has none of these keys and is ignored above.
        if (!std::isfinite(r.windowMinUm) || !std::isfinite(r.windowMaxUm) || r.windowMinUm > 0.0 ||
            r.windowMaxUm < 0.0) {
            SPDLOG_WARN("StageService: ignoring zero record {} with an invalid window", path_);
            return std::nullopt;
        }
        return r;
    } catch (const std::exception& e) {
        SPDLOG_WARN("StageService: ignoring unreadable zero record {}: {}", path_, e.what());
        return std::nullopt;
    }
}

void FileStageReferenceStore::save(const StageReferenceRecord& r)
{
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << nlohmann::json{{"controller_serial", r.controllerSerial},
                              {"token", r.token},
                              {"mid_travel_declared", r.midTravelDeclared},
                              {"window_min_um", r.windowMinUm},
                              {"window_max_um", r.windowMaxUm},
                              {"zero_valid", r.zeroValid},
                              {"frame_uncertain", r.frameUncertain}}
                   .dump();
        if (!out) {
            SPDLOG_WARN("StageService: could not write reference record {}", tmp);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) SPDLOG_WARN("StageService: could not store reference record {}: {}", path_, ec.message());
}

void FileStageReferenceStore::clear()
{
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

// --- lifecycle ---------------------------------------------------------------

StageService::StageService(serialbus::SerialBusManager& busManager, std::unique_ptr<IStageReferenceStore> store,
                           std::shared_ptr<stage::LimitsVerificationStore> limits)
    : StageService([&busManager] { return stage::createStage(stage::StageKind::Zc300, busManager); },
                   std::move(store), std::move(limits))
{
}

StageService::StageService(DriverFactory driverFactory, std::unique_ptr<IStageReferenceStore> store,
                           std::shared_ptr<stage::LimitsVerificationStore> limits)
    : driverFactory_(std::move(driverFactory)), store_(std::move(store)), limits_(std::move(limits))
{
    if (!store_) store_ = std::make_unique<MemoryStageReferenceStore>();
    worker_ = std::thread([this] { workerLoop(); });
}

StageService::~StageService()
{
    shutdown();
}

void StageService::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutDown_ || stopping_) return;
        stopping_ = true;
        cancelRequested_ = snapshot_.activeOperation;
        Job disconnect;
        disconnect.type = Job::Type::Disconnect;
        jobs_.push_back(disconnect);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    shutDown_ = true;
}

bool StageService::setConfig(const StageConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.connected) return false;
    config_ = config;
    return true;
}

StageConfig StageService::config() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

StageService::Snapshot StageService::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

std::shared_ptr<stage::IMotionStage> StageService::driver() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return driver_;
}

std::pair<StageError, std::string> StageService::runSync(Job::Type type)
{
    Job job;
    job.type = type;
    job.reply = std::make_shared<std::promise<std::pair<StageError, std::string>>>();
    auto future = job.reply->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {StageError::NotConnected, "stage service is shutting down"};
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
    return future.get();
}

std::pair<StageError, std::string> StageService::runExclusive(Job::Type type, bool midTravel)
{
    Job job;
    job.type = type;
    job.midTravel = midTravel;
    job.reply = std::make_shared<std::promise<std::pair<StageError, std::string>>>();
    auto future = job.reply->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {StageError::NotConnected, "stage service is shutting down"};
        if (snapshot_.activeOperation != 0 || exclusivePending_ > 0 || pendingDisconnects_ > 0) {
            return {StageError::Busy, "a stage operation is active; stop it or wait for it to finish"};
        }
        ++exclusivePending_;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
    auto result = future.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --exclusivePending_;
    }
    return result;
}

StageError StageService::connect(std::string* detail)
{
    auto [err, text] = runExclusive(Job::Type::Connect);
    if (detail) *detail = text;
    return err;
}

void StageService::disconnect()
{
    Job job;
    job.type = Job::Type::Disconnect;
    job.reply = std::make_shared<std::promise<std::pair<StageError, std::string>>>();
    auto future = job.reply->get_future();
    std::shared_ptr<stage::IMotionStage> toStop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        // Mark first, under the lock that admits operations: a move queued
        // behind this job ends at its first check, and no new one is admitted.
        ++pendingDisconnects_;
        if (snapshot_.activeOperation != 0) {
            cancelRequested_ = snapshot_.activeOperation;
            toStop = driver_;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
    if (toStop) toStop->stop(); // immediate; the worker ends the operation at its next poll
    future.get();
}

StageError StageService::startup(std::string* detail)
{
    const StageConfig cfg = config();
    if (!cfg.enabled) {
        if (detail) *detail = "stage disabled";
        return StageError::None;
    }
    return connect(detail);
}

StageError StageService::setZero(bool midTravel, std::string* detail)
{
    auto [err, text] = runExclusive(Job::Type::SetZero, midTravel);
    if (detail) *detail = text;
    return err;
}

StageError StageService::applyProfile()
{
    return runExclusive(Job::Type::ApplyProfile).first;
}

// --- operations: admission ---------------------------------------------------

StageError StageService::validateMoveLocked(double value, bool absolute, std::string& detail) const
{
    if (!isWhole(value)) {
        detail = "targets are whole micrometres";
        return StageError::OffGrid;
    }
    const auto& st = snapshot_.status;
    if (st.emergencyStop) return StageError::EmergencyStop;
    if (st.driverAlarm) return StageError::DriverAlarm;
    if (!snapshot_.zeroSet) {
        detail = "the stage zero is not set for this controller power-up; use Set zero here first";
        return StageError::ZeroNotSet;
    }
    const double target = absolute ? value : std::round(st.positionUm) + value;
    const double lo = snapshot_.envelopeMinUm;
    const double hi = snapshot_.envelopeMaxUm;
    const auto envelopeText = [&] {
        return "[" + std::to_string(static_cast<long long>(lo)) + ", " + std::to_string(static_cast<long long>(hi)) +
               "] um";
    };
    if (target < lo || target > hi) {
        detail = "target " + std::to_string(static_cast<long long>(target)) + " um is outside the travel envelope " +
                 envelopeText();
        return StageError::OutOfSoftLimits;
    }
    // Limit bits only ever stop a move heading toward an active switch, and a
    // stage sitting on one can always move away from it (ADR 0013 A4).
    if (target > st.positionUm && st.limitPositive) {
        detail = "the positive limit switch is active; the stage will not move toward it";
        return StageError::LimitSwitch;
    }
    if (target < st.positionUm && st.limitNegative) {
        detail = "the negative limit switch is active; the stage will not move toward it";
        return StageError::LimitSwitch;
    }
    // The one-sided approach overshoots first when the move runs against the
    // approach direction; that waypoint must stay inside the envelope too. It
    // is refused, never clamped, so the move never leaves the envelope.
    const double sign = config_.approach.direction == Direction::Positive ? 1.0 : -1.0;
    if (config_.approach.overshootUm > 0.0 && (target - st.positionUm) * sign <= 0.5) {
        const double waypoint = target - sign * config_.approach.overshootUm;
        if (std::abs(waypoint - target) >= 1.0 && (waypoint < lo || waypoint > hi)) {
            detail = "the approach overshoot to " + std::to_string(static_cast<long long>(std::llround(waypoint))) +
                     " um would leave the travel envelope " + envelopeText() +
                     "; choose a target further inside it";
            return StageError::OutOfSoftLimits;
        }
    }
    return StageError::None;
}

StageService::StartResult StageService::enqueueOperation(OperationKind kind, double targetUm, bool absolute)
{
    StartResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !snapshot_.connected || pendingDisconnects_ > 0) {
            result.error = StageError::NotConnected;
        } else if (exclusivePending_ > 0) {
            result.error = StageError::Busy;
            result.detail = "the stage is being connected or configured";
        } else if (!snapshot_.configured) {
            result.error = StageError::Misconfigured;
            result.detail = "the controller does not match the stage profile";
        } else if (snapshot_.activeOperation != 0) {
            result.error = StageError::Busy;
            result.detail = "another stage operation is active";
        } else {
            result.error = validateMoveLocked(targetUm, absolute, result.detail);
        }
        if (result.error == StageError::None) {
            result.id = nextOperation_++;
            operations_[result.id] = OperationInfo{result.id, kind, OperationState::Queued, StageError::None, {}};
            snapshot_.activeOperation = result.id;
            Job job;
            job.type = Job::Type::Operation;
            job.operation = result.id;
            job.kind = kind;
            job.targetUm = targetUm;
            job.absolute = absolute;
            jobs_.push_back(std::move(job));
        } else if (result.detail.empty()) {
            result.detail = stage::toString(result.error);
        }
    }
    if (result.accepted()) cv_.notify_all();
    return result;
}

StageService::StartResult StageService::moveTo(double targetUm)
{
    return enqueueOperation(OperationKind::Move, targetUm, true);
}

StageService::StartResult StageService::moveBy(double deltaUm)
{
    return enqueueOperation(OperationKind::Move, deltaUm, false);
}

bool StageService::limitsVerifiedFor(const std::string& serial) const
{
    return limits_ && !serial.empty() && limits_->find(serial).has_value();
}

StageError StageService::stop()
{
    std::shared_ptr<stage::IMotionStage> d;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.activeOperation != 0) cancelRequested_ = snapshot_.activeOperation;
        d = driver_;
    }
    cv_.notify_all();
    if (!d) return StageError::NotConnected;
    const StageError err = d->stop();
    stopEpoch_.fetch_add(1);
    cv_.notify_all();
    return err;
}

bool StageService::cancel(OperationId id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == 0 || snapshot_.activeOperation != id) return false;
    }
    stop();
    return true;
}

std::optional<StageService::OperationInfo> StageService::operation(OperationId id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = operations_.find(id);
    if (it == operations_.end()) return std::nullopt;
    return it->second;
}

bool StageService::waitForOperation(OperationId id, std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] {
        const auto it = operations_.find(id);
        return it == operations_.end() || terminal(it->second.state);
    });
}

bool StageService::cancelled(OperationId id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_ || pendingDisconnects_ > 0 || cancelRequested_ == id || stopEpoch_.load() != operationStopEpoch_;
}

void StageService::finishOperation(OperationId id, OperationState state, StageError error, std::string detail)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& op = operations_[id];
        op.state = state;
        op.error = error;
        op.detail = detail;
        if (snapshot_.activeOperation == id) snapshot_.activeOperation = 0;
        if (cancelRequested_ == id) cancelRequested_ = 0;
        if (state != OperationState::Completed) {
            snapshot_.lastError = detail.empty() ? stage::toString(error) : detail;
        }
        while (operations_.size() > kRetainedOperations && terminal(operations_.begin()->second.state)) {
            operations_.erase(operations_.begin());
        }
    }
    cv_.notify_all();
}

void StageService::invalidateZeroLocked(const char* why)
{
    if (snapshot_.zeroSet) SPDLOG_WARN("StageService: zero dropped: {}", why);
    snapshot_.zeroSet = false;
    snapshot_.midTravelDeclared = false;
    snapshot_.status.zeroSet = false;
    snapshot_.envelopeMinUm = snapshot_.envelopeMaxUm = 0.0;
    // Keep the window of this power-up's first zero: only a new power-up may
    // start a fresh one (ADR 0013 A3). The persisted record says "not valid".
    if (haveZeroRecord_ && zeroRecord_.zeroValid) {
        zeroRecord_.zeroValid = false;
        if (zeroRecord_.token != 0) store_->save(zeroRecord_);
    }
}

void StageService::resetPowerUpLocked(const char* why)
{
    if (snapshot_.zeroSet || haveZeroRecord_) SPDLOG_WARN("StageService: zero and window dropped: {}", why);
    snapshot_.zeroSet = false;
    snapshot_.midTravelDeclared = false;
    snapshot_.status.zeroSet = false;
    snapshot_.envelopeMinUm = snapshot_.envelopeMaxUm = 0.0;
    zeroRecord_ = StageReferenceRecord{};
    haveZeroRecord_ = false;
    store_->clear();
}

StageError StageService::checkPowerUp(const std::shared_ptr<stage::IMotionStage>& d)
{
    std::uint16_t expected = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!haveZeroRecord_ || config_.reference.powerUpTokenRegister == 0) return StageError::None;
        expected = zeroRecord_.token;
    }
    if (expected == 0) return StageError::None;
    std::uint16_t token = 0;
    if (const StageError err = d->readPowerUpToken(token); err != StageError::None) return err;
    if (token == expected) return StageError::None;
    std::lock_guard<std::mutex> lock(mutex_);
    resetPowerUpLocked("the controller was power-cycled (power-up token changed)");
    return StageError::ZeroNotSet;
}

void StageService::invalidateZero(const char* why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    invalidateZeroLocked(why);
}

void StageService::adoptZeroLocked(const StageReferenceRecord& record)
{
    zeroRecord_ = record;
    haveZeroRecord_ = true;
    const Envelope env = envelopeOf(record, config_);
    snapshot_.zeroSet = true;
    snapshot_.midTravelDeclared = record.midTravelDeclared;
    snapshot_.status.zeroSet = true;
    snapshot_.envelopeMinUm = env.min;
    snapshot_.envelopeMaxUm = env.max;
}

// --- worker ------------------------------------------------------------------

void StageService::workerLoop()
{
    for (;;) {
        Job job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const int pollMs = snapshot_.status.state == MoveState::Moving ? config_.pollMovingMs : config_.pollIdleMs;
            cv_.wait_for(lock, std::chrono::milliseconds(pollMs), [&] { return stopping_ || !jobs_.empty(); });
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                haveJob = true;
            } else if (stopping_) {
                break;
            }
        }
        if (!haveJob) {
            pollStatus();
            continue;
        }
        switch (job.type) {
        case Job::Type::Connect: {
            auto result = doConnect();
            if (job.reply) job.reply->set_value(std::move(result));
            break;
        }
        case Job::Type::Disconnect:
            doDisconnect();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pendingDisconnects_ > 0) --pendingDisconnects_; // shutdown() queues one without counting it
            }
            if (job.reply) job.reply->set_value({StageError::None, {}});
            break;
        case Job::Type::ApplyProfile: {
            std::pair<StageError, std::string> result{StageError::None, {}};
            const auto d = driver();
            const auto profile = stage::findStageProfile(config().profile);
            bool busy = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy = snapshot_.activeOperation != 0;
            }
            if (!d) result = {StageError::NotConnected, "not connected"};
            else if (busy) result = {StageError::Busy, "a stage operation is active"};
            else if (!profile) result = {StageError::InvalidArgument, "unknown stage profile"};
            else {
                result.first = d->applyProfile(*profile);
                invalidateZero("controller configuration was rewritten");
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.configured = d->isConfigured();
            }
            if (job.reply) job.reply->set_value(std::move(result));
            break;
        }
        case Job::Type::SetZero: {
            auto result = doSetZero(job.midTravel);
            if (job.reply) job.reply->set_value(std::move(result));
            break;
        }
        case Job::Type::Operation: runOperation(job); break;
        }
    }
}

std::pair<StageError, std::string> StageService::doConnect()
{
    doDisconnect();
    const StageConfig cfg = config();
    const auto profile = stage::findStageProfile(cfg.profile);
    if (!profile) return {StageError::InvalidArgument, "unknown stage profile '" + cfg.profile + "'"};

    std::shared_ptr<stage::IMotionStage> d(driverFactory_());
    stage::StageIdentity identity;
    std::string detail;
    const StageError err = d->connect(cfg.endpoint, *profile, identity, detail);
    if (err != StageError::None) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.lastError = detail;
        return {err, detail};
    }

    // Once-per-power-up zero: valid only if the controller still holds the
    // token written by the last Set zero (reads only).
    std::optional<StageReferenceRecord> restored;
    if (cfg.reference.powerUpTokenRegister != 0 && d->isConfigured()) {
        if (const auto record = store_->load()) {
            std::uint16_t token = 0;
            if (record->controllerSerial == identity.serial && record->token != 0 &&
                d->readPowerUpToken(token) == StageError::None && token == record->token) {
                restored = record;
                SPDLOG_INFO("StageService: controller {} kept the operator's zero since power-up", identity.serial);
            } else {
                SPDLOG_INFO("StageService: stored zero no longer valid (power cycle or other controller); "
                            "Set zero here required");
                store_->clear();
            }
        }
    }

    StageStatus status;
    const StageError statusErr = d->readStatus(status);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        driver_ = d;
        appliedSpeedUmS_ = 0.0;
        snapshot_ = Snapshot{};
        snapshot_.connected = true;
        snapshot_.configured = d->isConfigured();
        snapshot_.identity = identity;
        snapshot_.limitsVerified = limitsVerifiedFor(identity.serial);
        snapshot_.systemPort = cfg.endpoint.systemPort;
        if (snapshot_.systemPort.empty()) {
            for (const auto& port : serialbus::availablePorts()) {
                if (port.serialNumber == cfg.endpoint.usbSerial) snapshot_.systemPort = port.systemName;
            }
        }
        snapshot_.spanUm = cfg.reference.expectedSpanUm;
        zeroRecord_ = StageReferenceRecord{};
        haveZeroRecord_ = false;
        if (statusErr == StageError::None) snapshot_.status = status;
        if (restored) {
            // The window of this power-up is kept either way.
            zeroRecord_ = *restored;
            haveZeroRecord_ = true;
            // An e-stop or driver alarm at connect means the counter may be
            // off: do not trust a stored zero.
            if (restored->zeroValid && !restored->frameUncertain && statusErr == StageError::None &&
                !status.emergencyStop && !status.driverAlarm) {
                adoptZeroLocked(*restored);
            } else if (restored->zeroValid) {
                SPDLOG_WARN("StageService: stored zero not restored (status unreadable, e-stop or driver alarm)");
                invalidateZeroLocked("e-stop, driver alarm or unreadable status at connect");
            }
        }
        if (!snapshot_.configured) snapshot_.lastError = detail;
    }
    return {StageError::None, detail};
}

void StageService::doDisconnect()
{
    std::shared_ptr<stage::IMotionStage> d;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        d = driver_;
    }
    if (d) d->disconnect(); // stops the axis only if it is moving
    std::lock_guard<std::mutex> lock(mutex_);
    driver_.reset();
    const std::string lastError = snapshot_.lastError;
    snapshot_ = Snapshot{};
    snapshot_.lastError = lastError;
}

void StageService::pollStatus()
{
    const auto d = driver();
    if (!d) return;
    StageStatus s;
    const StageError err = d->readStatus(s);
    if (err == StageError::None) {
        publishStatus(s);
        // A power cycle while connected resets the counter and the token: the
        // zero, its window and the stored record must not outlive it. A read
        // error here is transient and only logged; moves insist on a match.
        if (const StageError tokenErr = checkPowerUp(d); tokenErr != StageError::None && tokenErr != StageError::ZeroNotSet) {
            SPDLOG_WARN("StageService: power-up token not readable: {}", stage::toString(tokenErr));
        }
        // A supervised check run by the bench tool while the application is
        // open takes effect without reconnecting (it only clears a badge).
        std::string serial;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            serial = snapshot_.identity.serial;
        }
        const bool verified = limitsVerifiedFor(serial); // file read, outside the lock
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.connected) snapshot_.limitsVerified = verified;
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.lastError = std::string("status poll: ") + stage::toString(err);
    }
}

void StageService::publishStatus(const StageStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.status = status;
    snapshot_.status.zeroSet = snapshot_.zeroSet;
    // An e-stop or a driver alarm can desync the open-loop counter, whether or
    // not a move was running: the operator must set zero again (ADR 0013 A2).
    if ((status.emergencyStop || status.driverAlarm) && snapshot_.zeroSet) {
        invalidateZeroLocked(status.emergencyStop ? "emergency stop" : "driver alarm");
    }
}

void StageService::runOperation(const Job& job)
{
    const OperationId id = job.operation;
    operationStopEpoch_ = stopEpoch_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_ && cancelRequested_ != id) operations_[id].state = OperationState::Running;
    }
    if (cancelled(id)) {
        finishOperation(id, OperationState::Cancelled, StageError::None, "cancelled before start");
        return;
    }

    std::string detail;
    bool hadZero = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hadZero = snapshot_.zeroSet;
    }
    const StageError err = runMove(id, job.targetUm, job.absolute, detail);
    const bool wasCancelled = cancelled(id);
    if (err != StageError::None || wasCancelled) {
        if (const auto d = driver()) d->stop(); // never leave the axis running
        pollStatus();
    }
    if (err != StageError::None && hadZero && losesReference(err)) {
        invalidateZero(stage::toString(err));
    }

    OperationState state = OperationState::Completed;
    if (wasCancelled) state = OperationState::Cancelled;
    else if (err == StageError::Timeout) state = OperationState::TimedOut;
    else if (err != StageError::None) state = OperationState::Failed;
    if (state != OperationState::Completed) {
        SPDLOG_WARN("StageService: operation {} {}: {}", id, toString(state),
                    detail.empty() ? stage::toString(err) : detail);
    }
    finishOperation(id, state, wasCancelled ? StageError::None : err, detail);
}

StageError StageService::applySpeed(double umPerS)
{
    if (appliedSpeedUmS_ == umPerS) return StageError::None;
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    const StageError err = d->setSpeed(umPerS, config().accelUmS2);
    if (err == StageError::None) appliedSpeedUmS_ = umPerS;
    return err;
}

StageError StageService::waitIdle(OperationId id, Clock::time_point deadline, StageStatus& status,
                                  std::string& detail,
                                  const std::function<StageError(const StageStatus&)>& abortIf)
{
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    const int pollMs = config().pollMovingMs;
    for (;;) {
        if (cancelled(id)) {
            d->stop();
            return StageError::None;
        }
        const StageError err = d->readStatus(status);
        if (err != StageError::None) return err;
        publishStatus(status);
        if (status.emergencyStop) return StageError::EmergencyStop;
        if (status.driverAlarm) return StageError::DriverAlarm;
        if (abortIf) {
            if (const StageError abort = abortIf(status); abort != StageError::None) {
                d->stop();
                return abort;
            }
        }
        if (status.state != MoveState::Moving) return StageError::None;
        if (Clock::now() > deadline) {
            d->stop();
            detail = "the move did not finish before its deadline";
            return StageError::Timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
}

StageError StageService::moveAndWait(OperationId id, double targetUm, double speedUmS, std::string& detail)
{
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    if (cancelled(id)) return StageError::None;
    StageStatus status;
    StageError err = d->readStatus(status);
    if (err != StageError::None) return err;
    const double target = std::round(targetUm);
    const double distance = std::abs(target - status.positionUm);
    if (distance < 0.5) return StageError::None;
    // Admission used the cached status. Whatever happened since (an e-stop, an
    // alarm, someone else moving the axis, a power cycle) is decided on fresh
    // facts before any opcode goes out.
    publishStatus(status); // an e-stop or alarm drops the zero here
    if (status.emergencyStop) return StageError::EmergencyStop;
    if (status.driverAlarm || status.state == MoveState::Faulted) return StageError::DriverAlarm;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.zeroSet) {
            detail = "the stage zero is no longer set";
            return StageError::ZeroNotSet;
        }
        if (target < snapshot_.envelopeMinUm || target > snapshot_.envelopeMaxUm) {
            detail = "target " + std::to_string(static_cast<long long>(target)) + " um is outside the travel envelope";
            return StageError::OutOfSoftLimits;
        }
    }
    if (status.state != MoveState::Idle) {
        detail = "the axis is already moving; no new motion command was sent";
        return StageError::Busy;
    }
    if ((err = checkPowerUp(d)) != StageError::None) {
        if (err == StageError::ZeroNotSet) detail = "the controller was power-cycled; set zero again";
        return err;
    }
    // Limit bits are a backstop that only stops (ADR 0013 A4): never a
    // precondition, and only the switch in the direction of travel counts, so
    // a stage sitting on a switch can always move away from it.
    const bool towardPositive = target > status.positionUm;
    const auto towardLimit = [towardPositive](const StageStatus& s) {
        return towardPositive ? s.limitPositive : s.limitNegative;
    };
    if (towardLimit(status)) {
        detail = std::string("the ") + (towardPositive ? "positive" : "negative") +
                 " limit switch is active; the stage will not move toward it";
        return StageError::LimitSwitch;
    }
    if ((err = d->moveAbsolute(target)) != StageError::None) return err;
    const auto budget = std::chrono::duration<double>(distance / speedUmS * config().moveTimeoutMargin + 1.0);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(budget);
    if ((err = waitIdle(id, deadline, status, detail, [&](const StageStatus& s) {
             if (!towardLimit(s)) return StageError::None;
             detail = std::string("the ") + (towardPositive ? "positive" : "negative") +
                      " limit switch became active during the move; stopped";
             return StageError::LimitSwitch;
         })) != StageError::None) {
        return err;
    }
    if (cancelled(id)) return StageError::None;
    // Whole-µm targets land within half a pulse (0.22 µm); anything further
    // means the axis stopped early.
    if (std::abs(status.positionUm - target) > 1.0) {
        if (status.limitPositive || status.limitNegative) {
            detail = "a limit switch stopped the move short of its target";
            return StageError::LimitSwitch;
        }
        detail = "the axis stopped short of its target";
        return StageError::Protocol;
    }
    return StageError::None;
}

StageError StageService::approachAndWait(OperationId id, double targetUm, double minUm, double maxUm,
                                         std::string& detail)
{
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    const StageConfig cfg = config();
    StageStatus status;
    StageError err = d->readStatus(status);
    if (err != StageError::None) return err;
    // One-sided approach: the final segment always runs in the configured
    // direction, so backlash is taken up the same way every time.
    const double sign = cfg.approach.direction == Direction::Positive ? 1.0 : -1.0;
    if (cfg.approach.overshootUm > 0.0 && (targetUm - status.positionUm) * sign <= 0.5) {
        const double waypoint = targetUm - sign * cfg.approach.overshootUm;
        if (waypoint < minUm || waypoint > maxUm) {
            detail = "the approach overshoot would leave the travel envelope";
            return StageError::OutOfSoftLimits;
        }
        if (std::abs(waypoint - targetUm) >= 1.0) {
            if ((err = moveAndWait(id, waypoint, cfg.speedUmS, detail)) != StageError::None) return err;
        }
    }
    return moveAndWait(id, targetUm, cfg.speedUmS, detail);
}

StageError StageService::runMove(OperationId id, double value, bool absolute, std::string& detail)
{
    double target = 0.0;
    Envelope env;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-check at start: the stage may have lost its zero or hit an
        // e-stop since the operation was queued.
        if (const StageError err = validateMoveLocked(value, absolute, detail); err != StageError::None) return err;
        target = absolute ? value : std::round(snapshot_.status.positionUm) + value;
        env = {snapshot_.envelopeMinUm, snapshot_.envelopeMaxUm};
    }
    if (const StageError err = applySpeed(config().speedUmS); err != StageError::None) return err;
    return approachAndWait(id, target, env.min, env.max, detail);
}

std::pair<StageError, std::string> StageService::doSetZero(bool midTravel)
{
    const auto d = driver();
    const StageConfig cfg = config();
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!d || !snapshot_.connected) return {StageError::NotConnected, "not connected"};
        if (!snapshot_.configured) {
            return {StageError::Misconfigured, "the controller does not match the stage profile"};
        }
        if (snapshot_.activeOperation != 0) return {StageError::Busy, "a stage operation is active"};
        serial = snapshot_.identity.serial;
    }
    // A power cycle since the last poll must not leave an old window behind.
    if (const StageError err = checkPowerUp(d); err != StageError::None && err != StageError::ZeroNotSet) {
        return {err, "could not read the controller's power-up token"};
    }
    StageStatus status;
    if (const StageError err = d->readStatus(status); err != StageError::None) {
        return {err, "could not read the stage status"};
    }
    if (status.emergencyStop) return {StageError::EmergencyStop, "emergency stop is active"};
    if (status.driverAlarm || status.state == MoveState::Faulted) {
        return {StageError::DriverAlarm, "the driver reports an alarm"};
    }
    if (status.state != MoveState::Idle) return {StageError::Busy, "the axis is moving; stop it first"};
    const double position = status.positionUm;
    if (!std::isfinite(position)) return {StageError::Protocol, "the position counter is unreadable"};

    StageReferenceRecord previous;
    bool windowKnown = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = zeroRecord_;
        windowKnown = haveZeroRecord_;
    }
    // ADR 0013 A3: without a mid-travel declaration a later zero must stay
    // inside the window the first zero of this power-up set, even if the zero
    // was dropped in between (e-stop, alarm, applied profile...), so repeated
    // re-zeroing cannot walk the envelope along the stage.
    if (windowKnown && !midTravel) {
        if (previous.frameUncertain) {
            return {StageError::OutOfSoftLimits,
                    "an earlier Set zero was interrupted, so the first window can no longer be trusted; declare "
                    "mid-travel (only if the stage really is there) or power-cycle the controller"};
        }
        if (position < previous.windowMinUm || position > previous.windowMaxUm) {
            return {StageError::OutOfSoftLimits,
                    "the stage is outside the window set by the first zero of this power-up; declare mid-travel "
                    "(only if the stage really is there) or power-cycle the controller"};
        }
    }

    StageReferenceRecord record;
    record.controllerSerial = serial;
    record.midTravelDeclared = midTravel;
    if (midTravel) {
        record.windowMinUm = -capFor(cfg);
        record.windowMaxUm = capFor(cfg);
    } else if (windowKnown) {
        record.windowMinUm = previous.windowMinUm - position;
        record.windowMaxUm = previous.windowMaxUm - position;
    } else {
        record.windowMinUm = -cfg.envelope.defaultUm;
        record.windowMaxUm = cfg.envelope.defaultUm;
    }

    // Order matters for a crash at any point: (1) a fresh power-up token goes to
    // the controller first, so no stored record, however stale or undeletable,
    // can match it any more; (2) an interim record keeps the old window but is
    // not restorable; (3) the counter is rewritten; (4) the final record. The
    // token is what makes this safe; the store is only trusted when it says "no".
    const bool tokenUsed = cfg.reference.powerUpTokenRegister != 0;
    std::uint16_t token = 0;
    if (tokenUsed) {
        token = randomToken();
        if (const StageError err = d->writePowerUpToken(token); err != StageError::None) {
            return {err, "could not write the power-up token (set stage.reference.power_up_token_register to 0 to "
                         "hold the zero for this session only); nothing was changed"};
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The zero is not usable until the counter write has been confirmed.
        snapshot_.zeroSet = false;
        snapshot_.midTravelDeclared = false;
        snapshot_.status.zeroSet = false;
        snapshot_.envelopeMinUm = snapshot_.envelopeMaxUm = 0.0;
        if (windowKnown) {
            zeroRecord_.token = token;
            zeroRecord_.zeroValid = false;
            zeroRecord_.frameUncertain = true;
            if (tokenUsed) store_->save(zeroRecord_);
        } else if (tokenUsed) {
            store_->clear(); // a record for another power-up can never match the new token anyway
        }
    }
    if (const StageError err = d->setPosition(0.0); err != StageError::None) {
        std::lock_guard<std::mutex> lock(mutex_);
        // The write may or may not have happened: the old window is in an unknown frame.
        if (haveZeroRecord_) zeroRecord_.frameUncertain = true;
        if (haveZeroRecord_ && tokenUsed) store_->save(zeroRecord_);
        snapshot_.lastError = "set zero: the position write failed";
        return {err, "could not write the position counter"};
    }
    record.token = token;
    record.zeroValid = true;
    record.frameUncertain = false;
    Envelope env;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        adoptZeroLocked(record);
        if (tokenUsed) store_->save(record);
        else store_->clear();
        env = {snapshot_.envelopeMinUm, snapshot_.envelopeMaxUm};
    }
    pollStatus();
    SPDLOG_INFO("StageService: zero set here (counter was {:.1f} um); mid-travel declared: {}; travel envelope "
                "[{:.0f}, {:.0f}] um",
                position, midTravel ? "yes" : "no", env.min, env.max);
    return {StageError::None, "zero set here; travel envelope [" + std::to_string(static_cast<long long>(env.min)) +
                                  ", " + std::to_string(static_cast<long long>(env.max)) + "] um"};
}

} // namespace backend::services
