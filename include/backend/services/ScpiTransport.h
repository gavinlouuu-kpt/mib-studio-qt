// Line-oriented SCPI transport behind the RF generator service (SIGLENT
// SSG3021X). The instrument speaks newline-terminated ASCII over USBTMC
// (NI-VISA on Windows, /dev/usbtmcN on Linux) or a raw LAN socket (port 5025);
// the service only sees "send a line, read a line" so tests drive it with an
// in-memory fake and a loopback TCP server.
//
// Timing never goes over this link: a SCPI round trip is milliseconds and
// jittery. It carries configuration readback and provenance only.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace backend::services::scpi {

class IScpiTransport {
public:
    virtual ~IScpiTransport() = default;

    // `resource` is transport-specific: "host[:port]" for LAN, a device path
    // ("/dev/usbtmc0") or VISA resource ("USB0::0xF4EC::...::INSTR") for USB,
    // or "auto" to take the first USBTMC instrument found. False + message on
    // failure; a transport that cannot exist on this platform says so.
    virtual bool open(const std::string& resource, std::string* error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Send one command (the transport appends the line terminator).
    virtual bool write(std::string_view command, std::string* error) = 0;
    // Read one reply line, terminator stripped. False on timeout/link error.
    virtual bool readLine(std::string& reply, std::chrono::milliseconds timeout,
                          std::string* error) = 0;
    // Human-readable link description for logs/provenance ("lan 10.0.0.5:5025").
    virtual std::string describe() const = 0;
};

// Built-in transports. Each returns nullptr only if the kind is unknown; a
// transport unsupported on this platform still constructs and fails open().
//   "lan"  raw TCP socket (all platforms)
//   "usb"  USBTMC: NI-VISA (Windows, visa64.dll loaded at runtime),
//          kernel usbtmc class driver (Linux); unavailable elsewhere
std::unique_ptr<IScpiTransport> makeScpiTransport(const std::string& kind);
using ScpiTransportFactory = std::function<std::unique_ptr<IScpiTransport>(const std::string& kind)>;

} // namespace backend::services::scpi
