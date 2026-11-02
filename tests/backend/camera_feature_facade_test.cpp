// camera_feature_facade_test (#806 E02 review, findings 1, 2, 3 and 5)
//
// The camera feature commands through the BackendFacade surface the bridge
// calls, with the real AppBackend, TriggerService, ExperimentCoordinator and
// the mock camera's fake node map (evidence fixture/mock), in the default boot
// (the trigger service started and bound with capture). Every gate is driven
// through backend state, never a hand-passed reason:
//   1. with the trigger started and bound and nothing queued, the tree (job and
//      synchronous), a read and a write succeed;
//   2. refused while sorting is possible: an experiment Starting, Active or
//      Stopping, realtime processing running, sort pulses queued, autofocus
//      running; a frame recording refuses writes only; afterwards a write reads
//      back;
//   3. the tree job is in flight once: while its walk is held at the trigger's
//      pulse lock, an identical start joins it (same job, fetch "" while it
//      runs), another request is refused at once, and the held walk completes.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace backend::app {
// Friend of ExperimentCoordinator (same body as the frontend tests' access).
struct ExperimentConfigurationTestAccess {
    static void setState(ExperimentCoordinator& coordinator, ExperimentRunState state) {
        std::lock_guard<std::mutex> lock(coordinator.mutex_);
        coordinator.state_ = state;
    }
};
} // namespace backend::app

namespace {
namespace bridge = backend::bridge;
namespace app = backend::app;
using json = nlohmann::json;

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void startCapture(backend::AppBackend& backendApp) {
    MIB_REQUIRE(backendApp.capture().start(), "mock capture starts");
    MIB_REQUIRE(backendApp.capture().waitForState({backend::services::CaptureLifecycleState::Running},
                                                  std::chrono::seconds(10)) ==
                    backend::services::CaptureLifecycleState::Running,
                "mock capture running");
}

// The tree through the job the bridge uses: start, then fetch until finished.
json treeJob(bridge::BackendFacade& facade, const std::string& request = "{}") {
    const auto id = facade.startCameraFeatureTree(request);
    std::string reply;
    MIB_REQUIRE(waitFor([&] { return !(reply = facade.fetchCameraFeatureTree(id)).empty(); },
                        std::chrono::seconds(20)),
                "the tree job finishes");
    return json::parse(reply);
}

json readReq(const json& session, const std::string& feature) {
    return {{"session", session}, {"module", "remote"}, {"feature", feature}};
}

json writeReq(const json& session, const std::string& feature, const json& value) {
    auto r = readReq(session, feature);
    r["value"] = value;
    return r;
}

bool mentions(const json& reply, const std::string& needle) {
    return reply["reason"].is_string() && reply["reason"].get<std::string>().find(needle) != std::string::npos;
}

} // namespace

