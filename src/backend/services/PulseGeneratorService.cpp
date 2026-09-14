#include "backend/services/PulseGeneratorService.h"
#include "backend/services/ModbusRtu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace backend::services {

namespace {
    // Per-channel holding registers (protocol addresses, channel 0-based):
    // 3N = frequency high word, 3N+1 = frequency low word, 3N+2 = duty.
    constexpr uint16_t regFreq(int channel) { return static_cast<uint16_t>(channel * 3); }
    constexpr uint16_t regDuty(int channel) { return static_cast<uint16_t>(channel * 3 + 2); }

    constexpr int SERIAL_TIMEOUT_MS = 1000;
    // The identity read: every channel's freq/duty registers in one FC03.
    constexpr uint16_t IDENTITY_REG_COUNT = PulseGeneratorService::CHANNEL_COUNT * 3;
} // namespace

// ---------------------------------------------------------------------------
// Pure encoding helpers
// ---------------------------------------------------------------------------
double PulseGeneratorService::clampFrequency(double hz) {
    return std::clamp(hz, MIN_FREQUENCY_HZ, MAX_FREQUENCY_HZ);
}

double PulseGeneratorService::clampDuty(double percent) {
    return std::clamp(percent, 0.0, 100.0);
}

uint32_t PulseGeneratorService::frequencyToRegisterValue(double hz) {
    return static_cast<uint32_t>(std::llround(clampFrequency(hz) * 100.0));
}

uint16_t PulseGeneratorService::dutyToRegisterValue(double percent) {
    return static_cast<uint16_t>(std::lround(clampDuty(percent) * 100.0));
}

std::vector<uint8_t> PulseGeneratorService::buildFrequencyFrame(uint8_t addr, int channel, double hz) {
    const uint32_t value = frequencyToRegisterValue(hz);
    std::vector<uint8_t> regData(4, 0);
    regData[0] = static_cast<char>((value >> 24) & 0xFF); // high word, high byte
    regData[1] = static_cast<char>((value >> 16) & 0xFF); // high word, low byte
    regData[2] = static_cast<char>((value >> 8) & 0xFF);  // low word, high byte
    regData[3] = static_cast<char>(value & 0xFF);         // low word, low byte
    return modbus::buildWriteMultipleRequest(addr, regFreq(channel), regData);
}

std::vector<uint8_t> PulseGeneratorService::buildDutyFrame(uint8_t addr, int channel, double percent) {
    return modbus::buildWriteSingleRequest(addr, regDuty(channel), dutyToRegisterValue(percent));
}

bool PulseGeneratorService::validChannel(int channel) {
    return channel >= 0 && channel < CHANNEL_COUNT;
}

bool PulseGeneratorService::identityLooksLikeGenerator(const std::vector<uint8_t>& identityData) {
    if (identityData.size() != CHANNEL_COUNT * 6) {
        return false;
    }
    const auto* regs = reinterpret_cast<const uint8_t*>(identityData.data());
    constexpr uint32_t minFreqRaw = static_cast<uint32_t>(MIN_FREQUENCY_HZ * 100.0);
    constexpr uint32_t maxFreqRaw = static_cast<uint32_t>(MAX_FREQUENCY_HZ * 100.0);
    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        const int off = ch * 6;
        const uint32_t freqRaw = (static_cast<uint32_t>(regs[off]) << 24) |
                                 (static_cast<uint32_t>(regs[off + 1]) << 16) |
                                 (static_cast<uint32_t>(regs[off + 2]) << 8) |
                                 static_cast<uint32_t>(regs[off + 3]);
        const uint16_t dutyRaw = static_cast<uint16_t>(
            (static_cast<uint16_t>(regs[off + 4]) << 8) | regs[off + 5]);
        if (freqRaw != 0 && (freqRaw < minFreqRaw || freqRaw > maxFreqRaw)) {
            return false;
        }
        if (dutyRaw > 10000) {
            return false;
        }
    }
    return true;
}

