// serial_startup_race_test
//
// Startup on simulated serial hardware (SimulatedSerial.h), through the real
// AppBackend: StartupDiscoveryCoordinator searches for the nanopositioner on
// its own thread, while the operator connects the Sample and Sheath pumps and
// the pulse generator at a seeded random moment, as on the rig. The rig opens
// ports exclusively (Windows semantics) on every platform, delays enumeration
// and replies, and keeps a ledger of every open, so a race that only showed
// on the Windows bench fails here, on Linux CI and under TSan.
//
// Per seed:
//  - every device the operator connects connects (a startup probe never
//    holds the port when the operator's connect needs it);
//  - startup connects the nanopositioner, which enumerates late;
//  - the ledger has no contended open and no foreign bytes on the pump bus
//    (an OEABT ASCII probe written onto RS485 reaches every pump on it);
//  - shutdown releases every port.
//
// Usage: serial_startup_race_test [seeds=6] [firstSeed=1]. A failing
// seed prints the ledger; rerun it alone with `1 <seed>`.

#include "backend/app/AppBackend.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SimulatedSerial.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>

namespace serialsim = backend::services::serialsim;
using backend::discovery::StartupDiscoveryCoordinator;
using NanoOutcome = StartupDiscoveryCoordinator::NanopositionerOutcome;
using namespace std::chrono_literals;

namespace {

// The bench of makeBenchRig, with startup delays scaled to the test's
// shortened coordinator timing.
std::shared_ptr<serialsim::Rig> makeRig(uint64_t seed)
{
    auto rig = std::make_shared<serialsim::Rig>(seed);

    serialsim::PortSpec rs485;
    rs485.systemName = "COM3";
    rs485.description = "USB-SERIAL CH340 (RS485 pump bus)";
    rs485.vendorId = 0x1a86; // CH340: the common USB-RS485 dongle, and an OEABT candidate
    rs485.productId = 0x7523;
    rs485.openLatencyMs = 15;
    rs485.replyLatencyMinMs = 1;
    rs485.replyLatencyMaxMs = 8;
    rig->addPort(rs485);
    rig->addDevice("COM3", serialsim::makeDlspPump(1));
    rig->addDevice("COM3", serialsim::makeDlspPump(2));

    serialsim::PortSpec generator;
    generator.systemName = "COM4";
    generator.description = "FT232R USB UART (pulse generator)";
    generator.vendorId = 0x0403;
    generator.productId = 0x6001;
    generator.serialNumber = "SIMPG001";
    generator.openLatencyMs = 10;
    rig->addPort(generator);
    rig->addDevice("COM4", serialsim::makePulseGenerator(1));

    serialsim::PortSpec nano;
    nano.systemName = "COM5";
    nano.description = "USB-SERIAL CH340 (OEABT nanopositioner)";
    nano.vendorId = 0x1a86;
    nano.productId = 0x7523;
    nano.presentAfterMs = 250; // misses the first attempt
    nano.openLatencyMs = 20;
    nano.silentAfterOpenMs = 40;
    nano.replyLatencyMinMs = 2;
    nano.replyLatencyMaxMs = 15;
    nano.fragmentReplies = true;
    rig->addPort(nano);
    rig->addDevice("COM5", std::make_shared<serialsim::OeabtDevice>());
    return rig;
}

struct SeedResult {
    bool samplePump{false};
    bool sheathPump{false};
    bool generator{false};
    std::optional<NanoOutcome::Kind> nano;
    std::string nanoPath;
};

bool runSeed(uint64_t seed, mib::test::Watchdog& watchdog)
{
    auto rig = makeRig(seed);
    serialsim::install(rig);
    std::mt19937_64 rng(seed * 7919u + 1);
    const auto operatorDelay = std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 700)(rng));

    SeedResult result;
    {
        mib::test::TempDir dir("serial_startup_race");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");

        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 20ms;
        timing.nanopositionerRetries = 4;
        timing.nanopositionerRetryDelay = 200ms;
        timing.nanopositionerDeadline = 10000ms;
        auto& coordinator = backend.startupDiscovery();
        coordinator.setTiming(timing);

        std::mutex m;
        std::condition_variable cv;
        coordinator.setNanopositionerListener([&](const NanoOutcome& o) {
            if (o.kind == NanoOutcome::Kind::Started || o.kind == NanoOutcome::Kind::Searching) return;
            std::lock_guard<std::mutex> lock(m);
            result.nano = o.kind;
            if (o.endpoint) result.nanoPath = o.endpoint->systemPath;
            cv.notify_all();
        });

        watchdog.mark("startup");
        coordinator.start();
        std::thread operatorThread([&] {
            std::this_thread::sleep_for(operatorDelay);
            using backend::services::SyringePumpService;
            result.samplePump = backend.syringePump().connect(SyringePumpService::PumpId::Sample, 3, 115200, 1);
            result.sheathPump = backend.syringePump().connect(SyringePumpService::PumpId::Sheath, 3, 115200, 2);
            result.generator = backend.pulseGenerator().connect("COM4", 9600, 1);
        });
        operatorThread.join();
        watchdog.mark("operator connected");
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, 15s, [&] { return result.nano.has_value(); });
        }
        watchdog.mark("startup outcome");
        backend.shutdown();
        watchdog.mark("shutdown");
    }

    bool ok = true;
    const auto check = [&](bool cond, const std::string& what) {
        MIB_EXPECT(cond, "seed " + std::to_string(seed) + ": " + what);
        ok = ok && cond;
    };
    check(result.samplePump, "Sample pump connects on COM3");
    check(result.sheathPump, "Sheath pump connects on COM3");
    check(result.generator, "pulse generator connects on COM4");
    check(result.nano == NanoOutcome::Kind::Connected, "startup connects the nanopositioner");
    check(serialsim::Rig::key(result.nanoPath) == "COM5", "the nanopositioner is the one on COM5");
    check(rig->contentionCount() == 0, "no two owners wanted one port at once");
    int foreignOnPumpBus = 0;
    for (const auto& e : rig->events(serialsim::Event::Kind::ForeignFrame)) foreignOnPumpBus += e.port == "COM3";
    check(foreignOnPumpBus == 0, "nothing but Modbus reaches the pump bus");
    for (const char* port : {"COM3", "COM4", "COM5"}) {
        check(rig->holders(port) == 0, std::string("shutdown released ") + port);
    }
    if (!ok) {
        std::fprintf(stderr, "seed %llu (operator at +%lldms) ledger: %s\n",
                     static_cast<unsigned long long>(seed),
                     static_cast<long long>(operatorDelay.count()), rig->summary().c_str());
    }
    serialsim::install(nullptr);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 6;
    const uint64_t firstSeed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    mib::test::Watchdog watchdog(60);
    int failed = 0;
    for (int i = 0; i < seeds; ++i) {
        failed += runSeed(firstSeed + static_cast<uint64_t>(i), watchdog) ? 0 : 1;
    }
    std::printf("serial_startup_race: %d/%d seeds clean\n", seeds - failed, seeds);
    return mib::test::exitCode();
}
