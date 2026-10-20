// Simulated serial hardware (Qt-free): a rig of fake COM ports with Modbus RTU
// and OEABT devices behind them, timing faults, and an ownership ledger.
//
// The unit-test fakes (tests/support/fake_modbus_bench.h, fake_zc300.h)
// answer instantly and are injected per service. That hides the races seen on
// the rig, because they come from *who opens which port when*: a startup probe
// opening an adapter a pump is about to claim, or a slow open overlapping a
// connect. Linux hides them further: POSIX opens are not exclusive, so a
// second open that fails with ACCESS_DENIED on Windows silently succeeds.
//
// Once a rig is installed (serialsim::install), makePlatformSerialPort() and
// enumerateSerialPorts() serve it instead of the OS. Every component that
// opens a port then talks to the simulation through its normal code path:
// SerialBusManager sessions, the OEABT nanopositioner transport, discovery
// probes. The rig:
//  - opens exclusively (Windows semantics) on every platform by default;
//  - delays enumeration (presentAfterMs), opens (openLatencyMs) and replies
//    (jittered from a seeded RNG, so a failing seed replays);
//  - keeps a device silent after open (silentAfterOpenMs, adapter/MCU boot);
//  - can unplug and replug a port;
//  - records every open, denial, close and foreign frame (bytes no device on
//    that port understands, e.g. an ASCII probe written onto an RS485 bus).
//
// The app selects it with MIB_SERIAL_MODE=sim (see installFromEnvironment);
// tests build a rig in code and install it. Guide: docs/howto/simulated-serial.md.
#pragma once

#include "backend/services/ISerialPort.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace backend::services::serialsim {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// One device on a simulated port. A port carries several devices when it is a
// multi-drop RS485 bus. The rig calls a device under the rig mutex, so a
// device needs no locking of its own.
class SimDevice {
public:
    virtual ~SimDevice() = default;
    virtual std::string kind() const = 0;
    // The line rate the device listens at; frames sent at another rate reach
    // it as noise and are never answered.
    virtual int baudRate() const = 0;
    // True when the frame is valid traffic for this device's protocol, even if
    // addressed to another device. A frame no device on the port understands
    // is recorded as ForeignFrame.
    virtual bool understands(const Bytes& frame) const = 0;
    // The reply to one request frame (one write() call), or nothing.
    virtual std::optional<Bytes> onFrame(const Bytes& frame) = 0;
};

// Holding (FC03/06/16) and input (FC04) registers at one slave address.
// Unmapped registers read 0. Writes land in the holding map.
class ModbusRegisterDevice : public SimDevice {
public:
    ModbusRegisterDevice(std::string kind, uint8_t address, int baudRate);

    std::string kind() const override { return kind_; }
    int baudRate() const override { return baudRate_; }
    bool understands(const Bytes& frame) const override;
    std::optional<Bytes> onFrame(const Bytes& frame) override;

    uint8_t address() const { return address_; }
    std::map<uint16_t, uint16_t> holding; // wire address -> value
    std::map<uint16_t, uint16_t> input;   // wire address -> value
    int writeCount{0};                    // FC06/FC16 frames accepted

private:
    std::string kind_;
    uint8_t address_;
    int baudRate_;
};

// OEABT piezo controller: ASCII commands ending in '\r', framed replies
// 0xFF 0x1F 0x30 0x60 <payload> 0x03 '\r' '\n'.
class OeabtDevice : public SimDevice {
public:
    explicit OeabtDevice(int baudRate = 115200) : baudRate_(baudRate) {}

    std::string kind() const override { return "oeabt"; }
    int baudRate() const override { return baudRate_; }
    bool understands(const Bytes& frame) const override;
    std::optional<Bytes> onFrame(const Bytes& frame) override;

    int voltageMv{25000};

private:
    int baudRate_;
};

// Device presets matching what the real services verify on connect.
std::unique_ptr<ModbusRegisterDevice> makePulseGenerator(uint8_t address, int baudRate = 9600);
std::unique_ptr<ModbusRegisterDevice> makeDlspPump(uint8_t address, int baudRate = 115200);
std::unique_ptr<ModbusRegisterDevice> makeTushuiPump(uint8_t address, int baudRate = 115200);
std::unique_ptr<ModbusRegisterDevice> makeZc300(uint8_t address, int32_t serial = 26017);

