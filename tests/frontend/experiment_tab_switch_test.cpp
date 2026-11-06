// experiment_tab_switch_test (issue #899)
//
// Leaving the Experiment tab during a run must not stop the run's realtime processing. The
// realtime loop is the run's only consumer: it admits, processes and saves the run's frames.
// MainWindow::onTabChanged stopped it whenever the user left Overview/Experiment (only Overview and
// Review are refused during a run, the Connect tab is not), and started it again on return. On the
// MC2066 bench the run then lost 3.3 s of frames, and the restarted loop, which began at write
// index 0, booked every frame since the capture started as storeOverwritten (2,196,816 for a 26 s
// run; ProcessingService::realtimeResumePointer, covered by processing.realtime_ring_loss).
//
// What this test pins, through a real MainWindow and its tab widget on a mock camera:
// 1. During a run, the Connect tab hides the view only: realtime processing keeps running and the
//    run keeps admitting frames while the tab is away. After the run, its accounting holds no
//    store loss and no more frames than the camera wrote during the run.
// 2. A run that ends while the user is on the Connect tab leaves no realtime loop running for the
//    hidden tab; showing the Experiment tab starts it again.
// 3. The same for a run started by another client (the bridge): the window learns of it only from
//    the coordinator, and still keeps processing running while the tab is away.

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "frontend/core/MainWindow.h"
#include "frontend/models/RunStatusModel.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

struct MainWindowTabSwitchTestAccess {
    // What onStartExperiment does once the coordinator accepted the Start (it opens a file dialog
    // first, so the test starts the run through the coordinator and then marks the window).
    static void started(MainWindow& window)
    {
        window.experimentActive_ = true;
        window.runOperationId_ = window.runStatusModel_->beginOperation(frontend::RunPhase::Running);
        window.updateExperimentButtonStates();
    }
    // The window rendered the terminal status of the run.
    static bool finalized(MainWindow& window) { return !window.experimentActive_ && !window.stopInProgress_; }
};

namespace {
constexpr int kConnectTab = 0;
constexpr int kExperimentTab = 2;

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        std::this_thread::yield();
    }
    return condition();
}

