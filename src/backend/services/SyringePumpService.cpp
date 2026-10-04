#include "backend/services/SyringePumpService.h"

#include <string>
#include "backend/services/ModbusRtu.h"
#include "backend/services/SerialBus.h"
#include "backend/services/TushuiPumpProtocol.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace backend::services {

namespace {
    // Modbus RTU register addresses (from dLSP 501X manual Appendix B)
    constexpr uint16_t REG_CHANNEL_ENABLE    = 0x0000;  // 0=disable, 1=enable (must be 1 to allow start)
    constexpr uint16_t REG_RUN_COMMAND       = 0x0001;  // 0=stop, 1=start
    constexpr uint16_t REG_FULL_SPEED_RUN    = 0x0008;  // 0=stop, 1=full speed infuse, 2=full speed withdraw
    constexpr uint16_t REG_ERROR_STATUS      = 0x0100;  // bit3=stall/blockage
    constexpr uint16_t REG_DIRECTION_STATUS  = 0x010A;  // R: 0=none, 1=infuse, 2=withdraw
    constexpr uint16_t REG_MIN_FLOW_RATE     = 0x004A;  // float32, 2 regs
    constexpr uint16_t REG_MAX_FLOW_RATE     = 0x004C;  // float32, 2 regs
    constexpr uint16_t REG_MODE              = 0x0060;  // 0=infuse, 1=withdraw, 2=infuse+withdraw, 3=withdraw+infuse
    constexpr uint16_t REG_SYRINGE_VOLUME    = 0x0061;  // uint16, 1~9999
    constexpr uint16_t REG_SYRINGE_VOL_UNIT  = 0x0062;  // uint16, volume unit code
    constexpr uint16_t REG_INFUSE_FLOW_RATE  = 0x006A;  // uint16, 1~9999
    constexpr uint16_t REG_INFUSE_FLOW_UNIT  = 0x006B;  // uint16, unit code
    constexpr uint16_t REG_WITHDRAW_FLOW_RATE = 0x006C; // uint16, 1~9999
    constexpr uint16_t REG_WITHDRAW_FLOW_UNIT = 0x006D; // uint16, unit code
    constexpr uint16_t REG_REALTIME_INFUSE_FLOW = 0x0102; // uint16, read-only
    constexpr uint16_t REG_ACCUM_VOLUME      = 0x00C7;  // float32, 2 regs

    // Serial timeout in milliseconds
    constexpr int SERIAL_TIMEOUT_MS = 1000;

    // Pump GUIs still address adapters by Windows COM number; the shared bus
    // layer takes system port names, so synthesize the name here.
    std::string comPortName(int comPort) { return "COM" + std::to_string(comPort); }

    const char* pumpName(SyringePumpService::PumpId id) {
        return id == SyringePumpService::PumpId::Sample ? "Sample" : "Sheath";
    }

    const char* pumpName(int idx) {
        return pumpName(static_cast<SyringePumpService::PumpId>(idx));
    }

    // Flow-rate unit codes shared with the dLSP protocol and the UI.
    constexpr uint16_t UNIT_UL_PER_MIN = 100;
    constexpr uint16_t UNIT_ML_PER_MIN = 103;
} // namespace

// ---------------------------------------------------------------------------
// CRC16 — standard Modbus polynomial (0xA001 reflected)
// ---------------------------------------------------------------------------
uint16_t SyringePumpService::crc16(const uint8_t* data, size_t len) {
    return modbus::crc16(data, len);
}

// ---------------------------------------------------------------------------
// Float32 <-> Modbus register conversion (big-endian ABCD word order)
// ---------------------------------------------------------------------------
std::vector<uint8_t> SyringePumpService::floatToRegisters(float value) {
    return modbus::floatToRegisters(value);
}

float SyringePumpService::registersToFloat(const uint8_t* data) {
    return modbus::registersToFloat(data);
}

