#include "backend/stage/zc300/Zc300Stage.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace backend::stage::zc300 {

namespace serialbus = services::serialbus;

namespace {

StageError fromBus(const serialbus::Transaction& t)
{
    switch (t.error) {
    case serialbus::BusError::None: return StageError::None;
    case serialbus::BusError::ModbusException: return errorFromException(t.exceptionCode);
    case serialbus::BusError::Timeout: return StageError::Timeout;
    case serialbus::BusError::PortUnavailable:
    case serialbus::BusError::PortBusy:
    case serialbus::BusError::NotOpen:
    case serialbus::BusError::WriteFailed: return StageError::Transport;
    default: return StageError::Protocol;
    }
}

std::string resolvePort(const StageEndpoint& endpoint)
{
    if (!endpoint.systemPort.empty() || endpoint.usbSerial.empty()) return endpoint.systemPort;
    for (const auto& port : serialbus::availablePorts()) {
        if (port.serialNumber == endpoint.usbSerial) return port.systemName;
    }
    return {};
}

std::int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// RAII access to the driver; see the Waiter / queue notes in the header.
// Stop: its own queue, served first. Command: the command queue, bounded by
// kLockTimeout. Poll: behind every waiting command, bounded the same way.
// Lifecycle (disconnect): the command queue, but it waits however long it
// takes, because teardown must not be skipped.
class Zc300Stage::Access {
public:
    enum class Kind { Stop, Command, Poll, Lifecycle };

    Access(const Zc300Stage& stage, Kind kind) : stage_(stage)
    {
        std::unique_lock<std::mutex> lock(stage_.gate_);
        if (!stage_.held_) { // free: a release with waiters hands off without ever clearing held_
            stage_.held_ = true;
            owned_ = true;
            return;
        }
        Waiter waiter;
        auto& queue = kind == Kind::Stop ? stage_.stopQueue_ : kind == Kind::Poll ? stage_.pollQueue_ : stage_.commandQueue_;
        queue.push_back(&waiter);
        if (kind == Kind::Lifecycle) {
            stage_.gateCv_.wait(lock, [&] { return waiter.granted; });
            owned_ = true;
            return;
        }
        if (stage_.gateCv_.wait_for(lock, kLockTimeout, [&] { return waiter.granted; })) {
            owned_ = true;
            return;
        }
        queue.erase(std::find(queue.begin(), queue.end(), &waiter)); // timed out; still queued, under gate_
        SPDLOG_WARN("Zc300Stage: driver busy for {} s; call refused", kLockTimeout.count());
    }
    ~Access()
    {
        if (!owned_) return;
        std::lock_guard<std::mutex> lock(stage_.gate_);
        Waiter* next = nullptr;
        const auto take = [&next](std::deque<Waiter*>& queue) {
            next = queue.front();
            queue.pop_front();
        };
        // Stop first, but never more than kMaxConsecutiveStops in a row while
        // a command, teardown or poll waits: a Stop storm must not starve them.
        const bool othersWaiting = !stage_.commandQueue_.empty() || !stage_.pollQueue_.empty();
        if (!stage_.stopQueue_.empty() && (stage_.consecutiveStops_ < kMaxConsecutiveStops || !othersWaiting)) {
            take(stage_.stopQueue_);
            ++stage_.consecutiveStops_;
        } else if (!stage_.commandQueue_.empty()) {
            take(stage_.commandQueue_);
            stage_.consecutiveStops_ = 0;
        } else if (!stage_.pollQueue_.empty()) {
            take(stage_.pollQueue_);
            stage_.consecutiveStops_ = 0;
        } else {
            stage_.consecutiveStops_ = 0;
        }
        if (next) { // direct hand-off: held_ stays true
            next->granted = true;
            stage_.gateCv_.notify_all();
        } else {
            stage_.held_ = false;
        }
    }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    bool owned() const { return owned_; }

private:
    const Zc300Stage& stage_;
    bool owned_{false};
};

Zc300Stage::Zc300Stage(serialbus::SerialBusManager& busManager) : Zc300Stage(busManager, Timing{}) {}

Zc300Stage::Zc300Stage(serialbus::SerialBusManager& busManager, Timing timing)
    : busManager_(busManager), timing_(timing)
{
}

Zc300Stage::~Zc300Stage()
{
    disconnect();
}

StageError Zc300Stage::connect(const StageEndpoint& endpoint, const StageProfile& profile,
                               StageIdentity& identity, std::string& detail)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) {
        detail = "driver busy";
        return StageError::Busy;
    }
    releaseLocked();
    if (endpoint.axis < 0 || endpoint.axis >= kAxisCount) {
        detail = "axis out of range";
        return StageError::InvalidArgument;
    }
    const std::string port = resolvePort(endpoint);
    if (port.empty()) {
        detail = "no serial port for USB serial '" + endpoint.usbSerial + "'";
        return StageError::Transport;
    }

    serialbus::SerialSettings settings;
    settings.baudRate = kBaudRate;
    serialbus::BusError busError = serialbus::BusError::None;
    std::string busDetail;
    bus_ = busManager_.acquire(port, settings, &busError, &busDetail);
    if (!bus_) {
        detail = port + ": " + serialbus::toString(busError) + (busDetail.empty() ? "" : " (" + busDetail + ")");
        return StageError::Transport;
    }
    address_ = endpoint.modbusAddress;
    axis_ = endpoint.axis;
    profile_ = profile;

    Frame model;
    Frame serialAndFirmware;
    StageError err = readLocked(kRegModel, 7, model);
    if (err == StageError::None) err = readLocked(kRegSerial, 3, serialAndFirmware); // 30008..30010
    if (err != StageError::None) {
        detail = port + ": no ZC300 identity response (" + toString(err) + ")";
        releaseLocked();
        return err == StageError::Protocol ? StageError::WrongDevice : err;
    }
    identity.model = decodeAscii(model);
    if (identity.model.rfind("ZC300", 0) != 0) {
        detail = port + ": model '" + identity.model + "' is not a ZC300";
        releaseLocked();
        return StageError::WrongDevice;
    }
    identity.serial = std::to_string(decodeLong(serialAndFirmware.data()));
    const std::uint16_t fw = decodeU16(serialAndFirmware.data() + 4);
    identity.firmware = std::to_string(fw / 10) + "." + std::to_string(fw % 10);

    ControllerConfig config;
    if ((err = readConfigLocked(config)) != StageError::None) {
        detail = port + ": could not read axis configuration (" + toString(err) + ")";
        releaseLocked();
        return err;
    }
    config_ = config;
    configured_ = configMatches(config, profile);
    connected_ = true;
    if (!configured_) {
        detail = "controller configuration does not match profile '" + profile.name +
                 "'; motion refused until the profile is applied";
        SPDLOG_WARN("Zc300Stage: {} {} on {} axis {}: {}", identity.model, identity.serial, port,
                    axis_, detail);
    } else {
        SPDLOG_INFO("Zc300Stage: connected {} serial {} fw {} on {} axis {} ({:.4f} um/pulse)",
                    identity.model, identity.serial, identity.firmware, port, axis_,
                    calibrationFor(config).umPerPulse);
    }
    return StageError::None;
}

