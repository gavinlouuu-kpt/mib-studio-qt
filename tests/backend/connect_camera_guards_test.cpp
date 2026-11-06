// connect_camera_guards_test (#863 CONNECT-013, CONNECT-019/020)
//
// The Connect stage's camera-source and delivery-mode rules at the facade (what the Tauri shell and
// the YOFO Studio server reach):
//  - ConfigureMockCamera is refused while a capture runs (the camera source is never swapped under a
//    live capture) or a run holds the camera (also a Failed run still saving), and accepted again once it is stopped;
//  - the delivery mode reads Every Frame by default, accepts Every/Latest Frame, refuses an unknown
//    mode, and says when it applies: at the next capture start while a capture runs (the running
//    capture is never restarted);
//  - the applied-config provenance follows the mode (a run's method records what ran);
//  - a configuration apply sets the mode from camera.frame_delivery_mode (missing = Every Frame);
//  - (B2) the mock camera is configurable by frame rate in whole microseconds (5000 fps = 200 us, CONNECT-016);
//  - (B2) a Stop whose device teardown is not confirmed is a failed command carrying the camera's message
//    (CONNECT-040); the experiment-phase refusals are in backend_facade_boundary_test.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/services/CaptureService.h"
#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

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

// CaptureService::setConfig / setDeliveryMode / config() run on the command thread while the worker of a
// capture takes its session's copy: a session must never see a half-written or mixed configuration, and
// setDeliveryMode must not reset the buffer sizing (Qt's own combo handler does). TSan cannot run on P5,
// so this is a stress: two writers, a reader and a start/stop cycle on one service for a fixed number of
// sessions; what it proves is that every configuration a session applied, and every one a reader saw, is
// exactly one that was set (a torn read of the three-field struct would show a mix), and that no
// operation deadlocks (the watchdog bounds it). It cannot prove the absence of a data race the way TSan does.
void configUnderConcurrentChange()
{
    using backend::services::CaptureService;
    using camera::common::FrameDeliveryMode;
    const auto fullConfig = [](int parts, int buffers, FrameDeliveryMode mode) {
        CaptureService::Config c;
        c.bufferPartCount = parts;
        c.numBuffers = buffers;
        c.deliveryMode = mode;
        return c;
    };
    const auto isOneOf = [](const CaptureService::Config& c, const std::vector<CaptureService::Config>& allowed) {
        for (const auto& a : allowed)
            if (a.bufferPartCount == c.bufferPartCount && a.numBuffers == c.numBuffers && a.deliveryMode == c.deliveryMode) return true;
        return false;
    };
    const auto cycle = [](CaptureService& service, int sessions) {
        for (int i = 0; i < sessions; ++i) {
            MIB_REQUIRE(service.start(), "session starts");
            MIB_REQUIRE(pollUntil([&] { return service.lifecycleSnapshot().state == backend::services::CaptureLifecycleState::Running; }, 5000ms),
                        "session runs");
            service.stop();
        }
    };

    // Phase 1: whole configurations A and B written alternately; sessions and readers see A or B, never a mix.
    {
        auto obs = std::make_shared<mib::test::FakeLifecycleCamera::Observations>();
        CaptureService service;
        service.setCameraFactory([obs]() -> std::unique_ptr<camera::common::ICamera> {
            return std::make_unique<mib::test::FakeLifecycleCamera>(mib::test::FakeLifecycleCamera::Script{}, obs.get());
        });
        const auto a = fullConfig(1, 8, FrameDeliveryMode::EveryFrame);
        const auto b = fullConfig(2, 16, FrameDeliveryMode::LatestFrame);
        service.setConfig(a);
        std::atomic<bool> done{false};
        std::atomic<int> torn{0};
        std::atomic<long> reads{0};
        std::thread writer([&] {
            for (long i = 0; !done.load(); ++i) service.setConfig(i % 2 ? a : b);
        });
        std::thread reader([&] {
            while (!done.load()) {
                if (!isOneOf(service.config(), {a, b})) torn.fetch_add(1);
                const auto mode = service.activeDeliveryMode(); // reads the configuration between sessions
                if (mode != FrameDeliveryMode::EveryFrame && mode != FrameDeliveryMode::LatestFrame) torn.fetch_add(1);
                reads.fetch_add(1);
            }
        });
        cycle(service, 60);
        done.store(true);
        writer.join();
        reader.join();
        MIB_EXPECT(torn.load() == 0, "a reader saw a configuration that was never set (torn read): " + std::to_string(torn.load()));
        MIB_EXPECT(reads.load() > 0, "the reader ran");
        const auto applied = obs->appliedConfigs();
        MIB_EXPECT(applied.size() == 60, "one configuration per session: " + std::to_string(applied.size()));
        int mixed = 0;
        for (const auto& c : applied) {
            const CaptureService::Config seen = fullConfig(c.bufferPartCount, c.numBuffers, c.deliveryMode);
            if (!isOneOf(seen, {a, b})) ++mixed;
        }
        MIB_EXPECT(mixed == 0, "a session applied a mixed configuration: " + std::to_string(mixed));
    }

    // Phase 2: only the delivery mode flips; the buffer sizing set once survives every flip and every session.
    {
        auto obs = std::make_shared<mib::test::FakeLifecycleCamera::Observations>();
        CaptureService service;
        service.setCameraFactory([obs]() -> std::unique_ptr<camera::common::ICamera> {
            return std::make_unique<mib::test::FakeLifecycleCamera>(mib::test::FakeLifecycleCamera::Script{}, obs.get());
        });
        service.setConfig(fullConfig(3, 33, FrameDeliveryMode::EveryFrame));
        std::atomic<bool> done{false};
        std::thread flipper([&] {
            for (long i = 0; !done.load(); ++i)
                service.setDeliveryMode(i % 2 ? FrameDeliveryMode::EveryFrame : FrameDeliveryMode::LatestFrame);
        });
        cycle(service, 60);
        done.store(true);
        flipper.join();
        int reset = 0, modes[2] = {0, 0};
        const auto applied = obs->appliedConfigs();
        for (const auto& c : applied) {
            if (c.bufferPartCount != 3 || c.numBuffers != 33) ++reset;
            ++modes[c.deliveryMode == FrameDeliveryMode::LatestFrame ? 1 : 0];
        }
        MIB_EXPECT(applied.size() == 60 && reset == 0, "setDeliveryMode left the buffer sizing alone in every session: reset=" + std::to_string(reset));
        MIB_EXPECT(modes[0] > 0 && modes[1] > 0, "both modes reached a session: every=" + std::to_string(modes[0]) + " latest=" + std::to_string(modes[1]));
        MIB_EXPECT(service.config().bufferPartCount == 3 && service.config().numBuffers == 33, "the sizing is still the one set");
    }

    // A change while a session runs applies at the next start: the running session keeps what it started with.
    {
        auto obs = std::make_shared<mib::test::FakeLifecycleCamera::Observations>();
        CaptureService service;
        service.setCameraFactory([obs]() -> std::unique_ptr<camera::common::ICamera> {
            return std::make_unique<mib::test::FakeLifecycleCamera>(mib::test::FakeLifecycleCamera::Script{}, obs.get());
        });
        service.setDeliveryMode(FrameDeliveryMode::EveryFrame);
        MIB_REQUIRE(service.start(), "start");
        MIB_REQUIRE(pollUntil([&] { return service.lifecycleSnapshot().state == backend::services::CaptureLifecycleState::Running; }, 5000ms), "runs");
        service.setDeliveryMode(FrameDeliveryMode::LatestFrame);
        MIB_EXPECT(service.config().deliveryMode == FrameDeliveryMode::LatestFrame, "the configuration changed");
        MIB_EXPECT(obs->appliedConfigs().size() == 1 && obs->appliedConfigs()[0].deliveryMode == FrameDeliveryMode::EveryFrame,
                   "the running session kept the mode it started with");
        MIB_EXPECT(service.activeDeliveryMode() == FrameDeliveryMode::EveryFrame, "the camera-confirmed mode is the running one");
        service.stop();
        MIB_REQUIRE(service.start(), "restart");
        MIB_REQUIRE(pollUntil([&] { return service.lifecycleSnapshot().state == backend::services::CaptureLifecycleState::Running; }, 5000ms), "runs again");
        MIB_EXPECT(obs->appliedConfigs().size() == 2 && obs->appliedConfigs()[1].deliveryMode == FrameDeliveryMode::LatestFrame,
                   "the next start used the new mode");
        service.stop();
    }
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

    configUnderConcurrentChange();

    // The camera source and the delivery mode are held by a run that is starting, running or saving its data, also a
    // Failed run still saving after a fatal save error (Qt flushInProgress_); a Failed run that has finished saving
    // (waiting for its fault to be acknowledged) and an idle one hold nothing.
    {
        using backend::app::ExperimentRunState;
        using backend::app::ExperimentStatus;
        using backend::app::cameraSourceChangeRefusal;
        using backend::app::experimentHoldsCamera;
        const auto status = [](ExperimentRunState state, bool flushing) {
            ExperimentStatus s;
            s.state = state;
            s.flushing = flushing;
            return s;
        };
        MIB_EXPECT(!experimentHoldsCamera(status(ExperimentRunState::Idle, false)) &&
                       !experimentHoldsCamera(status(ExperimentRunState::Failed, false)),
                   "Idle and a saved Failed run hold no camera");
        MIB_EXPECT(experimentHoldsCamera(status(ExperimentRunState::Starting, false)) &&
                       experimentHoldsCamera(status(ExperimentRunState::Active, false)) &&
                       experimentHoldsCamera(status(ExperimentRunState::Stopping, true)),
                   "Starting, Active and Stopping hold the camera");
        MIB_EXPECT(experimentHoldsCamera(status(ExperimentRunState::Failed, true)),
                   "a Failed run still saving its data holds the camera");
        // The camera source: the running capture is named first; a run holding the camera refuses on its own, also
        // when the capture already stopped under it.
        MIB_EXPECT(cameraSourceChangeRefusal(false, status(ExperimentRunState::Idle, false)).empty(), "idle: allowed");
        MIB_EXPECT(cameraSourceChangeRefusal(true, status(ExperimentRunState::Idle, false)) ==
                       "Cannot change camera connection while camera is running. Please stop the camera first.",
                   "a running capture refuses");
        for (const auto& held : {status(ExperimentRunState::Starting, false), status(ExperimentRunState::Active, false),
                                 status(ExperimentRunState::Stopping, true), status(ExperimentRunState::Failed, true)})
            MIB_EXPECT(cameraSourceChangeRefusal(false, held) == "Cannot change camera connection while an experiment is active.",
                       "a run that holds the camera refuses with the capture stopped");
        // Stop Camera (#863 CONNECT-038): Qt's words per state; a Failed run still saving refuses as "being saved".
        using backend::app::cameraStopRefusal;
        MIB_EXPECT(cameraStopRefusal(status(ExperimentRunState::Idle, false)).empty() &&
                       cameraStopRefusal(status(ExperimentRunState::Failed, false)).empty(),
                   "idle or saved: Stop allowed");
        MIB_EXPECT(cameraStopRefusal(status(ExperimentRunState::Active, false)) ==
                       "Cannot stop camera while experiment is active. Please stop the experiment first.", "Active");
        MIB_EXPECT(cameraStopRefusal(status(ExperimentRunState::Starting, false)) ==
                       "Cannot stop camera while an experiment is starting.", "Starting");
        for (const auto& saving : {status(ExperimentRunState::Stopping, true), status(ExperimentRunState::Failed, true)})
            MIB_EXPECT(cameraStopRefusal(saving) ==
                           "Cannot stop camera while experiment data is being saved. Wait for the save to finish.",
                       "Stopping, or Failed while still saving");
    }

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

    // ---- the mock camera by frame rate (CONNECT-016) ----------------------------------------------
    {
        CameraCommand byRate = mock;
        byRate.mockFrameIntervalMs = 33; // ignored when a rate is given
        const auto interval = [&](double fps) {
            byRate.mockFramesPerSecond = fps;
            MIB_REQUIRE(facade.dispatch(byRate).ok, "mock configured at " + std::to_string(fps) + " fps");
            return backend.mockFrameIntervalUs();
        };
        MIB_EXPECT(interval(5000) == 200, "5000 fps is 200 us, below the 1 ms floor of the millisecond field");
        MIB_EXPECT(interval(1) == 1'000'000, "1 fps is a second");
        MIB_EXPECT(interval(30) == 33'333, "30 fps rounds to whole microseconds");
        MIB_EXPECT(interval(20000) == 100, "the rate clamps to the spin box's 10000 fps");
        MIB_EXPECT(interval(0.2) == 1'000'000, "the rate clamps to 1 fps");
        byRate.mockFramesPerSecond = 0; // no rate: the millisecond field, floored at 1 ms
        byRate.mockFrameIntervalMs = 0;
        MIB_REQUIRE(facade.dispatch(byRate).ok, "millisecond path");
        MIB_EXPECT(backend.mockFrameIntervalUs() == 1000, "the millisecond field floors at 1 ms");
        byRate.mockFramesPerSecond = std::nan("");
        byRate.mockFrameIntervalMs = 7;
        MIB_REQUIRE(facade.dispatch(byRate).ok, "NaN rate falls back to the millisecond field");
        MIB_EXPECT(backend.mockFrameIntervalUs() == 7000, "NaN is not a rate");
        // the same refusal as the millisecond path
        byRate.mockFramesPerSecond = 5000;
        MIB_REQUIRE(facade.dispatch(byRate).ok, "mock at 5000 fps");
        CameraCommand start;
        start.action = CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(start).ok, "capture starts");
        const auto refused = facade.dispatch(byRate);
        MIB_EXPECT(!refused.ok && refused.message.find("camera is running") != std::string::npos,
                   "a rate change is refused while the camera runs: " + refused.message);
        MIB_EXPECT(backend.mockFrameIntervalUs() == 200, "the refused command changed nothing");
        CameraCommand stop;
        stop.action = CameraCommandAction::StopCapture;
        MIB_REQUIRE(facade.dispatch(stop).ok, "capture stops");
    }

    // ---- a teardown the device did not confirm (CONNECT-040) ----------------------------------------
    {
        mib::test::FakeLifecycleCamera::Observations obs;
        mib::test::FakeLifecycleCamera::Script script;
        script.stopFailureCode = "aravis.acquisition_stop";
        script.stopFailureMessage = "AcquisitionStop was not confirmed by the generator";
        backend.capture().setCameraFactory([&obs, script]() -> std::unique_ptr<camera::common::ICamera> {
            return std::make_unique<mib::test::FakeLifecycleCamera>(script, &obs);
        });
        CameraCommand start;
        start.action = CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(start).ok, "capture starts on the fake camera");
        MIB_REQUIRE(pollUntil([&] { return backend.capture().isRunning(); }, 5000ms), "fake camera runs");
        CameraCommand stop;
        stop.action = CameraCommandAction::StopCapture;
        const auto failed = facade.dispatch(stop);
        MIB_EXPECT(!failed.ok && failed.message == "AcquisitionStop was not confirmed by the generator",
                   "a shutdown failure is a failed command with the camera's message: " + failed.message);
        MIB_EXPECT(!backend.capture().isRunning(), "the capture is stopped all the same");
        // a Stop with nothing running is a plain success, not the earlier session's failure again
        const auto again = facade.dispatch(stop);
        MIB_EXPECT(again.ok, "Stop with nothing running does not repeat the old failure: " + again.message);
        // a camera whose teardown is confirmed stops cleanly
        script.stopFailureCode.clear();
        backend.capture().setCameraFactory([&obs, script]() -> std::unique_ptr<camera::common::ICamera> {
            return std::make_unique<mib::test::FakeLifecycleCamera>(script, &obs);
        });
        MIB_REQUIRE(facade.dispatch(start).ok, "capture restarts");
        MIB_REQUIRE(pollUntil([&] { return backend.capture().isRunning(); }, 5000ms), "fake camera runs again");
        MIB_EXPECT(facade.dispatch(stop).ok, "a confirmed teardown is a successful stop");
    }

    facade.shutdown();
    return mib::test::exitCode();
}
