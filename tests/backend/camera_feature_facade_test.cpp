// camera_feature_facade_test (#806 E02 review, findings 1, 2, 3 and 5)
//
// The camera feature commands through the BackendFacade surface the bridge
// calls, with the real AppBackend, TriggerService, ExperimentCoordinator and
// the mock camera's fake node map (evidence fixture/mock). Every gate is driven
// through backend state, never a hand-passed reason:
//   1. default boot (the sort trigger is bound and running with capture): the
//      tree (synchronous and as a job), a read and a write are all refused with
//      the trigger reason, before the device;
//   2. trigger service disabled: the tree job reads the node map; an experiment
//      Starting, Active or Stopping refuses the tree, reads and writes; a frame
//      recording and autofocus refuse writes only; afterwards a write reads back;
//   3. the tree job is in flight once: while its walk is held at the trigger's
//      pulse lock, an identical start joins it (same job, fetch "" while it
//      runs), another request is refused at once, and the held walk completes.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
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

    // ---- 1. Default boot: the sort trigger is armed with capture. ----------
    {
        wd.mark("trigger armed");
        unsetEnv("MIB_DISABLED_SERVICES");
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize((td.path() / "armed").string()), "facade initialize");
        startCapture(backendApp);
        MIB_REQUIRE(waitFor([&] { return backendApp.trigger().hasCamera(); }, std::chrono::seconds(10)),
                    "the trigger output is bound to the capture camera");

        const auto tree = json::parse(facade.cameraFeatureTreeJson("{}"));
        MIB_EXPECT(tree["outcome"] == "refused" && mentions(tree, "trigger") && tree["modules"].empty(),
                   "tree refused while the trigger is armed: " + tree.dump());
        const auto job = treeJob(facade);
        MIB_EXPECT(job["outcome"] == "refused" && mentions(job, "trigger"),
                   "the tree job is refused too: " + job.dump());
        const json someSession{{"source", "x"}, {"generation", "1"}, {"capability_revision", "1"}};
        const auto read = json::parse(facade.cameraFeatureReadJson(readReq(someSession, "N").dump()));
        MIB_EXPECT(read["outcome"] == "refused" && mentions(read, "trigger") && read["readback"].is_null(),
                   "read refused while the trigger is armed: " + read.dump());
        const auto write = json::parse(facade.cameraFeatureWriteJson(
            writeReq(someSession, "N", {{"type", "float"}, {"value", 1.0}}).dump()));
        MIB_EXPECT(write["outcome"] == "refused" && mentions(write, "trigger") && write["readback"].is_null(),
                   "write refused while the trigger is armed: " + write.dump());
        facade.shutdown();
    }

    // ---- 2-3. Trigger disabled: the run-state gates. -----------------------
    setEnv("MIB_DISABLED_SERVICES", "trigger");
    {
        wd.mark("run-state gates");
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize((td.path() / "gates").string()), "facade initialize");
        startCapture(backendApp);

        json tree;
        MIB_REQUIRE(waitFor([&] { return (tree = treeJob(facade, R"({"modules":["remote"]})"))["outcome"] == "read"; },
                            std::chrono::seconds(10)),
                    "the tree job reads the node map: " + tree.dump());
        MIB_EXPECT(tree["evidence"] == "fixture/mock", "fixture evidence");
        const auto session = tree["session"];
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

        for (const auto state : {app::ExperimentRunState::Starting, app::ExperimentRunState::Active,
                                 app::ExperimentRunState::Stopping}) {
            wd.mark(app::toString(state)); // a static string: the watchdog keeps the pointer
            app::ExperimentConfigurationTestAccess::setState(backendApp.experiment(), state);
            const std::string needle = std::string("experiment is ") + app::toString(state);
            const auto t = treeJob(facade);
            MIB_EXPECT(t["outcome"] == "refused" && mentions(t, needle) && t["modules"].empty(),
                       "tree refused during the run: " + t.dump());
            const auto r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
            MIB_EXPECT(r["outcome"] == "refused" && mentions(r, needle) && r["readback"].is_null(),
                       "read refused during the run: " + r.dump());
            const auto w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
            MIB_EXPECT(w["outcome"] == "refused" && mentions(w, needle) && w["readback"].is_null(),
                       "write refused during the run: " + w.dump());
        }
        app::ExperimentConfigurationTestAccess::setState(backendApp.experiment(), app::ExperimentRunState::Idle);

        wd.mark("frame recording");
        std::string why;
        MIB_REQUIRE(backendApp.startFrameRecording((td.path() / "raw.h5").string(), &why),
                    "frame recording starts: " + why);
        MIB_REQUIRE(backendApp.isFrameRecording(), "isFrameRecording");
        auto r = json::parse(facade.cameraFeatureReadJson(readReq(session, feature).dump()));
        MIB_EXPECT(r["outcome"] == "read", "reads stay open while recording: " + r.dump());
        auto w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "refused" && mentions(w, "frame recording") && w["session"] == session,
                   "write refused while recording, revision unchanged: " + w.dump());
        backendApp.stopFrameRecording();

        wd.mark("autofocus");
        backendApp.autofocus().setEnabled(true);
        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "refused" && mentions(w, "autofocus"), "write refused with autofocus: " + w.dump());
        backendApp.autofocus().setEnabled(false);

        w = json::parse(facade.cameraFeatureWriteJson(writeReq(session, feature, value).dump()));
        MIB_EXPECT(w["outcome"] == "readback" && w["session"]["capability_revision"] == "2",
                   "with every gate open the write reads back: " + w.dump());

        // ---- 3. In flight once. ---------------------------------------------
        wd.mark("in flight once");
        std::mutex m;
        std::condition_variable cv;
        bool entered = false, release = false;
        std::thread holder([&] {
            std::string reason;
            backendApp.trigger().runWhileTriggerIdle(
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
    unsetEnv("MIB_DISABLED_SERVICES");
    return mib::test::exitCode();
}
