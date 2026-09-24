// trigger_session_test (issue #365)
//
// TriggerService camera-session ownership:
//   - setCamera(nullptr) waits for an in-flight pulse and after it returns no
//     trigger call reaches the old camera (use-after-lifetime guard);
//   - requests enqueued under generation N are dropped (counted) once
//     generation N+1 is bound — never executed against the new session;
//   - pending requests are cleared on unbind;
//   - the full CaptureService -> TriggerService wiring drives pulses on the
//     live camera and unbinds before the camera object is destroyed.

#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/watchdog.h"

#include <chrono>
#include <memory>
#include <thread>

using backend::services::CaptureLifecycleState;
using backend::services::CaptureService;
using backend::services::CaptureStartOutcome;
using backend::services::TargetGroupSignal;
using backend::services::TriggerService;
using mib::test::FakeLifecycleCamera;

namespace {
bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

TargetGroupSignal target(uint64_t frame)
{
    TargetGroupSignal s;
    s.isTargetGroup = true;
    s.objectId = static_cast<int>(frame);
    s.frameIndex = frame;
    return s;
}
} // namespace

int main()
{
    mib::test::Watchdog wd(30);

    // ---- 1. Pulses reach the bound camera; unbind blocks until the in-flight
    //         pulse ends; afterwards no call lands on the old camera. -------
    {
        wd.mark("unbind waits for pulse");
        auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
        FakeLifecycleCamera::Script script;
        auto cam = std::make_unique<FakeLifecycleCamera>(script, obs.get());
        MIB_REQUIRE(cam->start(), "fake camera starts");

        TriggerService trig;
        // Long pulse so the unbind demonstrably overlaps an in-flight pulse.
        trig.setPulseDurationUs(20'000);
        trig.setCamera(cam.get(), 7);
        MIB_EXPECT(trig.boundGeneration() == 7, "bound generation recorded");
        trig.start();

        trig.onTargetGroupResult(target(1));
        MIB_REQUIRE(waitFor([&] { return obs->pulses.load() >= 1; }, std::chrono::seconds(5)),
                    "rising edge driven on bound camera");
        // Pulse is in its 20 ms busy-wait now (or just finishing). Queue one
        // more request so a pulse may be starting exactly when the unbind
        // lands. The contract: once setCamera(nullptr) RETURNS, the trigger
        // thread never touches the old camera again (calls that completed
        // before the return are legitimate — they ran while it was bound).
        trig.onTargetGroupResult(target(2));
        trig.setCamera(nullptr);
        const int callsAtReturn = obs->triggerCalls.load();
        obs->unbound.store(true);
        // Give the thread time to misbehave if it were going to.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int afterUnbind = obs->triggerCalls.load() - callsAtReturn;
        MIB_EXPECT(afterUnbind == 0, "no trigger call after unbind returned (saw " +
                                         std::to_string(afterUnbind) + ")");
        MIB_EXPECT(obs->triggerCallsAfterUnbind.load() == 0, "no call flagged after unbind");
        MIB_EXPECT(trig.boundGeneration() == 0, "unbound generation is 0");
        cam.reset(); // destroy the camera; the trigger thread must be indifferent
        trig.onTargetGroupResult(target(3));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        MIB_EXPECT(obs->accessAfterDestroy.load() == 0, "no access after camera destroy");
        MIB_EXPECT(trig.getDroppedPulsesNoCameraCount() >= 1, "unbound pulse counted as no-camera drop");
        trig.stop();
    }

    // ---- 2. Stale generation: requests made under session N are refused
    //         once N+1 is bound. --------------------------------------------
    {
        wd.mark("stale generation");
        auto obsA = std::make_shared<FakeLifecycleCamera::Observations>();
        auto obsB = std::make_shared<FakeLifecycleCamera::Observations>();
        FakeLifecycleCamera camA({}, obsA.get());
        FakeLifecycleCamera camB({}, obsB.get());
        camA.start();
        camB.start();

        TriggerService trig;
        trig.setCamera(&camA, 1);
        // Do NOT start the thread yet: requests queue up under generation 1.
        trig.onTargetGroupResult(target(10));
        trig.onTargetGroupResult(target(11));
        trig.onTargetGroupResult(target(12));
        // Rebind to session 2 clears the queue (counted as stale).
        trig.setCamera(&camB, 2);
        MIB_EXPECT(trig.getDroppedStaleRequestCount() == 3, "queued session-1 requests cleared on rebind");
        trig.start();
        trig.onTargetGroupResult(target(20));
        MIB_REQUIRE(waitFor([&] { return obsB->pulses.load() >= 1; }, std::chrono::seconds(5)),
                    "session-2 request fires on camera B");
        MIB_EXPECT(obsA->pulses.load() == 0, "camera A never pulsed");
        MIB_EXPECT(obsA->triggerCalls.load() == 0, "camera A never touched");
        trig.setCamera(nullptr);
        trig.stop();
    }

    // ---- 3. Full wiring through CaptureService: generation-tagged bind on
    //         camera start, unbind before destruction, restart rebinds. ------
    {
        wd.mark("capture wiring");
        auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
        FakeLifecycleCamera::Script script;
        script.produceInterval = std::chrono::microseconds(200);
        FakeLifecycleCamera* live = nullptr;

        TriggerService trig;
        CaptureService cap;
        cap.setCameraFactory([&]() {
            auto c = std::make_unique<FakeLifecycleCamera>(script, obs.get());
            live = c.get();
            return std::unique_ptr<camera::common::ICamera>(std::move(c));
        });
        cap.setCameraReadyCallback([&](camera::common::ICamera* cam, uint64_t gen) {
            trig.setCamera(cam, gen);
            if (cam) trig.start(); else trig.stop();
        });

        for (int cycle = 1; cycle <= 3; ++cycle) {
            wd.mark("capture wiring cycle");
            MIB_REQUIRE(cap.requestStart() == CaptureStartOutcome::Accepted, "start");
            MIB_REQUIRE(cap.waitForState({CaptureLifecycleState::Running}, std::chrono::seconds(5)) ==
                            CaptureLifecycleState::Running,
                        "running");
            MIB_REQUIRE(waitFor([&] { return trig.boundGeneration() == static_cast<uint64_t>(cycle); },
                                std::chrono::seconds(5)),
                        "trigger bound to capture generation " + std::to_string(cycle));
            const int before = obs->pulses.load();
            trig.onTargetGroupResult(target(100 + cycle));
            MIB_REQUIRE(waitFor([&] { return obs->pulses.load() > before; }, std::chrono::seconds(5)),
                        "pulse on live camera");
            (void)live;
            obs->unbound.store(false);
            cap.stop();
            MIB_EXPECT(trig.boundGeneration() == 0, "unbound after capture stop");
            MIB_EXPECT(obs->accessAfterDestroy.load() == 0, "no camera access after destroy");
            obs->destroyed.store(false); // next cycle creates a new camera
        }
    }

    return mib::test::exitCode();
}
