// Which USB-serial adapter a configured COM port refers to, and where that
// adapter is now.
//
// config.json names the pump bus by COM number only. Windows renumbers an
// adapter when it moves to another USB port (adapters without a serial
// number, such as CH340, are identified by where they are plugged in), and
// a COM number can be reused by another device. A Connect on the configured
// number then opens the wrong device or fails. After a successful connect
// the pump tab remembers the adapter's identity (`pump_*_adapter`), and
// resolveAdapterPort() checks it before the next connect:
//  - an adapter with a serial number is followed to its new COM port;
//  - an adapter without one is never guessed: a CH340 pump dongle and a
//    CH340 nanopositioner look the same. If its COM port is gone, the
//    operator is told which ports carry the same adapter type;
//  - a configured port that now carries a different adapter is refused, so
//    pump frames never go to another device.
// No identity remembered (older configs): the configured port, unchecked.
#pragma once

#include "backend/services/ISerialPort.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace backend::services {

struct AdapterIdentity {
    uint16_t vendorId{0};
    uint16_t productId{0};
    // Empty: the adapter reports none. identityOf() drops the location-based
    // instance suffix Windows reports for such adapters ("5&2c5f1a2b&0&3").
    std::string serialNumber;
    std::string description;  // for messages only, never matched

    bool known() const { return vendorId != 0 || productId != 0 || !serialNumber.empty(); }
    // "CH340 (1a86:7523, serial A1B2)"
    std::string describe() const;
};

AdapterIdentity identityOf(const SerialPortInfo& port);
// Same VID:PID, and the same serial number when `identity` has one.
bool matches(const SerialPortInfo& port, const AdapterIdentity& identity);

// config.json form: {"vid":"1a86","pid":"7523","serial":"","description":"..."}.
nlohmann::json toJson(const AdapterIdentity& identity);
// Anything malformed, or without both VID and PID, reads as an unknown identity.
AdapterIdentity adapterIdentityFromJson(const nlohmann::json& value);

struct PortResolution {
    enum class Kind {
        Configured, // connect on the configured port
        Moved,      // the remembered adapter is on `port` now
        Mismatch,   // the configured port carries another adapter: refuse
        Missing,    // the adapter is not connected (candidates: same type)
        Ambiguous   // several ports carry the same serial number (clones)
    };
    Kind kind{Kind::Configured};
    std::string port;                    // Configured, Moved
    std::vector<std::string> candidates; // Missing, Ambiguous
    std::string message;                 // operator-facing, empty for Configured

    bool connectable() const { return kind == Kind::Configured || kind == Kind::Moved; }
};

PortResolution resolveAdapterPort(const std::string& configuredPort, const AdapterIdentity& remembered,
                                  const std::vector<SerialPortInfo>& present);

} // namespace backend::services
