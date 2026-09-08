// capture_lifecycle_test (issue #365)
//
// Deterministic fake-camera coverage of the single-owner acquisition
// lifecycle: start/stop/failure/restart, stop while grabFrame is blocked,
// slow SDK teardown inside stop(), natural worker exit followed by a direct
// restart (the joinable-thread std::terminate hazard), duplicate/rapid calls,
// generation-tagged camera-ready callbacks, and an N-cycle stress loop.
//
// Every wait is bounded by the watchdog; the fake camera uses condition
// variables (no timing sleeps) for its barriers.

#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using backend::services::CaptureFailureKind;
using backend::services::CaptureLifecycleState;
using backend::services::CaptureService;
using backend::services::CaptureStartOutcome;
using mib::test::FakeLifecycleCamera;

namespace {

using Obs = FakeLifecycleCamera::Observations;

// Camera factory that hands the *same* fake instance to CaptureService and
// keeps a raw pointer for the test's barrier controls. The observation
// record is separate so it outlives the camera.
struct Rig {
    std::shared_ptr<Obs> obs = std::make_shared<Obs>();
    FakeLifecycleCamera* camera = nullptr; // valid while CaptureService owns it
    FakeLifecycleCamera::Script script;
    std::vector<std::pair<bool, uint64_t>> readyEvents; // (hasCamera, generation)
    std::mutex eventsMutex;

    CaptureService::CameraFactory factory()
    {
        return [this]() {
            // The observation record outlives each camera and is shared by
            // every camera this factory creates. A new session legitimately
            // admitted after a previous camera was destroyed (e.g. the
            // "slow stop + concurrent start" case on a coarse-timer host)
            // must not inherit the old camera's destroyed flag, or its own
            // calls would be misreported as access-after-destroy.
            obs->destroyed.store(false, std::memory_order_release);
            auto cam = std::make_unique<FakeLifecycleCamera>(script, obs.get());
            camera = cam.get();
            return std::unique_ptr<camera::common::ICamera>(std::move(cam));
        };
    }