// ---------------------------------------------------------------------------
// Modbus RTU frame builders
// ---------------------------------------------------------------------------
std::vector<uint8_t> SyringePumpService::buildReadRequest(uint8_t addr, uint16_t startReg, uint16_t count) {
    return modbus::buildReadRequest(addr, startReg, count);
}

std::vector<uint8_t> SyringePumpService::buildWriteSingleRequest(uint8_t addr, uint16_t reg, uint16_t value) {
    return modbus::buildWriteSingleRequest(addr, reg, value);
}

std::vector<uint8_t> SyringePumpService::buildWriteMultipleRequest(uint8_t addr, uint16_t startReg, const std::vector<uint8_t>& regData) {
    return modbus::buildWriteMultipleRequest(addr, startReg, regData);
}

// ---------------------------------------------------------------------------
// Serial send/receive
// ---------------------------------------------------------------------------
bool SyringePumpService::sendRequest(int pumpIdx, const std::vector<uint8_t>& request, std::vector<uint8_t>& response, int expectedBytes) {
    (void)expectedBytes; // the bus layer frames responses from their own headers
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    if (!pump.bus) {
        return false;
    }
    const auto result = pump.bus->transact(request, SERIAL_TIMEOUT_MS);
    if (result.error != serialbus::BusError::None) {
        SPDLOG_ERROR("SyringePumpService: pump {} transaction failed: {}",
                     pumpIdx, serialbus::toString(result.error));
        return false;
    }
    response = result.response;
    return true;
}

// ---------------------------------------------------------------------------
// High-level Modbus read/write
// ---------------------------------------------------------------------------
bool SyringePumpService::readHoldingRegisters(int pumpIdx, uint16_t startReg, uint16_t count, std::vector<uint8_t>& data) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    std::vector<uint8_t> request = buildReadRequest(pump.config.modbusAddress, startReg, count);
    // Expected response: addr(1) + func(1) + byteCount(1) + data(count*2) + crc(2)
    int expectedBytes = 3 + count * 2 + 2;
    std::vector<uint8_t> response;
    if (!sendRequest(pumpIdx, request, response, expectedBytes)) {
        return false;
    }
    // Extract data with bounds + byteCount validation so a short/garbled frame
    // can't yield fewer bytes than the caller indexes.
    if (!modbus::extractReadData(response, count, data)) {
        SPDLOG_ERROR("SyringePumpService: malformed read response from pump {} "
                     "(expected {} registers, got {} bytes)",
                     pumpIdx, count, response.size());
        return false;
    }
    return true;
}

bool SyringePumpService::writeSingleRegister(int pumpIdx, uint16_t reg, uint16_t value) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    std::vector<uint8_t> request = buildWriteSingleRequest(pump.config.modbusAddress, reg, value);
    // Expected response: echo of request (8 bytes)
    std::vector<uint8_t> response;
    return sendRequest(pumpIdx, request, response, 8);
}

bool SyringePumpService::writeMultipleRegisters(int pumpIdx, uint16_t startReg, const std::vector<uint8_t>& regData) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    std::vector<uint8_t> request = buildWriteMultipleRequest(pump.config.modbusAddress, startReg, regData);
    // Expected response: addr(1) + func(1) + startReg(2) + count(2) + crc(2) = 8
    std::vector<uint8_t> response;
    return sendRequest(pumpIdx, request, response, 8);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
SyringePumpService::SyringePumpService(serialbus::SerialBusManager& busManager)
    : busManager_(busManager) {}

