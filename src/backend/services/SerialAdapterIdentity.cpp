#include "backend/services/SerialAdapterIdentity.h"

#include "backend/services/SerialPortClaims.h"

#include <cstdio>
#include <cstdlib>

namespace backend::services {

namespace {

std::string hex4(uint16_t value)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(value));
    return buf;
}

uint16_t parseHex4(const nlohmann::json& value)
{
    if (!value.is_string()) return 0;
    const std::string text = value.get<std::string>();
    if (text.empty() || text.size() > 4) return 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
    return end && *end == '\0' ? static_cast<uint16_t>(parsed) : 0;
}

std::string joined(const std::vector<std::string>& names)
{
    std::string out;
    for (const auto& name : names) out += (out.empty() ? "" : ", ") + name;
    return out;
}

// Windows reports an instance-ID suffix as the serial. For an adapter without
// a USB serial number Windows generates it from the USB location
// ("5&2c5f1a2b&0&3"); a real serial never contains '&'.
std::string deviceSerial(const std::string& reported)
{
    return reported.find('&') == std::string::npos ? reported : std::string();
}

const SerialPortInfo* findPort(const std::vector<SerialPortInfo>& present, const std::string& port)
{
    const auto wanted = portclaims::key(port);
    for (const auto& info : present)
        if (portclaims::key(info.systemName) == wanted) return &info;
    return nullptr;
}

} // namespace

std::string AdapterIdentity::describe() const
{
    std::string out = description.empty() ? std::string("USB adapter") : description;
    out += " (" + hex4(vendorId) + ":" + hex4(productId);
    if (!serialNumber.empty()) out += ", serial " + serialNumber;
    return out + ")";
}

AdapterIdentity identityOf(const SerialPortInfo& port)
{
    AdapterIdentity id;
    id.vendorId = port.vendorId;
    id.productId = port.productId;
    id.serialNumber = deviceSerial(port.serialNumber);
    id.description = port.description;
    return id;
}

bool matches(const SerialPortInfo& port, const AdapterIdentity& identity)
{
    if (!identity.known()) return false;
    if (port.vendorId != identity.vendorId || port.productId != identity.productId) return false;
    return identity.serialNumber.empty() || deviceSerial(port.serialNumber) == identity.serialNumber;
}

nlohmann::json toJson(const AdapterIdentity& identity)
{
    return {{"vid", hex4(identity.vendorId)},
            {"pid", hex4(identity.productId)},
            {"serial", identity.serialNumber},
            {"description", identity.description}};
}

AdapterIdentity adapterIdentityFromJson(const nlohmann::json& value)
{
    AdapterIdentity id;
    if (!value.is_object()) return id;
    id.vendorId = parseHex4(value.value("vid", nlohmann::json()));
    id.productId = parseHex4(value.value("pid", nlohmann::json()));
    const auto serial = value.find("serial");
    if (serial != value.end() && serial->is_string()) id.serialNumber = serial->get<std::string>();
    const auto description = value.find("description");
    if (description != value.end() && description->is_string()) id.description = description->get<std::string>();
    // Both IDs or nothing: a half-read identity would refuse the right adapter.
    if (id.vendorId == 0 || id.productId == 0) return AdapterIdentity{};
    return id;
}

PortResolution resolveAdapterPort(const std::string& configuredPort, const AdapterIdentity& remembered,
                                  const std::vector<SerialPortInfo>& present)
{
    PortResolution r;
    r.port = configuredPort;
    if (!remembered.known()) return r; // nothing remembered: the configured port, as before

    const SerialPortInfo* configured = findPort(present, configuredPort);
    const std::string adapter = remembered.describe();

    if (!remembered.serialNumber.empty()) {
        std::vector<const SerialPortInfo*> hits;
        for (const auto& info : present)
            if (matches(info, remembered)) hits.push_back(&info);
        if (configured && matches(*configured, remembered)) return r; // still where it was
        if (hits.size() == 1) {
            r.kind = PortResolution::Kind::Moved;
            r.port = hits.front()->systemName;
            r.message = "The pump adapter " + adapter + " moved from " + configuredPort + " to " + r.port + ".";
            return r;
        }
        if (hits.size() > 1) {
            r.kind = PortResolution::Kind::Ambiguous;
            for (const auto* hit : hits) r.candidates.push_back(hit->systemName);
            r.message = "Several ports report the pump adapter's serial number " + remembered.serialNumber +
                        " (" + joined(r.candidates) + "), so the pump port is not certain. Select it in " +
                        "Syringe Pump Settings.";
            return r;
        }
        // Not connected: fall through to the configured-port checks below.
    } else if (configured && matches(*configured, remembered)) {
        return r; // the same adapter type is still on the configured port
    }

    if (configured) {
        r.kind = PortResolution::Kind::Mismatch;
        r.message = configuredPort + " now carries " + identityOf(*configured).describe() + ", not the pump adapter " +
                    adapter + ". If you replaced the adapter, select " + configuredPort +
                    " again in Syringe Pump Settings.";
        return r;
    }

    r.kind = PortResolution::Kind::Missing;
    AdapterIdentity sameType = remembered;
    sameType.serialNumber.clear();
    for (const auto& info : present)
        if (matches(info, sameType)) r.candidates.push_back(info.systemName);
    r.message = configuredPort + " is not present, and the pump adapter " + adapter + " is not connected.";
    if (!r.candidates.empty() && remembered.serialNumber.empty()) {
        // Without a serial number the adapter cannot be told apart from
        // others of its type (the nanopositioner may be one): never guessed.
        r.message = configuredPort + " is not present. Ports with the pump adapter's type " + adapter + ": " +
                    joined(r.candidates) + ". If the pumps moved to one of them, select it in Syringe Pump " +
                    "Settings.";
    }
    return r;
}

} // namespace backend::services
