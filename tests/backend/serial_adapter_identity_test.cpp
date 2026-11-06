// serial_adapter_identity_test
//
// resolveAdapterPort (SerialAdapterIdentity.h): where the pump adapter that
// config.json remembers is now, after Windows renumbered it or another device
// took its COM number. An adapter with a serial number is followed; one
// without is never guessed (a CH340 pump dongle and a CH340 nanopositioner
// look the same); a port that carries another adapter is refused. Also the
// startup reservation: probes skip a renumbered pump adapter's new port.

#include "backend/app/AppBackend.h"
#include "backend/services/SerialAdapterIdentity.h"
#include "backend/services/SerialPortClaims.h"
#include "backend/services/SimulatedSerial.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <string>
#include <vector>

namespace services = backend::services;
namespace serialsim = backend::services::serialsim;
using Kind = services::PortResolution::Kind;

namespace {

services::SerialPortInfo port(const std::string& name, uint16_t vid, uint16_t pid, const std::string& serial = "",
                              const std::string& description = "")
{
    services::SerialPortInfo info;
    info.systemName = name;
    info.vendorId = vid;
    info.productId = pid;
    info.serialNumber = serial;
    info.description = description;
    return info;
}

services::AdapterIdentity identity(uint16_t vid, uint16_t pid, const std::string& serial = "")
{
    services::AdapterIdentity id;
    id.vendorId = vid;
    id.productId = pid;
    id.serialNumber = serial;
    id.description = "pump dongle";
    return id;
}

bool has(const std::vector<std::string>& names, const std::string& name)
{
    for (const auto& n : names)
        if (n == name) return true;
    return false;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(60);
    const auto ch340 = [](const std::string& name, const std::string& serial = "") {
        return port(name, 0x1a86, 0x7523, serial, "USB-SERIAL CH340");
    };
    const auto ftdi = [](const std::string& name, const std::string& serial) {
        return port(name, 0x0403, 0x6001, serial, "USB Serial Port");
    };

    // ---- nothing remembered: the configured port, unchecked (older configs) --
    {
        const auto r = services::resolveAdapterPort("COM3", {}, {});
        MIB_EXPECT(r.kind == Kind::Configured && r.port == "COM3" && r.message.empty(),
                   "no identity: the configured port as before, even when absent");
    }

    // ---- an adapter with a serial number is followed ------------------------
    {
        const auto remembered = identity(0x0403, 0x6001, "FT9X1ABC");
        auto r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM3", "FT9X1ABC"), ch340("COM5")});
        MIB_EXPECT(r.kind == Kind::Configured && r.port == "COM3", "serial adapter still on its port");

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM7", "FT9X1ABC"), ch340("COM5")});
        MIB_EXPECT(r.kind == Kind::Moved && r.port == "COM7" && r.message.find("COM7") != std::string::npos,
                   "renumbered serial adapter is followed to COM7: " + r.message);

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM7", "FT9X1ABC"), ch340("COM3")});
        MIB_EXPECT(r.kind == Kind::Moved && r.port == "COM7",
                   "followed even when another device took the old number");

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM3", "OTHER123")});
        MIB_EXPECT(r.kind == Kind::Mismatch && !r.connectable() && r.message.find("OTHER123") != std::string::npos,
                   "another unit of the same type on COM3 is refused: " + r.message);

        r = services::resolveAdapterPort("COM3", remembered, {ch340("COM5")});
        MIB_EXPECT(r.kind == Kind::Missing && !r.connectable(), "serial adapter unplugged: missing");

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM7", "FT9X1ABC"), ftdi("COM8", "FT9X1ABC")});
        MIB_EXPECT(r.kind == Kind::Ambiguous && has(r.candidates, "COM7") && has(r.candidates, "COM8"),
                   "clones sharing a serial number elsewhere: ambiguous, not guessed");

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM3", "FT9X1ABC"), ftdi("COM8", "FT9X1ABC")});
        MIB_EXPECT(r.kind == Kind::Configured && r.port == "COM3", "clones, one still on COM3: COM3");

        r = services::resolveAdapterPort("com3", remembered, {ftdi("COM3", "FT9X1ABC")});
        MIB_EXPECT(r.kind == Kind::Configured, "port names compare like the bus layer (case)");
    }

    // ---- an adapter without one is never guessed ------------------------------
    {
        const auto remembered = identity(0x1a86, 0x7523);
        auto r = services::resolveAdapterPort("COM3", remembered, {ch340("COM3"), ch340("COM5")});
        MIB_EXPECT(r.kind == Kind::Configured && r.port == "COM3", "CH340 still on COM3");

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM3", "PG0001"), ch340("COM5")});
        MIB_EXPECT(r.kind == Kind::Mismatch && r.message.find("COM3") != std::string::npos,
                   "COM3 now carries an FTDI adapter: refused, no pump frames to another device: " + r.message);

        r = services::resolveAdapterPort("COM3", remembered, {ch340("COM5"), ftdi("COM4", "PG0001")});
        MIB_EXPECT(r.kind == Kind::Missing && r.candidates == std::vector<std::string>{"COM5"} &&
                       r.message.find("COM5") != std::string::npos,
                   "COM3 gone, one CH340 left: named as a candidate, not connected (it may be the "
                   "nanopositioner): " + r.message);

        r = services::resolveAdapterPort("COM3", remembered, {ftdi("COM4", "PG0001")});
        MIB_EXPECT(r.kind == Kind::Missing && r.candidates.empty(), "no adapter of that type: missing");
    }

    // ---- Windows-generated "serials" are not serial numbers ------------------
    {
        // Windows reports the instance-ID suffix; for an adapter without a USB
        // serial it encodes the USB location and changes when the adapter moves.
        const auto seen = services::identityOf(ch340("COM3", "5&2c5f1a2b&0&3"));
        MIB_EXPECT(seen.serialNumber.empty(), "a location-generated instance suffix is not remembered as a serial");
        MIB_EXPECT(services::matches(ch340("COM9", "5&1111aaaa&0&1"), seen), "and never blocks a match");
        const auto real = services::identityOf(ftdi("COM7", "FT9X1ABC"));
        MIB_EXPECT(real.serialNumber == "FT9X1ABC", "a real serial is kept");
    }

    // ---- config.json form ---------------------------------------------------------
    {
        const auto id = identity(0x0403, 0x6001, "FT9X1ABC");
        const auto back = services::adapterIdentityFromJson(services::toJson(id));
        MIB_EXPECT(back.vendorId == 0x0403 && back.productId == 0x6001 && back.serialNumber == "FT9X1ABC" &&
                       back.description == "pump dongle",
                   "identity round-trips through config.json");
        MIB_EXPECT(services::toJson(id)["vid"] == "0403", "VID stored as hex text");
        for (const auto& bad : {nlohmann::json(), nlohmann::json(3), nlohmann::json::object(),
                                nlohmann::json{{"vid", "zz"}, {"pid", "7523"}},
                                nlohmann::json{{"vid", 1234}, {"pid", 7523}},
                                nlohmann::json{{"vid", "12345"}, {"pid", "7523"}}}) {
            MIB_EXPECT(!services::adapterIdentityFromJson(bad).known(), "malformed identity reads as unknown: " +
                                                                            bad.dump());
        }
        const auto vidOnly = services::adapterIdentityFromJson(nlohmann::json{{"vid", "1a86"}});
        MIB_EXPECT(!vidOnly.known(), "a half identity is dropped: it would refuse the right adapter");
    }

    // ---- startup reservation follows a renumbered serial adapter --------------
    {
        watchdog.mark("reservation");
        auto rig = std::make_shared<serialsim::Rig>(5);
        serialsim::PortSpec moved;
        moved.systemName = "COM7";
        serialsim::applyChip(moved, *serialsim::findUsbSerialChip("ft232r"));
        moved.serialNumber = "FT9X1ABC";
        rig->addPort(moved);
        serialsim::PortSpec other;
        other.systemName = "COM5";
        serialsim::applyChip(other, *serialsim::findUsbSerialChip("ch340"));
        rig->addPort(other);
        serialsim::install(rig);
        {
            mib::test::TempDir dir("serial_adapter_identity");
            backend::AppBackend backend;
            MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
            backend.setLastConfigJson(R"({"pump_sample_com_port": 3,
                "pump_sample_adapter": {"vid": "0403", "pid": "6001", "serial": "FT9X1ABC"}})");
            MIB_EXPECT(services::portclaims::isReserved("COM3"), "the configured pump port stays reserved");
            MIB_EXPECT(services::portclaims::isReserved("COM7"), "the adapter's new port is reserved too");
            MIB_EXPECT(!services::portclaims::isReserved("COM5"), "other adapters are not");
            backend.setLastConfigJson(R"({"pump_sample_com_port": 3,
                "pump_sample_adapter": {"vid": "1a86", "pid": "7523", "serial": ""}})");
            MIB_EXPECT(!services::portclaims::isReserved("COM5"),
                       "an adapter without a serial is not followed: the other CH340 stays probe-able");
            backend.shutdown();
        }
        services::portclaims::setReserved("the syringe pumps (config.json)", {});
        serialsim::install(nullptr);
    }

    return mib::test::exitCode();
}
