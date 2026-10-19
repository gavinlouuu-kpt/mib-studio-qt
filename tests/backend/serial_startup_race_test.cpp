// serial_startup_race_test
//
// Startup on simulated serial hardware (SimulatedSerial.h), through the real
// AppBackend: StartupDiscoveryCoordinator searches for the nanopositioner on
// its own thread while the operator connects the Sample and Sheath pumps and
// the pulse generator at a seeded random moment, as on the rig. The rig opens
// ports exclusively (Windows semantics) on every platform, delays enumeration
// and replies, and keeps a ledger of every open, so a race that only showed
// on the Windows bench fails here, on Linux CI and under TSan.
//
// The pump RS485 adapter is a CH340, the common USB-RS485 dongle, so it is an
// OEABT candidate by USB identity. Before the port claims
// (SerialPortClaims.h) the startup probe held it for ~2 s and the operator's
// pump Connect failed with ACCESS_DENIED on every seed.
//
// Scenarios:
//  1. config.json names the pump port: the probe never touches the pump bus;
//  2. no pump port configured: the probe may identify the bus before the
//     pumps connect, but a pump Connect waits for it and never fails;
//  3. a stale process holds the nanopositioner port at startup (an earlier
//     instance that died without closing it, #417) until it has refused a
//     probe: startup keeps retrying and connects once it is released;
//  4. a stale process holds the pump bus: Connect fails fast as "busy", never
//     hangs, and succeeds once the port is released.
// Every scenario: no in-process contention, and shutdown releases every port.
//
// Usage: serial_startup_race_test [seeds=6] [firstSeed=1]. A failing seed
// prints the ledger; rerun it alone with `1 <seed>`.

#include "backend/app/AppBackend.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SimulatedSerial.h"
#include "backend/services/SyringePumpService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

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
using backend::services::SyringePumpService;
using NanoOutcome = StartupDiscoveryCoordinator::NanopositionerOutcome;
using namespace std::chrono_literals;

