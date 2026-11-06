// camera_window_save_test (#863, #846): the eGrabber camera window the Overview places.
//  - the geometry of a selected eGrabber camera is supported, with Qt's steps and sensor, the window
//    of the applied profile (else the Experiment script's) and the last Experiment apply's report;
//  - saving the window snaps it as Qt does, rewrites the Experiment script in place, takes it into
//    the applied profile (in memory and its config.json) and moves the revision the profile store
//    remembers, so a restart restores the edited window instead of refusing a "changed" profile;
//  - what is not safe to rewrite is left alone with the reason: a profile that changed on disk, a
//    configuration without a camera.window, a script that is not there;
//  - the backend refuses the save during an experiment and during a raw recording, before any file is
//    written (the buttons are not the only guard).
// No camera is needed: the eGrabber selection is set through a test-access struct (the SDK is absent
// on this host), and nothing here applies a script to a device.
#include "backend/app/AppBackend.h"
#include "backend/app/CameraWindowSave.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ProfileStore.h"
#include "backend/app/StartupConfiguration.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using ::camera::common::CameraWindow;

namespace backend {
// Friend of AppBackend: selects an eGrabber camera without the SDK and sets what a script apply would.
struct CameraGeometryTestAccess {
    static void selectEgrabber(AppBackend& b, std::optional<AppBackend::CameraScriptRole> role) {
        b.selectedIfIndex_ = 0;
        b.selectedDevIndex_ = 0;
        b.selectedMvCameraIndex_ = -1;
        b.selectedLabel_ = "MC2066";
        std::lock_guard<std::mutex> lock(b.acquisitionProfileMutex_);
        b.lastCameraScriptRole_ = role;
    }
    static void setSensor(AppBackend& b, int w, int h) {
        std::lock_guard<std::mutex> lock(b.acquisitionProfileMutex_);
        b.egrabberSensor_ = {w, h};
    }
    static void setApply(AppBackend& b, const app::CameraWindowApplyReport& report) {
        std::lock_guard<std::mutex> lock(b.acquisitionProfileMutex_);
        b.lastCameraWindowApply_ = report;
    }
};
} // namespace backend

namespace backend::app {
// Friend of ExperimentCoordinator (same definition as tests/frontend/app_config_apply_test.cpp): puts the
// run in a state without starting one.
struct ExperimentConfigurationTestAccess {
    static void setState(ExperimentCoordinator& coordinator, ExperimentRunState state) {
        std::lock_guard<std::mutex> lock(coordinator.mutex_);
        coordinator.state_ = state;
    }
};
} // namespace backend::app

namespace {

// Braced initializers do not survive a macro argument list: build the value in a call.
CameraWindow win(int width, int height, int offsetX, int offsetY) { return CameraWindow{width, height, offsetX, offsetY}; }

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

Json profileCommand(backend::AppBackend& backend, const std::string& base, const Json& request) {
    return Json::parse(backend::app::profileStoreCommand(backend, base, request.dump()));
}

const char* kScript =
    "var g = grabbers[0];\n"
    "g.RemotePort.execute(\"AcquisitionStop\");\n"
    "g.RemotePort.set(\"OffsetY\", 616);\n"
    "g.RemotePort.set(\"OffsetX\", 368);\n"
    "g.RemotePort.set(\"Width\", 1216);\n"
    "g.RemotePort.set(\"Height\", 208);\n"
    "g.DevicePort.set(\"CycleMinimumPeriod\", \"200\");\n";

const char* kConfigWithWindow =
    R"({"pixel_to_micron_factor":0.42,"target_fps":5000,"camera":{"frame_delivery_mode":"everyFrame",)"
    R"("window":{"width":1216,"height":208,"offset_x":368,"offset_y":616,"camera_vendor":"SVS-Vistek GmbH","camera_model":"MC2066"}}})";

// Create and apply a profile; returns its folder.
fs::path applyProfile(backend::AppBackend& backend, const std::string& base, const std::string& name,
                      const std::string& config, const std::string& script) {
    const auto created = profileCommand(backend, base, {{"operation", "create"}, {"name", name}, {"document_json", config}, {"script", script}});
    MIB_REQUIRE(created["ok"] == true, created.dump());
    const auto applied = profileCommand(backend, base, {{"operation", "apply"}, {"name", name}, {"baseline", created["profile"]["revision"]}});
    MIB_REQUIRE(applied["ok"] == true, applied.dump());
    return fs::path(base) / name;
}

std::string revisionOf(backend::AppBackend& backend, const std::string& base, const std::string& name) {
    const auto read = profileCommand(backend, base, {{"operation", "read"}, {"name", name}});
    MIB_REQUIRE(read["ok"] == true, read.dump());
    return read["profile"]["revision"].get<std::string>();
}

} // namespace

