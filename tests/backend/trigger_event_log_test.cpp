// trigger_event_log_test
//
// TriggerService's canonical pulse record (/trigger_events source):
//   - exactly one TriggerEventRecord per request, with the outcome that
//     actually happened (fired / no camera / set failed / stale / queue full);
//   - request, wake, fire and pulse-done stamps are ordered and always on
//     (no PipelineTimingRecorder needed);
//   - drainEvents() hands records out once, in sequence order;
//   - the buffer is bounded: overflow drops the oldest and is counted;
//   - a hardware-stamped loopback edge pairs with its fired pulse whether it
//     arrives synchronously from inside setTriggerOutput (MockCamera) or
//     later (real grabber event thread), and an edge with no pulse is counted
//     as unpaired instead of attached to the wrong pulse.

#include "backend/camera/mock/MockCamera.h"
#include "backend/services/TriggerService.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

using backend::recording::TriggerEventRecord;
using backend::recording::TriggerOutcome;
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

TargetGroupSignal target(uint64_t frame, uint64_t grabUs = 0)
{
    TargetGroupSignal s;
    s.isTargetGroup = true;
    s.objectId = static_cast<int>(frame * 10);
    s.trackId = static_cast<int>(frame * 100);
    s.frameIndex = frame;
    s.hostTimestampUs = grabUs;
    return s;
}

bool is(const TriggerEventRecord& r, TriggerOutcome o) { return r.outcome == static_cast<uint8_t>(o); }

} // namespace

