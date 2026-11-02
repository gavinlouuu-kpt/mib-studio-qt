// trigger_feature_interlock_test (#806 E02 review, finding 1)
//
// Camera feature I/O must never land between a sort pulse's edges (it would
// stretch the pulse to an SDK call and sort the wrong cell). Every feature
// operation runs under TriggerService::runSerializedWithPulses, i.e. under
// pulseMutex_, the lock the trigger thread holds across a whole pulse, and is
// refused only while a pulse is pending or imminent:
//   1. idle, bound, or started and bound with nothing queued: it runs; queued
//      pulses or the periodic test generator: refused with a reason, the port
//      is never touched;
//   2. contention: with the trigger started and bound, a pulse thread queues
//      bursts of pulses while a feature thread keeps attempting node-map
//      operations on the same camera. On one shared timeline no feature
//      operation ever falls between a rising and its falling edge (and no edge
//      falls inside a feature operation), every pulse fires, none is dropped.

#include "backend/camera/features/FakeNodeMap.h"
#include "backend/camera/features/FeatureSession.h"
#include "backend/services/TriggerService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using backend::services::TargetGroupSignal;
using backend::services::TriggerService;
namespace features = ::camera::features;

namespace {

// Rising edge 'H', falling edge 'L', feature operation begin 'O' / end 'o'.
struct Timeline {
    std::mutex m;
    std::vector<char> events;
    void push(char c) {
        std::lock_guard<std::mutex> lk(m);
        events.push_back(c);
    }
};

void spinFor(std::chrono::microseconds d) {
    const auto until = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < until) {
    }
}

// A camera with a trigger output and a (fake) GenApi node map behind its own
// port guard, both recording on one timeline.
class InterlockCamera final : public ::camera::common::ICamera, private features::FeatureSession {
public:
    explicit InterlockCamera(Timeline& t) : timeline_(t) {
        features::FakeNode node;
        node.name = "N0";
        node.kind = features::Kind::Integer;
        node.value = std::int64_t{1};
        map_.addFeature(features::Module::Remote, node);
    }
    void applyConfig(const ::camera::common::CameraConfig&) override {}
    bool start() override { return true; }
    void stop() override {}
    bool isRunning() const override { return true; }
    bool grabFrame(::camera::common::Frame&) override { return false; }
    bool pollStats(::camera::common::CameraStats&) const override { return false; }
    void configureTriggerOutput(const std::string&) override {}
    bool setTriggerOutput(bool high) override {
        std::lock_guard<std::mutex> lk(portMutex_);
        timeline_.push(high ? 'H' : 'L');
        return true;
    }
    features::FeatureSession* featureSession() override { return this; }
    std::atomic<int> portCalls{0};

private:
    std::string source() const override { return "interlock"; }
    std::string evidence() const override { return "fixture/mock"; }
    bool withPort(const std::function<void(features::Port&)>& fn, std::string&) override {
        std::lock_guard<std::mutex> lk(portMutex_);
        ++portCalls;
        fn(map_);
        return true;
    }

    Timeline& timeline_;
    features::FakeNodeMap map_;
    std::mutex portMutex_;
};

TargetGroupSignal target(uint64_t frame) {
    TargetGroupSignal s;
    s.isTargetGroup = true;
    s.frameIndex = frame;
    return s;
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

// One feature operation through the interlock: describe a node under the
// camera's port guard, bracketed on the timeline and widened by a short spin.
bool featureOp(TriggerService& trigger, InterlockCamera& camera, Timeline& timeline,
               std::string& reason) {
    return trigger.runSerializedWithPulses(
        [&] {
            std::string why;
            camera.featureSession()->withPort(
                [&](features::Port& port) {
                    timeline.push('O');
                    (void)port.describe({features::Module::Remote, "N0", std::nullopt});
                    spinFor(std::chrono::microseconds(20));
                    timeline.push('o');
                },
                why);
        },
        reason);
}

} // namespace