SyringePumpService::~SyringePumpService() {
    disconnect(PumpId::Sample);
    disconnect(PumpId::Sheath);
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------
bool SyringePumpService::connect(PumpId id, int comPort, int baudRate, uint8_t modbusAddress) {
    if (!connect(id, comPortName(comPort), baudRate, modbusAddress)) {
        return false;
    }
    auto& pump = pumps_[static_cast<size_t>(static_cast<int>(id))];
    std::scoped_lock lock(pump.mutex);
    pump.config.comPort = comPort;
    return true;
}

bool SyringePumpService::connect(PumpId id, const std::string& portName, int baudRate,
                                 uint8_t modbusAddress) {
    return connect(id, portName, baudRate, modbusAddress, PumpModel::DlspSyringe,
                   tushui::kDefaultMicrolitersPerRev);
}

bool SyringePumpService::connect(PumpId id, const std::string& portName, int baudRate,
                                 uint8_t modbusAddress, PumpModel model,
                                 double microlitersPerRev) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];

    // disconnect() takes pump.mutex itself, so a reconnect must release the
    // old session before this function takes the lock (non-recursive mutex).
    if (isConnected(id)) {
        disconnect(id);
    }

    std::scoped_lock lock(pump.mutex);

    pump.config.portName = portName;
    pump.config.comPort = -1; // unknown unless the int overload fills it in
    pump.config.baudRate = baudRate;
    pump.config.modbusAddress = modbusAddress;
    pump.config.model = model;
    pump.config.microlitersPerRev = microlitersPerRev;
    pump.purging = false;
    pump.status = PumpStatus{};

    // Acquire the shared bus session for this adapter (8N1, pump default).
    // If another service already holds the adapter with the same settings the
    // session is shared instead of a failing second open.
    serialbus::SerialSettings settings;
    settings.baudRate = baudRate;
    serialbus::BusError busError = serialbus::BusError::None;
    std::string errorDetail;
    pump.bus = busManager_.acquire(portName, settings, &busError, &errorDetail);
    if (!pump.bus) {
        SPDLOG_ERROR("SyringePumpService: Failed to open {} for {} pump: {} ({})",
                    portName, pumpName(id), serialbus::toString(busError),
                    errorDetail);
        return false;
    }
    SPDLOG_INFO("SyringePumpService: {} opened for {} pump (baud={}, addr={}, model={})",
                portName, pumpName(id), baudRate, modbusAddress,
                model == PumpModel::TushuiPeristaltic ? "Tushui peristaltic" : "dLSP syringe");

    if (model == PumpModel::TushuiPeristaltic) {
        if (!peristalticConnect(idx)) {
            SPDLOG_ERROR("SyringePumpService: {} pump not responding on {} addr={} — check wiring and address",
                         pumpName(id), portName, modbusAddress);
            pump.bus.reset();
            return false;
        }
        pump.status.connected = true;
        SPDLOG_INFO("SyringePumpService: {} pump connected on {}", pumpName(id), portName);
        return true;
    }

    // Verify communication by enabling the channel (required for start/stop commands)
    if (!writeSingleRegister(idx, REG_CHANNEL_ENABLE, 1)) {
        SPDLOG_ERROR("SyringePumpService: {} pump not responding on {} addr={} — check wiring and address",
                     pumpName(id), portName, modbusAddress);
        pump.bus.reset();
        return false;
    }

    // Read min/max flow rates
    std::vector<uint8_t> minData, maxData;
    if (readHoldingRegisters(idx, REG_MIN_FLOW_RATE, 2, minData)) {
        pump.status.minFlowRate = registersToFloat(reinterpret_cast<const uint8_t*>(minData.data()));
        SPDLOG_INFO("SyringePumpService: {} pump min flow rate: {}", pumpName(id), pump.status.minFlowRate);
    }
    if (readHoldingRegisters(idx, REG_MAX_FLOW_RATE, 2, maxData)) {
        pump.status.maxFlowRate = registersToFloat(reinterpret_cast<const uint8_t*>(maxData.data()));
        SPDLOG_INFO("SyringePumpService: {} pump max flow rate: {}", pumpName(id), pump.status.maxFlowRate);
    }

    pump.status.connected = true;
    SPDLOG_INFO("SyringePumpService: {} pump connected on {}", pumpName(id),
                portName);
    return true;
}

