// hw_peristaltic_pump_test  (LABEL: hardware) — runs only against a real Tushui pump.
//
// MIB_TEST_PERISTALTIC_PORT  system port (e.g. /dev/ttyPS1 on the PZ7035 PS); skips (77) if absent
// MIB_TEST_PERISTALTIC_ADDR  slave address (default 3, the instrument pump)
// MIB_TEST_PERISTALTIC_BAUD  default 115200
// MIB_TEST_PERISTALTIC_RUN_MS  0 (default) = read-only; > 0 turns the motor at 20 rpm
//                            (500 µL/min at 25 µL/rev) for this long, infuse, then stops.
//                            Only set it when the fluid path is ready for the run.
// The pump's speed and direction are put back afterwards.

#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"
#include "support/hardware.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

using backend::services::SyringePumpService;

int main()
{
    const char* port = mib::test::requireDeviceEnv("MIB_TEST_PERISTALTIC_PORT");
    const int addr = mib::test::envInt("MIB_TEST_PERISTALTIC_ADDR", 3);
    const int baud = mib::test::envInt("MIB_TEST_PERISTALTIC_BAUD", 115200);
    const int runMs = mib::test::envInt("MIB_TEST_PERISTALTIC_RUN_MS", 0);
    constexpr double kMicrolitersPerRev = 25.0;

    backend::services::serialbus::SerialBusManager busManager;
    SyringePumpService svc(busManager);
    const auto id = SyringePumpService::PumpId::Sample;

    MIB_REQUIRE(svc.connect(id, port, baud, static_cast<uint8_t>(addr),
                            SyringePumpService::PumpModel::TushuiPeristaltic, kMicrolitersPerRev),
                "connect to the peristaltic pump");
    const auto before = svc.getConfig(id);
    const auto st0 = svc.getStatus(id);
    std::printf("as found: %.2f rpm (%.2f uL/min), %s, runStatus=%d\n", st0.speedRpm,
                before.flowRate, before.direction == SyringePumpService::Direction::Infuse ? "infuse/CW" : "withdraw/CCW",
                static_cast<int>(st0.runStatus));
    MIB_EXPECT(st0.runStatus == SyringePumpService::RunStatus::Stop, "pump is stopped before the test");

    if (runMs > 0 && st0.runStatus == SyringePumpService::RunStatus::Stop) {
        MIB_REQUIRE(svc.setFlowRate(id, 500.0, 100), "500 uL/min (20 rpm)");
        MIB_REQUIRE(svc.setDirection(id, SyringePumpService::Direction::Infuse), "infuse");
        MIB_REQUIRE(svc.start(id), "start");
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(runMs)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            svc.pollStatus(id);
            const auto st = svc.getStatus(id);
            std::printf("  running: runStatus=%d live %.1f uL/min, delivered %.1f uL\n",
                        static_cast<int>(st.runStatus), st.currentFlowRate, st.accumulatedVolume);
        }
        MIB_EXPECT(svc.getStatus(id).runStatus == SyringePumpService::RunStatus::Forward,
                   "pump reports Forward while infusing");
        MIB_REQUIRE(svc.stop(id), "stop");
        svc.pollStatus(id);
        MIB_EXPECT(svc.getStatus(id).runStatus == SyringePumpService::RunStatus::Stop, "stopped");
        std::printf("delivered (estimated): %.1f uL\n", svc.getStatus(id).accumulatedVolume);

        MIB_EXPECT(svc.setFlowRate(id, before.flowRate, 100), "speed put back");
        MIB_EXPECT(svc.setDirection(id, before.direction), "direction put back");
    }

    svc.disconnect(id);
    MIB_EXPECT(!svc.isConnected(id), "pump disconnects cleanly");
    if (mib::test::exitCode() == 0) std::printf("peristaltic pump hardware OK\n");
    return mib::test::exitCode();
}