void printAccounting(const std::string& label, const backend::recording::RecordingAccountingSnapshot& a,
                     uint64_t written)
{
    std::fprintf(stderr,
                 "%s: written during the run=%llu admitted=%llu processed=%llu rejected=%llu ow=%llu "
                 "gaps=%llu completion=%s (%s)\n",
                 label.c_str(), (unsigned long long)written, (unsigned long long)a.admitted,
                 (unsigned long long)a.processed, (unsigned long long)a.scientificallyRejected,
                 (unsigned long long)a.storeOverwritten, (unsigned long long)a.sequenceGaps,
                 backend::recording::toString(a.completion), a.completionReason.c_str());
}
} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump,pulse_generator"));
    mib::test::Watchdog watchdog(150);
    QApplication app(argc, argv); // widgets: a real MainWindow and its tab widget

    mib::test::TempDir dir("experiment_tab_switch");
    QCoreApplication::setOrganizationName("mib_tab_switch_test");
    QCoreApplication::setApplicationName("mib_tab_switch_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString(dir.path().string()));
    const auto frames = dir / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 96, 96), "mock frames");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "initialize");
    for (const auto& id : backend.deviceDiscovery().providerIds())
        MIB_REQUIRE(backend.deviceDiscovery().unregisterProvider(id), "disable hardware discovery");
    MainWindow window(backend);
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabs"));
    MIB_REQUIRE(tabs && tabs->count() == 4, "the window's Connect/Overview/Experiment/Review tabs");

    camera::mock::MockCameraOptions options;
    options.folder = frames;
    options.loopFiles = true;
    options.frameInterval = std::chrono::milliseconds(2);
    backend.configureMockCamera(options);
    MIB_REQUIRE(backend.capture().start(), "start mock capture");

    auto& coordinator = backend.experiment();
    auto& processing = backend.processing();
    auto config = processing.getProcessingConfig();
    config.empty_frame_pixel_threshold = 1;
    config.auto_background_enabled = false;
    config.enable_target_group = false;
    processing.setProcessingConfig(config);
    processing.setRealtimeRoi({0, 0, 96, 96});
    // The mock delivers a frame every 2 ms (500 fps); the default profile asks for 5000 fps and the
    // storage gates would then need 92 MB/s from the runner's temp disk (#833).
    {
        auto acquisition = backend.acquisitionProfile();
        acquisition.targetFps = 500;
        backend.setAcquisitionProfile(acquisition);
    }

    // Whatever the window shows at the end of a run (the accounting or a save note) is closed.
    QTimer dialogCloser;
    dialogCloser.setInterval(10);
    QObject::connect(&dialogCloser, &QTimer::timeout, &window, [] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->close();
    });
    dialogCloser.start();

    tabs->setCurrentIndex(kExperimentTab);
    MIB_REQUIRE(waitFor([&] { return processing.isRealtimeRunning(); }), "the Experiment tab runs realtime processing");

    auto startRun = [&](const std::string& name, bool fromWindow) -> uint64_t {
        const auto output = (dir / name).string();
        if (!waitFor([&] { return coordinator.evaluateReadiness(output).ready; })) {
            for (const auto& g : coordinator.evaluateReadiness(output).gates)
                if (g.blocksStart())
                    std::fprintf(stderr, "blocking gate: %s: %s (%s)\n", g.id.c_str(), g.reason.c_str(),
                                 g.detail.c_str());
            MIB_REQUIRE(false, "ready to start " + name);
        }
        backend::app::ExperimentStartRequest request;
        request.outputPath = output;
        request.readinessGeneration = coordinator.evaluateReadiness(output).generation;
        const uint64_t writtenAtStart = backend.getFrameStore()->totalWritten();
        MIB_REQUIRE(coordinator.start(request).started(), "start " + name);
        if (fromWindow) MainWindowTabSwitchTestAccess::started(window);
        return writtenAtStart;
    };
    auto admitted = [&] { return processing.experimentAccountingSnapshot().admitted; };
    auto stopRun = [&](const std::string& name) {
        MIB_REQUIRE(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted,
                    "stop " + name);
        MIB_REQUIRE(waitFor([&] {
                        return coordinator.status().terminal && MainWindowTabSwitchTestAccess::finalized(window);
                    }),
                    name + ": the window rendered the end of the run");
    };

    // ---- 1. the Connect tab mid-run: the view pauses, processing keeps reading the run ----------
    {
        const uint64_t writtenAtStart = startRun("tab_away.h5", /*fromWindow=*/true);
        MIB_REQUIRE(waitFor([&] { return admitted() >= 100; }), "the run admits frames on the Experiment tab");

        tabs->setCurrentIndex(kConnectTab);
        MIB_EXPECT(processing.isRealtimeRunning(),
                   "leaving the Experiment tab during a run keeps realtime processing running");
        const uint64_t before = admitted();
        MIB_EXPECT(waitFor([&] { return admitted() >= before + 300; }, std::chrono::seconds(10)),
                   "the run keeps admitting frames while the Connect tab is shown");

        tabs->setCurrentIndex(kExperimentTab);
        MIB_EXPECT(processing.isRealtimeRunning(), "back on the Experiment tab, realtime processing runs");
        const uint64_t back = admitted();
        waitFor([&] { return admitted() >= back + 100; }, std::chrono::seconds(10));
        stopRun("tab_away.h5");

        const uint64_t written = backend.getFrameStore()->totalWritten() - writtenAtStart;
        const auto a = processing.experimentAccountingSnapshot();
        printAccounting("tab away mid-run", a, written);
        MIB_EXPECT(a.storeOverwritten == 0, "no store loss: no frame of the run went unread");
        // A loop restarted from write index 0 read the ring again from its start: frames from
        // before the run and frames already admitted (the ring of 5000 had not wrapped yet).
        // The slack is the loop's lag at the Start (frames written before it, read after it).
        MIB_EXPECT(a.admitted <= written + 100, "no more frames admitted than the camera wrote during the run");
        MIB_EXPECT(a.sequenceGaps == 0, "no hole in the admitted sequence");
        MIB_EXPECT(a.reconciled, "frame terms reconcile");
    }

    // ---- 2. a run that ends while the Connect tab is shown ------------------------------------
    {
        startRun("ends_away.h5", /*fromWindow=*/true);
        MIB_REQUIRE(waitFor([&] { return admitted() >= 100; }), "the second run admits frames");
        tabs->setCurrentIndex(kConnectTab);
        MIB_EXPECT(processing.isRealtimeRunning(), "kept for the run while the tab is away");
        stopRun("ends_away.h5");
        MIB_EXPECT(!processing.isRealtimeRunning(),
                   "once the run is over, no realtime loop runs for the hidden Experiment tab");
        tabs->setCurrentIndex(kExperimentTab);
        MIB_EXPECT(processing.isRealtimeRunning(), "showing the Experiment tab starts realtime processing again");
    }

    // ---- 3. a run started by another client: the coordinator's state alone keeps it running ----
    {
        startRun("bridge_run.h5", /*fromWindow=*/false);
        MIB_REQUIRE(waitFor([&] { return admitted() >= 100; }), "the bridge run admits frames");
        tabs->setCurrentIndex(kConnectTab);
        MIB_EXPECT(processing.isRealtimeRunning(), "a run the window did not start also keeps processing running");
        const uint64_t before = admitted();
        MIB_EXPECT(waitFor([&] { return admitted() >= before + 300; }, std::chrono::seconds(10)),
                   "the bridge run keeps admitting frames while the Connect tab is shown");
        stopRun("bridge_run.h5");
        // The window was never marked active, so wait for its rendering of the end of the run.
        MIB_EXPECT(waitFor([&] { return !processing.isRealtimeRunning(); }, std::chrono::seconds(10)),
                   "after the bridge run, nothing runs for the hidden tab");
        MIB_EXPECT(processing.experimentAccountingSnapshot().storeOverwritten == 0, "the bridge run lost no frame");
    }

    dialogCloser.stop();
    backend.capture().stop();
    backend.shutdown();
    return mib::test::exitCode();
}