void SyringePumpService::disconnect(PumpId id) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];

    std::scoped_lock lock(pump.mutex);
    if (!pump.bus) {
        return;
    }

    // Try to stop the pump before disconnecting
    if (pump.status.connected && pump.config.model == PumpModel::TushuiPeristaltic) {
        // The peristaltic pump reports no run state until polled, so stop it
        // unconditionally; this also restores the flow speed after a purge.
        peristalticStop(idx);
    } else if (pump.status.connected && pump.status.runStatus != RunStatus::Stop) {
        writeSingleRegister(idx, REG_RUN_COMMAND, 0);
    }

    // The adapter only closes once its last client (pump or otherwise) lets go.
    pump.bus.reset();
    pump.status.connected = false;
    pump.status.runStatus = RunStatus::Stop;
    pump.status.currentFlowRate = 0.0;
    pump.status.accumulatedVolume = 0.0;

    SPDLOG_INFO("SyringePumpService: {} pump disconnected", pumpName(id));
}

bool SyringePumpService::isConnected(PumpId id) const {
    int idx = static_cast<int>(id);
    const auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);
    return pump.status.connected;
}

// ---------------------------------------------------------------------------
// Control methods
// ---------------------------------------------------------------------------
bool SyringePumpService::setFlowRate(PumpId id, double rate, uint16_t unit) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;
    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        return peristalticSetFlowRate(idx, rate, unit);
    }

    uint16_t rateValue = static_cast<uint16_t>(std::clamp(rate, 1.0, 9999.0));

    // Set both infuse and withdraw flow rates
    if (!writeSingleRegister(idx, REG_INFUSE_FLOW_RATE, rateValue)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set infuse flow rate for {} pump", pumpName(id));
        return false;
    }
    if (!writeSingleRegister(idx, REG_INFUSE_FLOW_UNIT, unit)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set infuse flow rate unit for {} pump", pumpName(id));
        return false;
    }
    if (!writeSingleRegister(idx, REG_WITHDRAW_FLOW_RATE, rateValue)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set withdraw flow rate for {} pump", pumpName(id));
        return false;
    }
    if (!writeSingleRegister(idx, REG_WITHDRAW_FLOW_UNIT, unit)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set withdraw flow rate unit for {} pump", pumpName(id));
        return false;
    }

    pump.config.flowRate = rate;
    pump.config.flowRateUnit = unit;
    SPDLOG_INFO("SyringePumpService: {} pump flow rate set to {} (unit={})", pumpName(id), rateValue, unit);
    return true;
}

bool SyringePumpService::setDirection(PumpId id, Direction dir) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;

    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        if (!writeSingleRegister(idx, tushui::kRegDirection, peristalticRotation(idx, dir))) {
            SPDLOG_ERROR("SyringePumpService: Failed to set direction for {} pump", pumpName(id));
            return false;
        }
    } else if (!writeSingleRegister(idx, REG_MODE, static_cast<uint16_t>(dir))) {
        SPDLOG_ERROR("SyringePumpService: Failed to set direction for {} pump", pumpName(id));
        return false;
    }

    pump.config.direction = dir;
    SPDLOG_INFO("SyringePumpService: {} pump direction set to {}",
                pumpName(id), dir == Direction::Infuse ? "Infuse" : "Withdraw");
    return true;
}

bool SyringePumpService::start(PumpId id) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;

    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        if (!peristalticRun(idx, pump.status.speedRpm, pump.config.direction)) {
            SPDLOG_ERROR("SyringePumpService: Failed to start {} pump", pumpName(id));
            return false;
        }
        SPDLOG_INFO("SyringePumpService: {} pump started ({:.2f} rpm)", pumpName(id),
                    pump.status.speedRpm);
        return true;
    }

    // Ensure channel is enabled before starting
    if (!writeSingleRegister(idx, REG_CHANNEL_ENABLE, 1)) {
        SPDLOG_WARN("SyringePumpService: Could not enable channel for {} pump", pumpName(id));
    }

    if (!writeSingleRegister(idx, REG_RUN_COMMAND, 1)) {
        SPDLOG_ERROR("SyringePumpService: Failed to start {} pump", pumpName(id));
        return false;
    }

    SPDLOG_INFO("SyringePumpService: {} pump started", pumpName(id));
    return true;
}

