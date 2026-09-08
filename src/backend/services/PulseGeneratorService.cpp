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

QByteArray PulseGeneratorService::buildFrequencyFrame(uint8_t addr, int channel, double hz) {
    const uint32_t value = frequencyToRegisterValue(hz);
    QByteArray regData(4, 0);
    regData[0] = static_cast<char>((value >> 24) & 0xFF); // high word, high byte
    regData[1] = static_cast<char>((value >> 16) & 0xFF); // high word, low byte
    regData[2] = static_cast<char>((value >> 8) & 0xFF);  // low word, high byte
    regData[3] = static_cast<char>(value & 0xFF);         // low word, low byte
    return modbus::buildWriteMultipleRequest(addr, regFreq(channel), regData);
}

QByteArray PulseGeneratorService::buildDutyFrame(uint8_t addr, int channel, double percent) {
    return modbus::buildWriteSingleRequest(addr, regDuty(channel), dutyToRegisterValue(percent));
}

bool PulseGeneratorService::validChannel(int channel) {
    return channel >= 0 && channel < CHANNEL_COUNT;
}

bool PulseGeneratorService::identityLooksLikeGenerator(const QByteArray& identityData) {
    if (identityData.size() != CHANNEL_COUNT * 6) {
        return false;
    }
    const auto* regs = reinterpret_cast<const uint8_t*>(identityData.constData());
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
bool PulseGeneratorService::writeFrame(const QByteArray& request) {
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
    disconnect();
}

bool PulseGeneratorService::connect(const QString& portName, int baudRate, uint8_t modbusAddress) {
    SerialSettings settings;
    settings.baudRate = baudRate;
    return connect(portName, settings, modbusAddress);
}

bool PulseGeneratorService::connect(const QString& portName, const SerialSettings& settings,
                                    uint8_t modbusAddress) {
    std::scoped_lock lock(mutex_);

    bus_.reset();
    status_ = Status{};

    config_.portName = portName;
    config_.serial = settings;
    config_.modbusAddress = modbusAddress;

    serialbus::BusError busError = serialbus::BusError::None;
    QString detail;
    bus_ = busManager_.acquire(portName, settings, &busError, &detail);
    if (!bus_) {
        status_.lastError = mapBusError(busError);
        SPDLOG_ERROR("PulseGeneratorService: cannot open {}: {} ({})",
                     portName.toStdString(), serialbus::toString(busError),
                     detail.toStdString());
        return false;
    }

    // Verify the addressed device and seed channel state from the hardware:
    // all four channels' freq/duty registers in one read. Does NOT write
    // anything, so a generator that is already pulsing keeps pulsing.
    const QByteArray request =
        modbus::buildReadRequest(modbusAddress, 0, IDENTITY_REG_COUNT);
    const auto result = bus_->transact(request, SERIAL_TIMEOUT_MS);
    if (result.error != serialbus::BusError::None) {
        status_.lastError = result.error == serialbus::BusError::ModbusException
                                ? LinkError::IncompatibleDevice
                                : mapBusError(result.error);
        SPDLOG_ERROR("PulseGeneratorService: device not verified on {} addr={}: {} "
                     "— check wiring and address", portName.toStdString(),
                     modbusAddress, serialbus::toString(result.error));
        bus_.reset();
        return false;
    }
    QByteArray data;
    if (!modbus::extractReadData(result.response, IDENTITY_REG_COUNT, data)) {
        status_.lastError = LinkError::IncompatibleDevice;
        SPDLOG_ERROR("PulseGeneratorService: unexpected identity-read shape from {} addr={}",
                     portName.toStdString(), modbusAddress);
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
                     portName.toStdString(), modbusAddress);
        bus_.reset();
        return false;
    }
    const auto* regs = reinterpret_cast<const uint8_t*>(data.constData());
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
                portName.toStdString(), modbusAddress,
                status_.channels[0].frequencyHz, status_.channels[0].dutyPercent);
    return true;
}

void PulseGeneratorService::disconnect() {
    std::scoped_lock lock(mutex_);
    if (!bus_) {
        return;
    }
    // Deliberately leaves the module's outputs untouched: disconnecting the
    // control link must not stop a pulse train mid-experiment. Releasing the
    // shared session only closes the adapter once its last client lets go.
    bus_.reset();
    status_.connected = false;
    SPDLOG_INFO("PulseGeneratorService: disconnected from {} addr={}",
                config_.portName.toStdString(), config_.modbusAddress);
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
    const QString& portName, const SerialSettings& settings, uint8_t from, uint8_t to,
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
                         portName.toStdString(), serialbus::toString(busError));
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
        const QByteArray request = modbus::buildReadRequest(
            static_cast<uint8_t>(addr), 0, IDENTITY_REG_COUNT);
        const auto result = bus->transact(request, perAddressTimeoutMs);
        switch (result.error) {
        case serialbus::BusError::None: {
            // Right shape — but only plausible register values earn the
            // "pulse generator" label (see identityLooksLikeGenerator).
            QByteArray data;
            const bool plausible =
                modbus::extractReadData(result.response, IDENTITY_REG_COUNT, data) &&
                identityLooksLikeGenerator(data);
            hits.push_back({static_cast<uint8_t>(addr),
                            plausible ? ScanHit::Kind::PulseGenerator
                                      : ScanHit::Kind::ModbusDevice});
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
    if (!status_.connected) {
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
    if (!status_.connected) {
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
    if (!status_.connected) {
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

PulseGeneratorService::Status PulseGeneratorService::getStatus() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

PulseGeneratorService::Config PulseGeneratorService::getConfig() const {
    std::scoped_lock lock(mutex_);
    return config_;
}

} // namespace backend::services
