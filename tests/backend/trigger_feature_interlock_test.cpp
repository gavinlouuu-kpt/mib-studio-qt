// trigger_feature_interlock_test (#806 E02 review, finding 1)
//
// Camera feature I/O must never land between a sort pulse's edges (it would
// stretch the pulse to an SDK call and sort the wrong cell) and is refused
// while the sort trigger is armed or running. TriggerService::runWhileTriggerIdle
// decides and runs under pulseMutex_, the lock the trigger thread holds across
// a whole pulse:
//   1. armed (a camera bound for trigger output) or running: refused with a
//      reason, the feature operation never runs and the port is never touched;
//      idle: it runs;
//   2. contention: a pulse thread binds/starts the trigger, drives pulses and
//      unbinds/stops it in a loop while a feature thread keeps attempting node
//      map operations on the same camera. On one shared timeline no feature
//      operation ever falls between a rising and its falling edge (and no edge
//      falls inside a feature operation); both regimes (refused and ran) occur.

#include "backend/camera/features/FakeNodeMap.h"
#include "backend/camera/features/FeatureSession.h"
#include "backend/services/TriggerService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
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
    return trigger.runWhileTriggerIdle(
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

    // ---- 1. Armed or running: refused before the port; idle: runs. --------
    {
        wd.mark("refusal");
        Timeline timeline;
        InterlockCamera camera(timeline);
        TriggerService trigger;
        std::string reason;
        MIB_EXPECT(featureOp(trigger, camera, timeline, reason) && camera.portCalls == 1,
                   "an idle trigger lets the feature operation run");

        trigger.setCamera(&camera, 7);
        reason.clear();
        MIB_EXPECT(!featureOp(trigger, camera, timeline, reason), "armed: refused");
        MIB_EXPECT(reason.find("armed") != std::string::npos, "the refusal says armed: " + reason);
        MIB_EXPECT(camera.portCalls == 1, "a refused operation never reaches the port");

        trigger.start();
        reason.clear();
        MIB_EXPECT(!featureOp(trigger, camera, timeline, reason) && camera.portCalls == 1,
                   "armed and running: refused");

        trigger.setCamera(nullptr);
        reason.clear();
        MIB_EXPECT(!featureOp(trigger, camera, timeline, reason), "running without a camera: refused");
        MIB_EXPECT(reason.find("running") != std::string::npos, "the refusal says running: " + reason);
        MIB_EXPECT(camera.portCalls == 1, "still no port access");

        trigger.stop();
        reason.clear();
        MIB_EXPECT(featureOp(trigger, camera, timeline, reason) && camera.portCalls == 2,
                   "stopped and unbound: runs again");
    }

    // ---- 2. Contention: no feature operation between a pulse's edges. -----
    {
        wd.mark("contention");
        Timeline timeline;
        InterlockCamera camera(timeline);
        TriggerService trigger;
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
            trigger.setCamera(&camera, static_cast<uint64_t>(cycle + 1));
            trigger.start();
            const auto before = trigger.getTriggerCount();
            for (int p = 0; p < 4; ++p) trigger.onTargetGroupResult(target(++frame));
            MIB_REQUIRE(waitFor([&] { return trigger.getTriggerCount() >= before + 4; },
                                std::chrono::seconds(10)),
                        "the pulses of the cycle fire");
            fired += trigger.getTriggerCount() - before;
            trigger.setCamera(nullptr);
            trigger.stop();
            // An idle window in which feature operations may run.
            const int ranBefore = ran.load();
            waitFor([&] { return ran.load() > ranBefore; }, std::chrono::milliseconds(200));
        }
        done = true;
        feature.join();

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
        MIB_EXPECT(ops == ran.load() && ops > 0, "feature operations ran in the idle windows");
        MIB_EXPECT(refused.load() > 0, "feature operations were refused while the trigger was armed");
    }
    return mib::test::exitCode();
}
