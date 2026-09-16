// startup_discovery_policy_test (issue #419)
//
// The selection/connection policy is separate from detection:
//  - decideCamera / decideNanopositioner are pure functions over a snapshot
//    that refuse partial/cancelled/failed/overflowed results, ignore synthetic
//    and framegrabber entries, and require user selection for more than one
//    identified device even when a saved preference matches one of them;
//  - StartupDiscoveryCoordinator runs camera-then-nanopositioner with the
//    documented delay/retry defaults, calls the existing selection/connection
//    hooks exactly once, refuses duplicate steps, drops stale results, and
//    performs no action after stop().
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/discovery/StartupDiscoveryPolicy.h"
#include "support/assert.h"
#include "support/fake_discovery_providers.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace backend::discovery;
using mib::test::makeCandidate;
using mib::test::ScriptedProvider;
using namespace std::chrono_literals;

namespace {

DiscoverySnapshot completedSnapshot(std::vector<DiscoveredDevice> candidates)
{
    DiscoverySnapshot s;
    s.jobId = 1;
    s.state = JobState::Completed;
    s.complete = true;
    s.candidates = std::move(candidates);
    return s;
}

DiscoveredDevice nano(const std::string& id, const std::string& path,
                      IdentificationStatus status = IdentificationStatus::Identified)
{
    auto d = makeCandidate(DeviceKind::Nanopositioner, "nanopositioner", id, path, -1, -1, status);
    backend::nanopositioner::Endpoint ep;
    ep.persistentId = id;
    ep.systemPath = path;
    ep.displayName = path;
    d.nanopositioner = ep;
    return d;
}

template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(30);

    // ---- decideCamera --------------------------------------------------------------
    {
        watchdog.mark("decideCamera");
        auto cam = makeCandidate(DeviceKind::Camera, "mindvision", "SN-1");
        auto other = makeCandidate(DeviceKind::Camera, "egrabber", "IF0/DEV0");
        auto grabber = makeCandidate(DeviceKind::Framegrabber, "egrabber-framegrabber", "IF0/DEV0/S0");
        auto mock = makeCandidate(DeviceKind::Camera, "mock", "mock");
        mock.synthetic = true;

        auto d = decideCamera(completedSnapshot({cam}));
        MIB_EXPECT(d.kind == CameraDecision::Kind::SelectUnique && d.device &&
                       d.device->stableIdentity == "SN-1",
                   "one identified physical camera is selected");
        d = decideCamera(completedSnapshot({cam, grabber, mock}));
        MIB_EXPECT(d.kind == CameraDecision::Kind::SelectUnique && d.physicalCount == 1,
                   "framegrabbers and the synthetic mock never inflate the camera count");
        d = decideCamera(completedSnapshot({cam, other}));
        MIB_EXPECT(d.kind == CameraDecision::Kind::RequireSelection && d.physicalCount == 2,
                   "two cameras require user selection");
        d = decideCamera(completedSnapshot({grabber, mock}));
        MIB_EXPECT(d.kind == CameraDecision::Kind::NoneFound, "no physical camera");
        auto unidentified = makeCandidate(DeviceKind::Camera, "mindvision", "", "", 0, -1,
                                          IdentificationStatus::Unidentified,
                                          IdentityStrength::SessionLocal);
        d = decideCamera(completedSnapshot({unidentified}));
        MIB_EXPECT(d.kind == CameraDecision::Kind::NoneFound, "unidentified entries are not selectable");

        auto partial = completedSnapshot({cam});
        partial.complete = false;
        MIB_EXPECT(decideCamera(partial).kind == CameraDecision::Kind::NotDecidable,
                   "incomplete coverage never selects");
        auto overflow = completedSnapshot({cam});
        overflow.overflow = true;
        MIB_EXPECT(decideCamera(overflow).kind == CameraDecision::Kind::NotDecidable,
                   "overflow never looks like a unique match");
        for (auto state : {JobState::Queued, JobState::Running, JobState::Cancelled, JobState::Failed}) {
            auto s = completedSnapshot({cam});
            s.state = state;
            MIB_EXPECT(decideCamera(s).kind == CameraDecision::Kind::NotDecidable,
                       "non-completed jobs never select");
        }
    }