    CaptureService::CameraReadyCallback readyCallback()
    {
        return [this](camera::common::ICamera* cam, uint64_t gen) {
            std::lock_guard<std::mutex> lk(eventsMutex);
            readyEvents.emplace_back(cam != nullptr, gen);
        };
    }
};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

int main(int argc, char** argv)
{
    const int stressCycles = argc > 1 ? std::atoi(argv[1]) : 120;
    mib::test::Watchdog wd(30);

    // ---- 1. Happy path: Accepted -> Running with cameraReady, generation 1,
    //         ready callback carries the generation; stop -> Idle. -------------
    {
        wd.mark("happy path");
        Rig rig;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        svc.setCameraReadyCallback(rig.readyCallback());

        const auto before = svc.lifecycleSnapshot();
        MIB_EXPECT(before.state == CaptureLifecycleState::Idle, "starts Idle");
        MIB_EXPECT(before.generation == 0, "generation 0 before first start");
        MIB_EXPECT(!before.cameraReady, "not ready before start");

        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "first start accepted");
        // Acceptance is not readiness.
        const auto accepted = svc.lifecycleSnapshot();
        MIB_EXPECT(accepted.generation == 1, "generation 1 after first start");
        MIB_EXPECT(accepted.state == CaptureLifecycleState::Starting ||
                       accepted.state == CaptureLifecycleState::Running,
                   "Starting or Running right after acceptance");

        const auto reached = svc.waitForState({CaptureLifecycleState::Running},
                                              std::chrono::seconds(5));
        MIB_REQUIRE(reached == CaptureLifecycleState::Running, "reaches Running");
        MIB_EXPECT(svc.lifecycleSnapshot().cameraReady, "cameraReady once Running");
        MIB_EXPECT(svc.isRunning(), "isRunning while Running");
        MIB_REQUIRE(waitFor([&] { return rig.obs->framesDelivered.load() >= 3; },
                            std::chrono::seconds(5)),
                    "frames flow");

        svc.stop();
        const auto stopped = svc.lifecycleSnapshot();
        MIB_EXPECT(stopped.state == CaptureLifecycleState::Idle, "stop -> Idle");
        MIB_EXPECT(!stopped.cameraReady, "not ready after stop");
        MIB_EXPECT(!svc.isRunning(), "not running after stop");
        MIB_EXPECT(rig.obs->destroyed.load(), "camera destroyed by stop");
        MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0, "no access after destroy");
        {
            std::lock_guard<std::mutex> lk(rig.eventsMutex);
            MIB_REQUIRE(rig.readyEvents.size() >= 2, "ready callback fired for start and stop");
            MIB_EXPECT(rig.readyEvents.front().first && rig.readyEvents.front().second == 1,
                       "ready(camera, gen=1) first");
            bool sawNull = false;
            for (auto& e : rig.readyEvents) {
                if (!e.first) {
                    sawNull = true;
                    MIB_EXPECT(e.second == 1, "unbind carries the same generation");
                }
            }
            MIB_EXPECT(sawNull, "ready(nullptr) on stop");
        }
        // Duplicate stop is a no-op.
        svc.stop();
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "duplicate stop idempotent");
    }

    // ---- 2. Duplicate start while active -> AlreadyActive, generation stable.
    {
        wd.mark("duplicate start");
        Rig rig;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_EXPECT(svc.requestStart() == CaptureStartOutcome::AlreadyActive, "duplicate -> AlreadyActive");
        MIB_EXPECT(svc.start(), "bool start() stays true for an active session");
        svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5));
        MIB_EXPECT(svc.requestStart() == CaptureStartOutcome::AlreadyActive, "duplicate while Running");
        MIB_EXPECT(svc.lifecycleSnapshot().generation == 1, "generation unchanged by duplicates");
        MIB_EXPECT(rig.obs->startCalls.load() == 1, "camera started exactly once");
        svc.stop();
    }

    // ---- 3. Camera start failure -> Faulted with CameraStartFailed + the
    //         camera's structured message; direct restart (no stop) works. ----
    {
        wd.mark("start failure");
        Rig rig;
        rig.script.startSucceeds = false;
        rig.script.failureCode = "fake.mono8_rejected";
        rig.script.failureMessage = "ISP output format is not Mono8";
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        svc.setCameraReadyCallback(rig.readyCallback());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        const auto st = svc.waitForState({CaptureLifecycleState::Faulted, CaptureLifecycleState::Idle},
                                         std::chrono::seconds(5));
        MIB_REQUIRE(st == CaptureLifecycleState::Faulted, "start failure -> Faulted");
        const auto snap = svc.lifecycleSnapshot();
        MIB_EXPECT(snap.lastFailure == CaptureFailureKind::CameraStartFailed, "failure kind");
        MIB_EXPECT(snap.lastFailureMessage == "ISP output format is not Mono8",
                   "camera failure message propagated: " + snap.lastFailureMessage);
        MIB_EXPECT(snap.lastFailureGeneration == 1, "failure tagged with generation");
        MIB_EXPECT(!snap.cameraReady, "never ready");
        MIB_EXPECT(!svc.isRunning(), "not running after fault");
        {
            std::lock_guard<std::mutex> lk(rig.eventsMutex);
            for (auto& e : rig.readyEvents) {
                MIB_EXPECT(!e.first, "no ready(camera) callback for a failed start");
            }
        }

        // Direct restart from Faulted: must reap the old thread, not terminate.
        rig.script.startSucceeds = true;
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "restart from Faulted accepted");
        MIB_EXPECT(svc.lifecycleSnapshot().generation == 2, "generation advances on restart");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running,
                    "restart reaches Running");
        MIB_EXPECT(svc.lifecycleSnapshot().lastFailure == CaptureFailureKind::None,
                   "successful camera start clears the failure");
        svc.stop();
    }

    // ---- 4. Natural worker exit (stream ended) -> Faulted; direct start
    //         again; repeat several times (the joinable-thread hazard). -------
    {
        wd.mark("stream ended restart");
        Rig rig;
        rig.script.endStreamAfterFrames = 5;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        for (int i = 1; i <= 5; ++i) {
            wd.mark("stream ended restart loop");
            MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "restart accepted");
            const auto st = svc.waitForState({CaptureLifecycleState::Faulted},
                                             std::chrono::seconds(5));
            MIB_REQUIRE(st == CaptureLifecycleState::Faulted, "natural exit -> Faulted");
            const auto snap = svc.lifecycleSnapshot();
            MIB_EXPECT(snap.lastFailure == CaptureFailureKind::StreamEnded, "StreamEnded kind");
            MIB_EXPECT(snap.generation == static_cast<uint64_t>(i), "generation per cycle");
            MIB_EXPECT(!snap.cameraReady, "not ready after fault");
            MIB_EXPECT(!svc.isRunning(), "isRunning false after fault");
            // Reset the observation counter so the next cycle ends again.
            rig.obs->framesDelivered.store(0);
        }
        svc.stop();
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "stop after fault -> Idle");
    }

    // ---- 5. Stop while grabFrame is blocked in the SDK: bounded stop. ------
    {
        wd.mark("stop while blocked");
        Rig rig;
        rig.script.blockGrab = true;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running,
                    "running");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)),
                    "grab parked in blocking wait");
        const auto t0 = std::chrono::steady_clock::now();
        svc.stop();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        MIB_EXPECT(elapsed < std::chrono::seconds(5), "stop bounded while grab blocked");
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "Idle after stop");
        MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0, "no access after destroy");
    }

    // ---- 6. Slow SDK teardown inside camera->stop(): stop() still serializes
    //         and a start issued from another thread during it is rejected
    //         or queued behind it (never overlapping sessions). --------------
    {
        wd.mark("slow stop + concurrent start");
        Rig rig;
        rig.script.stopDelay = std::chrono::milliseconds(150);
        rig.script.blockGrab = true;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5));
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "blocked");

        std::atomic<int> rejected{0};
        std::atomic<int> accepted{0};
        std::thread stopper([&] { svc.stop(); });
        // Hammer start while stop is in flight.
        for (int i = 0; i < 50; ++i) {
            const auto o = svc.requestStart();
            if (o == CaptureStartOutcome::RejectedStopping) rejected++;
            if (o == CaptureStartOutcome::Accepted) accepted++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stopper.join();
        MIB_EXPECT(accepted.load() <= 1, "at most one new session admitted during a stop");
        // Whatever happened, the service must be in a consistent, stoppable state.
        svc.stop();
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "consistent final state");
        MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0, "no access after destroy");
    }

    // ---- 7. No factory -> RejectedNoFactory (no thread spawned). -----------
    {
        wd.mark("no factory");
        CaptureService svc;
        svc.setCameraFactory({});
        MIB_EXPECT(svc.requestStart() == CaptureStartOutcome::RejectedNoFactory, "no factory rejected");
        MIB_EXPECT(!svc.start(), "bool start false without factory");
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "stays Idle");
    }

    // ---- 8. Destroy the service while running: destructor owns teardown. ---
    {
        wd.mark("destroy while running");
        Rig rig;
        {
            CaptureService svc;
            svc.setCameraFactory(rig.factory());
            MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
            svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5));
        }
        MIB_EXPECT(rig.obs->destroyed.load(), "camera destroyed with service");
        MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0, "no access after destroy");
    }

    // ---- 9. Stress: N cycles mixing clean stops, start failures and natural
    //         exits. Each cycle asserts generation monotonicity and a
    //         consistent terminal state. ------------------------------------
    {
        wd.mark("stress");
        Rig rig;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        svc.setCameraReadyCallback(rig.readyCallback());
        uint64_t lastGen = 0;
        for (int i = 0; i < stressCycles; ++i) {
            wd.mark("stress cycle");
            const int variant = i % 3;
            rig.script = {};
            rig.script.produceInterval = std::chrono::microseconds(50);
            if (variant == 1) rig.script.startSucceeds = false;
            if (variant == 2) rig.script.endStreamAfterFrames = 2;
            rig.obs->framesDelivered.store(0);
            // One observation record across cycles: clear the previous
            // camera's destroyed flag before the factory builds the next one.
            rig.obs->destroyed.store(false);

            const auto outcome = svc.requestStart();
            MIB_REQUIRE(outcome == CaptureStartOutcome::Accepted,
                        "cycle " + std::to_string(i) + " start outcome " +
                            backend::services::toString(outcome));
            const auto gen = svc.lifecycleSnapshot().generation;
            MIB_EXPECT(gen == lastGen + 1, "generation strictly increases");
            lastGen = gen;

            if (variant == 0) {
                MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running},
                                             std::chrono::seconds(5)) ==
                                CaptureLifecycleState::Running,
                            "stress running");
                svc.stop();
                MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "Idle");
            } else {
                MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Faulted},
                                             std::chrono::seconds(5)) ==
                                CaptureLifecycleState::Faulted,
                            "stress faulted");
                // Alternate: half the faulted cycles restart directly, half stop first.
                if (i % 2 == 0) svc.stop();
            }
            MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0, "no access after destroy");
        }
        svc.stop();
        std::fprintf(stderr, "capture_lifecycle: %d stress cycles, final generation %llu\n",
                     stressCycles, static_cast<unsigned long long>(lastGen));
    }

    return mib::test::exitCode();
}