void Zc300Stage::disconnect()
{
    Access access(*this, Access::Kind::Lifecycle);
    if (connected_) {
        StageStatus status;
        if (readStatusLocked(status) == StageError::None && status.state == MoveState::Moving) {
            SPDLOG_INFO("Zc300Stage: stopping the moving axis before disconnect");
            writeLocked(buildOpcode(address_, Opcode::Stop, 1, axisCode(axis_)), timing_.transactionMs);
        }
    }
    releaseLocked();
}

void Zc300Stage::releaseLocked()
{
    bus_.reset();
    connected_ = false;
    configured_ = false;
}

bool Zc300Stage::isConnected() const { return connected_.load(); }
bool Zc300Stage::isConfigured() const { return configured_.load(); }

AxisCalibration Zc300Stage::calibration() const
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return AxisCalibration{};
    return calibrationFor(config_);
}

std::size_t Zc300Stage::waitingCalls() const
{
    std::lock_guard<std::mutex> lock(gate_);
    return stopQueue_.size() + commandQueue_.size() + pollQueue_.size();
}

ControllerConfig Zc300Stage::controllerConfig() const
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return ControllerConfig{};
    return config_;
}

StageError Zc300Stage::readLocked(int reg, std::uint16_t count, Frame& data)
{
    if (!bus_) return StageError::NotConnected;
    const Frame request = buildRead(address_, reg, count);
    for (int attempt = 0; attempt <= timing_.silenceRetries; ++attempt) {
        const auto t = bus_->transact(request, timing_.transactionMs);
        if (t.error == serialbus::BusError::Timeout) continue;
        if (t.error != serialbus::BusError::None) return fromBus(t);
        if (!services::modbus::extractReadData(t.response, count, data)) return StageError::Protocol;
        return StageError::None;
    }
    return StageError::Timeout;
}

StageError Zc300Stage::writeLocked(const Frame& request, int timeoutMs)
{
    if (!bus_) return StageError::NotConnected;
    for (int attempt = 0; attempt <= timing_.silenceRetries; ++attempt) {
        const auto t = bus_->transact(request, timeoutMs);
        if (t.error == serialbus::BusError::Timeout) continue;
        return fromBus(t);
    }
    return StageError::Timeout;
}