const char* PulseGeneratorService::toString(LinkError error) {
    switch (error) {
    case LinkError::None:               return "ok";
    case LinkError::PortUnavailable:    return "port unavailable";
    case LinkError::PortBusy:           return "port busy";
    case LinkError::Timeout:            return "bus timeout";
    case LinkError::CrcFrameError:      return "CRC/frame error";
    case LinkError::ModbusException:    return "Modbus exception";
    case LinkError::AddressCollision:   return "address collision";
    case LinkError::IncompatibleDevice: return "incompatible device";
    case LinkError::NotConnected:       return "not connected";
    case LinkError::WriteFailed:        return "write failed";
    }
    return "unknown";
}

PulseGeneratorService::LinkError PulseGeneratorService::mapBusError(serialbus::BusError error) {
    using BE = serialbus::BusError;
    switch (error) {
    case BE::None:               return LinkError::None;
    case BE::PortUnavailable:    return LinkError::PortUnavailable;
    case BE::PortBusy:           return LinkError::PortBusy;
    case BE::NotOpen:            return LinkError::NotConnected;
    case BE::WriteFailed:        return LinkError::WriteFailed;
    case BE::Timeout:            return LinkError::Timeout;
    case BE::CrcError:           return LinkError::AddressCollision;
    case BE::FrameError:         return LinkError::CrcFrameError;
    case BE::WrongAddress:       return LinkError::Timeout;
    case BE::WrongFunction:      return LinkError::IncompatibleDevice;
    case BE::ModbusException:    return LinkError::ModbusException;
    case BE::CollisionSuspected: return LinkError::AddressCollision;
    }
    return LinkError::CrcFrameError;
}

