// device_discovery_stress_test (issue #419)
//
// AppBackend owns the discovery service and the startup coordinator:
//  - the production providers are registered and guarded (camera enumeration
//    reports Busy while capture runs);
//  - AppBackend::shutdown() drains discovery BEFORE releasing serial hardware
//    (#417 order preserved): a blocked fake job reaches its terminal state
//    before the shared Modbus port closes;
//  - repeated start/cancel/refresh from several threads with a shutdown in
//    flight yields exactly one terminal outcome per accepted job, no worker
//    left behind, no notification after teardown; shutdown is idempotent.
// Watchdog-guarded; no naked joins.
#include "backend/app/AppBackend.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/services/CaptureService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/SerialBus.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/fake_modbus_bench.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace backend::discovery;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    for (const auto& x : v) {
        if (x == s) return true;
    }
    return false;
}

} // namespace

int main()
{
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "yolo,auto_update");
    mib::test::Watchdog watchdog(60);

    // ---- wiring ----------------------------------------------------------------------
    {
        watchdog.mark("wiring");
        mib::test::TempDir dir("discovery_wiring");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
        auto& discovery = backend.deviceDiscovery();
        const auto ids = discovery.providerIds();
        MIB_EXPECT(contains(ids, "mindvision") && contains(ids, "egrabber") &&
                       contains(ids, "egrabber-framegrabber") && contains(ids, "nanopositioner") &&
                       contains(ids, "pulse-generator"),
                   "production providers registered");
        MIB_EXPECT(!backend.startupDiscovery().isStopped(), "startup coordinator ready");

        // Camera guard: a running capture makes camera enumeration report Busy
        // instead of touching an SDK behind a live camera.
        MIB_REQUIRE(backend.capture().start(), "mock capture starts");
        DiscoveryRequest cam;
        cam.kinds = {DeviceKind::Camera};
        cam.providers = {"mindvision"};
        auto job = discovery.startDiscovery(cam);
        MIB_REQUIRE(job.accepted && discovery.waitForTerminal(job.jobId, 10000ms), "camera job ends");
        auto snap = discovery.discoverySnapshot(job.jobId);
        bool busy = false;
        for (const auto& e : snap.errors) busy |= e.kind == ErrorKind::Busy;
        MIB_EXPECT(busy && !snap.complete, "camera enumeration is Busy while capturing");
        backend.capture().stop();

        // Explicit pulse-generator scope over the fake bus through the real
        // service graph: read-only, generator found, port released.
        mib::test::FakeModbusBench bench;
        mib::test::FakeModbusWire generator;
        generator.address = 4;
        mib::test::makeGeneratorLike(generator);
        bench.attach("fake-bus", &generator);
        backend.serialBus().setSerialPortFactory([&] { return std::make_unique<mib::test::FakeModbusPort>(bench); });
        DiscoveryRequest pg;
        pg.kinds = {DeviceKind::PulseGenerator};
        pg.serialScope = SerialScanScope{};
        pg.serialScope->portName = "fake-bus";
        pg.serialScope->addressFrom = 1;
        pg.serialScope->addressTo = 6;
        pg.serialScope->perAddressTimeoutMs = 20;
        job = discovery.startDiscovery(pg);
        MIB_REQUIRE(job.accepted && discovery.waitForTerminal(job.jobId, 10000ms), "generator scan ends");
        snap = discovery.discoverySnapshot(job.jobId);
        MIB_EXPECT(snap.state == JobState::Completed && snap.candidates.size() == 1 &&
                       snap.candidates[0].endpoint.busAddress == 4,
                   "generator discovered through AppBackend's service");
        MIB_EXPECT(generator.writeCommands.load() == 0, "no writes through the production wiring");
        MIB_EXPECT(bench.openPorts.load() == 0, "scan released the shared port");
        backend.shutdown();
        MIB_EXPECT(discovery.isShutdown(), "AppBackend::shutdown shuts discovery down");
        MIB_EXPECT(backend.startupDiscovery().isStopped(), "AppBackend::shutdown stops the coordinator");
    }

    // ---- shutdown order: discovery drains before hardware release ------------------
    {
        watchdog.mark("shutdown-order");
        mib::test::TempDir dir("discovery_shutdown");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
        std::mutex m;
        std::vector<std::string> timeline;
        auto record = [&](std::string event) {
            std::lock_guard<std::mutex> lk(m);
            timeline.push_back(std::move(event));
        };
        mib::test::FakeModbusBench bench;
        mib::test::FakeModbusWire generator;
        generator.address = 3;
        mib::test::makeGeneratorLike(generator);
        bench.attach("fake-shared", &generator);
        bench.onClose = [&](const std::string& name) { record("port-closed:" + name); };
        backend.serialBus().setSerialPortFactory([&] { return std::make_unique<mib::test::FakeModbusPort>(bench); });
        MIB_REQUIRE(backend.pulseGenerator().connect("fake-shared", 9600, 3), "generator connects");

        auto* blocker = new ScriptedProvider("blocker", DeviceKind::Nanopositioner);
        blocker->block = true;
        backend.deviceDiscovery().registerProvider(std::unique_ptr<ScriptedProvider>(blocker));
        const auto observer = backend.deviceDiscovery().addObserver([&](const DiscoverySnapshot& s) {
            if (isTerminal(s.state)) record("job-terminal:" + std::to_string(s.jobId));
        });
        DiscoveryRequest req;
        req.kinds = {DeviceKind::Nanopositioner};
        req.providers = {"blocker"};
        auto job = backend.deviceDiscovery().startDiscovery(req);
        MIB_REQUIRE(job.accepted && blocker->waitUntilEntered(2000ms), "probe blocked");

        backend.shutdown();
        MIB_EXPECT(!backend.pulseGenerator().isConnected(), "shutdown released the generator");
        MIB_EXPECT(backend.deviceDiscovery().activeWorkerCount() == 0, "no worker after shutdown");
        MIB_EXPECT(backend.deviceDiscovery().discoverySnapshot(job.jobId).state == JobState::Cancelled,
                   "in-flight job cancelled by shutdown");
        {
            std::lock_guard<std::mutex> lk(m);
            std::size_t terminalAt = timeline.size(), closedAt = timeline.size();
            for (std::size_t i = 0; i < timeline.size(); ++i) {
                if (timeline[i] == "job-terminal:" + std::to_string(job.jobId) && terminalAt == timeline.size()) terminalAt = i;
                if (timeline[i].rfind("port-closed:", 0) == 0 && closedAt == timeline.size()) closedAt = i;
            }
            MIB_EXPECT(terminalAt < timeline.size() && closedAt < timeline.size(),
                       "both the job end and the port release were observed");
            MIB_EXPECT(terminalAt < closedAt, "discovery drained before the serial hardware was released");
        }
        backend.deviceDiscovery().removeObserver(observer);
        backend.shutdown(); // idempotent
    }

    // ---- stress: start/cancel/refresh across threads with shutdown in flight --------
    {
        watchdog.mark("stress");
        mib::test::TempDir dir("discovery_stress");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize(dir.path().string()), "backend initializes");
        auto& discovery = backend.deviceDiscovery();
        auto* fast = new ScriptedProvider("fast", DeviceKind::Nanopositioner);
        fast->result.candidates = {makeCandidate(DeviceKind::Nanopositioner, "fast", "np", "COM7")};
        auto* slow = new ScriptedProvider("slow", DeviceKind::Nanopositioner);
        slow->script = [](const DiscoveryRequest&, const ProviderContext& ctx) {
            for (int i = 0; i < 20 && !(ctx.cancelled && ctx.cancelled()); ++i) {
                std::this_thread::sleep_for(1ms);
            }
            ProviderResult r;
            r.complete = !(ctx.cancelled && ctx.cancelled());
            return r;
        };
        discovery.registerProvider(std::unique_ptr<ScriptedProvider>(fast));
        discovery.registerProvider(std::unique_ptr<ScriptedProvider>(slow));

        std::mutex m;
        std::map<std::uint64_t, int> terminals;
        std::atomic<bool> notifiedAfterShutdown{false};
        std::atomic<bool> shutdownDone{false};
        const auto observer = discovery.addObserver([&](const DiscoverySnapshot& s) {
            if (!isTerminal(s.state)) return;
            if (shutdownDone.load()) notifiedAfterShutdown = true;
            std::lock_guard<std::mutex> lk(m);
            ++terminals[s.jobId];
        });

        std::vector<std::uint64_t> accepted;
        std::mutex acceptedMutex;
        std::atomic<int> rejected{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 60; ++i) {
                    DiscoveryRequest req;
                    req.kinds = {DeviceKind::Nanopositioner};
                    req.providers = {(i + t) % 2 ? "fast" : "slow"};
                    req.deadline = std::chrono::milliseconds(1000 + i); // distinct requests
                    auto r = discovery.startDiscovery(req);
                    if (!r.accepted) {
                        ++rejected;
                        std::this_thread::sleep_for(1ms);
                        continue;
                    }
                    if (!r.coalesced) {
                        std::lock_guard<std::mutex> lk(acceptedMutex);
                        accepted.push_back(r.jobId);
                    }
                    if (i % 3 == 0) discovery.cancelDiscovery(r.jobId);
                    if (i % 7 == 0) (void)discovery.discoverySnapshot(r.jobId);
                }
            });
        }
        // Shut the backend down while the producers are still running.
        std::this_thread::sleep_for(30ms);
        watchdog.mark("stress-shutdown");
        backend.shutdown();
        shutdownDone = true;
        watchdog.mark("stress-join");
        for (auto& th : threads) th.join(); // producers only call non-blocking APIs
        MIB_EXPECT(discovery.activeWorkerCount() == 0, "no worker survives shutdown");
        MIB_EXPECT(discovery.startDiscovery(DiscoveryRequest{{DeviceKind::Nanopositioner}}).rejection ==
                       ErrorKind::ShuttingDown,
                   "no job accepted after shutdown");
        {
            std::lock_guard<std::mutex> lk(m);
            std::lock_guard<std::mutex> lk2(acceptedMutex);
            MIB_EXPECT(!accepted.empty(), "some jobs were accepted before shutdown");
            int wrong = 0;
            for (auto id : accepted) {
                const auto it = terminals.find(id);
                const int n = it == terminals.end() ? 0 : it->second;
                // A job may be evicted from the retained list, but its
                // observer notification happened exactly once regardless.
                if (n != 1) ++wrong;
            }
            MIB_EXPECT(wrong == 0, "exactly one terminal notification per accepted job");
            for (auto id : accepted) {
                const auto s = discovery.discoverySnapshot(id);
                MIB_EXPECT(s.jobId == 0 || isTerminal(s.state), "every retained job is terminal");
            }
        }
        MIB_EXPECT(!notifiedAfterShutdown.load(), "no terminal notification after shutdown returned");
        discovery.removeObserver(observer);
        backend.shutdown();
    }

    return mib::test::exitCode();
}