bool SyringePumpService::stop(PumpId id) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;

    if (pump.config.model == PumpModel::TushuiPeristaltic ? !peristalticStop(idx)
                                                          : !writeSingleRegister(idx, REG_RUN_COMMAND, 0)) {
        SPDLOG_ERROR("SyringePumpService: Failed to stop {} pump", pumpName(id));
        return false;
    }

    SPDLOG_INFO("SyringePumpService: {} pump stopped", pumpName(id));
    return true;
}

bool SyringePumpService::purge(PumpId id, Direction dir) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;

    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        // The head runs at the fixed purge speed; stop/stopPurge restore the
        // configured flow speed and direction.
        pump.purging = true;
        if (!peristalticRun(idx, tushui::kPurgeRpm, dir)) {
            SPDLOG_ERROR("SyringePumpService: Failed to purge {} pump", pumpName(id));
            peristalticStop(idx);
            return false;
        }
        SPDLOG_INFO("SyringePumpService: {} pump purge started ({}, {:.0f} rpm)", pumpName(id),
                    dir == Direction::Infuse ? "infuse" : "withdraw", tushui::kPurgeRpm);
        return true;
    }

    if (!writeSingleRegister(idx, REG_CHANNEL_ENABLE, 1)) {
        SPDLOG_WARN("SyringePumpService: Could not enable channel for {} pump", pumpName(id));
    }

    // Full speed: 1=infuse, 2=withdraw
    uint16_t value = (dir == Direction::Infuse) ? 1 : 2;
    if (!writeSingleRegister(idx, REG_FULL_SPEED_RUN, value)) {
        SPDLOG_ERROR("SyringePumpService: Failed to purge {} pump", pumpName(id));
        return false;
    }

    SPDLOG_INFO("SyringePumpService: {} pump purge started ({})",
                pumpName(id), dir == Direction::Infuse ? "infuse" : "withdraw");
    return true;
}

bool SyringePumpService::stopPurge(PumpId id) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;

    if (pump.config.model == PumpModel::TushuiPeristaltic ? !peristalticStop(idx)
                                                          : !writeSingleRegister(idx, REG_FULL_SPEED_RUN, 0)) {
        SPDLOG_ERROR("SyringePumpService: Failed to stop purge for {} pump", pumpName(id));
        return false;
    }

    SPDLOG_INFO("SyringePumpService: {} pump purge stopped", pumpName(id));
    return true;
}

bool SyringePumpService::setSyringeVolume(PumpId id, uint16_t volume, uint16_t unit) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected) return false;
    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        SPDLOG_WARN("SyringePumpService: {} pump is peristaltic; syringe volume does not apply",
                    pumpName(id));
        return false;
    }

    uint16_t clampedVol = static_cast<uint16_t>(std::clamp(static_cast<int>(volume), 1, 9999));
    if (!writeSingleRegister(idx, REG_SYRINGE_VOLUME, clampedVol)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set syringe volume for {} pump", pumpName(id));
        return false;
    }
    if (!writeSingleRegister(idx, REG_SYRINGE_VOL_UNIT, unit)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set syringe volume unit for {} pump", pumpName(id));
        return false;
    }

    SPDLOG_INFO("SyringePumpService: {} pump syringe volume set to {} (unit={})", pumpName(id), clampedVol, unit);
    return true;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
// REMOVED: setModbusAddress, setModbusAddressOneShot, scanModbusAddress
// (address is volatile on dLSP 501X; using separate COM ports instead)

SyringePumpService::PumpStatus SyringePumpService::getStatus(PumpId id) const {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    return pump.status;
}

SyringePumpService::PumpConfig SyringePumpService::getConfig(PumpId id) const {
    int idx = static_cast<int>(id);
    const auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);
    return pump.config;
}

