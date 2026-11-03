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
#include "backend/playback/FrameStore.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

    // ---- First-frame observation is distinct from ready and fires once only
    //      after FrameStore has committed a readable frame. ------------------
    //
    // Barrier: FakeLifecycleCamera::releaseGrab() returns only after the
    // blocked grabFrame() consumed the release and returned its frame, and the
    // capture thread handles that frame before it calls grabFrame() again. So
    // "release, then waitUntilGrabBlocked()" means the released frame was fully
    // processed (framesProcessed is bumped after the observation block), with
    // no polling of the very state under test.
    {
        wd.mark("first frame observed");
        Rig rig;
        rig.script.blockGrab = true;
        CaptureService svc;
        auto store = std::make_shared<backend::playback::FrameStore>(8);
        svc.setFrameStore(store);
        std::atomic<int> events{0};
        CaptureService::FirstFrameObservedEvent event;
        svc.setFirstFrameObservedCallback([&](const auto& value) { event = value; ++events; });
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running, "camera ready");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "first grab blocked");
        const auto snap = svc.lifecycleSnapshot();
        MIB_EXPECT(snap.cameraReady && !snap.firstFrameObserved, "ready does not imply observed");
        MIB_EXPECT(snap.firstFrameHostTimeUs == 0 && snap.firstFrameIndex == 0,
                   "unobserved snapshot carries no first-frame metadata");
        MIB_EXPECT(events.load() == 0 && store->committedCount() == 0, "no event before a frame");

        MIB_REQUIRE(rig.camera->releaseGrab(), "first grab released");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "second grab blocked");
        MIB_EXPECT(svc.stats().framesProcessed.load() == 1,
                   "released frame was fully processed before the next grab");
        const auto observed = svc.lifecycleSnapshot();
        MIB_REQUIRE(observed.firstFrameObserved, "first frame observed");
        MIB_REQUIRE(events.load() == 1, "event delivered");
        MIB_EXPECT(event.generation == 1 && event.frameIndex == 0, "event identifies committed frame");
        MIB_EXPECT(event.hostTimeUs != 0 && event.hostTimeUs == observed.firstFrameHostTimeUs &&
                       event.frameIndex == observed.firstFrameIndex,
                   "snapshot carries the event's metadata");
        MIB_EXPECT(store->committedCount() == 1, "publication committed");

        // A duplicate start is a no-op and keeps the latched observation.
        MIB_EXPECT(svc.requestStart() == CaptureStartOutcome::AlreadyActive, "duplicate start");
        const auto dup = svc.lifecycleSnapshot();
        MIB_EXPECT(dup.generation == 1 && dup.firstFrameObserved &&
                       dup.firstFrameHostTimeUs == observed.firstFrameHostTimeUs &&
                       dup.firstFrameIndex == observed.firstFrameIndex,
                   "duplicate start preserves the latched observation");

        MIB_REQUIRE(rig.camera->releaseGrab(), "second grab released");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "third grab blocked");
        MIB_EXPECT(svc.stats().framesProcessed.load() == 2, "second frame was fully processed");
        MIB_EXPECT(store->committedCount() == 2, "second frame committed");
        MIB_EXPECT(events.load() == 1, "event fires exactly once");
        MIB_EXPECT(svc.lifecycleSnapshot().firstFrameIndex == observed.firstFrameIndex,
                   "later frames do not move the latched index");

        svc.stop();
        const auto stopped = svc.lifecycleSnapshot();
        MIB_EXPECT(stopped.state == CaptureLifecycleState::Idle && stopped.firstFrameObserved,
                   "observation is retained after stop");

        // Restart: the reset is synchronous with acceptance, and the new
        // generation can observe again.
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "restart accepted");
        const auto restarted = svc.lifecycleSnapshot();
        MIB_EXPECT(restarted.generation == 2 && !restarted.firstFrameObserved &&
                       restarted.firstFrameHostTimeUs == 0 && restarted.firstFrameIndex == 0,
                   "new generation resets observation");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running, "restart reaches Running");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "restart grab blocked");
        MIB_EXPECT(!svc.lifecycleSnapshot().firstFrameObserved && events.load() == 1,
                   "new generation is not observed before its own frame");
        MIB_REQUIRE(rig.camera->releaseGrab(), "restart grab released");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "restart next grab blocked");
        const auto again = svc.lifecycleSnapshot();
        MIB_EXPECT(again.generation == 2 && again.firstFrameObserved,
                   "new generation observes again");
        MIB_REQUIRE(events.load() == 2, "second generation event delivered");
        // The FrameStore outlives restarts: its index is absolute.
        MIB_EXPECT(event.generation == 2 && event.frameIndex == 2 &&
                       event.frameIndex == store->committedCount() - 1 &&
                       again.firstFrameIndex == event.frameIndex,
                   "second generation event carries the store's absolute index");
        svc.stop();
    }

    // A successful SDK grab is insufficient when configured preview storage
    // cannot commit the frame (zero-capacity store rejects publication).
    {
        wd.mark("preview publication rejected");
        Rig rig;
        rig.script.blockGrab = true;
        CaptureService svc;
        svc.setFrameStore(std::make_shared<backend::playback::FrameStore>(0));
        std::atomic<int> events{0};
        svc.setFirstFrameObservedCallback([&](const auto&) { ++events; });
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running, "ready");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "grab blocked");
        MIB_REQUIRE(rig.camera->releaseGrab(), "grab released");
        MIB_REQUIRE(rig.camera->waitUntilGrabBlocked(std::chrono::seconds(5)), "next grab blocked");
        MIB_EXPECT(svc.stats().framesProcessed.load() == 1,
                   "the frame was processed, so the negative result is not vacuous");
        MIB_EXPECT(!svc.lifecycleSnapshot().firstFrameObserved && events.load() == 0,
                   "uncommitted preview does not produce observation");
        svc.stop();
    }

    // A frame the SDK delivered but that is empty (no payload) or has zero
    // width/height never satisfies observation, with or without a store (a
    // store commits any non-empty payload, so the validity check is what
    // gates the zero-dimension case).
    for (int variant = 0; variant < 2; ++variant) {
        wd.mark("malformed first frame");
        Rig rig;
        rig.script.emptyPayloadFrames = variant == 0;
        rig.script.zeroDimensionFrames = variant == 1;
        CaptureService svc;
        std::shared_ptr<backend::playback::FrameStore> store;
        if (variant == 1) {
            store = std::make_shared<backend::playback::FrameStore>(8);
            svc.setFrameStore(store);
        }
        std::atomic<int> events{0};
        svc.setFirstFrameObservedCallback([&](const auto&) { ++events; });
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running, "ready");
        MIB_REQUIRE(waitFor([&] { return svc.stats().framesProcessed.load() >= 5; },
                            std::chrono::seconds(5)),
                    "malformed frames were processed");
        const auto snap = svc.lifecycleSnapshot();
        MIB_EXPECT(snap.cameraReady && !snap.firstFrameObserved && events.load() == 0 &&
                       snap.firstFrameHostTimeUs == 0 && snap.firstFrameIndex == 0,
                   variant == 0 ? "empty frame does not satisfy observation"
                                : "zero-dimension frame does not satisfy observation");
        if (store) {
            MIB_EXPECT(store->committedCount() > 0, "the store did commit the zero-dimension frames");
        }
        svc.stop();
    }

    // A frame that is mid-delivery when stop() publishes Stopping is not a
    // first frame. The frame callback runs before publication, so it can park
    // the capture thread deterministically while stop() runs elsewhere.
    {
        wd.mark("first frame while stopping");
        struct Gate {
            std::mutex m;
            std::condition_variable cv;
            bool entered = false;
            bool open = false;
        };
        auto gate = std::make_shared<Gate>();
        Rig rig;
        CaptureService svc;
        auto store = std::make_shared<backend::playback::FrameStore>(8);
        svc.setFrameStore(store);
        svc.setFrameCallback([gate](auto&&...) {
            std::unique_lock<std::mutex> lk(gate->m);
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lk, [&] { return gate->open; });
        });
        std::atomic<int> events{0};
        svc.setFirstFrameObservedCallback([&](const auto&) { ++events; });
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        {
            std::unique_lock<std::mutex> lk(gate->m);
            MIB_REQUIRE(gate->cv.wait_for(lk, std::chrono::seconds(5), [&] { return gate->entered; }),
                        "first frame parked in the frame callback");
        }
        std::thread stopper([&] { svc.stop(); });
        MIB_REQUIRE(waitFor([&] { return svc.lifecycleSnapshot().state == CaptureLifecycleState::Stopping; },
                            std::chrono::seconds(5)),
                    "stop() published Stopping");
        {
            std::lock_guard<std::mutex> lk(gate->m);
            gate->open = true;
        }
        gate->cv.notify_all();
        stopper.join();
        MIB_EXPECT(store->committedCount() >= 1, "the in-flight frame was still published");
        MIB_EXPECT(!svc.lifecycleSnapshot().firstFrameObserved && events.load() == 0,
                   "a frame delivered after stop() began is not a first frame");
    }

    // Concurrent snapshot/event accounting: while sessions cycle at full frame
    // rate, a reader never sees a torn first-frame tuple, and every generation
    // that observed delivers exactly one event.
    {
        wd.mark("first frame concurrent accounting");
        Rig rig;
        rig.script.produceInterval = std::chrono::microseconds(50);
        CaptureService svc;
        svc.setFrameStore(std::make_shared<backend::playback::FrameStore>(16));
        std::mutex perGenMutex;
        std::map<uint64_t, int> perGeneration;
        svc.setFirstFrameObservedCallback([&](const auto& value) {
            std::lock_guard<std::mutex> lk(perGenMutex);
            ++perGeneration[value.generation];
        });
        svc.setCameraFactory(rig.factory());
        std::atomic<bool> readerStop{false};
        std::atomic<int> torn{0};
        std::thread reader([&] {
            while (!readerStop.load()) {
                const auto s = svc.lifecycleSnapshot();
                const bool tupleTorn = s.firstFrameObserved
                                           ? s.firstFrameHostTimeUs == 0
                                           : (s.firstFrameHostTimeUs != 0 || s.firstFrameIndex != 0);
                if (tupleTorn) torn.fetch_add(1);
                std::this_thread::yield();
            }
        });
        constexpr int kCycles = 20;
        for (int i = 1; i <= kCycles; ++i) {
            wd.mark("first frame accounting cycle");
            MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "cycle start accepted");
            MIB_REQUIRE(waitFor([&] { return svc.lifecycleSnapshot().firstFrameObserved; },
                                std::chrono::seconds(5)),
                        "cycle observes a first frame");
            MIB_EXPECT(svc.lifecycleSnapshot().generation == static_cast<uint64_t>(i),
                       "observation belongs to this cycle's generation");
            MIB_REQUIRE(waitFor([&] {
                            std::lock_guard<std::mutex> lk(perGenMutex);
                            return perGeneration[static_cast<uint64_t>(i)] >= 1;
                        }, std::chrono::seconds(5)),
                        "cycle event delivered");
            svc.stop();
        }
        readerStop.store(true);
        reader.join();
        MIB_EXPECT(torn.load() == 0, "no torn first-frame snapshot");
        std::lock_guard<std::mutex> lk(perGenMutex);
        MIB_EXPECT(perGeneration.size() == static_cast<size_t>(kCycles), "one generation per cycle");
        for (const auto& entry : perGeneration) {
            MIB_EXPECT(entry.second == 1, "generation " + std::to_string(entry.first) +
                                              " delivered exactly one event");
        }
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

    // A stream can end after camera readiness but before any first frame.
    {
        wd.mark("stream ended before first frame");
        Rig rig;
        rig.script.endStreamBeforeFirstFrame = true;
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        std::atomic<int> events{0};
        svc.setFirstFrameObservedCallback([&](const auto&) { ++events; });
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Faulted}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Faulted, "stream ends naturally");
        const auto snap = svc.lifecycleSnapshot();
        MIB_EXPECT(!snap.firstFrameObserved && events.load() == 0, "no observation before first frame");
        MIB_EXPECT(snap.lastFailure == CaptureFailureKind::StreamEnded, "stream end recorded");
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
        MIB_EXPECT(!svc.lifecycleSnapshot().firstFrameObserved,
                   "stop before first frame leaves observation false");
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
    //     A device-side acquisition-stop failure is still surfaced after the
    //     host camera object has been torn down. This is the same structured
    //     failure code used by the Aravis adapter when AcquisitionStop fails.
    {
        wd.mark("acquisition stop failure");
        Rig rig;
        rig.script.stopFailureCode = "aravis.acquisition_stop";
        rig.script.stopFailureMessage = "fake Aravis AcquisitionStop rejected";
        CaptureService svc;
        svc.setCameraFactory(rig.factory());
        MIB_REQUIRE(svc.requestStart() == CaptureStartOutcome::Accepted, "accepted");
        MIB_REQUIRE(svc.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                        CaptureLifecycleState::Running,
                    "running before acquisition-stop failure");

        svc.stop();
        const auto snap = svc.lifecycleSnapshot();
        MIB_EXPECT(snap.state == CaptureLifecycleState::Idle, "host teardown still reaches Idle");
        MIB_EXPECT(snap.lastFailure == CaptureFailureKind::ShutdownFailed,
                   "Aravis acquisition-stop failure is propagated");
        MIB_EXPECT(snap.lastFailureMessage == "fake Aravis AcquisitionStop rejected",
                   "shutdown failure message is preserved");
        MIB_EXPECT(snap.lastFailureGeneration == 1, "shutdown failure is generation tagged");
        MIB_EXPECT(rig.obs->destroyed.load(), "host camera object is cleaned up after stop failure");
        MIB_EXPECT(rig.obs->accessAfterDestroy.load() == 0,
                   "no camera access occurs after host cleanup");
    }

    // ---- 8. No factory -> RejectedNoFactory (no thread spawned). -----------
    {
        wd.mark("no factory");
        CaptureService svc;
        svc.setCameraFactory({});
        MIB_EXPECT(svc.requestStart() == CaptureStartOutcome::RejectedNoFactory, "no factory rejected");
        MIB_EXPECT(!svc.start(), "bool start false without factory");
        MIB_EXPECT(svc.lifecycleSnapshot().state == CaptureLifecycleState::Idle, "stays Idle");
    }

    // ---- 9. Destroy the service while running: destructor owns teardown. ---
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

    // ---- 10. Stress: N cycles mixing clean stops, start failures and natural
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