int main() {
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("camera_window_save");
    const auto data = td.path() / "data";
    const auto pointer = data / "startup_configuration.json";
    const auto baseDir = td.path() / "profiles";
    const std::string base = baseDir.string();
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");

    wd.mark("geometry before any script");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(data.string()), "backend init");
    {
        const auto g = backend.cameraGeometry();
        MIB_EXPECT(!g.supported, "no camera selected: no window box");
    }
    backend::CameraGeometryTestAccess::selectEgrabber(backend, std::nullopt);
    {
        const auto g = backend.cameraGeometry();
        MIB_EXPECT(g.supported && g.camera == "egrabber", "an eGrabber camera has a window");
        MIB_EXPECT(!g.overview && g.scriptRole.empty(), "no script applied yet");
        MIB_EXPECT(g.sensorWidth == 1920 && g.sensorHeight == 1080, "the sensor is Qt's 1920x1080 until the camera says");
        MIB_EXPECT(g.offsetXIncrement == 16 && g.offsetYIncrement == 4 && g.widthIncrement == 16 && g.heightIncrement == 4 &&
                       g.minWidth == 64 && g.minHeight == 16,
                   "Qt's camera steps");
        MIB_EXPECT(g.windowSource == "none" && g.roiWidth == 0 && !g.windowApplyKnown, "no window, no report");
    }

    wd.mark("window from the script, the role, the sensor");
    const fs::path loose = td.path() / "egrabberConfig.js";
    writeText(loose, kScript);
    backend.setExperimentScriptPath(loose.string());
    backend::CameraGeometryTestAccess::selectEgrabber(backend, backend::AppBackend::CameraScriptRole::Overview);
    backend::CameraGeometryTestAccess::setSensor(backend, 1936, 1216);
    {
        const auto g = backend.cameraGeometry();
        MIB_EXPECT(g.overview && g.scriptRole == "overview", "the last script was the Overview one");
        MIB_EXPECT(g.sensorWidth == 1936 && g.sensorHeight == 1216, "the camera's own sensor");
        MIB_EXPECT(g.windowSource == "script" && g.roiX == 368 && g.roiY == 616 && g.roiWidth == 1216 && g.roiHeight == 208,
                   "no profile window: the Experiment script's");
        MIB_EXPECT(g.experimentScriptPath == loose.string(), "the script path is reported");
    }
    backend::CameraGeometryTestAccess::setSensor(backend, 0, 0);
    backend::CameraGeometryTestAccess::selectEgrabber(backend, backend::AppBackend::CameraScriptRole::Experiment);

    wd.mark("a profile with a window wins, and is what saving edits");
    const auto folder = applyProfile(backend, base, "lab", kConfigWithWindow, kScript);
    const auto script = folder / "egrabberConfig.js";
    {
        backend::app::CameraWindowApplyReport report;
        report.sequence = 1;
        report.requested = true;
        report.applied = true;
        report.profile = *backend.acquisitionProfile().window;
        report.scriptWindow = {512, 96, 704, 500};
        report.scriptWindowKnown = true;
        report.result = report.profile.window;
        report.resultKnown = true;
        backend::CameraGeometryTestAccess::setApply(backend, report);
        backend.setExperimentScriptPath(script.string());
        const auto g = backend.cameraGeometry();
        MIB_EXPECT(!g.overview && g.scriptRole == "experiment", "Experiment role");
        MIB_EXPECT(g.windowSource == "profile" && g.roiX == 368 && g.roiY == 616 && g.roiWidth == 1216 && g.roiHeight == 208,
                   "the profile's camera.window wins over the script");
        MIB_EXPECT(g.windowApplyKnown && g.windowApply.applied && g.windowApply.scriptWindow.width == 512,
                   "the last Experiment apply's report");
    }
    const std::string before = revisionOf(backend, base, "lab");
    const auto saved = backend::app::saveEgrabberWindow(backend, script.string(), {1000, 130, 371, 203});
    MIB_REQUIRE(saved.ok, saved.error);
    MIB_EXPECT(saved.window == win(992, 128, 368, 200), "snapped as Qt snaps it");
    MIB_EXPECT(saved.requested == win(1000, 130, 371, 203), "and the request is kept to say so");
    MIB_EXPECT(saved.scriptUpdated && saved.scriptChanged && saved.scriptMissing.empty(), "all four script lines rewritten");
    MIB_EXPECT(saved.profile.localProfile && saved.profile.configUpdated && saved.profile.memoryUpdated &&
                   saved.profile.reason.empty() && saved.profile.profileName == "lab",
               "the profile followed: " + saved.profile.reason);
    {
        const std::string text = readText(script);
        MIB_EXPECT(text.find("g.RemotePort.set(\"Width\", 992);") != std::string::npos &&
                       text.find("g.RemotePort.set(\"Height\", 128);") != std::string::npos &&
                       text.find("g.RemotePort.set(\"OffsetX\", 368);") != std::string::npos &&
                       text.find("g.RemotePort.set(\"OffsetY\", 200);") != std::string::npos &&
                       text.find("g.DevicePort.set(\"CycleMinimumPeriod\", \"200\");") != std::string::npos,
                   "the script: " + text);
        const auto config = Json::parse(readText(folder / "config.json"));
        MIB_EXPECT(config["camera"]["window"]["width"] == 992 && config["camera"]["window"]["height"] == 128 &&
                       config["camera"]["window"]["offset_x"] == 368 && config["camera"]["window"]["offset_y"] == 200,
                   "config.json camera.window");
        MIB_EXPECT(config["camera"]["window"]["camera_model"] == "MC2066" &&
                       config["camera"]["window"]["camera_vendor"] == "SVS-Vistek GmbH" &&
                       config["pixel_to_micron_factor"] == 0.42 && config["target_fps"] == 5000 &&
                       config["camera"]["frame_delivery_mode"] == "everyFrame",
                   "nothing else in config.json changed: " + config.dump());
        const auto profile = backend.acquisitionProfile();
        MIB_REQUIRE(profile.window.has_value(), "in-memory window");
        MIB_EXPECT(profile.window->window == win(992, 128, 368, 200) &&
                       profile.window->cameraModel == "MC2066" && profile.targetFps == 5000.0,
                   "the in-memory profile keeps its camera and rate");
        MIB_EXPECT(backend.cameraGeometry().roiWidth == 992 && backend.cameraGeometry().windowSource == "profile",
                   "the geometry shows the edited window");
    }
    const std::string after = revisionOf(backend, base, "lab");
    MIB_EXPECT(after != before, "the profile's files changed, so its revision did");
    MIB_EXPECT(Json::parse(readText(baseDir / ".selection.json"))["revision"] == after, ".selection.json follows");
    MIB_EXPECT(Json::parse(readText(pointer))["revision"] == after, "the startup pointer follows");
    {
        const auto runtime = Json::parse(backend.getLastConfigJson());
        MIB_EXPECT(runtime["profile_selection"]["revision"] == after &&
                       runtime["camera"]["window"]["width"] == 992 && runtime["camera"]["window"]["offset_y"] == 200,
                   "the runtime provenance follows: " + runtime.dump());
        const auto active = profileCommand(backend, base, {{"operation", "selection"}});
        MIB_EXPECT(active["active_profile"]["revision"] == after, "the active profile is the edited one");
    }

    wd.mark("saving the same window again changes nothing");
    {
        const auto again = backend::app::saveEgrabberWindow(backend, script.string(), {992, 128, 368, 200});
        MIB_REQUIRE(again.ok, again.error);
        MIB_EXPECT(!again.scriptChanged && !again.profile.configUpdated && again.requested == again.window,
                   "unchanged window: no file is rewritten");
        MIB_EXPECT(revisionOf(backend, base, "lab") == after, "so the revision stays");
    }

    wd.mark("a restart restores the edited window");
    backend.shutdown();
    {
        backend::AppBackend restarted;
        MIB_REQUIRE(restarted.initialize(data.string()), "backend init after restart");
        const auto r = Json::parse(backend::app::restoreStartupConfiguration(restarted, base));
        MIB_REQUIRE(r["ok"] == true && r["restored"] == true && r["kind"] == "profile", r.dump());
        const auto window = restarted.acquisitionProfile().window;
        MIB_REQUIRE(window.has_value(), "the restored profile has its window");
        MIB_EXPECT(window->window == win(992, 128, 368, 200), "the edited window is applied after the restart");
        restarted.shutdown();
    }

    wd.mark("what is not safe to rewrite stays");
    backend::AppBackend second;
    MIB_REQUIRE(second.initialize((td.path() / "data2").string()), "second backend init");
    backend::CameraGeometryTestAccess::selectEgrabber(second, std::nullopt);
    {
        // The profile changed on disk after it was applied: its config.json is not rewritten.
        const auto dir = applyProfile(second, base, "edited", kConfigWithWindow, kScript);
        writeText(dir / "config.json", readText(dir / "config.json") + "\n");
        const auto r = backend::app::saveEgrabberWindow(second, (dir / "egrabberConfig.js").string(), {640, 100, 16, 8});
        MIB_REQUIRE(r.ok, r.error);
        MIB_EXPECT(r.scriptChanged && r.profile.memoryUpdated && !r.profile.configUpdated &&
                       r.profile.reason.find("changed on disk") != std::string::npos,
                   "profile changed on disk: script and session updated, file left alone: " + r.profile.reason);
        MIB_EXPECT(Json::parse(readText(dir / "config.json"))["camera"]["window"]["width"] == 1216, "config.json untouched");
        MIB_EXPECT(second.acquisitionProfile().window->window == win(640, 100, 16, 8), "the session window follows");
    }
    {
        // A configuration without camera.window: the script is the whole story.
        const auto dir = applyProfile(second, base, "plain", R"({"pixel_to_micron_factor":0.5})", kScript);
        const auto r = backend::app::saveEgrabberWindow(second, (dir / "egrabberConfig.js").string(), {512, 96, 704, 500});
        MIB_REQUIRE(r.ok, r.error);
        MIB_EXPECT(r.scriptChanged && !r.profile.configHasWindow && !r.profile.memoryUpdated && !r.profile.configUpdated &&
                       r.profile.reason.find("no camera.window") != std::string::npos,
                   "no camera.window in the profile: only the script: " + r.profile.reason);
        MIB_EXPECT(!second.acquisitionProfile().window.has_value(), "and none appears in memory");
        const auto g = second.cameraGeometry();
        MIB_EXPECT(g.windowSource == "script" && g.roiWidth == 512 && g.roiX == 704 && g.roiY == 500,
                   "the geometry reads the script it was saved to");
        MIB_EXPECT(second.lastExperimentScriptPath() == fs::canonical(dir / "egrabberConfig.js").string(),
                   "the saved script is registered as the Experiment script");
    }
    {
        // The eGrabber geometry names the window of a script that may not be the one a run applies: a config
        // document's ROI larger than it is still pending (as before the geometry existed), not refused.
        const auto dir = applyProfile(second, base, "bigroi", R"({"roi":{"x":0,"y":0,"w":1500,"h":900}})", kScript);
        MIB_EXPECT(second.processing().realtimeRoiPending(), "no frame yet: the ROI waits for the first captured frame");
        (void)dir;
    }
    {
        // A script path that cannot be written is refused before anything changes.
        const auto missing = backend::app::saveEgrabberWindow(second, (td.path() / "nope.js").string(), {512, 96, 0, 0});
        MIB_EXPECT(!missing.ok && missing.error.find("not found") != std::string::npos, "missing script: " + missing.error);
        const auto notJs = backend::app::saveEgrabberWindow(second, (td.path() / "x.txt").string(), {512, 96, 0, 0});
        MIB_EXPECT(!notJs.ok && notJs.error.find(".js") != std::string::npos, "not a .js file: " + notJs.error);
        const auto nowhere = backend::app::saveEgrabberWindow(second, "", {512, 96, 0, 0});
        MIB_EXPECT(!nowhere.ok && nowhere.error.find("nowhere") != std::string::npos,
                   "no script and no profile window: " + nowhere.error);
        const auto bad = backend::app::saveEgrabberWindow(second, loose.string(), {0, 96, 0, 0});
        MIB_EXPECT(!bad.ok, "a window without a width is refused");
        const auto json = Json::parse(backend::app::toJson(missing));
        MIB_EXPECT(json["ok"] == false && json["script"]["path"] == (td.path() / "nope.js").string(), "the reply is JSON with the reason");
    }
    {
        // An empty script path is fine when the profile holds the window.
        backend::app::ProfileCameraWindow w;
        w.window = {1216, 208, 368, 616};
        backend::app::AcquisitionProfile p;
        p.window = w;
        second.setAcquisitionProfile(p);
        second.setLastConfigJson("{}");
        const auto r = backend::app::saveEgrabberWindow(second, "", {1216, 208, 368, 616 + 8});
        MIB_REQUIRE(r.ok, r.error);
        MIB_EXPECT(!r.profile.localProfile && r.profile.memoryUpdated && r.window.offsetY == 624 &&
                       r.profile.reason.find("local profile") != std::string::npos,
                   "a window that is not a local profile's changes this session only: " + r.profile.reason);
        MIB_EXPECT(second.acquisitionProfile().window->window.offsetY == 624, "in memory");
    }
    second.shutdown();

    wd.mark("a profile without camera.window: the script is in its folder, so the revision still follows");
    {
        const auto data4 = td.path() / "data4";
        std::string revision;
        {
            backend::AppBackend fourth;
            MIB_REQUIRE(fourth.initialize(data4.string()), "fourth backend init");
            backend::CameraGeometryTestAccess::selectEgrabber(fourth, std::nullopt);
            const auto dir = applyProfile(fourth, base, "script-only", R"({"pixel_to_micron_factor":0.5})", kScript);
            const std::string applied = revisionOf(fourth, base, "script-only");
            const auto r = backend::app::saveEgrabberWindow(fourth, (dir / "egrabberConfig.js").string(), {800, 120, 160, 40});
            MIB_REQUIRE(r.ok, r.error);
            MIB_EXPECT(r.scriptChanged && r.profile.localProfile && !r.profile.configHasWindow && !r.profile.configUpdated &&
                           !r.profile.memoryUpdated,
                       "only the script changed: " + r.profile.reason);
            revision = revisionOf(fourth, base, "script-only");
            MIB_EXPECT(revision != applied, "the profile's script changed, so its revision did");
            MIB_EXPECT(Json::parse(readText(baseDir / ".selection.json"))["revision"] == revision, ".selection.json follows");
            MIB_EXPECT(Json::parse(readText(data4 / "startup_configuration.json"))["revision"] == revision, "the startup pointer follows");
            const auto runtime = Json::parse(fourth.getLastConfigJson());
            MIB_EXPECT(runtime["profile_selection"]["revision"] == revision, "the runtime provenance follows");
            fourth.shutdown();
        }
        backend::AppBackend restarted;
        MIB_REQUIRE(restarted.initialize(data4.string()), "backend init after restart");
        const auto r = Json::parse(backend::app::restoreStartupConfiguration(restarted, base));
        MIB_EXPECT(r["ok"] == true && r["restored"] == true && r["kind"] == "profile",
                   "the profile whose script the window edit rewrote is restored, not refused as changed: " + r.dump());
        restarted.shutdown();
    }

    wd.mark("before any script: the applied profile's own egrabberConfig.js");
    {
        // The Overview is usually entered first: the box already shows the window the Experiment script
        // of the applied profile would give (Qt reads the script when the tab is built).
        backend::AppBackend third;
        MIB_REQUIRE(third.initialize((td.path() / "data3").string()), "third backend init");
        backend::CameraGeometryTestAccess::selectEgrabber(third, std::nullopt);
        const auto dir = applyProfile(third, base, "fallback", R"({"pixel_to_micron_factor":0.5})", kScript);
        MIB_EXPECT(third.lastExperimentScriptPath().empty(), "nothing was applied or saved yet");
        const auto g = third.cameraGeometry();
        MIB_EXPECT(g.windowSource == "script" && g.roiX == 368 && g.roiY == 616 && g.roiWidth == 1216 && g.roiHeight == 208,
                   "the applied profile's egrabberConfig.js gives the window");
        std::error_code ec;
        MIB_EXPECT(!g.experimentScriptPath.empty() && fs::equivalent(fs::path(g.experimentScriptPath), dir / "egrabberConfig.js", ec),
                   "and the geometry names that script: " + g.experimentScriptPath);
        fs::remove(dir / "egrabberConfig.js");
        const auto gone = third.cameraGeometry();
        MIB_EXPECT(gone.windowSource == "none" && gone.experimentScriptPath.empty(), "no such script: no window and no path");
        third.shutdown();
    }

    wd.mark("an experiment holds the camera: the save is refused before anything is written");
    {
        backend::AppBackend busy;
        MIB_REQUIRE(busy.initialize((td.path() / "data5").string()), "busy backend init");
        backend::CameraGeometryTestAccess::selectEgrabber(busy, std::nullopt);
        const auto dir = applyProfile(busy, base, "busy-run", kConfigWithWindow, kScript);
        const std::string script = readText(dir / "egrabberConfig.js");
        const std::string config = readText(dir / "config.json");
        const std::string revision = revisionOf(busy, base, "busy-run");
        for (const auto state : {backend::app::ExperimentRunState::Starting, backend::app::ExperimentRunState::Active,
                                 backend::app::ExperimentRunState::Stopping}) {
            backend::app::ExperimentConfigurationTestAccess::setState(busy.experiment(), state);
            const auto r = backend::app::saveEgrabberWindow(busy, (dir / "egrabberConfig.js").string(), {800, 120, 160, 40});
            MIB_EXPECT(!r.ok && r.error == "Stop the experiment before editing the camera window",
                       std::string("refused in ") + backend::app::toString(state) + ": " + r.error);
            MIB_EXPECT(readText(dir / "egrabberConfig.js") == script && readText(dir / "config.json") == config &&
                           revisionOf(busy, base, "busy-run") == revision,
                       std::string("nothing written in ") + backend::app::toString(state));
            MIB_EXPECT(busy.acquisitionProfile().window->window == win(1216, 208, 368, 616), "the session window stays");
        }
        backend::app::ExperimentConfigurationTestAccess::setState(busy.experiment(), backend::app::ExperimentRunState::Idle);
        const auto r = backend::app::saveEgrabberWindow(busy, (dir / "egrabberConfig.js").string(), {800, 120, 160, 40});
        MIB_REQUIRE(r.ok, "the same save goes through once the run is idle: " + r.error);
        MIB_EXPECT(readText(dir / "egrabberConfig.js") != script, "and rewrites the script");
        busy.shutdown();
    }

    wd.mark("a raw recording: the save is refused before anything is written");
    {
        using backend::services::CaptureLifecycleState;
        const auto frames = td.path() / "frames";
        MIB_REQUIRE(mib::test::writeFrames(frames, 4, 64, 64), "mock frames");
        setEnv("MIB_CAMERA_MODE", "mock");
        setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
        backend::AppBackend recording;
        MIB_REQUIRE(recording.initialize((td.path() / "data6").string()), "recording backend init");
        const auto dir = applyProfile(recording, base, "busy-raw", kConfigWithWindow, kScript);
        const std::string script = readText(dir / "egrabberConfig.js");
        const std::string config = readText(dir / "config.json");
        const std::string revision = revisionOf(recording, base, "busy-raw");
        MIB_REQUIRE(recording.capture().start(), "mock capture accepted");
        MIB_REQUIRE(recording.capture().waitForState({CaptureLifecycleState::Running, CaptureLifecycleState::Faulted},
                                                     std::chrono::seconds(10)) == CaptureLifecycleState::Running,
                    "mock capture running");
        MIB_REQUIRE(recording.startFrameRecording((td.path() / "raw.h5").string()), "raw recording started");
        MIB_REQUIRE(recording.isFrameRecording(), "raw recording running");
        const auto r = backend::app::saveEgrabberWindow(recording, (dir / "egrabberConfig.js").string(), {800, 120, 160, 40});
        MIB_EXPECT(!r.ok && r.error == "Stop recording before editing the camera window", "refused while recording: " + r.error);
        MIB_EXPECT(readText(dir / "egrabberConfig.js") == script && readText(dir / "config.json") == config &&
                       revisionOf(recording, base, "busy-raw") == revision,
                   "nothing written while recording");
        recording.stopFrameRecording();
        recording.capture().stop();
        MIB_EXPECT(!recording.isFrameRecording() && !recording.capture().isRunning(), "recording and capture stopped");
        const auto after = backend::app::saveEgrabberWindow(recording, (dir / "egrabberConfig.js").string(), {800, 120, 160, 40});
        MIB_REQUIRE(after.ok, "the same save goes through once the recording ended: " + after.error);
        MIB_EXPECT(readText(dir / "egrabberConfig.js") != script, "and rewrites the script");
        recording.shutdown();
        unsetEnv("MIB_CAMERA_MODE");
        unsetEnv("MIB_MOCK_CAMERA_DIR");
    }
    return mib::test::exitCode();
}