StageError Zc300Stage::motionLocked(Opcode op, std::uint16_t direction)
{
    // Sent exactly once: a re-sent move could run twice. When the reply is
    // lost, status decides whether the controller accepted the command.
    const auto t = bus_->transact(buildOpcode(address_, op, 2, axisCode(axis_), direction),
                                  timing_.transactionMs);
    if (t.error != serialbus::BusError::Timeout) return fromBus(t);
    StageStatus status;
    if (readStatusLocked(status) == StageError::None && status.state == MoveState::Moving) {
        SPDLOG_WARN("Zc300Stage: motion command reply lost; axis is moving, treating as accepted");
        return StageError::None;
    }
    SPDLOG_WARN("Zc300Stage: motion command reply lost and the axis is not moving");
    return StageError::LostAck;
}

StageError Zc300Stage::readStatusLocked(StageStatus& status)
{
    Frame data;
    const StageError err = readLocked(kStatusBlockStart, kStatusBlockCount, data);
    if (err != StageError::None) return err;
    const std::uint16_t moving = decodeU16(data.data() + 2 * axis_);
    const SwitchState sw = decodeSwitches(decodeU16(data.data() + 6), axis_);
    const float raw = decodeFloat(data.data() + 8 + 4 * axis_);
    status.positionUm = positionToMicrons(raw, config_);
    status.limitPositive = sw.limitPositive;
    status.limitNegative = sw.limitNegative;
    status.home = sw.home;
    status.emergencyStop = sw.emergencyStop;
    status.driverAlarm = sw.driverAlarm;
    status.zeroSet = false;
    status.state = sw.driverAlarm ? MoveState::Faulted : moving ? MoveState::Moving : MoveState::Idle;
    status.sampledAtNs = steadyNowNs();
    return StageError::None;
}

StageError Zc300Stage::readConfigLocked(ControllerConfig& config)
{
    Frame unit, type, ppr, lead;
    StageError err = readLocked(reg16(kRegUnit, axis_), 1, unit);
    if (err == StageError::None) err = readLocked(reg16(kRegStageType, axis_), 1, type);
    if (err == StageError::None) err = readLocked(reg32(kRegPulsesPerRev, axis_), 2, ppr);
    if (err == StageError::None) err = readLocked(reg32(kRegLead, axis_), 2, lead);
    if (err != StageError::None) return err;
    config.unit = static_cast<Unit>(decodeU16(unit.data()));
    config.stageType = static_cast<StageType>(decodeU16(type.data()));
    config.pulsesPerRev = decodeLong(ppr.data());
    config.leadMm = decodeFloat(lead.data());
    return StageError::None;
}

StageError Zc300Stage::requireMotionLocked() const
{
    if (!connected_) return StageError::NotConnected;
    if (!configured_) return StageError::Misconfigured;
    return StageError::None;
}

StageError Zc300Stage::readStatus(StageStatus& status)
{
    Access access(*this, Access::Kind::Poll);
    if (!access.owned()) return StageError::Busy;
    if (!connected_) return StageError::NotConnected;
    return readStatusLocked(status);
}

StageError Zc300Stage::moveLocked(Opcode op, double um)
{
    if (const StageError err = requireMotionLocked(); err != StageError::None) return err;
    const auto mm = encodeMicronsAsMm(um);
    if (!mm) return StageError::OffGrid;
    const StageError err =
        writeLocked(buildWriteFloat(address_, reg32(kRegStepDistance, axis_), *mm), timing_.transactionMs);
    if (err != StageError::None) return err;
    return motionLocked(op, um < 0 ? kDirNegative : kDirPositive);
}

StageError Zc300Stage::moveAbsolute(double targetUm, std::uint64_t expectedStopGeneration)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    // Under the driver lock, right before the opcode: a Stop that was issued
    // after the caller read the generation has either run already (Stop is
    // served first) or is still queued behind us; either way this move must not
    // start motion after it.
    if (expectedStopGeneration != kAnyStopGeneration && stopGeneration_.load() != expectedStopGeneration) {
        return StageError::Stopped;
    }
    return moveLocked(Opcode::MoveAbsolute, targetUm);
}

StageError Zc300Stage::moveRelative(double deltaUm)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (const StageError err = requireMotionLocked(); err != StageError::None) return err;
    if (!encodeMicronsAsMm(deltaUm)) return StageError::OffGrid;
    if (std::round(deltaUm) == 0.0) return StageError::None;
    return moveLocked(Opcode::MoveRelative, deltaUm);
}

