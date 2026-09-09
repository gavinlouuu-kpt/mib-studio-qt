// serial_port_enumeration_test
//
// Guard for the Qt-free serial port enumeration (ISerialPort.h
// enumerateSerialPorts(): Win32 SetupAPI / Linux sysfs) that replaced
// QSerialPortInfo behind serialbus::availablePorts(). Hardware-agnostic:
// asserts the call is safe, names are well formed and unique, and every
// entry has a system location; when MIB_TEST_EXPECT_PORT names a port that
// is attached (e.g. "COM6"), it must be listed with a non-zero USB VID.

#include "backend/services/ISerialPort.h"
#include "backend/services/SerialBus.h"

#include "support/assert.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

int main()
{
    const auto ports = backend::services::enumerateSerialPorts();
    std::printf("%zu serial port(s)\n", ports.size());
    std::set<std::string> names;
    for (const auto& p : ports) {
        std::printf("  %s | %s | %s | %s | sn=%s | %04x:%04x\n", p.systemName.c_str(),
                    p.systemLocation.c_str(), p.description.c_str(), p.manufacturer.c_str(),
                    p.serialNumber.c_str(), p.vendorId, p.productId);
        MIB_EXPECT(!p.systemName.empty(), "every entry has a system name");
        MIB_EXPECT(!p.systemLocation.empty(), "every entry has a system location");
        MIB_EXPECT(p.systemName.find('/') == std::string::npos && p.systemName.find('\\') == std::string::npos,
                   "system name is a bare device name, not a path");
        MIB_EXPECT(names.insert(p.systemName).second, "system names are unique");
    }
    // The bus-layer alias returns the same list.
    MIB_EXPECT(backend::services::serialbus::availablePorts().size() == ports.size(),
               "serialbus::availablePorts() mirrors enumerateSerialPorts()");

    if (const char* expected = std::getenv("MIB_TEST_EXPECT_PORT")) {
        const auto it = names.find(expected);
        MIB_REQUIRE(it != names.end(), std::string("expected attached port listed: ") + expected);
        for (const auto& p : ports) {
            if (p.systemName == expected) {
                MIB_EXPECT(p.vendorId != 0, "attached USB port carries its vendor id");
            }
        }
    }
    return mib::test::exitCode();
}