int main() {
    mib::test::Watchdog wd(90);

    // ---- 1. Refused only while pulses are pending or imminent. ------------
    {
        wd.mark("refusal");
        Timeline timeline;
        InterlockCamera camera(timeline);
        TriggerService trigger;
        std::string reason;
        MIB_EXPECT(featureOp(trigger, camera, timeline, reason) && camera.portCalls == 1,
                   "an idle trigger lets the feature operation run");

        // Bound for trigger output (not started), nothing queued: not a refusal.
        trigger.setCamera(&camera, 7);
        MIB_EXPECT(featureOp(trigger, camera, timeline, reason) && camera.portCalls == 2,
                   "a bound trigger with nothing queued is not a refusal");

        // Queued pulses (the loop is not running, so they stay queued): refused.
        trigger.onTargetGroupResult(target(1));
        reason.clear();
        MIB_EXPECT(!featureOp(trigger, camera, timeline, reason), "queued pulses: refused");
        MIB_EXPECT(reason.find("pending") != std::string::npos, "the refusal says pending: " + reason);
        MIB_EXPECT(camera.portCalls == 2, "a refused operation never reaches the port");

        // Starting the loop drains the queue (the pulse fires); afterwards a started,
        // bound trigger with nothing queued runs the operation.
        trigger.start();
        MIB_REQUIRE(waitFor([&] { return trigger.getTriggerCount() == 1; }, std::chrono::seconds(10)),
                    "the queued pulse fires");
        MIB_REQUIRE(waitFor([&] {
                        std::string r;
                        return featureOp(trigger, camera, timeline, r);
                    },
                    std::chrono::seconds(10)),
                    "started and bound with nothing queued: runs");

        // Periodic test pulses are imminent: refused while on.
        trigger.startPeriodicTest(1000);
        reason.clear();
        MIB_EXPECT(!featureOp(trigger, camera, timeline, reason) &&
                       reason.find("periodic") != std::string::npos,
                   "periodic test pulses: refused, " + reason);
        trigger.stopPeriodicTest();
        MIB_REQUIRE(waitFor([&] {
                        std::string r;
                        return featureOp(trigger, camera, timeline, r);
                    },
                    std::chrono::seconds(10)),
                    "periodic test off: runs again");
        trigger.setCamera(nullptr);
        trigger.stop();
    }

    // ---- 2. Contention: no feature operation between a pulse's edges. -----
    {
        wd.mark("contention");
        Timeline timeline;
        InterlockCamera camera(timeline);
        TriggerService trigger;
        trigger.setCamera(&camera, 1);
        trigger.start();
        std::atomic<bool> done{false};
        std::atomic<int> ran{0}, refused{0};
        std::thread feature([&] {
            while (!done.load()) {
                std::string reason;
                if (featureOp(trigger, camera, timeline, reason))
                    ++ran;
                else
                    ++refused;
            }
        });
        uint64_t frame = 0, fired = 0;
        for (int cycle = 0; cycle < 150; ++cycle) {
            wd.mark("contention cycle");
            const auto before = trigger.getTriggerCount();
            for (int p = 0; p < 4; ++p) trigger.onTargetGroupResult(target(++frame));
            MIB_REQUIRE(waitFor([&] { return trigger.getTriggerCount() >= before + 4; },
                                std::chrono::seconds(10)),
                        "the pulses of the cycle fire");
            fired += trigger.getTriggerCount() - before;
            // An idle window in which feature operations may run.
            const int ranBefore = ran.load();
            waitFor([&] { return ran.load() > ranBefore; }, std::chrono::milliseconds(200));
        }
        done = true;
        feature.join();
        trigger.setCamera(nullptr);
        trigger.stop();

        bool inPulse = false, inOp = false, interleaved = false;
        int rising = 0, ops = 0;
        for (char c : timeline.events) {
            switch (c) {
            case 'H':
                if (inOp) interleaved = true;
                inPulse = true;
                ++rising;
                break;
            case 'L':
                inPulse = false;
                break;
            case 'O':
                if (inPulse) interleaved = true;
                inOp = true;
                ++ops;
                break;
            case 'o':
                inOp = false;
                break;
            }
        }
        MIB_EXPECT(!interleaved, "no feature operation between a pulse's edges, no edge inside one");
        MIB_EXPECT(rising > 0 && static_cast<uint64_t>(rising) == fired, "every pulse is on the timeline");
        MIB_EXPECT(ops == ran.load() && ops > 0, "feature operations ran between bursts");
        MIB_EXPECT(trigger.getDroppedPulseCount() == 0 && trigger.getDroppedRequestCount() == 0,
                   "no pulse was lost to feature I/O");
        std::printf("contention: %d pulses, %d feature ops ran, %d refused while pulses were pending\n",
                    rising, ran.load(), refused.load());
    }
    return mib::test::exitCode();
}