StageError Zc300Stage::jog(Direction direction)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (const StageError err = requireMotionLocked(); err != StageError::None) return err;
    return motionLocked(Opcode::Jog, direction == Direction::Positive ? kDirPositive : kDirNegative);
}

StageError Zc300Stage::stop()
{
    stopGeneration_.fetch_add(1); // before waiting for the driver: a queued move must see it
    Access access(*this, Access::Kind::Stop);
    if (!access.owned()) return StageError::Busy;
    if (!connected_) return StageError::NotConnected;
    // Idempotent, so it is retried after silence; allowed even when the
    // controller is misconfigured.
    return writeLocked(buildOpcode(address_, Opcode::Stop, 1, axisCode(axis_)), timing_.transactionMs);
}

StageError Zc300Stage::setPosition(double positionUm)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (const StageError err = requireMotionLocked(); err != StageError::None) return err;
    if (!encodeMicronsAsMm(positionUm)) return StageError::OffGrid;
    const float mm = static_cast<float>(std::round(positionUm) / 1000.0);
    return writeLocked(buildWriteFloat(address_, reg32(kRegPosition, axis_), mm), timing_.transactionMs);
}

StageError Zc300Stage::setSpeed(double umPerS, double umPerS2)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (const StageError err = requireMotionLocked(); err != StageError::None) return err;
    if (!(umPerS > 0.0) || !(umPerS2 > 0.0) || !std::isfinite(umPerS) || !std::isfinite(umPerS2)) {
        return StageError::InvalidArgument;
    }
    StageError err = writeLocked(
        buildWriteFloat(address_, reg32(kRegSpeed, axis_), static_cast<float>(umPerS / 1000.0)),
        timing_.transactionMs);
    if (err == StageError::None) {
        err = writeLocked(
            buildWriteFloat(address_, reg32(kRegAccel, axis_), static_cast<float>(umPerS2 / 1000.0)),
            timing_.transactionMs);
    }
    return err;
}

StageError Zc300Stage::applyProfile(const StageProfile& profile)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (!connected_) return StageError::NotConnected;
    if (profile.kind != AxisKind::Linear || profile.pulsesPerRev <= 0 || !(profile.leadMm > 0.0)) {
        return StageError::InvalidArgument;
    }
    StageStatus status;
    if (const StageError err = readStatusLocked(status); err != StageError::None) return err;
    if (status.state == MoveState::Moving) return StageError::Busy;

    const int ms = timing_.transactionMs;
    // Lead and pulses/rev first, unit last: the unit change re-scales the
    // controller's displayed distances against the new calibration.
    StageError err = writeLocked(
        buildWriteU16(address_, reg16(kRegStageType, axis_), static_cast<std::uint16_t>(StageType::Linear)), ms);
    if (err == StageError::None)
        err = writeLocked(buildWriteFloat(address_, reg32(kRegLead, axis_), static_cast<float>(profile.leadMm)), ms);
    if (err == StageError::None)
        err = writeLocked(buildWriteLong(address_, reg32(kRegPulsesPerRev, axis_), profile.pulsesPerRev), ms);
    if (err == StageError::None)
        err = writeLocked(
            buildWriteU16(address_, reg16(kRegUnit, axis_), static_cast<std::uint16_t>(Unit::Millimetres)), ms);
    if (err == StageError::None) err = writeLocked(buildOpcode(address_, Opcode::Save), timing_.saveMs);
    if (err != StageError::None) {
        SPDLOG_ERROR("Zc300Stage: applying profile '{}' failed: {}", profile.name, toString(err));
        return err;
    }

    ControllerConfig config;
    if ((err = readConfigLocked(config)) != StageError::None) return err;
    config_ = config;
    profile_ = profile;
    configured_ = configMatches(config, profile);
    SPDLOG_INFO("Zc300Stage: applied and saved profile '{}' (configured={})", profile.name,
                configured_.load());
    return configured_ ? StageError::None : StageError::Misconfigured;
}

StageError Zc300Stage::readPowerUpToken(std::uint16_t& token)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (!connected_) return StageError::NotConnected;
    Frame data;
    const StageError err = readLocked(kRegScratch, 1, data);
    if (err == StageError::None) token = decodeU16(data.data());
    return err;
}

StageError Zc300Stage::writePowerUpToken(std::uint16_t token)
{
    Access access(*this, Access::Kind::Command);
    if (!access.owned()) return StageError::Busy;
    if (!connected_) return StageError::NotConnected;
    return writeLocked(buildWriteU16(address_, kRegScratch, token), timing_.transactionMs);
}

} // namespace backend::stage::zc300
