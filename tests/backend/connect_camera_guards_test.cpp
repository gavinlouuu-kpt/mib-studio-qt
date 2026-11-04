// connect_camera_guards_test (#863 CONNECT-013, CONNECT-019/020)
//
// The Connect stage's camera-source and delivery-mode rules at the facade (what the Tauri shell and
// the YOFO Studio server reach):
//  - ConfigureMockCamera is refused while a capture runs (the camera source is never swapped under a
//    live capture), and accepted again once it is stopped;
//  - the delivery mode reads Every Frame by default, accepts Every/Latest Frame, refuses an unknown
//    mode, and says when it applies: at the next capture start while a capture runs (the running
//    capture is never restarted);
//  - the applied-config provenance follows the mode (a run's method records what ran);
//  - a configuration apply sets the mode from camera.frame_delivery_mode (missing = Every Frame).
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/services/CaptureService.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

using backend::bridge::BackendFacade;
using backend::bridge::CameraCommand;
using backend::bridge::CameraCommandAction;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

template <typename Pred>
bool pollUntil(Pred pred, std::chrono::milliseconds timeout)
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
    setEnv("MIB_DISABLED_SERVICES", "auto_update");
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("connect_camera_guards");
    const auto frames = dir.path() / "mock_frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 3, 32, 24), "mock frames written");

    backend::AppBackend backend;
    BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((dir.path() / "data").string()), "facade initializes");

    CameraCommand mock;
    mock.action = CameraCommandAction::ConfigureMockCamera;
    mock.mockFrameDirectory = frames.string();
    mock.mockFrameIntervalMs = 2;
    mock.mockLoopFiles = true;
    MIB_REQUIRE(facade.dispatch(mock).ok, "mock configured while stopped");

    // ---- delivery mode ---------------------------------------------------------------
    {
        const auto mode = json::parse(facade.fetchFrameDeliveryModeJson());
        MIB_EXPECT(mode.at("valid").get<bool>() && mode.at("mode") == "everyFrame" && !mode.at("capturing").get<bool>(),
                   "Every Frame is the default: " + mode.dump());

        const auto bad = json::parse(facade.setFrameDeliveryModeJson("newest"));
        MIB_EXPECT(!bad.at("ok").get<bool>(), "an unknown mode is refused: " + bad.dump());
        MIB_EXPECT(json::parse(facade.fetchFrameDeliveryModeJson()).at("mode") == "everyFrame", "a refusal changes nothing");

        // Provenance: only an applied configuration is touched.
        MIB_EXPECT(backend.getLastConfigJson().empty(), "no applied configuration yet");
        const auto set = json::parse(facade.setFrameDeliveryModeJson("latestFrame"));
        MIB_EXPECT(set.at("ok").get<bool>() && !set.at("applies_at_next_start").get<bool>() &&
                       set.at("message") == "Delivery mode set to Latest Frame.",
                   "stopped: applies at once: " + set.dump());
        MIB_EXPECT(backend.getLastConfigJson().empty(), "no provenance is invented where none was applied");
        MIB_EXPECT(backend.capture().config().deliveryMode == ::camera::common::FrameDeliveryMode::LatestFrame,
                   "the capture service holds the mode");

        backend.setLastConfigJson(R"({"pixel_to_micron_factor":0.5,"camera":{"window":{"width":64}}})");
        MIB_REQUIRE(json::parse(facade.setFrameDeliveryModeJson("everyFrame")).at("ok").get<bool>(), "set Every Frame");
        const auto applied = json::parse(backend.getLastConfigJson());
        MIB_EXPECT(applied.at("camera").at("frame_delivery_mode") == "everyFrame" &&
                       applied.at("camera").at("window").at("width") == 64 && applied.at("pixel_to_micron_factor") == 0.5,
                   "the provenance follows the mode and keeps the rest: " + applied.dump());
    }

    // ---- a loaded profile / config document sets the mode (the control shows it) -------------------
    {
        const auto latest = backend::app::applyConfigDocument(backend, R"({"camera":{"frame_delivery_mode":"latestFrame"}})");
        MIB_REQUIRE(latest.ok, "config applies: " + latest.error);
        MIB_EXPECT(json::parse(facade.fetchFrameDeliveryModeJson()).at("mode") == "latestFrame", "the document's mode is shown");
        const auto none = backend::app::applyConfigDocument(backend, R"({"pixel_to_micron_factor":0.5})");
        MIB_REQUIRE(none.ok, "config applies: " + none.error);
        MIB_EXPECT(json::parse(facade.fetchFrameDeliveryModeJson()).at("mode") == "everyFrame", "a document without the key means Every Frame");
    }

    // ---- a running capture -----------------------------------------------------------
    {
        MIB_REQUIRE(facade.setFrameDeliveryModeJson("latestFrame").size() > 0, "set before start");
        CameraCommand start;
        start.action = CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(start).ok, "capture starts");
        MIB_REQUIRE(pollUntil([&] { return backend.capture().isRunning(); }, 5000ms), "capture runs");

        const auto refused = facade.dispatch(mock);
        MIB_EXPECT(!refused.ok && refused.message.find("camera is running") != std::string::npos,
                   "ConfigureMockCamera is refused while the camera runs: " + refused.message);
        MIB_EXPECT(backend.capture().isRunning(), "the refused command left the capture running");

        const auto set = json::parse(facade.setFrameDeliveryModeJson("everyFrame"));
        MIB_EXPECT(set.at("ok").get<bool>() && set.at("applies_at_next_start").get<bool>() &&
                       set.at("message") == "Delivery mode set to Every Frame \xE2\x80\x94 applies at the next capture start.",
                   "running: applies at the next start: " + set.dump());
        MIB_EXPECT(backend.capture().isRunning(), "a delivery-mode change never restarts the capture");
        const auto mode = json::parse(facade.fetchFrameDeliveryModeJson());
        MIB_EXPECT(mode.at("capturing").get<bool>() && mode.at("mode") == "everyFrame", mode.dump());

        CameraCommand stop;
        stop.action = CameraCommandAction::StopCapture;
        MIB_REQUIRE(facade.dispatch(stop).ok, "capture stops");
        MIB_EXPECT(facade.dispatch(mock).ok, "the mock is configurable again once stopped");
    }

    facade.shutdown();
    return mib::test::exitCode();
}