void SyringePumpService::setConfig(PumpId id, const PumpConfig& config) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);
    pump.config = config;
    if (pump.bus) pump.config.portName = pump.bus->portName();
}

int SyringePumpService::getComPort(PumpId id) const {
    int idx = static_cast<int>(id);
    const auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);
    return pump.config.comPort;
}

std::vector<uint8_t> SyringePumpService::scanModbusAddresses(
    int comPort,
    int baudRate,
    uint8_t startAddress,
    uint8_t endAddress,
    int timeoutMs) {
    std::vector<uint8_t> addresses;
    if (comPort <= 0 || baudRate <= 0) {
        return addresses;
    }
    if (startAddress == 0 || endAddress == 0 || startAddress > endAddress) {
        return addresses;
    }

    serialbus::SerialSettings settings;
    settings.baudRate = baudRate;
    serialbus::BusError busError = serialbus::BusError::None;
    auto bus = busManager_.acquire(comPortName(comPort), settings, &busError, nullptr);
    if (!bus) {
        SPDLOG_WARN("SyringePumpService: scan failed to open COM{}: {}",
                    comPort, serialbus::toString(busError));
        return addresses;
    }

    // Read-only FC03 probe; the bus layer does CRC/address/shape correlation.
    for (uint16_t addr = startAddress; addr <= endAddress; ++addr) {
        const std::vector<uint8_t> request =
            modbus::buildReadRequest(static_cast<uint8_t>(addr), REG_RUN_COMMAND, 1);
        const auto result = bus->transact(request, timeoutMs);
        if (result.error == serialbus::BusError::None) {
            addresses.push_back(static_cast<uint8_t>(addr));
        }
    }
    return addresses;
}

void SyringePumpService::pollStatus(PumpId id) {
    int idx = static_cast<int>(id);
    auto& pump = pumps_[static_cast<size_t>(idx)];
    std::scoped_lock lock(pump.mutex);

    if (!pump.status.connected || !pump.bus) {
        return;
    }
    if (pump.config.model == PumpModel::TushuiPeristaltic) {
        peristalticPoll(idx);
        return;
    }

    // Read run state (0=stopped, 1=running)
    std::vector<uint8_t> runData;
    if (readHoldingRegisters(idx, REG_RUN_COMMAND, 1, runData)) {
        uint16_t running = static_cast<uint16_t>(
            (static_cast<uint8_t>(runData[0]) << 8) | static_cast<uint8_t>(runData[1]));
        if (running == 0) {
            pump.status.runStatus = RunStatus::Stop;
        } else {
            // Read direction to distinguish forward/backward
            std::vector<uint8_t> dirData;
            if (readHoldingRegisters(idx, REG_DIRECTION_STATUS, 1, dirData)) {
                uint16_t dir = static_cast<uint16_t>(
                    (static_cast<uint8_t>(dirData[0]) << 8) | static_cast<uint8_t>(dirData[1]));
                pump.status.runStatus = (dir == 2) ? RunStatus::Backward : RunStatus::Forward;
            } else {
                pump.status.runStatus = RunStatus::Forward;
            }
        }
    }

    // Read error status (bit3 = stall/blockage)
    std::vector<uint8_t> errData;
    if (readHoldingRegisters(idx, REG_ERROR_STATUS, 1, errData)) {
        uint16_t err = static_cast<uint16_t>(
            (static_cast<uint8_t>(errData[0]) << 8) | static_cast<uint8_t>(errData[1]));
        pump.status.stalled = (err & 0x0008) != 0;
    }

    // Read current flow rate (uint16 from realtime register)
    std::vector<uint8_t> flowData;
    if (readHoldingRegisters(idx, REG_REALTIME_INFUSE_FLOW, 1, flowData)) {
        pump.status.currentFlowRate = static_cast<double>(
            (static_cast<uint8_t>(flowData[0]) << 8) | static_cast<uint8_t>(flowData[1]));
    }

    // Read accumulated volume
    std::vector<uint8_t> volData;
    if (readHoldingRegisters(idx, REG_ACCUM_VOLUME, 2, volData)) {
        pump.status.accumulatedVolume = registersToFloat(
            reinterpret_cast<const uint8_t*>(volData.data()));
    }
}

