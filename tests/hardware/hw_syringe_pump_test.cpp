// hw_syringe_pump_test  (LABEL: hardware) — runs only against a real pump.
//
// Set MIB_TEST_PUMP_PORT to the COM number (e.g. 6) of a connected dLSP pump.
// Optional: MIB_TEST_PUMP_BAUD (default 115200), MIB_TEST_PUMP_ADDR (default 1).
// Skips (exit 77) when MIB_TEST_PUMP_PORT is absent.

#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"
#include "support/hardware.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using backend::services::SyringePumpService;

int main()
{
    const int port = std::atoi(mib::test::requireDeviceEnv("MIB_TEST_PUMP_PORT"));
    const int baud = mib::test::envInt("MIB_TEST_PUMP_BAUD", 115200);
    const int addr = mib::test::envInt("MIB_TEST_PUMP_ADDR", 1);

    backend::services::serialbus::SerialBusManager busManager;
    SyringePumpService svc(busManager);
    const auto id = SyringePumpService::PumpId::Sample;

    MIB_REQUIRE(svc.connect(id, port, baud, static_cast<uint8_t>(addr)),
                "connect to syringe pump");
    MIB_EXPECT(svc.isConnected(id), "pump reports connected");

    svc.pollStatus(id);
    const auto st = svc.getStatus(id);
    std::printf("pump status: connected=%d runStatus=%d stalled=%d\n",
                st.connected, static_cast<int>(st.runStatus), st.stalled);
    MIB_EXPECT(st.connected, "status snapshot shows connected");

    svc.disconnect(id);
    MIB_EXPECT(!svc.isConnected(id), "pump disconnects cleanly");

    if (mib::test::exitCode() == 0) std::printf("syringe pump hardware OK\n");
    return mib::test::exitCode();
}
