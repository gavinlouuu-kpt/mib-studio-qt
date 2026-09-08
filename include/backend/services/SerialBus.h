#pragma once

#include "backend/services/ISerialPort.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend::services::serialbus {

/**
 * Shared RS485/Modbus RTU bus layer (Qt-free).
 *
 * RS485 is a multi-drop bus: one physical USB/RS485 adapter can carry several
 * Modbus RTU devices at different slave addresses, so the serial port must be
 * owned once per adapter and shared by every service that talks on it.
 * SerialBusManager hands out one ModbusBusSession per (system port name,
 * serial settings); the session serializes all transactions (inter-frame
 * delay + strict request/response correlation) so concurrent clients can
 * never cross-associate responses.
 */

using SerialSettings = backend::services::SerialSettings;
using PortInfo = backend::services::SerialPortInfo;
using Bytes = std::vector<uint8_t>;

// Cross-platform enumeration; see ISerialPort.h.
std::vector<PortInfo> availablePorts();

enum class BusError {
    None,
    PortUnavailable,   // port does not exist / cannot be opened
    PortBusy,          // held by another process, or by us with other settings
    NotOpen,           // session lost its port
    WriteFailed,
    Timeout,           // no (matching) response before the deadline
    CrcError,          // corrupt frame — possible duplicate-address collision
    FrameError,        // unframeable bytes — possible collision or wrong baud
    WrongAddress,      // only frames from other slave addresses arrived
    WrongFunction,     // addressed device answered a different function code
    ModbusException,   // addressed device answered with a Modbus exception
    CollisionSuspected // extra bytes after a valid frame — duplicate address?
};

const char* toString(BusError error);

struct Transaction {
    BusError error{BusError::Timeout};
    uint8_t exceptionCode{0}; // valid when error == ModbusException
    Bytes response;           // full validated frame when error is None/ModbusException
};

// Exclusive owner of one ISerialPort. Construct only through
// SerialBusManager::acquire(); hold via shared_ptr — the port closes when the
// last client releases its reference.
//
// The port lives on a dedicated I/O thread (created and used only there), so
// transact() marshals the request to that thread and blocks for the result.
// Any thread may call transact(); callers are serialized.
class ModbusBusSession {
public:
    ~ModbusBusSession();

    ModbusBusSession(const ModbusBusSession&) = delete;
    ModbusBusSession& operator=(const ModbusBusSession&) = delete;

    // One serialized Modbus RTU transaction: drains stale bytes, enforces the
    // inter-frame delay (measured from the last bus activity), writes
    // `request` (byte 0 = slave address) and reads exactly one
    // strictly-correlated response frame. Complete frames from a different
    // slave address (stale/delayed traffic) are discarded and the read
    // continues until the deadline; corrupt or unframeable bytes fail the
    // transaction rather than being reinterpreted.
    Transaction transact(const Bytes& request, int timeoutMs = 1000);

    std::string portName() const { return portName_; }
    SerialSettings settings() const { return settings_; }
    bool isOpen() const { return portOpen_.load(); }

private:
    friend class SerialBusManager;
    ModbusBusSession(std::string portName, const SerialSettings& settings, SerialPortFactory factory);

    // Spawns the I/O thread, which opens the port; blocks for the outcome.
    bool start(BusError* error, std::string* errorDetail);
    void ioLoop();
    bool openPortOnIoThread(); // io thread only
    Transaction runTransaction(const Bytes& request, int timeoutMs); // io thread only

    const std::string portName_;
    const SerialSettings settings_;
    SerialPortFactory factory_;

    // Touched only by the I/O thread.
    std::unique_ptr<ISerialPort> serial_;
    std::chrono::steady_clock::time_point lastBusActivity_{};

    std::thread ioThread_;
    std::mutex callMutex_; // serializes transact() callers

    // Handshake between callers and the I/O thread.
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    const Bytes* jobRequest_{nullptr};
    int jobTimeoutMs_{0};
    Transaction jobResult_;
    bool jobPending_{false};
    bool jobDone_{false};
    bool stopRequested_{false};
    bool openDone_{false};
    BusError openErrorCode_{BusError::PortUnavailable};
    std::string openErrorDetail_;
    std::atomic<bool> portOpen_{false};
};

// Process-wide registry: one live session per system port. acquire() returns
// the existing session when the settings match, refuses with PortBusy when the
// port is already held with different settings, and opens the port otherwise.
// Session teardown (port close + registry erase) happens atomically under the
// registry lock, so a dying session and a fresh acquire cannot interleave.
class SerialBusManager {
public:
    SerialBusManager();
    ~SerialBusManager() = default;

    SerialBusManager(const SerialBusManager&) = delete;
    SerialBusManager& operator=(const SerialBusManager&) = delete;

    // Inject the serial-port factory (defaults to the platform port). Tests
    // supply a fake to drive the Modbus protocol without hardware. Applies to
    // sessions opened afterwards.
    void setSerialPortFactory(SerialPortFactory factory);

    std::shared_ptr<ModbusBusSession> acquire(const std::string& portName,
                                              const SerialSettings& settings,
                                              BusError* error = nullptr,
                                              std::string* errorDetail = nullptr);

private:
    static std::string normalizeKey(const std::string& portName);

    std::mutex mutex_;
    SerialPortFactory factory_;
    std::map<std::string, std::weak_ptr<ModbusBusSession>> sessions_;
};

} // namespace backend::services::serialbus