// ---------------------------------------------------------------------------
// Peristaltic (Tushui) pump — TushuiPumpProtocol.h. Callers hold pump.mutex.
// ---------------------------------------------------------------------------
uint16_t SyringePumpService::peristalticRotation(int pumpIdx, Direction dir) const {
    (void)pumpIdx;
    // Infuse turns the head clockwise; which way that pushes liquid depends on
    // how the tubing is loaded, so swap the tubing ends if Infuse withdraws.
    return dir == Direction::Infuse ? tushui::kClockwise : tushui::kCounterClockwise;
}

bool SyringePumpService::peristalticConnect(int pumpIdx) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    // Read-only: registers 100-107 prove the device answers and give its
    // current setpoints. Nothing is written until the operator asks.
    std::vector<uint8_t> data;
    tushui::Status st;
    if (!readHoldingRegisters(pumpIdx, tushui::kStatusFirst, tushui::kStatusCount, data) ||
        !tushui::decodeStatus(data, st)) {
        return false;
    }
    if (st.slaveId != pump.config.modbusAddress) {
        SPDLOG_WARN("SyringePumpService: {} pump reports slave id {} but answered at {}",
                    pumpName(pumpIdx), st.slaveId, pump.config.modbusAddress);
    }
    const double ulPerRev = pump.config.microlitersPerRev;
    pump.status.minFlowRate = tushui::rpmToFlow(tushui::kMinRpm, ulPerRev);
    pump.status.maxFlowRate = tushui::rpmToFlow(tushui::kMaxRpm, ulPerRev);
    pump.status.speedRpm = st.speedRpm;
    pump.config.flowRate = tushui::rpmToFlow(st.speedRpm, ulPerRev);
    pump.config.flowRateUnit = UNIT_UL_PER_MIN;
    pump.config.direction = st.direction == peristalticRotation(pumpIdx, Direction::Withdraw)
                                ? Direction::Withdraw
                                : Direction::Infuse;
    pump.lastPoll = {};
    SPDLOG_INFO("SyringePumpService: {} peristaltic pump: {:.2f} rpm ({:.2f} µL/min at {} µL/rev), "
                "direction {}, run state {}",
                pumpName(pumpIdx), st.speedRpm, pump.config.flowRate, ulPerRev,
                st.direction == tushui::kClockwise ? "CW" : "CCW", st.rawRunState);
    peristalticPoll(pumpIdx);
    return true;
}

bool SyringePumpService::peristalticSetFlowRate(int pumpIdx, double rate, uint16_t unit) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    if (unit != UNIT_UL_PER_MIN && unit != UNIT_ML_PER_MIN) {
        SPDLOG_ERROR("SyringePumpService: {} pump: flow unit {} is not supported (µL/min or mL/min)",
                     pumpName(pumpIdx), unit);
        return false;
    }
    const double ulPerMin = unit == UNIT_ML_PER_MIN ? rate * 1000.0 : rate;
    const double rpm = tushui::flowToRpm(ulPerMin, pump.config.microlitersPerRev);
    // Out-of-range rates fail instead of clamping: a silently different flow
    // is worse than an error the operator sees.
    if (rpm < tushui::kMinRpm || rpm > tushui::kMaxRpm) {
        SPDLOG_ERROR("SyringePumpService: {} pump: {} µL/min needs {:.3f} rpm, outside {}-{} rpm",
                     pumpName(pumpIdx), ulPerMin, rpm, tushui::kMinRpm, tushui::kMaxRpm);
        return false;
    }
    const uint16_t value = tushui::rpmToRegister(rpm);
    if (!writeSingleRegister(pumpIdx, tushui::kRegSpeed, value)) {
        SPDLOG_ERROR("SyringePumpService: Failed to set speed for {} pump", pumpName(pumpIdx));
        return false;
    }
    pump.status.speedRpm = value / 100.0;
    pump.config.flowRate = rate;
    pump.config.flowRateUnit = unit;
    SPDLOG_INFO("SyringePumpService: {} pump flow rate {} {} -> {:.2f} rpm", pumpName(pumpIdx), rate,
                unit == UNIT_ML_PER_MIN ? "mL/min" : "µL/min", pump.status.speedRpm);
    return true;
}

