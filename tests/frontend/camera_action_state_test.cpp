// camera_action_state_test (issue #360)
//
// The CameraController is the one UI command path for camera acquisition.
// With a scripted fake camera injected into the real backend this proves:
//  1. start from the action and from direct dispatch produce the same state
//     (Running) and enable flags;
//  2. rapid duplicate starts issue exactly one backend command;
//  3. while the operation guard reports an active experiment, every stop
//     route (action trigger, direct dispatch, toggle) is refused with the
//     same reason and capture keeps running — even though the action is
//     disabled, direct dispatch is still guarded;
//  4. a failed start yields Failed with the backend's structured message,
//     identically for every route;
//  5. a camera that dies on its own is projected as Failed by the poll;
//  6. stop returns to Idle; toggle behaves as start/stop.

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "frontend/controllers/CameraController.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>

using frontend::CameraActionState;
using frontend::CameraCommandResult;
using frontend::CameraController;
using mib::test::FakeLifecycleCamera;

namespace {
using Phase = CameraActionState::Phase;

// Pump the Qt event loop until pred() or timeout.
bool pumpUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (pred()) return true;
    }
    return pred();
}

const char* phaseName(Phase p)
{
    switch (p) {
    case Phase::Idle: return "Idle";
    case Phase::Starting: return "Starting";
    case Phase::Running: return "Running";
    case Phase::Stopping: return "Stopping";
    case Phase::Failed: return "Failed";
    case Phase::NotConfigured: return "NotConfigured";
    }
    return "?";
}
} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    QGuiApplication app(argc, argv);
    mib::test::Watchdog wd(40);

    mib::test::TempDir dataRoot("camera_action_state");
    std::filesystem::create_directories(dataRoot.path() / "mock_frames");
    qputenv("MIB_MOCK_CAMERA_DIR", QByteArray::fromStdString((dataRoot.path() / "mock_frames").string()));

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(dataRoot.path().string()), "backend initializes in mock mode");

    auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
    FakeLifecycleCamera::Script script;
    script.produceInterval = std::chrono::microseconds(500);
    backend.capture().setCameraFactory([&]() {
        obs->destroyed.store(false);
        return std::unique_ptr<camera::common::ICamera>(new FakeLifecycleCamera(script, obs.get()));
    });

    CameraController ctl(backend);
    ctl.setPollIntervalMs(20);
    bool experimentActive = false;
    ctl.setOperationGuard([&]() {
        frontend::CameraOperationBlock b;
        if (experimentActive) {
            b.blocked = true;
            b.reason = QStringLiteral("Cannot stop camera while an experiment is active. Stop the experiment first.");
        }
        return b;
    });

    int stateChanges = 0;
    QStringList failures;
    QObject::connect(&ctl, &CameraController::stateChanged, [&](const CameraActionState&) { ++stateChanges; });
    QObject::connect(&ctl, &CameraController::commandFailed, [&](const QString& m) { failures << m; });

    // ---- 1. Initial state: configured (mock) + idle -> start enabled ------
    wd.mark("initial");
    MIB_EXPECT(ctl.state().phase == Phase::Idle, std::string("initial phase ") + phaseName(ctl.state().phase));
    MIB_EXPECT(ctl.state().startEnabled && !ctl.state().stopEnabled, "start enabled, stop disabled when idle");
    MIB_EXPECT(ctl.startAction()->isEnabled() && !ctl.stopAction()->isEnabled(), "actions mirror state");

    // ---- 2. Start via the shared action; rapid duplicates -----------------
    wd.mark("start via action + duplicates");
    ctl.startAction()->trigger();
    for (int i = 0; i < 10; ++i) {
        const auto r = ctl.requestStart();
        MIB_EXPECT(r.outcome == CameraCommandResult::Outcome::AlreadyInState ||
                       r.outcome == CameraCommandResult::Outcome::Blocked,
                   "duplicate start refused");
    }
    ctl.startAction()->trigger();
    MIB_REQUIRE(pumpUntil([&] { return ctl.state().phase == Phase::Running; }, std::chrono::seconds(5)),
                std::string("reaches Running via action, got ") + phaseName(ctl.state().phase));
    MIB_EXPECT(obs->startCalls.load() == 1, "exactly one backend start for a burst of requests");
    MIB_EXPECT(ctl.state().generation == 1, "generation 1");
    MIB_EXPECT(!ctl.state().startEnabled && ctl.state().stopEnabled, "running: start disabled, stop enabled");
    MIB_EXPECT(!ctl.startAction()->isEnabled() && ctl.stopAction()->isEnabled(), "actions mirror running");
    MIB_EXPECT(backend.capture().lifecycleSnapshot().cameraReady, "backend camera ready");

    // ---- 3. Experiment active: every stop route blocked identically -------
    wd.mark("blocked stop");
    experimentActive = true;
    ctl.refreshState();
    MIB_EXPECT(!ctl.state().stopEnabled, "stop disabled during experiment");
    MIB_EXPECT(!ctl.state().stopBlockedReason.isEmpty(), "blocked reason exposed in state");
    MIB_EXPECT(!ctl.stopAction()->isEnabled(), "stop action disabled during experiment");
    const int failuresBefore = failures.size();
    // Route A: direct dispatch while the action is disabled.
    const auto rA = ctl.requestStop();
    MIB_EXPECT(rA.outcome == CameraCommandResult::Outcome::Blocked, "direct stop blocked");
    // Route B: action trigger (disabled actions do not fire; force via trigger()).
    ctl.stopAction()->setEnabled(true);
    ctl.stopAction()->trigger();
    ctl.refreshState();
    // Route C: toggle (space bar).
    const auto rC = ctl.requestToggle();
    MIB_EXPECT(rC.outcome == CameraCommandResult::Outcome::Blocked, "toggle stop blocked");
    // Route D: legacy wrapper.
    QString err;
    MIB_EXPECT(!ctl.stopCapture(&err) && err == rA.message, "legacy wrapper blocked with same message");
    MIB_EXPECT(failures.size() == failuresBefore + 4, "each refused stop reported once (4 routes)");
    for (int i = failuresBefore; i < failures.size(); ++i) {
        MIB_EXPECT(failures[i] == rA.message, "identical blocked reason on every route");
    }
    MIB_EXPECT(backend.capture().isRunning(), "capture still running after blocked stops");
    MIB_EXPECT(obs->stopCalls.load() == 0, "no backend stop reached the camera");
    MIB_EXPECT(ctl.state().phase == Phase::Running, "still Running");

    // ---- 4. Experiment over: stop succeeds -> Idle ------------------------
    wd.mark("stop");
    experimentActive = false;
    ctl.refreshState();
    MIB_EXPECT(ctl.state().stopEnabled, "stop re-enabled after experiment");
    const auto rStop = ctl.requestStop();
    MIB_EXPECT(rStop.accepted(), "stop accepted");
    MIB_EXPECT(ctl.state().phase == Phase::Idle, std::string("Idle after stop, got ") + phaseName(ctl.state().phase));
    MIB_EXPECT(obs->destroyed.load(), "camera released");
    MIB_EXPECT(ctl.requestStop().outcome == CameraCommandResult::Outcome::AlreadyInState, "second stop AlreadyInState");

    // ---- 5. Failed start: same outcome via every route --------------------
    wd.mark("failed start");
    script.startSucceeds = false;
    script.failureMessage = "ISP output format is not Mono8";
    for (int route = 0; route < 3; ++route) {
        if (route == 0) ctl.startAction()->trigger();
        else if (route == 1) MIB_EXPECT(ctl.requestStart().accepted(), "direct start accepted");
        else MIB_EXPECT(ctl.requestToggle().accepted(), "toggle start accepted");
        MIB_REQUIRE(pumpUntil([&] { return ctl.state().phase == Phase::Failed; }, std::chrono::seconds(5)),
                    "start failure projected as Failed");
        MIB_EXPECT(ctl.state().failureMessage == QStringLiteral("ISP output format is not Mono8"),
                   "structured failure message: " + ctl.state().failureMessage.toStdString());
        MIB_EXPECT(ctl.state().startEnabled && !ctl.state().stopEnabled, "Failed: start enabled, stop disabled");
        MIB_EXPECT(!backend.capture().isRunning(), "not running after failure");
    }

    // ---- 6. Camera dies on its own while running -> Failed via poll --------
    wd.mark("device loss");
    script.startSucceeds = true;
    script.endStreamAfterFrames = 3;
    obs->framesDelivered.store(0);
    MIB_EXPECT(ctl.requestStart().accepted(), "restart after failure accepted");
    MIB_REQUIRE(pumpUntil([&] { return ctl.state().phase == Phase::Failed; }, std::chrono::seconds(5)),
                std::string("device loss projected, got ") + phaseName(ctl.state().phase));
    MIB_EXPECT(ctl.state().failureMessage.contains("ended"), "stream-ended message: " + ctl.state().failureMessage.toStdString());
    MIB_EXPECT(ctl.state().startEnabled, "restart offered after device loss");
    MIB_EXPECT(!backend.capture().isRunning(), "backend not running after device loss");

    // ---- 7. Restart cleanly, then toggle stops -----------------------------
    wd.mark("toggle");
    script.endStreamAfterFrames = 0;
    obs->framesDelivered.store(0);
    MIB_EXPECT(ctl.requestToggle().accepted(), "toggle starts when idle/failed");
    MIB_REQUIRE(pumpUntil([&] { return ctl.state().phase == Phase::Running; }, std::chrono::seconds(5)), "running");
    MIB_EXPECT(ctl.requestToggle().accepted(), "toggle stops when running");
    MIB_EXPECT(ctl.state().phase == Phase::Idle, "Idle after toggle stop");
    MIB_EXPECT(stateChanges > 0, "state change signals fired");

    std::fprintf(stderr, "camera_action_state: %d state changes, %d refused commands\n",
                 stateChanges, failures.size());
    backend.shutdown();
    return mib::test::exitCode();
}