    // ---- decideNanopositioner ------------------------------------------------------
    {
        watchdog.mark("decideNanopositioner");
        auto d = decideNanopositioner(completedSnapshot({nano("np-a", "COM7")}));
        MIB_EXPECT(d.kind == NanopositionerDecision::Kind::ConnectUnique && d.endpoint &&
                       d.endpoint->systemPath == "COM7",
                   "one identified nanopositioner connects");
        d = decideNanopositioner(completedSnapshot(
            {nano("np-a", "COM7"), nano("", "COM8", IdentificationStatus::Unidentified)}));
        MIB_EXPECT(d.kind == NanopositionerDecision::Kind::ConnectUnique,
                   "unidentified adapters do not block a unique identified device");
        d = decideNanopositioner(completedSnapshot({nano("np-a", "COM7"), nano("np-b", "COM8")}),
                                 "np-a");
        MIB_EXPECT(d.kind == NanopositionerDecision::Kind::RequireSelection && d.identifiedCount == 2,
                   "saved preference does not override multiple identified devices");
        d = decideNanopositioner(completedSnapshot(
            {nano("np-a", "COM7"), nano("np-x", "COM9", IdentificationStatus::Ambiguous)}));
        MIB_EXPECT(d.kind == NanopositionerDecision::Kind::RequireSelection,
                   "an ambiguous endpoint forces selection");
        d = decideNanopositioner(completedSnapshot({nano("", "COM8", IdentificationStatus::Unidentified)}));
        MIB_EXPECT(d.kind == NanopositionerDecision::Kind::NotFound, "enumeration only is not found");
        auto cancelled = completedSnapshot({nano("np-a", "COM7")});
        cancelled.state = JobState::Cancelled;
        MIB_EXPECT(decideNanopositioner(cancelled).kind == NanopositionerDecision::Kind::NotDecidable,
                   "cancelled scan never connects");
        auto ordered = orderByPreference({nano("np-b", "COM8"), nano("np-a", "COM7")}, "np-a");
        MIB_EXPECT(ordered.front().stableIdentity == "np-a", "preference reorders only");
    }

