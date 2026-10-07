#include "backend/app/AppBackend.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "frontend/core/MainWindow.h"
#include "frontend/system/PlaybackPanel.h"
#include "frontend/widgets/AlertBanner.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/faultinject.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QApplication>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <chrono>
#include <functional>
#include <thread>

struct MainWindowFaultTestAccess {
    static void started(MainWindow& window) {
        window.experimentActive_ = true;
        window.runOperationId_ =
            window.runStatusModel_->beginOperation(frontend::RunPhase::Running);
        window.updateExperimentButtonStates();
    }
    static bool rendered(MainWindow& window, uint64_t revision) {
        return window.presentedFault_.faultRevision == revision;
    }
    static void explain(MainWindow& window,
                        const backend::app::ExperimentReadinessSnapshot& readiness) {
        window.explainReadiness(readiness);
    }
};

struct PlaybackPanelFaultTestAccess {
    static bool enabled(PlaybackPanel& panel) {
        panel.roiActive_ = true;
        panel.updateConfigurationUI();
        return panel.clearRoiBtn_->isEnabled() && panel.autoBgCheck_->isEnabled();
    }
    static bool disabled(PlaybackPanel& panel) {
        panel.updateConfigurationUI();
        return !panel.clearRoiBtn_->isEnabled() && !panel.autoBgCheck_->isEnabled();
    }
};

namespace {
bool waitFor(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        std::this_thread::yield();
    }
    return condition();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("MIB_CAMERA_MODE", "mock");
    qputenv("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator");
    mib::test::Watchdog watchdog(90);
    QApplication app(argc, argv);
    mib::test::TempDir dir("experiment_fault_recovery");
    QCoreApplication::setOrganizationName("mib_fault_test");
    QCoreApplication::setApplicationName("mib_fault_test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(dir.path().string()));
    const auto frames = dir / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 96, 96), "mock frames");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "initialize");
    for (const auto& id : backend.deviceDiscovery().providerIds())
        MIB_REQUIRE(backend.deviceDiscovery().unregisterProvider(id), "disable hardware discovery");
    MainWindow window(backend);
    auto* panel = window.findChild<PlaybackPanel*>();
    MIB_REQUIRE(panel, "playback controls exist");
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
    processing.setInvalidFrameSamplingRate(1);
    processing.setFlushInterval(1000000);
    QTimer dialogCloser;
    dialogCloser.setInterval(10);
    QObject::connect(&dialogCloser, &QTimer::timeout, &window, [] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box && box->windowTitle() == "Save Error") box->close();
    });
    dialogCloser.start();
    for (int run = 0; run < 3; ++run) {
        const auto output = (dir / ("failed_" + std::to_string(run) + ".h5")).string();
        MIB_REQUIRE(waitFor([&] { return coordinator.evaluateReadiness(output).ready; }),
                    "ready to start");
        backend::app::ExperimentStartRequest request;
        request.outputPath = output;
        request.readinessGeneration = coordinator.evaluateReadiness(output).generation;
        MIB_REQUIRE(coordinator.start(request).started(), "start experiment");
        MainWindowFaultTestAccess::started(window);
        MIB_REQUIRE(waitFor([&] { return processing.getBufferedFrameCounts().total() > 0; }),
                    "first batch buffered");
        MIB_REQUIRE(processing.flushBufferedFrames(backend.hdf5()) > 0 && processing.finishFlush(),
                    "first batch saved");
        MIB_REQUIRE(backend.hdf5().flush() && mib::test::blockHdf5ImageAppends(output),
                    "inject save failure");
        MIB_REQUIRE(waitFor([&] { return processing.getBufferedFrameCounts().total() > 0; }),
                    "second batch buffered");
        processing.flushBufferedFrames(backend.hdf5());
        MIB_REQUIRE(waitFor([&] {
                        const auto status = coordinator.status();
                        return status.terminal &&
                               MainWindowFaultTestAccess::rendered(window, status.faultRevision);
                    }),
                    "failed finalization rendered through fatal callback");
        MIB_REQUIRE(mib::test::restoreHdf5ImageAppends(output), "restore image datasets");
        MIB_EXPECT(PlaybackPanelFaultTestAccess::disabled(*panel),
                   "ROI and background disabled after failure");
        if (run == 0) {
            const auto readiness = coordinator.evaluateReadiness(output);
            QTimer::singleShot(0, &window, [&] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                MIB_REQUIRE(box, "readiness dialog open for dismissal");
                box->close();
            });
            MainWindowFaultTestAccess::explain(window, readiness);
            MIB_EXPECT(coordinator.hasUnresolvedFault(),
                       "dismissing readiness does not acknowledge fault");
            bool offered = false;
            QTimer::singleShot(0, &window, [&] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                MIB_REQUIRE(box, "readiness dialog open");
                for (auto* button : box->buttons()) {
                    if (button->text() == "Acknowledge fault and re-check") {
                        offered = true;
                        button->click();
                        return;
                    }
                }
                box->close();
            });
            MainWindowFaultTestAccess::explain(window, readiness);
            MIB_EXPECT(offered, "Failed lifecycle plus fault offers acknowledgement");
        } else {
            MIB_EXPECT(window.alertModel()->find("save.fatal") != nullptr,
                       "fatal callback raised save alert");
            if (run == 1) {
                const auto displayed = coordinator.status();
                coordinator.reportUnresolvedFault(displayed.faultCode, displayed.faultMessage);
                window.alertBanner()->acknowledgeButton()->click();
                MIB_EXPECT(coordinator.hasUnresolvedFault(),
                           "banner cannot acknowledge newer unseen fault");
                // Review the new identity through the readiness dialog.
                const auto readiness = coordinator.evaluateReadiness(output);
                QTimer::singleShot(0, &window, [&] {
                    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    MIB_REQUIRE(box, "readiness dialog open for new fault");
                    for (auto* button : box->buttons())
                        if (button->text() == "Acknowledge fault and re-check") {
                            button->click();
                            return;
                        }
                    box->close();
                });
                MainWindowFaultTestAccess::explain(window, readiness);
            } else {
                window.alertBanner()->acknowledgeButton()->click();
            }
        }
        MIB_EXPECT(coordinator.state() == backend::app::ExperimentRunState::Idle,
                   "Qt acknowledgement returns coordinator to Idle");
        MIB_EXPECT(!coordinator.hasUnresolvedFault(), "fault cleared");
        MIB_EXPECT(PlaybackPanelFaultTestAccess::enabled(*panel), "ROI and background re-enabled");
        for (const auto& gate : coordinator.evaluateReadiness(output).gates)
            MIB_EXPECT(!(gate.id == "lifecycle.experiment" && gate.blocksStart()),
                       "Start no longer blocked by Failed");
        // Let the acknowledgement publication render without replaying dialogs.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (coordinator.state() != backend::app::ExperimentRunState::Idle) break;
    }
    backend.capture().stop();
    backend.shutdown();
    return mib::test::exitCode();
}