int main()
{
    mib::test::Watchdog wd(30);

    // ---- 1. Fired record: identity + ordered stamps, drained once. --------
    {
        wd.mark("fired record");
        auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
        FakeLifecycleCamera cam({}, obs.get());
        cam.start();
        TriggerService trig;
        trig.setCamera(&cam, 3);
        trig.start();

        trig.onTargetGroupResult(target(42, 123456));
        MIB_REQUIRE(waitFor([&] { return trig.getTriggerCount() >= 1; }, std::chrono::seconds(5)),
                    "pulse fired");
        MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 1; }, std::chrono::seconds(5)),
                    "record buffered");
        auto events = trig.drainEvents();
        MIB_REQUIRE(events.size() == 1, "one record per request");
        const auto& e = events[0];
        MIB_EXPECT(is(e, TriggerOutcome::Fired), "outcome is Fired");
        MIB_EXPECT(e.frameIndex == 42 && e.grabUs == 123456, "source frame identity carried");
        MIB_EXPECT(e.objectId == 420 && e.trackId == 4200, "object/track identity carried");
        MIB_EXPECT(e.generation == 3, "session generation carried");
        MIB_EXPECT(e.sequence == 1, "sequence starts at 1");
        MIB_EXPECT(e.requestUs > 0 && e.requestUs <= e.wakeUs && e.wakeUs <= e.fireUs &&
                       e.fireUs <= e.pulseDoneUs,
                   "request <= wake <= fire <= done, all stamped without the diagnostics recorder");
        MIB_EXPECT(e.lineEdgeTimestamp == 0 && e.lineEdgeHostUs == 0,
                   "no loopback on a camera that cannot stamp inputs");
        MIB_EXPECT(trig.drainEvents().empty(), "drain hands records out once");
        trig.setCamera(nullptr);
        trig.stop();
    }

    // ---- 2. Every non-fired outcome still produces exactly one record. ----
    {
        wd.mark("outcomes");
        // No camera bound.
        {
            TriggerService trig;
            trig.start();
            trig.onTargetGroupResult(target(1));
            MIB_REQUIRE(waitFor([&] { return trig.getDroppedPulsesNoCameraCount() >= 1; },
                                std::chrono::seconds(5)),
                        "no-camera drop counted");
            // The worker bumps the counter (and logs) before it records the event.
            MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 1; },
                                std::chrono::seconds(5)),
                        "no-camera record buffered");
            auto ev = trig.drainEvents();
            MIB_REQUIRE(ev.size() == 1, "no-camera drop recorded");
            MIB_EXPECT(is(ev[0], TriggerOutcome::DroppedNoCamera), "outcome DroppedNoCamera");
            MIB_EXPECT(ev[0].wakeUs >= ev[0].requestUs && ev[0].fireUs == 0,
                       "woke but never fired");
            trig.stop();
        }
        // setTriggerOutput refused.
        {
            auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
            FakeLifecycleCamera::Script script;
            script.triggerOutputSucceeds = false;
            FakeLifecycleCamera cam(script, obs.get());
            cam.start();
            TriggerService trig;
            trig.setCamera(&cam, 1);
            trig.start();
            trig.onTargetGroupResult(target(2));
            MIB_REQUIRE(waitFor([&] { return trig.getDroppedPulsesSetFailedCount() >= 1; },
                                std::chrono::seconds(5)),
                        "set-failed drop counted");
            // The worker bumps the counter (and logs) before it records the event.
            MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 1; },
                                std::chrono::seconds(5)),
                        "set-failed record buffered");
            auto ev = trig.drainEvents();
            MIB_REQUIRE(ev.size() == 1, "set-failed drop recorded");
            MIB_EXPECT(is(ev[0], TriggerOutcome::DroppedSetFailed), "outcome DroppedSetFailed");
            trig.setCamera(nullptr);
            trig.stop();
        }
        // Queue overflow (thread not started, so nothing dequeues).
        {
            auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
            FakeLifecycleCamera cam({}, obs.get());
            cam.start();
            TriggerService trig;
            trig.setCamera(&cam, 1);
            for (uint64_t i = 0; i < TriggerService::kMaxPendingRequests + 2; ++i) {
                trig.onTargetGroupResult(target(100 + i));
            }
            MIB_EXPECT(trig.getDroppedRequestCount() == 2, "two oldest requests evicted");
            auto ev = trig.drainEvents();
            MIB_REQUIRE(ev.size() == 2, "each evicted request recorded at eviction time");
            MIB_EXPECT(is(ev[0], TriggerOutcome::DroppedQueueFull) &&
                           is(ev[1], TriggerOutcome::DroppedQueueFull),
                       "outcome DroppedQueueFull");
            MIB_EXPECT(ev[0].frameIndex == 100 && ev[1].frameIndex == 101,
                       "the OLDEST requests are the ones evicted");
            MIB_EXPECT(ev[0].wakeUs == 0 && ev[0].fireUs == 0, "never reached the loop");
            // Rebinding is a session boundary: the still-queued requests are
            // cleared and counted as stale. They never dequeued, so they get
            // no record (the record set is "requests that reached the loop or
            // were evicted from it"). A later request fires normally.
            trig.setCamera(&cam, 2);
            MIB_EXPECT(trig.getDroppedStaleRequestCount() == TriggerService::kMaxPendingRequests,
                       "queued requests cleared on rebind");
            trig.start();
            trig.onTargetGroupResult(target(201));
            MIB_REQUIRE(waitFor([&] { return trig.getTriggerCount() >= 1; }, std::chrono::seconds(5)),
                        "post-rebind request fires");
            MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 1; }, std::chrono::seconds(5)),
                        "post-rebind record buffered");
            ev = trig.drainEvents();
            MIB_REQUIRE(ev.size() == 1, "only the dequeued request produced a record");
            MIB_EXPECT(ev[0].frameIndex == 201 && ev[0].generation == 2 &&
                           is(ev[0], TriggerOutcome::Fired),
                       "record is the fired post-rebind request");
            trig.setCamera(nullptr);
            trig.stop();
        }
    }

    // ---- 3. Bounded buffer: overflow drops oldest and is counted. ---------
    {
        wd.mark("bounded buffer");
        TriggerService trig;
        // Unbound + not started: every request is evicted from the pending
        // queue after it fills, producing one record per eviction cheaply.
        const size_t total = TriggerService::kMaxBufferedEvents + 50;
        for (size_t i = 0; i < total + TriggerService::kMaxPendingRequests; ++i) {
            trig.onTargetGroupResult(target(i));
        }
        MIB_EXPECT(trig.bufferedEventCount() == TriggerService::kMaxBufferedEvents,
                   "buffer capped at kMaxBufferedEvents");
        MIB_EXPECT(trig.getDroppedEventCount() == 50, "50 oldest records dropped and counted");
        auto ev = trig.drainEvents();
        MIB_REQUIRE(ev.size() == TriggerService::kMaxBufferedEvents, "drain returns the cap");
        bool ordered = true;
        for (size_t i = 1; i < ev.size(); ++i) {
            if (ev[i].sequence != ev[i - 1].sequence + 1) ordered = false;
        }
        MIB_EXPECT(ordered, "sequence numbers contiguous and increasing");
        MIB_EXPECT(ev.front().sequence == 51, "the dropped records were the oldest");
    }

    // ---- 4. Loopback pairing, synchronous (MockCamera reports the edge from
    //         inside setTriggerOutput, before the fired record exists). -----
    {
        wd.mark("sync loopback");
        mib::test::TempDir td("mib_trigger_loopback");
        camera::mock::MockCameraOptions opts;
        opts.folder = td.path();
        camera::mock::MockCamera cam(opts); // never started: the line does not need frames
        TriggerService trig;
        trig.setCamera(&cam, 5);
        trig.start();
        for (uint64_t i = 0; i < 3; ++i) trig.onTargetGroupResult(target(300 + i));
        MIB_REQUIRE(waitFor([&] { return trig.getTriggerCount() >= 3; }, std::chrono::seconds(5)),
                    "three pulses fired on the mock line");
        MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 3; }, std::chrono::seconds(5)),
                    "three records buffered");
        auto ev = trig.drainEvents();
        MIB_REQUIRE(ev.size() == 3, "three records");
        for (const auto& e : ev) {
            MIB_EXPECT(is(e, TriggerOutcome::Fired), "fired");
            MIB_EXPECT(e.lineEdgeTimestamp != 0, "loopback edge stamped in the frame clock");
            MIB_EXPECT(e.lineEdgeHostUs != 0 && e.lineEdgeHostUs >= e.wakeUs &&
                           e.lineEdgeHostUs <= e.pulseDoneUs,
                       "edge host stamp lies within the pulse window");
        }
        MIB_EXPECT(trig.getUnpairedLineEdgeCount() == 0, "every edge found its pulse");
        MIB_EXPECT(cam.triggerPulseCount() == 3, "mock counted the rising edges");
        trig.setCamera(nullptr);
        trig.stop();
    }

    // ---- 5. Loopback pairing, deferred (edge arrives after the record) and
    //         FIFO order across several pulses; an orphan edge is counted. --
    {
        wd.mark("deferred loopback");
        auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
        FakeLifecycleCamera cam({}, obs.get());
        cam.start();
        TriggerService trig;
        trig.setCamera(&cam, 1);
        trig.start();
        for (uint64_t i = 0; i < 3; ++i) trig.onTargetGroupResult(target(500 + i));
        MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 3; }, std::chrono::seconds(5)),
                    "three fired records");
        camera::common::LineEvent edge;
        edge.line = "TTLIO11";
        edge.rising = true;
        edge.timestamp = 7000;
        edge.hostTimestampUs = 7001;
        trig.onLineEvent(edge);
        edge.timestamp = 8000;
        edge.hostTimestampUs = 8001;
        trig.onLineEvent(edge);
        camera::common::LineEvent falling = edge;
        falling.rising = false;
        falling.timestamp = 8500;
        trig.onLineEvent(falling); // falling edges are ignored
        auto ev = trig.drainEvents();
        MIB_REQUIRE(ev.size() == 3, "three records");
        MIB_EXPECT(ev[0].lineEdgeTimestamp == 7000 && ev[0].lineEdgeHostUs == 7001,
                   "first edge -> oldest pulse");
        MIB_EXPECT(ev[1].lineEdgeTimestamp == 8000, "second edge -> second pulse (FIFO)");
        MIB_EXPECT(ev[2].lineEdgeTimestamp == 0, "third pulse still unpaired");
        MIB_EXPECT(trig.getUnpairedLineEdgeCount() == 0, "no orphan yet");

        // The third pulse's record was drained before its edge arrived, so
        // that edge has nothing to attach to. Orphan edges are held briefly
        // (for the synchronous-loopback ordering) in a bounded slot; what
        // overflows the slot, and whatever is still held at a session
        // boundary, is counted as unpaired — never attached to a later pulse
        // of another session.
        for (int i = 0; i < 12; ++i) {
            edge.timestamp = 9000 + static_cast<uint64_t>(i);
            trig.onLineEvent(edge); // 12 orphan edges, hold capacity is 8
        }
        MIB_EXPECT(trig.getUnpairedLineEdgeCount() == 4, "held edges beyond capacity are counted");
        // A pulse fired now must not adopt those stale held edges: their host
        // stamps (8001) predate its wake stamp, so they are discarded and
        // counted and the pulse stays unpaired.
        trig.onTargetGroupResult(target(503));
        MIB_REQUIRE(waitFor([&] { return trig.bufferedEventCount() >= 1; }, std::chrono::seconds(5)),
                    "fourth pulse recorded");
        ev = trig.drainEvents();
        MIB_REQUIRE(ev.size() == 1, "one new record");
        MIB_EXPECT(ev[0].lineEdgeTimestamp == 0, "stale held edges never attach to a later pulse");
        MIB_EXPECT(trig.getUnpairedLineEdgeCount() == 12, "the 8 held stale edges were counted");
        trig.setCamera(nullptr); // session boundary discards any remaining hold
        MIB_EXPECT(trig.getUnpairedLineEdgeCount() == 12, "nothing left to discard");
        trig.stop();
    }

    return mib::test::exitCode();
}
