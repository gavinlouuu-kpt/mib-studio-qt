#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QString>

class QByteArray;

namespace backend::services::serialbus {
class ModbusBusSession;
class SerialBusManager;
} // namespace backend::services::serialbus

namespace backend::services {

class SyringePumpService {
public:
    enum class PumpId : int { Sample = 0, Sheath = 1 };
    static constexpr int PUMP_COUNT = 2;

    enum class RunStatus : uint16_t {
        Stop     = 0,
        Forward  = 1,
        Backward = 2,
        Pause    = 3
    };

    enum class Direction : uint16_t {
        Infuse   = 0,
        Withdraw = 1
    };

    struct PumpConfig {
        int comPort{-1};
        int baudRate{115200};
        uint8_t modbusAddress{1};
        double flowRate{0.0};
        uint16_t flowRateUnit{100};      // 100 = µL/min
        Direction direction{Direction::Infuse};
    };

    struct PumpStatus {
        bool connected{false};
        RunStatus runStatus{RunStatus::Stop};
        double currentFlowRate{0.0};
        double accumulatedVolume{0.0};
        double minFlowRate{0.0};
        double maxFlowRate{0.0};
        bool stalled{false};
    };

    // Pump serial I/O goes through the shared RS485 bus layer so a pump and
    // other Modbus devices (e.g. the pulse generator) can share one adapter.
    explicit SyringePumpService(serialbus::SerialBusManager& busManager);
    ~SyringePumpService();

    // Connection management. The int overload keeps the historical Windows
    // COM-number API; the QString overload takes a system port name
    // ("ttyUSB0", "COM3") so a pump can share a Linux RS485 adapter with
    // other Modbus services. Reconnecting while connected is safe (the old
    // session is released first).
    bool connect(PumpId id, int comPort, int baudRate, uint8_t modbusAddress);
    bool connect(PumpId id, const QString& portName, int baudRate, uint8_t modbusAddress);
    void disconnect(PumpId id);
    bool isConnected(PumpId id) const;

    // Control
    bool setFlowRate(PumpId id, double rate, uint16_t unit);
    bool setDirection(PumpId id, Direction dir);
    bool start(PumpId id);
    bool stop(PumpId id);
    bool purge(PumpId id, Direction dir);
    bool stopPurge(PumpId id);
    bool setSyringeVolume(PumpId id, uint16_t volume, uint16_t unit);

    // Status (thread-safe reads)
    PumpStatus getStatus(PumpId id) const;
    PumpConfig getConfig(PumpId id) const;
    void setConfig(PumpId id, const PumpConfig& config);

    // Poll current status from pump hardware (call from UI timer)
    void pollStatus(PumpId id);

    // Get COM port in use by a specific pump (for port collision avoidance)
    int getComPort(PumpId id) const;

    // Probe a COM port and return responsive Modbus addresses for dLSP pumps.
    std::vector<uint8_t> scanModbusAddresses(
        int comPort,
        int baudRate,
        uint8_t startAddress = 1,
        uint8_t endAddress = 8,
        int timeoutMs = 300);

private:
    // Modbus RTU helpers
    static uint16_t crc16(const uint8_t* data, size_t len);
    QByteArray buildReadRequest(uint8_t addr, uint16_t startReg, uint16_t count);
    QByteArray buildWriteSingleRequest(uint8_t addr, uint16_t reg, uint16_t value);
    QByteArray buildWriteMultipleRequest(uint8_t addr, uint16_t startReg, const QByteArray& regData);
    bool sendRequest(int pumpIdx, const QByteArray& request, QByteArray& response, int expectedBytes);

    // Float32 <-> register conversion (big-endian / ABCD word order)
    static QByteArray floatToRegisters(float value);
    static float registersToFloat(const uint8_t* data);

    // Read helpers
    bool readHoldingRegisters(int pumpIdx, uint16_t startReg, uint16_t count, QByteArray& data);
    bool writeSingleRegister(int pumpIdx, uint16_t reg, uint16_t value);
    bool writeMultipleRegisters(int pumpIdx, uint16_t startReg, const QByteArray& regData);

    struct PumpConnection {
        std::shared_ptr<serialbus::ModbusBusSession> bus;
        PumpConfig config;
        PumpStatus status;
        mutable std::mutex mutex;
    };

    serialbus::SerialBusManager& busManager_;
    std::array<PumpConnection, PUMP_COUNT> pumps_;
};

} // namespace backend::services
