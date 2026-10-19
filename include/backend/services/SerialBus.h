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
    std::string detail;       // why it failed, for logs and messages (e.g. which timeout); empty on success
};

// Exclusive owner of one ISerialPort. Construct only through
// SerialBusManager::acquire(); hold via shared_ptr — the port closes when the
// last client releases its reference.
//
// The port lives on a dedicated I/O thread (created, used and closed only
// there), so transact() marshals the request to that thread and blocks for
// the result. Any thread may call transact(); callers are serialized.
//
// transact() never waits without limit (#720): it gives the I/O thread
// transactBudgetMs(timeoutMs) and then returns Timeout. Every request carries
// a sequence number, so the late result of an abandoned request is dropped
// and never taken as the answer to the next one; a request the I/O thread had
// not picked up is withdrawn and never sent. While an abandoned request still
// holds the I/O thread (its port call is stuck), further calls fail fast with
// Timeout instead of queueing behind it. Once the thread is back, the next
// transaction drains any late reply bytes before it writes, as for any stale
// traffic, and the bus works again.
//
// Teardown is bounded too: the destructor asks the I/O thread to stop,
// interrupts a port call in progress (ISerialPort::interrupt) and waits at
// most kShutdownJoinMs for the thread. A thread still inside a port call then
// is detached and logged: it shares ownership of everything it touches (the
// port, the request, the handshake state, the logger), so it never uses freed
// memory or a dropped logger, and it closes the port when the call returns.
// Until it has, the port stays reserved: SerialBusManager refuses a new
// session on it.
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

    // How long transact(request, timeoutMs) waits for the I/O thread: the
    // worst case of the transaction's own bounded steps (inter-frame delay,
    // write up to kSerialWriteTimeoutMs, drain up to timeoutMs, response up
    // to timeoutMs, collision listen) plus scheduling slack. A transaction
    // that runs to its own deadlines is never cut short; only a port call
    // that ignores its bound trips it.
    static int transactBudgetMs(int timeoutMs);

    // How long the destructor waits for the I/O thread before detaching it.
    static constexpr int kShutdownJoinMs = 1000;

    std::string portName() const { return portName_; }
    SerialSettings settings() const { return settings_; }
    bool isOpen() const;

    // Registry bookkeeping: says whether the port of a session that is gone
    // is closed yet (defined in SerialBus.cpp).
    struct PortReservation;

private:
    friend class SerialBusManager;
    ModbusBusSession(std::string portName, const SerialSettings& settings, SerialPortFactory factory);

    // Spawns the I/O thread, which opens the port; blocks for the outcome.
    bool start(BusError* error, std::string* errorDetail);

    // Everything the I/O thread touches: the port, the request and result,
    // the handshake, the logger. Shared by the session and the thread
    // (SerialBus.cpp).
    struct IoState;

    // The I/O thread holds its port's reservation until it has closed the
    // port; SerialBusManager keeps a weak reference to it (SerialBus.cpp).
    std::weak_ptr<PortReservation> reservation() const;

    const std::string portName_;
    const SerialSettings settings_;
    std::shared_ptr<IoState> io_;
    std::thread ioThread_;
    std::mutex callMutex_; // serializes transact() callers
};

// Process-wide registry: one live session per system port. acquire() returns
// the existing session when the settings match, refuses with PortBusy when the
// port is already held with different settings, and opens the port otherwise.
// A port stays reserved until the I/O thread of its last session has closed
// it, so a dying session and a fresh acquire never hold the device at once.
// While that session's destructor is still deciding (it waits at most
// kShutdownJoinMs), acquire() waits for the outcome; once the destructor has
// detached a thread stuck in a stalled transaction, acquire() refuses at once
// with PortBusy ("still closing after a stalled transaction") until the
// thread has closed the port. The registry lock is never held while a session
// is torn down or waited for.
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

    // Reuse bus identity normalization for facade collision checks.
    static bool samePort(const std::string& a, const std::string& b) {
        return normalizeKey(a) == normalizeKey(b);
    }

private:
    static std::string normalizeKey(const std::string& portName);

    struct Entry {
        std::weak_ptr<ModbusBusSession> session;
        std::weak_ptr<ModbusBusSession::PortReservation> reservation;
    };

    std::mutex mutex_;
    SerialPortFactory factory_;
    std::map<std::string, Entry> sessions_;
};

} // namespace backend::services::serialbus