bool SyringePumpService::peristalticRun(int pumpIdx, double rpm, Direction dir) {
    // Turns = 0 makes the pump run until stopped; the vendor default is a
    // fixed number of turns, which would end a run on its own.
    return writeSingleRegister(pumpIdx, tushui::kRegSpeed, tushui::rpmToRegister(rpm)) &&
           writeSingleRegister(pumpIdx, tushui::kRegDirection, peristalticRotation(pumpIdx, dir)) &&
           writeMultipleRegisters(pumpIdx, tushui::kRegTurnsHigh, tushui::turnsPayload(0.0)) &&
           writeSingleRegister(pumpIdx, tushui::kRegRunState,
                               static_cast<uint16_t>(tushui::RunState::Running));
}

bool SyringePumpService::peristalticStop(int pumpIdx) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    // Stop first; restoring the flow speed after a purge is secondary.
    const bool stopped = writeSingleRegister(pumpIdx, tushui::kRegRunState,
                                             static_cast<uint16_t>(tushui::RunState::Stopped));
    if (stopped) {
        pump.status.runStatus = RunStatus::Stop;
        pump.status.currentFlowRate = 0.0;
    }
    if (pump.purging) {
        const bool restored =
            writeSingleRegister(pumpIdx, tushui::kRegSpeed, tushui::rpmToRegister(pump.status.speedRpm)) &&
            writeSingleRegister(pumpIdx, tushui::kRegDirection,
                                peristalticRotation(pumpIdx, pump.config.direction));
        if (restored) {
            pump.purging = false;
        } else {
            SPDLOG_WARN("SyringePumpService: {} pump: could not restore flow speed after purge",
                        pumpName(pumpIdx));
        }
    }
    return stopped;
}

void SyringePumpService::peristalticPoll(int pumpIdx) {
    auto& pump = pumps_[static_cast<size_t>(pumpIdx)];
    std::vector<uint8_t> data;
    tushui::Status st;
    if (!readHoldingRegisters(pumpIdx, tushui::kStatusFirst, tushui::kStatusCount, data) ||
        !tushui::decodeStatus(data, st)) {
        return;
    }
    const bool moving = tushui::isMoving(st.runState);
    if (st.runState == tushui::RunState::Stopped) {
        pump.status.runStatus = RunStatus::Stop;
    } else if (st.runState == tushui::RunState::Paused) {
        pump.status.runStatus = RunStatus::Pause;
    } else {
        const bool infusing = st.direction == peristalticRotation(pumpIdx, Direction::Infuse) &&
                              st.runState != tushui::RunState::SuckBack;
        pump.status.runStatus = infusing ? RunStatus::Forward : RunStatus::Backward;
    }
    if (!pump.purging) {
        pump.status.speedRpm = st.speedRpm; // a purge leaves the flow setpoint untouched
    }
    pump.status.currentFlowRate =
        moving ? tushui::rpmToFlow(st.speedRpm, pump.config.microlitersPerRev) : 0.0;

    // The pump has no volume counter: integrate the delivered volume (µL)
    // between polls from the head speed.
    const auto now = std::chrono::steady_clock::now();
    if (moving && pump.lastPoll != std::chrono::steady_clock::time_point{}) {
        const double minutes = std::chrono::duration<double>(now - pump.lastPoll).count() / 60.0;
        pump.status.accumulatedVolume += pump.status.currentFlowRate * minutes;
    }
    pump.lastPoll = now;
}

} // namespace backend::services