namespace {

enum class Scenario { ConfiguredPumpPort, NoPumpConfig, StaleNanopositionerHolder, StalePumpBusHolder };

const char* name(Scenario s)
{
    switch (s) {
    case Scenario::ConfiguredPumpPort: return "configured pump port";
    case Scenario::NoPumpConfig: return "no pump config";
    case Scenario::StaleNanopositionerHolder: return "stale holder on the nanopositioner port";
    case Scenario::StalePumpBusHolder: return "stale holder on the pump bus";
    }
    return "?";
}

// The bench of makeBenchRig, with startup delays scaled to the test's
// shortened coordinator timing.
std::shared_ptr<serialsim::Rig> makeRig(uint64_t seed, Scenario scenario)
{
    auto rig = std::make_shared<serialsim::Rig>(seed);

    serialsim::PortSpec rs485;
    rs485.systemName = "COM3";
    rs485.description = "USB-SERIAL CH340 (RS485 pump bus)";
    rs485.vendorId = 0x1a86;
    rs485.productId = 0x7523;
    rs485.openLatencyMs = 15;
    rs485.replyLatencyMinMs = 1;
    rs485.replyLatencyMaxMs = 8;
    if (scenario == Scenario::StalePumpBusHolder) rs485.staleHolderUntilMs = -1;
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
    if (scenario == Scenario::StaleNanopositionerHolder) nano.staleHolderUntilMs = -1; // until the test releases it
    rig->addPort(nano);
    rig->addDevice("COM5", std::make_shared<serialsim::OeabtDevice>());
    return rig;
}

struct Result {
    bool samplePump{false};
    bool sheathPump{false};
    bool generator{false};
    bool pumpRefusedWhileStale{false};
    std::chrono::milliseconds staleRefusalTook{0};
    std::optional<NanoOutcome::Kind> nano;
    std::string nanoPath;
};

bool run(uint64_t seed, Scenario scenario, mib::test::Watchdog& watchdog)
{
    auto rig = makeRig(seed, scenario);
    serialsim::install(rig);
    std::mt19937_64 rng(seed * 7919u + static_cast<uint64_t>(scenario));
    const auto operatorDelay = std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 700)(rng));

    Result result;
    {
        mib::test::TempDir dir("serial_startup_race");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
        // AppConfigWatcher hands config.json to the backend before the shell
        // starts discovery; this one names the pump bus.
        backend.setLastConfigJson(scenario == Scenario::NoPumpConfig
                                      ? std::string("{}")
                                      : std::string(R"({"pump_sample_com_port": 3, "pump_sheath_com_port": 3})"));

        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 20ms;
        // The stale-holder run releases the port only after a refused probe,
        // which may be late under TSan: leave startup room to retry.
        timing.nanopositionerRetries = scenario == Scenario::StaleNanopositionerHolder ? 10 : 6; // request limit is 10
        timing.nanopositionerRetryDelay = 200ms;
        timing.nanopositionerDeadline = 15000ms;
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

        // The stale process exits once it has visibly refused a probe, so the
        // ordering is forced by a condition, not by timing.
        std::thread staleProcess;
        if (scenario == Scenario::StaleNanopositionerHolder) {
            staleProcess = std::thread([&] {
                const auto deadline = std::chrono::steady_clock::now() + 20s;
                while (std::chrono::steady_clock::now() < deadline) {
                    bool refused = false;
                    for (const auto& e : rig->events(serialsim::Event::Kind::OpenDenied)) refused |= e.port == "COM5";
                    if (refused) break;
                    std::this_thread::sleep_for(10ms);
                }
                rig->releaseStaleHolder("COM5");
            });
        }

        watchdog.mark("startup");
        coordinator.start();
        std::thread operatorThread([&] {
            std::this_thread::sleep_for(operatorDelay);
            auto& pumps = backend.syringePump();
            if (scenario == Scenario::StalePumpBusHolder) {
                const auto t0 = std::chrono::steady_clock::now();
                result.pumpRefusedWhileStale = !pumps.connect(SyringePumpService::PumpId::Sample, 3, 115200, 1);
                result.staleRefusalTook = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0);
                rig->releaseStaleHolder("COM3"); // the operator closes the other program
            }
            result.samplePump = pumps.connect(SyringePumpService::PumpId::Sample, 3, 115200, 1);
            result.sheathPump = pumps.connect(SyringePumpService::PumpId::Sheath, 3, 115200, 2);
            result.generator = backend.pulseGenerator().connect("COM4", 9600, 1);
        });
        operatorThread.join();
        if (staleProcess.joinable()) staleProcess.join();
        watchdog.mark("operator connected");
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, 30s, [&] { return result.nano.has_value(); });
        }
        watchdog.mark("startup outcome");
        // No explicit shutdown(): the destructor must release the ports too,
        // or the next instance finds them held, as in #417.
    }
    watchdog.mark("backend destroyed");

    bool ok = true;
    const auto check = [&](bool cond, const std::string& what) {
        MIB_EXPECT(cond, "seed " + std::to_string(seed) + " [" + name(scenario) + "]: " + what);
        ok = ok && cond;
    };
    check(result.samplePump, "Sample pump connects on COM3");
    check(result.sheathPump, "Sheath pump connects on COM3");
    check(result.generator, "pulse generator connects on COM4");
    check(result.nano == NanoOutcome::Kind::Connected, "startup connects the nanopositioner");
    check(serialsim::Rig::key(result.nanoPath) == "COM5", "the nanopositioner is the one on COM5");

    int inProcessContention = 0;
    int staleRefusals = 0;
    for (const auto& e : rig->events()) {
        const bool stale = e.detail.find("another process") != std::string::npos;
        if (e.kind == serialsim::Event::Kind::SharedOpen ||
            (e.kind == serialsim::Event::Kind::OpenDenied && !stale))
            ++inProcessContention;
        if (e.kind == serialsim::Event::Kind::OpenDenied && stale) ++staleRefusals;
    }
    check(inProcessContention == 0, "no two owners in this process wanted one port at once");
    if (scenario != Scenario::NoPumpConfig) {
        int foreignOnPumpBus = 0;
        for (const auto& e : rig->events(serialsim::Event::Kind::ForeignFrame)) foreignOnPumpBus += e.port == "COM3";
        check(foreignOnPumpBus == 0, "the probe never writes onto the reserved pump bus");
    }
    if (scenario == Scenario::StaleNanopositionerHolder) {
        check(staleRefusals > 0, "the stale holder refused the first probes");
    }
    if (scenario == Scenario::StalePumpBusHolder) {
        check(result.pumpRefusedWhileStale, "Connect is refused while another process holds the bus");
        // Generous: busy must surface as an error, not as a hang.
        check(result.staleRefusalTook < 5000ms, "the refusal is prompt");
    }
    for (const char* port : {"COM3", "COM4", "COM5"}) {
        check(rig->holders(port) == 0, std::string("teardown released ") + port);
    }
    if (!ok) {
        std::fprintf(stderr, "seed %llu [%s] (operator at +%lldms) ledger: %s\n",
                     static_cast<unsigned long long>(seed), name(scenario),
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
    int runs = 0;
    int failed = 0;
    for (int i = 0; i < seeds; ++i) {
        const uint64_t seed = firstSeed + static_cast<uint64_t>(i);
        for (const auto scenario : {Scenario::ConfiguredPumpPort, Scenario::NoPumpConfig}) {
            ++runs;
            failed += run(seed, scenario, watchdog) ? 0 : 1;
        }
    }
    // The stale-holder scenarios are about ordering against a fixed event,
    // not jitter: one seed each.
    for (const auto scenario : {Scenario::StaleNanopositionerHolder, Scenario::StalePumpBusHolder}) {
        ++runs;
        failed += run(firstSeed, scenario, watchdog) ? 0 : 1;
    }
    std::printf("serial_startup_race: %d/%d runs clean\n", runs - failed, runs);
    return mib::test::exitCode();
}