int main() {
    mib::test::Watchdog wd(150);
    mib::test::TempDir td("mib_camera_feature_facade");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(120))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", mockDir.string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    // ---- 1. Default boot: the trigger service is started and bound with capture. ----
    unsetEnv("MIB_DISABLED_SERVICES");
    {
        wd.mark("default boot");
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize((td.path() / "gates").string()), "facade initialize");
        startCapture(backendApp);
        MIB_REQUIRE(waitFor([&] { return backendApp.trigger().hasCamera(); }, std::chrono::seconds(10)),
                    "the trigger output is bound to the capture camera");

        // A started, bound trigger with nothing queued is not a refusal: tree, read, write.
        json tree;
        MIB_REQUIRE(waitFor([&] { return (tree = treeJob(facade, R"({"modules":["remote"]})"))["outcome"] == "read"; },
                            std::chrono::seconds(10)),
                    "the tree job reads the node map with the trigger bound: " + tree.dump());
        MIB_EXPECT(json::parse(facade.cameraFeatureTreeJson(R"({"modules":["remote"]})"))["outcome"] == "read",
                   "the synchronous tree reads too");
        MIB_EXPECT(tree["evidence"] == "fixture/mock", "fixture evidence");
        auto session = tree["session"];
        std::string feature;
        json lo, hi;
        for (const auto& f : tree["modules"][0]["features"]) {
            const auto& d = f["descriptor"];
            if (!d.is_null() && d["kind"] == "float" && d["writable"] == true && d["minimum"].is_object() &&
                d["maximum"].is_object()) {
                feature = f["name"];
                lo = d["minimum"]["value"];
                hi = d["maximum"]["value"];
                break;
            }
        }
        MIB_REQUIRE(!feature.empty(), "the fixture has a bounded writable float");
        const json value{{"type", "float"}, {"value", lo.get<double>() + (hi.get<double>() - lo.get<double>()) / 3}};
        auto r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "read", "read with the trigger bound: " + r.dump());
        auto w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "readback" && w["session"]["capability_revision"] == "2",
                   "write reads back with the trigger bound: " + w.dump());
        session = w["session"];

        // Refused while sorting is possible: a run from Starting to Stopping.
        for (const auto state : {app::ExperimentRunState::Starting, app::ExperimentRunState::Active,
                                 app::ExperimentRunState::Stopping}) {
            wd.mark(app::toString(state)); // a static string: the watchdog keeps the pointer
            app::ExperimentConfigurationTestAccess::setState(backendApp.experiment(), state);
            const std::string needle = std::string("experiment is ") + app::toString(state);
            const auto t = treeJob(facade);
            MIB_EXPECT(t["outcome"] == "refused" && mentions(t, needle) && t["modules"].empty(),
                       "tree refused during the run: " + t.dump());
            r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
            MIB_EXPECT(r["outcome"] == "refused" && mentions(r, needle) && r["readback"].is_null(),
                       "read refused during the run: " + r.dump());
            w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
            MIB_EXPECT(w["outcome"] == "refused" && mentions(w, needle) && w["readback"].is_null(),
                       "write refused during the run: " + w.dump());
        }
        app::ExperimentConfigurationTestAccess::setState(backendApp.experiment(), app::ExperimentRunState::Idle);

        // Realtime processing: its target selection feeds the trigger.
        wd.mark("realtime");
        backendApp.processing().startRealtime(backendApp.getFrameStore());
        MIB_REQUIRE(waitFor([&] { return backendApp.processing().isRealtimeRunning(); }, std::chrono::seconds(10)),
                    "realtime processing runs");
        r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "refused" && mentions(r, "realtime processing"),
                   "read refused while realtime processing runs: " + r.dump());
        auto t = treeJob(facade);
        MIB_EXPECT(t["outcome"] == "refused" && mentions(t, "realtime processing"), "tree refused: " + t.dump());
        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "refused" && mentions(w, "realtime processing"), "write refused: " + w.dump());
        backendApp.processing().stopRealtime();
        MIB_REQUIRE(waitFor([&] { return !backendApp.processing().isRealtimeRunning(); }, std::chrono::seconds(10)),
                    "realtime processing stops");

        // Queued sort pulses: the trigger loop is paused so a request stays queued.
        wd.mark("pending pulses");
        auto& trigger = backendApp.trigger();
        trigger.stop();
        backend::services::TargetGroupSignal signal;
        signal.isTargetGroup = true;
        trigger.onTargetGroupResult(signal);
        r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "refused" && mentions(r, "pending"), "read refused while pulses are queued: " + r.dump());
        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "refused" && mentions(w, "pending") && w["readback"].is_null(),
                   "write refused while pulses are queued: " + w.dump());
        const auto fired = trigger.getTriggerCount();
        trigger.start();
        MIB_REQUIRE(waitFor([&] { return trigger.getTriggerCount() > fired; }, std::chrono::seconds(10)),
                    "the queued pulse fires");
        MIB_REQUIRE(waitFor([&] {
                        return json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()))["outcome"] ==
                               "read";
                    },
                    std::chrono::seconds(10)),
                    "the queue drained: reads again");

        wd.mark("autofocus");
        backendApp.autofocus().setEnabled(true);
        r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "refused" && mentions(r, "autofocus"), "read refused with autofocus: " + r.dump());
        backendApp.autofocus().setEnabled(false);

        wd.mark("frame recording");
        std::string why;
        MIB_REQUIRE(backendApp.startFrameRecording((td.path() / "raw.h5").string(), &why),
                    "frame recording starts: " + why);
        MIB_REQUIRE(backendApp.isFrameRecording(), "isFrameRecording");
        r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "read", "reads stay open while recording: " + r.dump());
        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "refused" && mentions(w, "frame recording") && w["session"] == session,
                   "write refused while recording, revision unchanged: " + w.dump());
        backendApp.stopFrameRecording();

        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "readback" && w["session"]["capability_revision"] == "3",
                   "with every gate open the write reads back: " + w.dump());

        // ---- 3. In flight once. ---------------------------------------------
        wd.mark("in flight once");
        std::mutex m;
        std::condition_variable cv;
        bool entered = false, release = false;
        std::thread holder([&] {
            std::string reason;
            backendApp.trigger().runSerializedWithPulses(
                [&] {
                    std::unique_lock<std::mutex> lk(m);
                    entered = true;
                    cv.notify_all();
                    cv.wait(lk, [&] { return release; });
                },
                reason);
        });
        {
            std::unique_lock<std::mutex> lk(m);
            MIB_REQUIRE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return entered; }),
                        "the pulse lock is held");
        }
        const auto first = facade.startCameraFeatureTree("{}");
        MIB_EXPECT(facade.fetchCameraFeatureTree(first).empty(), "the held walk is still running");
        MIB_EXPECT(facade.startCameraFeatureTree("{}") == first, "an identical request joins the walk in flight");
        const auto other = facade.startCameraFeatureTree(R"({"modules":["remote"]})");
        MIB_EXPECT(other != first, "another request gets its own job");
        const auto refused = json::parse(facade.fetchCameraFeatureTree(other));
        MIB_EXPECT(refused["outcome"] == "refused" && mentions(refused, "already running"),
                   "another request while one runs is refused at once: " + refused.dump());
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
        holder.join();
        std::string reply;
        MIB_REQUIRE(waitFor([&] { return !(reply = facade.fetchCameraFeatureTree(first)).empty(); },
                            std::chrono::seconds(20)),
                    "the held walk completes");
        MIB_EXPECT(json::parse(reply)["outcome"] == "read", "the joined walk reads the tree: " + reply);
        const auto expired = json::parse(facade.fetchCameraFeatureTree(first + 1000));
        MIB_EXPECT(expired["outcome"] == "refused", "an unknown job is refused");
        facade.shutdown();
    }
    return mib::test::exitCode();
}