    // ---- coordinator: startup sequence ---------------------------------------------
    {
        watchdog.mark("coordinator-sequence");
        DeviceDiscoveryService service;
        auto* cam = new ScriptedProvider("mindvision", DeviceKind::Camera, "camera-sdk");
        auto* np = new ScriptedProvider("nanopositioner", DeviceKind::Nanopositioner);
        cam->result.candidates = {makeCandidate(DeviceKind::Camera, "mindvision", "SN-1")};
        np->script = [](const DiscoveryRequest& req, const ProviderContext& ctx) {
            ProviderResult r;
            MIB_EXPECT(req.preferredNanopositioner && req.preferredNanopositioner->persistentId == "np-pref",
                       "preferred endpoint travels with the request");
            if (ctx.attempt >= 2) r.candidates.push_back(nano("np-a", "COM7"));
            return r;
        };
        service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));
        service.registerProvider(std::unique_ptr<ScriptedProvider>(np));

        std::atomic<int> selectCalls{0}, connectCalls{0};
        std::atomic<bool> configured{false}, connected{false};
        StartupDiscoveryCoordinator::Hooks hooks;
        hooks.cameraConfigured = [&] { return configured.load(); };
        hooks.captureRunning = [] { return false; };
        hooks.nanopositionerConnected = [&] { return connected.load(); };
        hooks.selectCamera = [&](const DiscoveredDevice& d) {
            ++selectCalls;
            configured = true;
            return d.stableIdentity == "SN-1";
        };
        hooks.connectNanopositioner = [&](const backend::nanopositioner::Endpoint& ep) {
            ++connectCalls;
            connected = ep.systemPath == "COM7";
            return connected.load();
        };
        hooks.preferredNanopositioner = [] {
            backend::nanopositioner::Endpoint ep;
            ep.persistentId = "np-pref";
            return std::optional<backend::nanopositioner::Endpoint>(ep);
        };
        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 30ms;
        timing.nanopositionerRetries = 3;
        timing.nanopositionerRetryDelay = 10ms;
        StartupDiscoveryCoordinator coordinator(service, hooks, timing);

        std::mutex m;
        std::vector<StartupDiscoveryCoordinator::CameraOutcome> cameraOutcomes;
        std::vector<StartupDiscoveryCoordinator::NanopositionerOutcome> nanoOutcomes;
        coordinator.setCameraListener([&](const auto& o) {
            std::lock_guard<std::mutex> lk(m);
            cameraOutcomes.push_back(o);
        });
        coordinator.setNanopositionerListener([&](const auto& o) {
            std::lock_guard<std::mutex> lk(m);
            nanoOutcomes.push_back(o);
        });

        const auto t0 = std::chrono::steady_clock::now();
        coordinator.start();
        MIB_EXPECT(coordinator.cameraStepRunning(), "camera step owns a job right after start");
        MIB_EXPECT(!coordinator.runCameraStep(), "manual camera step refused while one is running");
        MIB_REQUIRE(waitFor([&] { return connected.load(); }), "startup connects the nanopositioner");
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        MIB_EXPECT(elapsed >= 30ms, "camera step honours the initial delay");
        MIB_REQUIRE(waitFor([&] { return !coordinator.nanopositionerStepRunning(); }),
                    "nanopositioner step finishes");
        MIB_EXPECT(selectCalls.load() == 1, "camera selected exactly once");
        MIB_EXPECT(connectCalls.load() == 1, "nanopositioner connected exactly once");
        MIB_EXPECT(cam->calls.load() == 1 && np->calls.load() == 2,
                   "camera ran once, nanopositioner retried once before identifying");
        {
            std::lock_guard<std::mutex> lk(m);
            MIB_REQUIRE(!cameraOutcomes.empty(), "camera outcome delivered");
            MIB_EXPECT(cameraOutcomes.back().kind == StartupDiscoveryCoordinator::CameraOutcome::Kind::Selected,
                       "camera outcome is Selected");
            bool sawSearching = false, sawConnected = false;
            for (const auto& o : nanoOutcomes) {
                using K = StartupDiscoveryCoordinator::NanopositionerOutcome::Kind;
                if (o.kind == K::Searching && o.attempt == 2 && o.maxAttempts == 4) sawSearching = true;
                if (o.kind == K::Connected && o.endpoint && o.endpoint->systemPath == "COM7") sawConnected = true;
            }
            MIB_EXPECT(sawSearching, "retry progress reported with attempt counters");
            MIB_EXPECT(sawConnected, "connected outcome carries the endpoint");
        }
        // Once connected, a further nanopositioner step is refused.
        MIB_EXPECT(!coordinator.runNanopositionerStep(), "connected device: step refused");
        coordinator.stop();
    }

    // ---- coordinator: skip camera when configured, multiple devices, stop ------------
    {
        watchdog.mark("coordinator-skip-multi-stop");
        DeviceDiscoveryService service;
        auto* cam = new ScriptedProvider("mindvision", DeviceKind::Camera);
        auto* np = new ScriptedProvider("nanopositioner", DeviceKind::Nanopositioner);
        cam->result.candidates = {makeCandidate(DeviceKind::Camera, "mindvision", "SN-1"),
                                  makeCandidate(DeviceKind::Camera, "mindvision", "SN-2")};
        np->result.candidates = {nano("np-a", "COM7"), nano("np-b", "COM8")};
        service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));
        service.registerProvider(std::unique_ptr<ScriptedProvider>(np));

        std::atomic<int> selectCalls{0}, connectCalls{0};
        StartupDiscoveryCoordinator::Hooks hooks;
        hooks.cameraConfigured = [] { return true; };
        hooks.captureRunning = [] { return false; };
        hooks.nanopositionerConnected = [] { return false; };
        hooks.selectCamera = [&](const DiscoveredDevice&) { ++selectCalls; return true; };
        hooks.connectNanopositioner = [&](const backend::nanopositioner::Endpoint&) { ++connectCalls; return true; };
        hooks.preferredNanopositioner = [] { return std::optional<backend::nanopositioner::Endpoint>{}; };
        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 5ms;
        timing.nanopositionerRetries = 0;
        StartupDiscoveryCoordinator coordinator(service, hooks, timing);
        std::atomic<int> cameraSkipped{0}, nanoRequireSelection{0};
        coordinator.setCameraListener([&](const auto& o) {
            if (o.kind == StartupDiscoveryCoordinator::CameraOutcome::Kind::Skipped) ++cameraSkipped;
        });
        coordinator.setNanopositionerListener([&](const auto& o) {
            if (o.kind == StartupDiscoveryCoordinator::NanopositionerOutcome::Kind::RequireSelection)
                ++nanoRequireSelection;
        });
        coordinator.start();
        MIB_REQUIRE(waitFor([&] { return nanoRequireSelection.load() == 1; }),
                    "configured camera is skipped and the nanopositioner step still runs");
        MIB_EXPECT(cam->calls.load() == 0, "configured camera: no camera enumeration");
        MIB_EXPECT(cameraSkipped.load() == 1, "camera skip reported");
        MIB_EXPECT(connectCalls.load() == 0, "two identified nanopositioners: no auto-connect");

        // Manual camera step now (camera no longer configured): two cameras
        // require selection and no hook fires.
        std::atomic<int> requireSelection{0};
        coordinator.setCameraListener([&](const auto& o) {
            if (o.kind == StartupDiscoveryCoordinator::CameraOutcome::Kind::RequireSelection) ++requireSelection;
        });
        hooks.cameraConfigured = [] { return false; };
        StartupDiscoveryCoordinator manual(service, hooks, timing);
        manual.setCameraListener([&](const auto& o) {
            if (o.kind == StartupDiscoveryCoordinator::CameraOutcome::Kind::RequireSelection) ++requireSelection;
        });
        MIB_EXPECT(manual.runCameraStep(), "manual camera step accepted");
        MIB_REQUIRE(waitFor([&] { return requireSelection.load() == 1; }), "multiple cameras reported");
        MIB_EXPECT(selectCalls.load() == 0, "multiple cameras: no selection hook");
        MIB_REQUIRE(waitFor([&] { return !manual.nanopositionerStepRunning() && !manual.cameraStepRunning(); }),
                    "manual chain finishes");

        // stop() before a blocked probe completes: no hooks, no listener calls.
        cam->result.candidates = {makeCandidate(DeviceKind::Camera, "mindvision", "SN-1")};
        cam->block = true;
        cam->reset();
        std::atomic<int> lateOutcomes{0};
        StartupDiscoveryCoordinator stopping(service, hooks, timing);
        stopping.setCameraListener([&](const auto&) { ++lateOutcomes; });
        MIB_EXPECT(stopping.runCameraStep(), "blocked camera step accepted");
        MIB_REQUIRE(cam->waitUntilEntered(2000ms), "camera probe blocked");
        stopping.stop();
        MIB_EXPECT(stopping.isStopped(), "coordinator stopped");
        MIB_REQUIRE(waitFor([&] { return !stopping.cameraStepRunning(); }), "stop cancels the owned job");
        cam->release();
        std::this_thread::sleep_for(50ms);
        MIB_EXPECT(selectCalls.load() == 0, "no selection after stop");
        MIB_EXPECT(lateOutcomes.load() == 0, "no listener calls after stop");
        MIB_EXPECT(!stopping.runCameraStep() && !stopping.runNanopositionerStep(),
                   "a stopped coordinator refuses new steps");
        MIB_EXPECT(service.activeWorkerCount() == 0 || waitFor([&] { return service.activeWorkerCount() == 0; }),
                   "cancelled worker drains");
    }

    // ---- coordinator: executor runs actions where the shell wants them ----------------
    {
        watchdog.mark("executor");
        DeviceDiscoveryService service;
        auto* cam = new ScriptedProvider("mindvision", DeviceKind::Camera);
        cam->result.candidates = {makeCandidate(DeviceKind::Camera, "mindvision", "SN-1")};
        service.registerProvider(std::unique_ptr<ScriptedProvider>(cam));
        std::mutex qm;
        std::vector<std::function<void()>> queue;
        std::thread::id selectThread;
        StartupDiscoveryCoordinator::Hooks hooks;
        hooks.cameraConfigured = [] { return false; };
        hooks.captureRunning = [] { return false; };
        hooks.nanopositionerConnected = [] { return true; };
        hooks.selectCamera = [&](const DiscoveredDevice&) { selectThread = std::this_thread::get_id(); return true; };
        hooks.connectNanopositioner = [](const backend::nanopositioner::Endpoint&) { return false; };
        hooks.preferredNanopositioner = [] { return std::optional<backend::nanopositioner::Endpoint>{}; };
        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 0ms;
        StartupDiscoveryCoordinator coordinator(service, hooks, timing);
        coordinator.setExecutor([&](std::function<void()> fn) {
            std::lock_guard<std::mutex> lk(qm);
            queue.push_back(std::move(fn));
        });
        MIB_EXPECT(coordinator.runCameraStep(), "camera step accepted");
        MIB_REQUIRE(waitFor([&] { std::lock_guard<std::mutex> lk(qm); return !queue.empty(); }),
                    "decision action posted to the executor");
        MIB_EXPECT(selectThread == std::thread::id{}, "no hook ran on the worker thread");
        std::vector<std::function<void()>> drained;
        {
            std::lock_guard<std::mutex> lk(qm);
            drained.swap(queue);
        }
        for (auto& fn : drained) fn();
        MIB_EXPECT(selectThread == std::this_thread::get_id(), "hook ran on the executor's thread");
        // Actions posted before stop() but executed afterwards are dropped.
        cam->reset();
        coordinator.stop();
        {
            std::lock_guard<std::mutex> lk(qm);
            drained.swap(queue);
            queue.clear();
        }
        selectThread = std::thread::id{};
        for (auto& fn : drained) fn();
        MIB_EXPECT(selectThread == std::thread::id{}, "stale action after stop performs nothing");
    }

    return mib::test::exitCode();
}