// ---------------------------------------------------------------------------
// Bus I/O
// ---------------------------------------------------------------------------
bool PulseGeneratorService::writeFrame(const std::vector<uint8_t>& request) {
    if (!bus_) {
        status_.lastError = LinkError::NotConnected;
        return false;
    }
    const auto result = bus_->transact(request, SERIAL_TIMEOUT_MS);
    status_.lastError = mapBusError(result.error);
    if (result.error != serialbus::BusError::None) {
        SPDLOG_ERROR("PulseGeneratorService: write to addr {} failed: {}",
                     config_.modbusAddress, serialbus::toString(result.error));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
PulseGeneratorService::PulseGeneratorService(serialbus::SerialBusManager& busManager)
    : busManager_(busManager) {}

PulseGeneratorService::~PulseGeneratorService() {
    if (!endLiveView(liveViewOwner_)) SPDLOG_ERROR("Pulse generator OFF unconfirmed during shutdown");
    disconnect();
}

bool PulseGeneratorService::connect(const std::string& portName, int baudRate, uint8_t modbusAddress) {
    SerialSettings settings;
    settings.baudRate = baudRate;
    return connect(portName, settings, modbusAddress);
}

bool PulseGeneratorService::connect(const std::string& portName, const SerialSettings& settings,
                                    uint8_t modbusAddress) {
    std::scoped_lock lock(mutex_);

    if (liveViewOwned_) return false;
    bus_.reset();
    status_ = Status{};

    config_.portName = portName;
    config_.serial = settings;
    config_.modbusAddress = modbusAddress;

    serialbus::BusError busError = serialbus::BusError::None;
    std::string detail;
    bus_ = busManager_.acquire(portName, settings, &busError, &detail);
    if (!bus_) {
        status_.lastError = mapBusError(busError);
        SPDLOG_ERROR("PulseGeneratorService: cannot open {}: {} ({})",
                     portName, serialbus::toString(busError),
                     detail);
        return false;
    }

    // Verify the addressed device and seed channel state from the hardware:
    // all four channels' freq/duty registers in one read. Does NOT write
    // anything, so a generator that is already pulsing keeps pulsing.
    const std::vector<uint8_t> request =
        modbus::buildReadRequest(modbusAddress, 0, IDENTITY_REG_COUNT);
    const auto result = bus_->transact(request, SERIAL_TIMEOUT_MS);
    if (result.error != serialbus::BusError::None) {
        status_.lastError = result.error == serialbus::BusError::ModbusException
                                ? LinkError::IncompatibleDevice
                                : mapBusError(result.error);
        SPDLOG_ERROR("PulseGeneratorService: device not verified on {} addr={}: {} "
                     "— check wiring and address", portName,
                     modbusAddress, serialbus::toString(result.error));
        bus_.reset();
        return false;
    }
    std::vector<uint8_t> data;
    if (!modbus::extractReadData(result.response, IDENTITY_REG_COUNT, data)) {
        status_.lastError = LinkError::IncompatibleDevice;
        SPDLOG_ERROR("PulseGeneratorService: unexpected identity-read shape from {} addr={}",
                     portName, modbusAddress);
        bus_.reset();
        return false;
    }
    // Refuse to adopt a device whose register values are outside the module's
    // documented ranges — writing frequency/duty into an unrelated Modbus
    // device's registers 0..11 is the failure this guards against.
    if (!identityLooksLikeGenerator(data)) {
        status_.lastError = LinkError::IncompatibleDevice;
        SPDLOG_ERROR("PulseGeneratorService: device on {} addr={} answers the identity read "
                     "but its register values are not plausible for this module — refusing",
                     portName, modbusAddress);
        bus_.reset();
        return false;
    }
    const auto* regs = reinterpret_cast<const uint8_t*>(data.data());
    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        const int off = ch * 6;
        const uint32_t freqRaw = (static_cast<uint32_t>(regs[off]) << 24) |
                                 (static_cast<uint32_t>(regs[off + 1]) << 16) |
                                 (static_cast<uint32_t>(regs[off + 2]) << 8) |
                                 static_cast<uint32_t>(regs[off + 3]);
        const uint16_t dutyRaw = static_cast<uint16_t>(
            (static_cast<uint16_t>(regs[off + 4]) << 8) | regs[off + 5]);
        auto& state = status_.channels[static_cast<size_t>(ch)];
        state.frequencyHz = freqRaw / 100.0;
        state.dutyPercent = dutyRaw / 100.0;
        state.outputEnabled = dutyRaw != 0;
    }

    status_.connected = true;
    status_.lastError = LinkError::None;
    SPDLOG_INFO("PulseGeneratorService: connected on {} addr={} (ch1: {} Hz, {} %)",
                portName, modbusAddress,
                status_.channels[0].frequencyHz, status_.channels[0].dutyPercent);
    return true;
}

void PulseGeneratorService::disconnect() {
    std::scoped_lock lock(mutex_);
    if (liveViewOwned_) return;
    if (!bus_) {
        return;
    }
    // Deliberately leaves the module's outputs untouched: disconnecting the
    // control link must not stop a pulse train mid-experiment. Releasing the
    // shared session only closes the adapter once its last client lets go.
    bus_.reset();
    status_.connected = false;
    SPDLOG_INFO("PulseGeneratorService: disconnected from {} addr={}",
                config_.portName, config_.modbusAddress);
}

bool PulseGeneratorService::isConnected() const {
    std::scoped_lock lock(mutex_);
    return status_.connected;
}

PulseGeneratorService::LinkError PulseGeneratorService::lastError() const {
    std::scoped_lock lock(mutex_);
    return status_.lastError;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
std::vector<PulseGeneratorService::ScanHit> PulseGeneratorService::scanBus(
    const std::string& portName, const SerialSettings& settings, uint8_t from, uint8_t to,
    const std::atomic<bool>& cancel, int perAddressTimeoutMs, LinkError* error) {
    std::vector<ScanHit> hits;
    if (error) {
        *error = LinkError::None;
    }
    if (from == 0 || to < from) {
        return hits;
    }

    // Reuse the connected session when the scan targets the same bus;
    // otherwise acquire one for the duration of the scan.
    std::shared_ptr<serialbus::ModbusBusSession> bus;
    {
        std::scoped_lock lock(mutex_);
        bus = bus_;
    }
    if (!bus || bus->portName() != portName || bus->settings() != settings) {
        serialbus::BusError busError = serialbus::BusError::None;
        bus = busManager_.acquire(portName, settings, &busError, nullptr);
        if (!bus) {
            SPDLOG_ERROR("PulseGeneratorService: scan cannot open {}: {}",
                         portName, serialbus::toString(busError));
            if (error) {
                *error = mapBusError(busError);
            }
            return hits;
        }
    }

    for (int addr = from; addr <= to; ++addr) {
        if (cancel.load(std::memory_order_relaxed)) {
            SPDLOG_INFO("PulseGeneratorService: scan cancelled at addr {}", addr);
            break;
        }
        // Read-only FC03 identity probe — scanning must never write frequency,
        // duty, or any other register.
        const std::vector<uint8_t> request = modbus::buildReadRequest(
            static_cast<uint8_t>(addr), 0, IDENTITY_REG_COUNT);
        const auto result = bus->transact(request, perAddressTimeoutMs);
        switch (result.error) {
        case serialbus::BusError::None: {
            // Right shape — but only plausible register values earn the
            // "pulse generator" label (see identityLooksLikeGenerator).
            std::vector<uint8_t> data;
            const bool plausible =
                modbus::extractReadData(result.response, IDENTITY_REG_COUNT, data) &&
                identityLooksLikeGenerator(data);
            ScanHit hit;
            hit.address = static_cast<uint8_t>(addr);
            hit.kind = plausible ? ScanHit::Kind::PulseGenerator : ScanHit::Kind::ModbusDevice;
            hit.allChannelsConfigured = plausible && identityLooksLikeConfiguredGenerator(data);
            hits.push_back(hit);
            break;
        }
        case serialbus::BusError::ModbusException:
        case serialbus::BusError::WrongFunction:
            // Somebody answered, but not with the pulse-generator register map.
            hits.push_back({static_cast<uint8_t>(addr), ScanHit::Kind::ModbusDevice});
            break;
        case serialbus::BusError::CrcError:
        case serialbus::BusError::FrameError:
        case serialbus::BusError::CollisionSuspected:
            hits.push_back({static_cast<uint8_t>(addr), ScanHit::Kind::Error});
            break;
        default:
            break; // silence — no device at this address
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
bool PulseGeneratorService::setFrequency(int channel, double hz) {
    if (!validChannel(channel)) {
        SPDLOG_ERROR("PulseGeneratorService: invalid channel {}", channel);
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (liveViewOwned_ || !status_.connected) {
        return false;
    }
    const double clamped = clampFrequency(hz);
    if (clamped != hz) {
        SPDLOG_WARN("PulseGeneratorService: frequency {} Hz clamped to {} Hz", hz, clamped);
    }
    if (!writeFrame(buildFrequencyFrame(config_.modbusAddress, channel, clamped))) {
        return false;
    }
    status_.channels[static_cast<size_t>(channel)].frequencyHz = clamped;
    SPDLOG_INFO("PulseGeneratorService: addr{} ch{} frequency set to {} Hz",
                config_.modbusAddress, channel + 1, clamped);
    return true;
}

bool PulseGeneratorService::setDutyCycle(int channel, double percent) {
    if (!validChannel(channel)) {
        SPDLOG_ERROR("PulseGeneratorService: invalid channel {}", channel);
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (liveViewOwned_ || !status_.connected) {
        return false;
    }
    const double clamped = clampDuty(percent);
    if (clamped != percent) {
        SPDLOG_WARN("PulseGeneratorService: duty {} % clamped to {} %", percent, clamped);
    }
    auto& state = status_.channels[static_cast<size_t>(channel)];
    // Only touch the device while the output is enabled; a disabled channel
    // keeps duty 0 on the wire and picks the new duty up on the next enable.
    if (state.outputEnabled &&
        !writeFrame(buildDutyFrame(config_.modbusAddress, channel, clamped))) {
        return false;
    }
    state.dutyPercent = clamped;
    SPDLOG_INFO("PulseGeneratorService: addr{} ch{} duty set to {} %{}",
                config_.modbusAddress, channel + 1, clamped,
                state.outputEnabled ? "" : " (deferred until enable)");
    return true;
}

bool PulseGeneratorService::setOutputEnabled(int channel, bool on) {
    if (!validChannel(channel)) {
        SPDLOG_ERROR("PulseGeneratorService: invalid channel {}", channel);
        return false;
    }
    std::scoped_lock lock(mutex_);
    if (liveViewOwned_ || !status_.connected) {
        return false;
    }
    auto& state = status_.channels[static_cast<size_t>(channel)];
    const double dutyToWrite = on ? state.dutyPercent : 0.0;
    if (!writeFrame(buildDutyFrame(config_.modbusAddress, channel, dutyToWrite))) {
        return false;
    }
    state.outputEnabled = on;
    SPDLOG_INFO("PulseGeneratorService: addr{} ch{} output {} (duty {} %)",
                config_.modbusAddress, channel + 1, on ? "enabled" : "disabled", dutyToWrite);
    return true;
}

bool PulseGeneratorService::identityLooksLikeConfiguredGenerator(
    const std::vector<uint8_t>& identityData) {
    if (!identityLooksLikeGenerator(identityData)) {
        return false;
    }
    const auto* regs = reinterpret_cast<const uint8_t*>(identityData.data());
    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        const int off = ch * 6;
        const uint32_t freqRaw = (static_cast<uint32_t>(regs[off]) << 24) |
                                 (static_cast<uint32_t>(regs[off + 1]) << 16) |
                                 (static_cast<uint32_t>(regs[off + 2]) << 8) |
                                 static_cast<uint32_t>(regs[off + 3]);
        if (freqRaw == 0) {
            return false;
        }
    }
    return true;
}

bool PulseGeneratorService::discoverLiveView(
    Config& config, const std::vector<serialbus::PortInfo>& ports, std::string* error) {
    std::vector<std::string> matches;   // strict identity passed
    std::vector<std::string> lookalikes; // answered at the address, not a generator
    std::vector<std::string> busy;       // adapter held by another program/settings
    std::atomic<bool> cancel{false};
    auto listed = [](const std::vector<std::string>& names) {
        std::string out;
        for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
        return out;
    };
    for (const auto& port : ports) {
        // Avoid opening native serial ports belonging to unrelated instruments.
        if (port.vendorId == 0 || port.productId == 0) continue;
        // The system name ("COM6", "ttyUSB0") is what Hardware Setup saves and
        // what the platform port opens; the location form is only a display hint.
        const auto name = port.systemName.empty() ? port.systemLocation : port.systemName;
        if (name.empty() || std::find(matches.begin(), matches.end(), name) != matches.end() ||
            std::find(lookalikes.begin(), lookalikes.end(), name) != lookalikes.end() ||
            std::find(busy.begin(), busy.end(), name) != busy.end())
            continue;
        LinkError linkError = LinkError::None;
        const auto hits = scanBus(name, config.serial, config.modbusAddress,
                                  config.modbusAddress, cancel, 250, &linkError);
        if (linkError == LinkError::PortBusy) {
            busy.push_back(name);
            continue;
        }
        for (const auto& hit : hits) {
            if (hit.kind == ScanHit::Kind::PulseGenerator && hit.allChannelsConfigured) {
                matches.push_back(name);
            } else {
                // Lenient generator shape, foreign Modbus device or garbled reply:
                // something answered here, and it must not be written to.
                lookalikes.push_back(name);
                SPDLOG_WARN("PulseGeneratorService: {} answered addr {} but is not a configured "
                            "pulse generator (kind={}); excluded from automatic discovery",
                            name, config.modbusAddress, static_cast<int>(hit.kind));
            }
        }
    }
    if (matches.size() == 1) {
        config.portName = matches.front();
        return true;
    }
    if (!error) return false;
    if (matches.empty()) {
        std::string message = "No pulse generator found at Modbus address " +
                              std::to_string(config.modbusAddress) + ".";
        if (!busy.empty())
            message += " " + listed(busy) + (busy.size() == 1 ? " is" : " are") +
                       " in use by another program; close it or disconnect the generator there.";
        if (!lookalikes.empty())
            message += " " + listed(lookalikes) + " answered but " +
                       (lookalikes.size() == 1 ? "is" : "are") + " not a pulse generator.";
        message += " Connect the generator's USB adapter and power, then retry. Non-default "
                   "address/serial settings or an explicit port can be set in Hardware Setup.";
        *error = message;
    } else {
        *error = "Multiple pulse generators found (" + listed(matches) +
                 "). Select the intended adapter in Hardware Setup.";
    }
    return false;
}

bool PulseGeneratorService::beginLiveView(const Config& cfg, int channel, double hz, double duty,
                                          const void* owner) {
    std::scoped_lock lock(mutex_);
    if (liveViewOwned_ || !validChannel(channel) || !std::isfinite(hz) || !std::isfinite(duty) ||
        hz < MIN_FREQUENCY_HZ || hz > MAX_FREQUENCY_HZ || duty <= 0 || duty >= 100 ||
        cfg.portName.empty() || cfg.modbusAddress < 1 || cfg.modbusAddress > 247)
        return false;
    if (!connect(cfg.portName, cfg.serial, cfg.modbusAddress)) return false;
    // Take ownership even on partial failure: the caller must run endLiveView.
    liveViewChannel_ = channel;
    liveViewOwner_ = owner;
    const bool ok = setOutputEnabled(channel, false) && setFrequency(channel, hz) &&
                    setDutyCycle(channel, duty) && verifyLiveView(0);
    liveViewOwned_ = true;
    return ok;
}

bool PulseGeneratorService::enableLiveView(const void* owner) {
    std::scoped_lock lock(mutex_);
    if (!liveViewOwned_ || owner != liveViewOwner_ || !status_.connected) return false;
    auto& state = status_.channels[static_cast<size_t>(liveViewChannel_)];
    if (!writeFrame(buildDutyFrame(config_.modbusAddress, liveViewChannel_, state.dutyPercent)))
        return false;
    state.outputEnabled = true;
    return verifyLiveView(state.dutyPercent);
}

bool PulseGeneratorService::endLiveView(const void* owner) {
    std::scoped_lock lock(mutex_);
    if (!liveViewOwned_ || owner != liveViewOwner_) return true;
    const bool ok = status_.connected &&
                    writeFrame(buildDutyFrame(config_.modbusAddress, liveViewChannel_, 0)) &&
                    verifyLiveView(0);
    if (ok) status_.channels[static_cast<size_t>(liveViewChannel_)].outputEnabled = false;
    // Release manual control so an operator can reconnect/retry Stop if the
    // link failed. Never change the cached output state to OFF on failure.
    liveViewOwned_ = false;
    return ok;
}

bool PulseGeneratorService::verifyLiveView(double duty) {
    if (!bus_) return false;
    const auto response =
        bus_->transact(modbus::buildReadRequest(config_.modbusAddress, liveViewChannel_ * 3, 3),
                       SERIAL_TIMEOUT_MS);
    std::vector<uint8_t> data;
    if (response.error != serialbus::BusError::None ||
        !modbus::extractReadData(response.response, 3, data))
        return false;
    const uint32_t frequency =
        (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
    const uint16_t actualDuty = (uint16_t(data[4]) << 8) | data[5];
    return frequency == frequencyToRegisterValue(
                            status_.channels[static_cast<size_t>(liveViewChannel_)].frequencyHz) &&
           actualDuty == dutyToRegisterValue(duty);
}

bool PulseGeneratorService::liveViewOwned() const {
    std::scoped_lock lock(mutex_);
    return liveViewOwned_;
}

PulseGeneratorService::Status PulseGeneratorService::getStatus() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

PulseGeneratorService::Config PulseGeneratorService::getConfig() const {
    std::scoped_lock lock(mutex_);
    return config_;
}

} // namespace backend::services
