// In-process arbitration of serial ports between the components that hold a
// port for a session (pump and generator bus sessions, the connected
// nanopositioner) and discovery probes that open a port for a moment to
// identify what is behind it.
//
// Without it, two discovery paths took ports the operator was connecting,
// whatever the adapter's chip (found by the simulated rig, SimulatedSerial.h,
// backend.serial_startup_race):
//  - the startup nanopositioner probe opens every CH34x adapter, including a
//    CH340 USB-RS485 pump dongle, for about 1 s per attempt, writing ASCII
//    onto the bus: a pump Connect then failed with ACCESS_DENIED (Windows);
//  - the pulse generator's "auto" sweep opens every USB adapter at 9600: a
//    pump Connect at 115200 then failed with "different settings".
//
// Rules:
//  - a probe never opens a port a service holds or that is reserved;
//  - a service registers, then waits (bounded) for probes on its port to
//    finish, and opens; it never fails because of our own probe, and new
//    probes cannot take the port while it waits;
//  - reservations name ports configured for other devices (config.json
//    pump ports), so probes skip the pump bus even before the pumps connect.
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
// holds its port about 1 s (two commands, 500 ms timeouts); backend.serial_sim
// checks a failed probe releases it well inside this bound.
inline constexpr std::chrono::milliseconds kProbeYield{3000};

class Claim;

// What refused a probe.
enum class Holder {
    None,
    BusSession, // a SerialBus session (shareable by another bus client at the same settings)
    Service,    // an exclusive holder (the connected nanopositioner)
    Probe,      // another probe, still on the port after any wait
    Reserved    // config.json assigns the port to the pumps
};

// A probe may open `port` only when no service claims it, no other probe
// is on it, and nothing reserves it. Returns an empty Claim when refused;
// `why` names the holder or reservation. With `waitForProbes`, a port another
// probe is on is waited for (up to that long) instead of refused: a sweep that
// must look at every adapter (the pulse generator's "auto" port) waits; the
// startup nanopositioner probe, which retries anyway, does not.
Claim tryProbe(const std::string& port, const std::string& owner, std::string* why = nullptr,
               std::chrono::milliseconds waitForProbes = std::chrono::milliseconds(0),
               Holder* heldBy = nullptr);

// A service claim always succeeds, after waiting up to `maxWait` for probes on
// the port to finish. The claim is registered before the wait, so no new
// probe takes the port meanwhile (probes retrying back to back would starve
// it). Several services may claim one port (the bus layer shares one session
// between them; `busSession` marks those claims). A probe held by the calling
// thread is not waited for: a probe that opens its port through the bus layer
// (the generator sweep) must not wait for itself.
Claim claimForService(const std::string& port, const std::string& owner,
                      std::chrono::milliseconds maxWait = kProbeYield, bool busSession = false);

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
    friend Claim tryProbe(const std::string&, const std::string&, std::string*, std::chrono::milliseconds, Holder*);
    friend Claim claimForService(const std::string&, const std::string&, std::chrono::milliseconds, bool);
    explicit Claim(uint64_t id) : id_(id) {}
    uint64_t id_{0};
};

// Replace `owner`'s reserved ports (empty clears them).
void setReserved(const std::string& owner, const std::vector<std::string>& ports);
// True when some owner reserves `port`; `forWhom` names it.
bool isReserved(const std::string& port, std::string* forWhom = nullptr);

// "COM3", "\\\\.\\COM3", "/dev/COM3", "com3" -> "com3"; "/dev/ttyUSB0" -> "ttyusb0".
std::string key(const std::string& port);

// Who holds `port` now (claims only; reservations are isReserved's): a
// service (BusSession or exclusive Service) wins over a probe; None when free.
Holder holder(const std::string& port);

// Diagnostics/tests: who holds `port` now ("" when free).
std::string describe(const std::string& port);

} // namespace backend::services::portclaims
