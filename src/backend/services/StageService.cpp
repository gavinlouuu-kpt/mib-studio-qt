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
        r.spanUm = j.at("span_um").get<double>();
        return r;
    } catch (const std::exception& e) {
        SPDLOG_WARN("StageService: ignoring unreadable reference record {}: {}", path_, e.what());
        return std::nullopt;
    }
}

void FileStageReferenceStore::save(const StageReferenceRecord& r)
{
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << nlohmann::json{{"controller_serial", r.controllerSerial}, {"token", r.token}, {"span_um", r.spanUm}}
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

StageService::StageService(serialbus::SerialBusManager& busManager, std::unique_ptr<IStageReferenceStore> store)
    : StageService([&busManager] { return stage::createStage(stage::StageKind::Zc300, busManager); },
                   std::move(store))
{
}

StageService::StageService(DriverFactory driverFactory, std::unique_ptr<IStageReferenceStore> store)
    : driverFactory_(std::move(driverFactory)), store_(std::move(store))
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

StageError StageService::connect(std::string* detail)
{
    auto [err, text] = runSync(Job::Type::Connect);
    if (detail) *detail = text;
    return err;
}

void StageService::disconnect()
{
    runSync(Job::Type::Disconnect);
}

StageError StageService::startup(std::string* detail)
{
    const StageConfig cfg = config();
    if (!cfg.enabled) {
        if (detail) *detail = "stage disabled";
        return StageError::None;
    }
    const StageError err = connect(detail);
    if (err != StageError::None) return err;
    const Snapshot s = snapshot();
    if (cfg.reference.onStartup && s.configured && !s.referenced) {
        SPDLOG_INFO("StageService: reference.on_startup is set; homing at start-up");
        const StartResult r = reference();
        if (!r.accepted()) SPDLOG_WARN("StageService: start-up Home refused: {}", r.detail);
    }
    return StageError::None;
}

StageError StageService::applyProfile()
{
    return runSync(Job::Type::ApplyProfile).first;
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
    if (!snapshot_.referenced) {
        if (!config_.requireReference) return StageError::None;
        if (absolute || std::abs(value) > config_.maxUnreferencedJogUm) {
            detail = "the stage is not homed for this power-up; press Home first";
            return StageError::NotReferenced;
        }
        return StageError::None;
    }
    const double target = absolute ? value : std::round(st.positionUm) + value;
    if (target < snapshot_.softMinUm || target > snapshot_.softMaxUm) {
        detail = "target " + std::to_string(static_cast<long long>(target)) + " um is outside the soft limits";
        return StageError::OutOfSoftLimits;
    }
    return StageError::None;
}

StageService::StartResult StageService::enqueueOperation(OperationKind kind, double targetUm, bool absolute)
{
    StartResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !snapshot_.connected) {
            result.error = StageError::NotConnected;
        } else if (!snapshot_.configured) {
            result.error = StageError::Misconfigured;
            result.detail = "the controller does not match the stage profile";
        } else if (snapshot_.activeOperation != 0) {
            result.error = StageError::Busy;
            result.detail = "another stage operation is active";
        } else if (kind == OperationKind::Move) {
            result.error = validateMoveLocked(targetUm, absolute, result.detail);
        } else if (snapshot_.status.emergencyStop) {
            result.error = StageError::EmergencyStop;
        } else if (snapshot_.status.driverAlarm) {
            result.error = StageError::DriverAlarm;
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

StageService::StartResult StageService::reference()
{
    return enqueueOperation(OperationKind::Reference, 0.0, true);
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
    return stopping_ || cancelRequested_ == id || stopEpoch_.load() != operationStopEpoch_;
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

void StageService::invalidateReference(const char* why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.referenced) SPDLOG_WARN("StageService: reference dropped: {}", why);
    snapshot_.referenced = false;
    snapshot_.status.referenced = false;
    snapshot_.spanUm = snapshot_.softMinUm = snapshot_.softMaxUm = 0.0;
    store_->clear();
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
                invalidateReference("controller configuration was rewritten");
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.configured = d->isConfigured();
            }
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

    // Once-per-power-up reference: valid only if the controller still holds
    // the token written by the last successful Home (reads only).
    bool referenced = false;
    double span = 0.0;
    if (cfg.reference.powerUpTokenRegister != 0 && d->isConfigured()) {
        if (const auto record = store_->load()) {
            std::uint16_t token = 0;
            if (record->controllerSerial == identity.serial && record->token != 0 &&
                d->readPowerUpToken(token) == StageError::None && token == record->token) {
                referenced = true;
                span = record->spanUm;
                SPDLOG_INFO("StageService: controller {} kept its Home since power-up (span {:.1f} um)",
                            identity.serial, span);
            } else {
                SPDLOG_INFO("StageService: stored reference no longer valid (power cycle or other "
                            "controller); Home required");
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
        snapshot_.referenced = referenced;
        if (referenced) {
            snapshot_.spanUm = span;
            snapshot_.softMaxUm = span / 2.0 - cfg.reference.softLimitMarginUm;
            snapshot_.softMinUm = -snapshot_.softMaxUm;
        }
        if (statusErr == StageError::None) {
            status.referenced = referenced;
            snapshot_.status = status;
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
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.lastError = std::string("status poll: ") + stage::toString(err);
    }
}

void StageService::publishStatus(const StageStatus& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.status = status;
    snapshot_.status.referenced = snapshot_.referenced;
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
    bool wasReferenced = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasReferenced = snapshot_.referenced;
    }
    const StageError err = job.kind == OperationKind::Move ? runMove(id, job.targetUm, job.absolute, detail)
                                                           : runReference(id, detail);
    const bool wasCancelled = cancelled(id);
    if (err != StageError::None || wasCancelled) {
        if (const auto d = driver()) d->stop(); // never leave the axis running
        pollStatus();
    }
    if (err != StageError::None && wasReferenced && losesReference(err)) {
        invalidateReference(stage::toString(err));
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
                                  std::string& detail, const std::function<bool(const StageStatus&)>& abortIf)
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
        if (abortIf && abortIf(status)) {
            d->stop();
            return StageError::ReferenceFailed;
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
    if ((err = d->moveAbsolute(target)) != StageError::None) return err;
    const auto budget = std::chrono::duration<double>(distance / speedUmS * config().moveTimeoutMargin + 1.0);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(budget);
    if ((err = waitIdle(id, deadline, status, detail)) != StageError::None) return err;
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
        const double waypoint = std::clamp(targetUm - sign * cfg.approach.overshootUm, minUm, maxUm);
        if (std::abs(waypoint - targetUm) >= 1.0) {
            if ((err = moveAndWait(id, waypoint, cfg.speedUmS, detail)) != StageError::None) return err;
        }
    }
    return moveAndWait(id, targetUm, cfg.speedUmS, detail);
}

StageError StageService::runMove(OperationId id, double value, bool absolute, std::string& detail)
{
    double target = 0.0;
    double minUm = -std::numeric_limits<double>::infinity();
    double maxUm = std::numeric_limits<double>::infinity();
    bool referenced = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-check at start: the stage may have lost its reference or hit an
        // e-stop since the operation was queued.
        if (const StageError err = validateMoveLocked(value, absolute, detail); err != StageError::None) return err;
        target = absolute ? value : std::round(snapshot_.status.positionUm) + value;
        referenced = snapshot_.referenced;
        if (referenced) {
            minUm = snapshot_.softMinUm;
            maxUm = snapshot_.softMaxUm;
        }
    }
    if (const StageError err = applySpeed(config().speedUmS); err != StageError::None) return err;
    // An unreferenced jog (only when a rig allows it) moves directly, so the
    // overshoot never exceeds max_unreferenced_jog_um.
    if (!referenced) return moveAndWait(id, target, config().speedUmS, detail);
    return approachAndWait(id, target, minUm, maxUm, detail);
}

StageError StageService::jogToLimit(OperationId id, Direction direction, double& positionUm, std::string& detail)
{
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    const StageConfig cfg = config();
    StageStatus status;
    StageError err = d->readStatus(status);
    if (err != StageError::None) return err;
    const bool negative = direction == Direction::Negative;
    if (negative ? status.limitNegative : status.limitPositive) {
        positionUm = status.positionUm;
        return StageError::None;
    }
    if ((err = applySpeed(cfg.reference.searchSpeedUmS)) != StageError::None) return err;
    const double start = status.positionUm;
    const double maxTravel = cfg.reference.expectedSpanUm + cfg.reference.searchMarginUm;
    const auto budget = std::chrono::duration<double>(
        maxTravel / cfg.reference.searchSpeedUmS * cfg.moveTimeoutMargin + 2.0);
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(budget);
    if ((err = d->jog(direction)) != StageError::None) return err;
    const char* side = negative ? "negative" : "positive";
    err = waitIdle(id, deadline, status, detail, [&](const StageStatus& s) {
        if (std::abs(s.positionUm - start) <= maxTravel) return false;
        detail = std::string("no ") + side + " limit within " + std::to_string(static_cast<int>(maxTravel)) + " um";
        return true;
    });
    if (err != StageError::None || cancelled(id)) return err;
    if (!(negative ? status.limitNegative : status.limitPositive)) {
        detail = std::string("the stage stopped before the ") + side + " limit switch";
        return StageError::ReferenceFailed;
    }
    positionUm = status.positionUm;
    return StageError::None;
}

StageError StageService::runReference(OperationId id, std::string& detail)
{
    const auto d = driver();
    if (!d) return StageError::NotConnected;
    const StageConfig cfg = config();
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        serial = snapshot_.identity.serial;
    }
    invalidateReference("Home started");
    SPDLOG_INFO("StageService: Home started (probing both limits)");

    double negUm = 0.0;
    double posUm = 0.0;
    StageError err = jogToLimit(id, Direction::Negative, negUm, detail);
    if (err == StageError::None && !cancelled(id)) err = jogToLimit(id, Direction::Positive, posUm, detail);
    if (err != StageError::None || cancelled(id)) return err;

    const double span = posUm - negUm;
    if (std::abs(span - cfg.reference.expectedSpanUm) > cfg.reference.spanToleranceUm) {
        detail = "measured span " + std::to_string(static_cast<int>(std::lround(span))) + " um is outside " +
                 std::to_string(static_cast<int>(cfg.reference.expectedSpanUm)) + " +/- " +
                 std::to_string(static_cast<int>(cfg.reference.spanToleranceUm)) + " um";
        return StageError::ReferenceFailed;
    }
    if ((err = applySpeed(cfg.speedUmS)) != StageError::None) return err;
    const double mid = std::round((negUm + posUm) / 2.0);
    if ((err = approachAndWait(id, mid, negUm, posUm, detail)) != StageError::None || cancelled(id)) return err;
    if ((err = d->setPosition(0.0)) != StageError::None) return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.referenced = true;
        snapshot_.spanUm = span;
        snapshot_.softMaxUm = span / 2.0 - cfg.reference.softLimitMarginUm;
        snapshot_.softMinUm = -snapshot_.softMaxUm;
    }
    if (cfg.reference.powerUpTokenRegister != 0) {
        const std::uint16_t token = randomToken();
        if (d->writePowerUpToken(token) == StageError::None) {
            store_->save({serial, token, span});
        } else {
            SPDLOG_WARN("StageService: power-up token not written; the reference lasts this session only");
        }
    }
    pollStatus();
    SPDLOG_INFO("StageService: Home complete: span {:.1f} um, zero at mid-travel, soft limits +/-{:.1f} um", span,
                span / 2.0 - cfg.reference.softLimitMarginUm);
    return StageError::None;
}

} // namespace backend::services