struct PortSpec {
    std::string systemName;     // "COM3"; how services name the port
    std::string description{"Simulated serial port"};
    std::string manufacturer{"MIB serial-sim"};
    std::string serialNumber;
    uint16_t vendorId{0};       // 0x1a86/0x7523 (CH340) makes it an OEABT candidate
    uint16_t productId{0};
    int presentAfterMs{0};      // absent from enumeration and open until then
    int openLatencyMs{0};       // each open() blocks this long
    int replyLatencyMinMs{2};   // reply delay, uniform in [min, max]
    int replyLatencyMaxMs{8};
    int silentAfterOpenMs{0};   // devices ignore frames this soon after an open
    bool fragmentReplies{false}; // deliver each reply in two chunks
    // Scheduled cable pulls, ms after the rig was made: {from, to}; to < 0
    // never comes back. A handle opened before `from` stays dead afterwards.
    std::vector<std::pair<int, int>> unpluggedWindowsMs;
    // Another process holds the port from the start: an earlier instance
    // that crashed or hung without closing it (#417), or a vendor tool.
    // Opens are refused with ACCESS_DENIED until this many ms after the rig
    // was made (the stale process exits); -1 holds until releaseStaleHolder().
    int staleHolderUntilMs{0};
};

struct Event {
    enum class Kind {
        Opened,
        OpenDenied,   // another handle or process holds the port (exclusive open)
        SharedOpen,   // a second handle opened it (non-exclusive mode)
        OpenFailed,   // port absent, not yet present, or unplugged
        Closed,
        ForeignFrame, // no device on the port understands the bytes
        BaudMismatch, // a device heard the frame at the wrong line rate
        Unplugged,
        Replugged
    };
    Kind kind;
    std::string port;
    std::string detail;
    Clock::time_point at;
    std::thread::id thread;
};

const char* toString(Event::Kind kind);

class Rig : public std::enable_shared_from_this<Rig> {
public:
    explicit Rig(uint64_t seed = 1);
    ~Rig();

    // Windows semantics (default): a second open of a held port fails with
    // ACCESS_DENIED / EACCES. false mimics POSIX: it succeeds and is recorded
    // as SharedOpen.
    void setExclusiveOpen(bool exclusive);
    uint64_t seed() const { return seed_; }

    void addPort(PortSpec spec);
    // Attach a device to a port added before.
    void addDevice(const std::string& port, std::shared_ptr<SimDevice> device);

    // While unplugged a port is absent from enumeration, opens fail, and an
    // open handle's writes fail, as when the USB cable is pulled.
    void unplug(const std::string& port);
    void replug(const std::string& port);

    // Another process grabs the port (any open handle here keeps working, as
    // Windows only refuses new opens), or lets it go.
    void holdByStaleProcess(const std::string& port);
    void releaseStaleHolder(const std::string& port);

    std::unique_ptr<ISerialPort> makePort();
    std::vector<SerialPortInfo> enumerate() const;
    std::vector<int> comPortNumbers() const; // present "COMn" ports, for COM-number pickers

    std::vector<Event> events() const;
    std::vector<Event> events(Event::Kind kind) const;
    // OpenDenied + SharedOpen: two owners wanted one port at the same time.
    int contentionCount() const;
    // Number of handles currently holding the port.
    int holders(const std::string& port) const;
    std::string summary() const;

    // Normalized port key: "COM3", "\\\\.\\COM3", "/dev/COM3" and "com3" match.
    static std::string key(const std::string& portName);

private:
    friend class SimPort;
    struct PortState;
    struct Holder {
        const void* handle;
        std::thread::id thread;
        Clock::time_point since;
        std::string firstFrame; // protocol fingerprint of its first write
    };

    PortState* findLocked(const std::string& portName);
    const PortState* findLocked(const std::string& portName) const;
    bool presentLocked(const PortState& port, Clock::time_point now) const;
    bool staleLocked(const PortState& port, const void* handle, Clock::time_point now) const;
    bool heldElsewhereLocked(const PortState& port, Clock::time_point now) const;
    void recordLocked(Event::Kind kind, const std::string& port, std::string detail);
    int latencyLocked(const PortState& port);

    const uint64_t seed_;
    const Clock::time_point start_;
    mutable std::mutex mutex_;
    std::mt19937_64 rng_;
    bool exclusive_{true};
    std::vector<std::unique_ptr<PortState>> ports_;
    std::vector<Event> events_;
};

// Process-wide switch. While a rig is installed, makePlatformSerialPort(),
// and enumerateSerialPorts() serve it. Tools::availableComPortNumbers()
// (portable core, no serial dependency) still reads the OS; COM-number
// pickers ask the rig's comPortNumbers() instead.
// install(nullptr) restores the OS ports. Ports made before stay bound to
// the rig that made them.
void install(std::shared_ptr<Rig> rig);
std::shared_ptr<Rig> installed();

// The built-in bench: a CH340 RS485 adapter with the Sample (addr 1) and
// Sheath (addr 2) dLSP pumps on COM3, the pulse generator at 9600 baud on
// COM4, the OEABT nanopositioner on its own CH340 on COM5, and a ZC300 Z stage
// on COM6. Startup delays and jitter are on, so races can show.
std::shared_ptr<Rig> makeBenchRig(uint64_t seed = 1);

} // namespace backend::services::serialsim
