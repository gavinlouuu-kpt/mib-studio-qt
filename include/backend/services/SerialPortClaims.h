// In-process arbitration of serial ports between the components that hold a
// port for a session (pump and generator bus sessions, the connected
// nanopositioner) and discovery probes that open a port for a moment to
// identify what is behind it.
//
// Without it, the startup nanopositioner probe opened every CH340 adapter,
// including a CH340 USB-RS485 dongle carrying the pumps. It held the port for
// about 2 s and wrote ASCII onto the RS485 bus. A pump Connect in that window
// failed with ACCESS_DENIED on Windows, and silently shared the tty on Linux.
// Found by the simulated rig (SimulatedSerial.h,
// backend.serial_startup_race).
//
// Rules:
//  - a probe never opens a port a service holds or that is reserved;
//  - a service waits (bounded) for a probe on its port to finish, then opens;
//    it never fails because of our own probe;
//  - reservations name ports configured for other devices (config.json
//    pump ports), so a probe skips the pump bus even before the pumps connect.
// The registry only covers this process. A port another process holds (or a
// handle a crashed instance never closed) still fails at open with the OS
// "busy" code, which callers report as PortBusy.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::services::portclaims {

// How long a service waits for a probe on its port. An OEABT identity probe
// takes up to ~2 s (two commands, 500 ms timeouts, one retry).
inline constexpr std::chrono::milliseconds kProbeYield{3000};

class Claim;

// A probe may open `port` only when no service claims it, no other probe
// is on it, and nothing reserves it. Returns an empty Claim when refused;
// `why` names the holder or reservation.
Claim tryProbe(const std::string& port, const std::string& owner, std::string* why = nullptr);

// A service claim always succeeds, after waiting up to `maxWait` for probes on
// the port to finish. Several services may claim one port (the bus layer
// shares one session between them).
Claim claimForService(const std::string& port, const std::string& owner,
                      std::chrono::milliseconds maxWait = kProbeYield);

// Held for as long as the port is in use; releases on destruction.
class Claim {
public:
    Claim() = default;
    ~Claim();
    Claim(Claim&& other) noexcept;
    Claim& operator=(Claim&& other) noexcept;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    explicit operator bool() const { return id_ != 0; }
    void release();

private:
    friend Claim tryProbe(const std::string&, const std::string&, std::string*);
    friend Claim claimForService(const std::string&, const std::string&, std::chrono::milliseconds);
    explicit Claim(uint64_t id) : id_(id) {}
    uint64_t id_{0};
};

// Replace `owner`'s reserved ports (empty clears them).
void setReserved(const std::string& owner, const std::vector<std::string>& ports);

// "COM3", "\\\\.\\COM3", "/dev/COM3", "com3" -> "com3"; "/dev/ttyUSB0" -> "ttyusb0".
std::string key(const std::string& port);

// Diagnostics/tests: who holds `port` now ("" when free).
std::string describe(const std::string& port);

} // namespace backend::services::portclaims
