#include "frontend/core/MainWindow.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <QTabWidget>
#include <QSplitter>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QDesktopServices>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

#include "frontend/dialogs/SoftwareUpdatesDialog.h"
#include <QSizePolicy>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSet>
#include <QSettings>
#include <QSysInfo>
#include <QScreen>
#include <QToolButton>
#include <QShowEvent>
#include <QResizeEvent>
#include <QWindow>
#include <QGuiApplication>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <optional>
#include <vector>

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/common/ICamera.h"
#include "backend/services/CaptureService.h"
#include "backend/services/CrashReporter.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/playback/PlaybackService.h"
#include "backend/services/AutofocusService.h"
#include "frontend/system/PlaybackPanel.h"
#include "frontend/controllers/CameraController.h"
#include "frontend/utils/StatsDisplayManager.h"
#include "frontend/utils/ElidingLabel.h"
#include "frontend/models/RunStatusModel.h"
#include "frontend/widgets/RunStatusWidget.h"
#include "frontend/widgets/AlertBanner.h"
#include <QPlainTextEdit>
#include "frontend/utils/WindowGeometryPolicy.h"
#include "frontend/tabs/ConnectTab.h"
#include "frontend/tabs/PreviewPage.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/tabs/ExperimentMonitoringTab.h"
#include "frontend/system/AppConfigWatcher.h"
#include "frontend/dialogs/MonitoringSettingsDialog.h"
#include "frontend/tabs/OverviewTab.h"
#include "frontend/tabs/ConfigTabs.h"
#include "frontend/system/AutoUpdater.h"
#include "frontend/system/DeviceInitManager.h"
#include "frontend/utils/SidebarWidget.h"
#include "frontend/utils/StatisticsPanel.h"
#include <spdlog/spdlog.h>
#include <opencv2/core.hpp>
#include <chrono>
#include <sstream>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QSpinBox>
#include <QLayout>
#include <QThread>
#include <thread>
#include <QMenuBar>
#include <QMenu>
#include "frontend/dialogs/ProcessingSettingsDialog.h"
#include "frontend/dialogs/ProcessingCoreDialog.h"
#include "frontend/dialogs/ConversionFactorDialog.h"
#include "frontend/dialogs/SyringePumpSettingsDialog.h"
#include "backend/app/Tools.h"
#include "frontend/qt/BackgroundFrameQtAdapter.h"
#include <QCloseEvent>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
QString normalizeServiceToken(QString token)
{
    token = token.trimmed().toLower();
    token.replace('-', '_');
    return token;
}

QSet<QString> parseDisabledServicesCsv(const QString &raw)
{
    QSet<QString> out;
    const QStringList parts = raw.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts)
    {
        const QString token = normalizeServiceToken(part);
        if (!token.isEmpty())
        {
            out.insert(token);
        }
    }
    return out;
}

QString serializeDisabledServicesCsv(const QSet<QString> &disabled)
{
    QStringList values = disabled.values();
    std::sort(values.begin(), values.end());
    return values.join(',');
}

QSet<QString> loadPersistedDisabledServices()
{
    QSettings settings;
    return parseDisabledServicesCsv(settings.value("Startup/DisabledServices").toString());
}

void savePersistedDisabledServices(const QSet<QString> &disabled)
{
    QSettings settings;
    const QString csv = serializeDisabledServicesCsv(disabled);
    if (csv.isEmpty())
    {
        settings.remove("Startup/DisabledServices");
    }
    else
    {
        settings.setValue("Startup/DisabledServices", csv);
    }
}

bool isServiceDisabledAtBoot(const QString &serviceToken)
{
    const QSet<QString> disabled = parseDisabledServicesCsv(QString::fromUtf8(qgetenv("MIB_DISABLED_SERVICES")));
    const QString token = normalizeServiceToken(serviceToken);
    return disabled.contains("all") || disabled.contains(token);
}
} // namespace

namespace frontend::detail {
// RAII reentrancy guard for layout application (issues #358/#359).
struct ScopedFlag {
    explicit ScopedFlag(bool& f) : flag(f) { flag = true; }
    ~ScopedFlag() { flag = false; }
    bool& flag;
};
} // namespace frontend::detail

MainWindow::MainWindow(backend::AppBackend &backend, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), backend_(backend)
{
    ui->setupUi(this);

    // Connect menu actions
    connect(ui->exitAct, &QAction::triggered, this, &QWidget::close);
    connect(ui->processingSettingsAct, &QAction::triggered, this, [this]()
            {
        SPDLOG_INFO("Opening Processing Settings dialog");
        // Find PlaybackPanel from PreviewPage tab
        PlaybackPanel* playbackPanel = nullptr;
        if (experimentTabs_) {
            // Preview tab is at index 0 within Experiment tab
            if (experimentTabs_->count() > 0) {
                auto* previewPage = qobject_cast<frontend::PreviewPage*>(experimentTabs_->widget(0));
                if (previewPage) {
                    playbackPanel = previewPage->getPlaybackPanel();
                }
            }
        }
        ProcessingSettingsDialog dlg(backend_, playbackPanel, this);
        dlg.exec(); });
    connect(ui->monitoringSettingsAct, &QAction::triggered, this, [this]()
            {
        SPDLOG_INFO("Opening Monitoring Settings dialog");
        // Find ExperimentMonitoringTab
        frontend::ExperimentMonitoringTab* monitoringTab = nullptr;
        if (experimentTabs_) {
            // Monitoring tab is at index 1 within Experiment tab (0=Preview, 1=Monitoring)
            if (experimentTabs_->count() > 1) {
                monitoringTab = qobject_cast<frontend::ExperimentMonitoringTab*>(experimentTabs_->widget(1));
            }
        }
        if (monitoringTab) {
            MonitoringSettingsDialog dlg(monitoringTab, this);
            dlg.exec();
        } });
    connect(ui->conversionFactorAct, &QAction::triggered, this, [this]()
            {
        SPDLOG_INFO("Opening Pixel to Micron Conversion dialog");
        ConversionFactorDialog dlg(backend_, this);
        dlg.exec(); });
    connect(ui->syringePumpSettingsAct, &QAction::triggered, this, [this]()
            {
        SPDLOG_INFO("Opening Syringe Pump Settings dialog");
        SyringePumpSettingsDialog dlg(backend_, this);
        dlg.exec(); });

    auto* processingCoreAct = new QAction(tr("Processing Core..."), this);
    ui->settingsMenu->addAction(processingCoreAct);
    connect(processingCoreAct, &QAction::triggered, this, [this]() {
        SPDLOG_INFO("Opening Processing Core dialog");
        frontend::ProcessingCoreDialog dialog(backend_, this);
        dialog.exec();
        const auto identity = backend_.processing().activeProcessingCoreIdentity();
        if (processingCoreLabel_) {
            processingCoreLabel_->setText(
                backend_.processing().isProcessingCorePinSatisfied()
                    ? tr("Core: %1 · contract %2")
                          .arg(QString::fromStdString(identity.version))
                          .arg(identity.contractVersion)
                    : tr("Core: unavailable (selection failed)"));
        }
    });
    connect(ui->aboutAct, &QAction::triggered, this, [this]()
            {
        const QString v = QCoreApplication::applicationVersion();
        QMessageBox::about(this,
                           tr("About MIB Studio Qt"),
                           tr("MIB Studio Qt\nVersion: %1\n\nProvides camera capture, processing, and HDF5 logging.")
                               .arg(v.isEmpty() ? tr("(unknown)") : v));
    });

    auto *bootServicesAct = new QAction(tr("Boot Service Toggles..."), this);
    ui->settingsMenu->addSeparator();
    ui->settingsMenu->addAction(bootServicesAct);
    connect(bootServicesAct, &QAction::triggered, this, [this]()
            {
        struct Option { const char* token; const char* label; };
        const std::vector<Option> options = {
            {"sqlite", "SQLite service"},
            {"hdf5", "HDF5 service"},
            {"processing", "Processing service"},
            {"yolo", "YOLO service"},
            {"autofocus", "Autofocus wiring"},
            {"trigger", "Trigger wiring"},
            {"capture", "Capture service"},
            {"playback", "Playback service"},
            {"auto_update", "Auto update checker"}
        };

        QDialog dlg(this);
        dlg.setWindowTitle(tr("Boot Service Toggles"));
        dlg.setModal(true);
        auto* layout = new QVBoxLayout(&dlg);
        auto* info = new QLabel(tr("Disable selected services at next application startup.\n"
                                   "Changes are persisted and applied on next launch."), &dlg);
        info->setWordWrap(true);
        layout->addWidget(info);

        const QSet<QString> persisted = loadPersistedDisabledServices();
        const bool allEnabled = persisted.contains("all");
        QVector<QCheckBox*> checkboxes;
        checkboxes.reserve(static_cast<int>(options.size()));
        for (const auto& option : options)
        {
            auto* box = new QCheckBox(QString::fromUtf8(option.label), &dlg);
            box->setChecked(allEnabled || persisted.contains(QString::fromUtf8(option.token)));
            box->setProperty("serviceToken", QString::fromUtf8(option.token));
            layout->addWidget(box);
            checkboxes.push_back(box);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        layout->addWidget(buttons);

        if (dlg.exec() == QDialog::Accepted)
        {
            QSet<QString> disabled;
            for (QCheckBox* box : checkboxes)
            {
                if (box && box->isChecked())
                {
                    disabled.insert(box->property("serviceToken").toString());
                }
            }
            savePersistedDisabledServices(disabled);
            QMessageBox::information(this, tr("Boot Service Toggles"),
                                     tr("Saved. Restart the application for changes to take effect."));
        } });

    const bool autoUpdateDisabled = isServiceDisabledAtBoot(QStringLiteral("auto_update"));
    if (!autoUpdateDisabled)
    {
        updater_ = new frontend::AutoUpdater(this, this);
        connect(ui->checkUpdatesAct, &QAction::triggered, this, [this]() {
            if (!updater_) return;
            frontend::SoftwareUpdatesDialog dlg(updater_, this);
            dlg.exec();
        });
    }
    else
    {
        ui->checkUpdatesAct->setEnabled(false);
        SPDLOG_INFO("MainWindow: auto update disabled by MIB_DISABLED_SERVICES");
    }

    // Folder shortcuts, docs, and issue reporting (always available).
    auto openFolder = [](const QString& path) {
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    };
    connect(ui->openDataFolderAct, &QAction::triggered, this, [openFolder]() {
        openFolder(QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                       .absoluteFilePath(QStringLiteral("MIB_Studio_Qt")));
    });
    connect(ui->openLogsFolderAct, &QAction::triggered, this, [openFolder]() {
#ifdef _WIN32
        const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
        const QString base = localAppData.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            : localAppData;
#else
        const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
        openFolder(QDir(base).absoluteFilePath(QStringLiteral("MIB_Studio_Qt/logs")));
    });
    connect(ui->documentationAct, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/KPT1020/mib-studio-qt")));
    });
    connect(ui->reportProblemAct, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/KPT1020/mib-studio-qt/issues/new")));
    });
    connect(ui->profilesAct, &QAction::triggered, this, [this]() {
        // The config/profiles editor lives in the Experiment > Preview page.
        if (ui->tabs && experimentTabs_) {
            ui->tabs->setCurrentWidget(experimentTabs_);
            experimentTabs_->setCurrentIndex(0);
        }
    });

    // Fatal save errors (recording or experiment flush) fire on a writer thread.
    // Marshal to the UI thread: stop the active operation and show a dialog so
    // the failure is never silent.
    backend_.setFatalSaveErrorCallback([this](const std::string& msg) {
        const QString q = QString::fromStdString(msg);
        QMetaObject::invokeMethod(this, [this, q]() {
            // Persistent, actionable and latched (issue #363): the alert
            // survives metrics refreshes and the run stays Failed even if the
            // metadata write later succeeds.
            alertModel_->raise(QStringLiteral("save.fatal"), frontend::AlertSeverity::Critical,
                               tr("Data could not be saved: %1").arg(q),
                               tr("Check free space and permissions on the destination; the run is incomplete."));
            if (runStatusModel_ && experimentActive_) runStatusModel_->latchFailure(runOperationId_, q);
            backend_.experiment().reportUnresolvedFault("save.fatal", q.toStdString());
            if (backend_.isFrameRecording()) backend_.stopFrameRecording();
            // One finalization only: a fatal error during Stopping/Saving is
            // reported into the in-flight stop, never a second stop.
            if (experimentActive_ && !stopInProgress_) onStopExperiment();
            statusBar()->showMessage(tr("Save error: %1").arg(q));
            QMessageBox::critical(this, tr("Save Error"),
                tr("Data could not be saved and the operation was stopped:\n\n%1").arg(q));
        }, Qt::QueuedConnection);
    });

    // One camera command path for every presentation (issue #360). The
    // operation guard is a read-only view of the experiment/recording/flush
    // ownership held by this window; the controller consults it on every
    // stop request, including direct dispatch.
    // Application identity recorded in every frozen run snapshot (issue #369).
    backend_.experiment().setApplicationIdentity(
        MIB_STUDIO_QT_VERSION_FULL, std::string(),
        QSysInfo::prettyProductName().toStdString() + " " + QSysInfo::currentCpuArchitecture().toStdString());

    cameraController_ = new frontend::CameraController(backend_, this);
    cameraController_->setOperationGuard([this]() {
        frontend::CameraOperationBlock block;
        if (experimentActive_) {
            block.blocked = true;
            block.reason = tr("Cannot stop camera while an experiment is active. Stop the experiment first.");
        } else if (flushInProgress_) {
            block.blocked = true;
            block.reason = tr("Cannot stop camera while experiment data is being saved. Wait for the save to finish.");
        } else if (backend_.isFrameRecording()) {
            block.blocked = true;
            block.reason = tr("Cannot stop camera while frame recording is active. Stop recording first.");
        }
        return block;
    });
    connect(cameraController_, &frontend::CameraController::stateChanged,
            this, &MainWindow::onCameraStateChanged);
    connect(cameraController_, &frontend::CameraController::commandFailed, this,
            [this](const QString& message) {
                statusLabel_->setText(message);
                QMessageBox::warning(this, tr("Camera"), message);
            });

    // Experiment buttons and indicator will be added to Experiment tab, not toolbar
    startExperimentAct_ = new QAction("Start Experiment", this);
    stopExperimentAct_ = new QAction("Stop Experiment", this);

    // Initialize defaults for this session
    backend_.processing().setInvalidFrameSamplingRate(200);
    backend_.processing().setFlushInterval(200);

    connect(startExperimentAct_, &QAction::triggered, this, &MainWindow::onStartExperiment);
    connect(stopExperimentAct_, &QAction::triggered, this, &MainWindow::onStopExperiment);

    // Issue #358: the status text is elided, never a minimum-width driver.
    statusLabel_ = new frontend::ElidingLabel(QStringLiteral("Idle"), this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setElideMode(Qt::ElideRight);
    statusLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui->statusbar->addPermanentWidget(statusLabel_, /*stretch=*/1);
    setupStatusSurfaces();
    processingCoreLabel_ = new QLabel(this);
    processingCoreLabel_->setToolTip(
        tr("Active deformability-cytometry processing core; click Settings > Processing Core to change it."));
    ui->statusbar->addPermanentWidget(processingCoreLabel_);
    QString processingCoreRestoreError;
    const bool processingCoreReady = frontend::ProcessingCoreDialog::restorePersistedCore(
        backend_, &processingCoreRestoreError);
    if (!processingCoreReady) {
        SPDLOG_ERROR("MainWindow: persisted processing core could not be restored: {}",
                     processingCoreRestoreError.toStdString());
        statusBar()->showMessage(
            tr("Pinned processing core unavailable: %1").arg(processingCoreRestoreError));
        alertModel_->raise(QStringLiteral("processing.core"), frontend::AlertSeverity::Error,
                           tr("Pinned processing core unavailable: %1").arg(processingCoreRestoreError),
                           tr("Activate the pinned core in Settings › Processing Core; experiments cannot start until then."));
    }
    const auto startupCore = backend_.processing().activeProcessingCoreIdentity();
    processingCoreLabel_->setText(processingCoreReady
        ? tr("Core: %1 · contract %2")
              .arg(QString::fromStdString(startupCore.version))
              .arg(startupCore.contractVersion)
        : tr("Core: unavailable (selection failed)"));

    // Permanent acquisition-mode badge, visible across all tabs (#332).
    deliveryModeLabel_ = new QLabel(this);
    deliveryModeLabel_->setToolTip(
        tr("Every Frame preserves the complete acquisition sequence, at the cost of growing latency when the consumer falls behind.\n"
           "Latest Frame minimizes latency by intentionally discarding stale frames, so recorded sequences may have gaps."));
    ui->statusbar->addPermanentWidget(deliveryModeLabel_);
    updateDeliveryModeBadge();

    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(500);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::onUpdateStats);

    // Setup async flush watcher
    flushWatcher_ = new QFutureWatcher<size_t>(this);
    connect(flushWatcher_, &QFutureWatcher<size_t>::finished, this, [this]()
            {
        flushInProgress_ = false;
        size_t flushed = flushWatcher_->result();
        if (flushed > 0) {
            SPDLOG_INFO("Auto-flushed {} frames to HDF5", flushed);
        } });

    // Setup sidebar and main layout
    setupSidebar();

    // Tabs: Connect + Overview + Experiment (Preview + Monitoring) + Review
    connectTab_ = new frontend::ConnectTab(backend_, ui->tabs);
    overviewTab_ = new frontend::OverviewTab(backend_, ui->tabs);
    
    // Create Experiment tab with nested Preview and Monitoring tabs
    experimentTabs_ = new QTabWidget(this);
    auto *previewPage = new frontend::PreviewPage(backend_, experimentTabs_);
    auto *monitoringTab = new frontend::ExperimentMonitoringTab(backend_, experimentTabs_);
    experimentTabs_->addTab(previewPage, tr("Preview"));
    experimentTabs_->addTab(monitoringTab, tr("Monitoring"));
    
    // Connect PlaybackPanel background signal to SidebarWidget
    if (previewPage) {
        PlaybackPanel* playbackPanel = previewPage->getPlaybackPanel();
        if (playbackPanel) {
            // Space-bar toggle in the preview goes through the same guarded
            // command path as the chrome buttons (issue #360).
            connect(playbackPanel, &PlaybackPanel::captureToggleRequested, this,
                    [this]() { cameraController_->requestToggle(); });
            if (sidebarWidget_) {
                connect(playbackPanel, &PlaybackPanel::backgroundImageSet,
                        sidebarWidget_, &frontend::SidebarWidget::updateBackgroundPreview);
            }
            // Set initial background if one exists
            QImage currentBg = playbackPanel->getBackgroundImage();
            if (sidebarWidget_ && !currentBg.isNull()) {
                sidebarWidget_->updateBackgroundPreview(currentBg);
            }

            backend_.setBackgroundCaptureCallback([playbackPanel](const backend::BackgroundCaptureEvent& event) {
                QTimer::singleShot(0, playbackPanel, [playbackPanel, event]() {
                    const QImage background = frontend::qt::toQImage(event.frame);
                    playbackPanel->onBackgroundAutoCaptured(background, event.frameIndex);
                });
            });
        }
    }
    
    setupCornerWidgets();
    
    auto *hdfReviewTab = new frontend::HdfReviewTab(backend_, ui->tabs);
    ui->tabs->addTab(connectTab_, tr("Connect"));
    ui->tabs->addTab(overviewTab_, tr("Overview"));
    ui->tabs->addTab(experimentTabs_, tr("Experiment"));
    ui->tabs->addTab(hdfReviewTab, tr("Review"));

    connect(connectTab_, &frontend::ConnectTab::connected, this, [this]()
            {
        // On connection, switch to Overview and enable ROI overlay by default.
        if (overviewTab_) {
            overviewTab_->setRoiOverlayVisible(true);
        }
        if (ui->tabs) {
            ui->tabs->setCurrentIndex(1); // Overview tab
        } });

    // Config conflicts are actionable alerts (issue #363/#361).
    if (auto* cfgTabs = previewPage->getConfigTabs()) {
        connect(cfgTabs, &frontend::ConfigTabs::documentStateChanged, this, [this, cfgTabs]() {
            if (!alertModel_) return;
            if (cfgTabs->appConfigDocument().conflict) {
                alertModel_->raise(QStringLiteral("config.conflict"), frontend::AlertSeverity::Warning,
                                   tr("config.json changed elsewhere while it has unsaved edits in the inspector."),
                                   tr("Use Reset to load the file or Save to overwrite it."));
            } else {
                alertModel_->resolve(QStringLiteral("config.conflict"));
            }
        });
    }

    // Issue #364: one acknowledged apply/persist path for the Monitoring tune
    // panel. The panel asks the config watcher (request), the watcher writes
    // the patched keys through the checked document store, applies the same
    // patch to the processing service and answers with a result; the panel
    // clears Dirty only on a confirmed result. File changes (external or our
    // own, once) refresh the panel's baseline with the document fingerprint.
    {
        auto* configWatcher = previewPage->getConfigWatcher();
        connect(monitoringTab, &frontend::ExperimentMonitoringTab::applyRequested,
                configWatcher, &frontend::AppConfigWatcher::onApplyProcessingDraft);
        connect(configWatcher, &frontend::AppConfigWatcher::processingDraftApplied,
                monitoringTab, &frontend::ExperimentMonitoringTab::onApplyResult);
        connect(configWatcher, &frontend::AppConfigWatcher::configFileChanged, monitoringTab,
                [monitoringTab, configWatcher](const QString&) { monitoringTab->loadCurrentConfig(configWatcher->documentFingerprint()); });
        // Conflicts and failed applies are actionable alerts (issue #363).
        connect(monitoringTab, &frontend::ExperimentMonitoringTab::tuneStateChanged, this, [this, monitoringTab]() {
            if (!alertModel_) return;
            const auto& draft = monitoringTab->tuneDraft();
            if (draft.conflict()) {
                alertModel_->raise(QStringLiteral("tune.conflict"), frontend::AlertSeverity::Warning,
                                   tr("The processing configuration changed elsewhere while the Monitoring tune panel has unapplied edits."),
                                   tr("Revert in the tune panel to load the new values, then re-enter your edits."));
            } else {
                alertModel_->resolve(QStringLiteral("tune.conflict"));
            }
            if (!draft.applying() && !draft.lastError().isEmpty()) {
                alertModel_->raise(QStringLiteral("tune.apply"), frontend::AlertSeverity::Error,
                                   tr("Processing criteria were not applied: %1").arg(draft.lastError()),
                                   draft.savedNotApplied() ? tr("The file was updated but processing reports different values; Revert and check the configuration.")
                                                           : tr("Fix the reported problem and click Apply changes again."));
            } else if (!draft.applying()) {
                alertModel_->resolve(QStringLiteral("tune.apply"));
            }
        });
    }

    // Delivery mode: combo change -> persist to active profile + refresh badge;
    // config (re)load -> reflect in combo + badge without re-persisting.
    {
        auto* configWatcher = previewPage->getConfigWatcher();
        connect(connectTab_, &frontend::ConnectTab::deliveryModeChanged,
                this, [this, configWatcher](camera::common::FrameDeliveryMode mode) {
                    configWatcher->writeBackCameraConfig(mode);
                    updateDeliveryModeBadge();
                });
        connect(configWatcher, &frontend::AppConfigWatcher::deliveryModeLoaded,
                this, [this](camera::common::FrameDeliveryMode mode) {
                    if (connectTab_)
                        connectTab_->syncDeliveryMode(mode);
                    updateDeliveryModeBadge();
                });
        // The initial config load happened inside PreviewPage's constructor,
        // before these connections existed; sync once now.
        connectTab_->syncDeliveryMode(configWatcher->loadedDeliveryMode());
        updateDeliveryModeBadge();
    }

    connect(overviewTab_, &frontend::OverviewTab::roiChanged,
            monitoringTab, &frontend::ExperimentMonitoringTab::updateRoiDisplay);
    connect(overviewTab_, &frontend::OverviewTab::roiChanged,
            this, [this](int offsetX, int offsetY, int width, int height) {
        if (roiLabel_)
            roiLabel_->setText(tr("ROI: %1 x %2 @ (%3, %4)").arg(width).arg(height).arg(offsetX).arg(offsetY));
        backend::services::ProcessingService::Roi roi{};
        roi.x = offsetX;
        roi.y = offsetY;
        roi.w = width;
        roi.h = height;
        backend_.processing().setRealtimeRoi(roi);
    });
    // Initialize displays and processing ROI with current values
    {
        int ox = static_cast<int>(overviewTab_->roiPosition().x());
        int oy = static_cast<int>(overviewTab_->roiPosition().y());
        int w = overviewTab_->roiWidth();
        int h = overviewTab_->roiHeight();
        monitoringTab->updateRoiDisplay(ox, oy, w, h);
        if (roiLabel_)
            roiLabel_->setText(tr("ROI: %1 x %2 @ (%3, %4)").arg(w).arg(h).arg(ox).arg(oy));
        backend::services::ProcessingService::Roi initialRoi{};
        initialRoi.x = ox;
        initialRoi.y = oy;
        initialRoi.w = w;
        initialRoi.h = h;
        backend_.processing().setRealtimeRoi(initialRoi);
    }

    connect(connectTab_, &frontend::ConnectTab::noCamerasFound, this, &MainWindow::onNoCamerasFound);

    // Device init manager runs camera and nanopositioner auto-connect off the UI thread
    initManager_ = new frontend::DeviceInitManager(backend_, this);
    initManager_->setConnectTab(connectTab_);
    initManager_->setNanopositionerTab(sidebarWidget_ ? sidebarWidget_->nanopositionerTab() : nullptr);
    connectTab_->setDeviceInitManager(initManager_);

    // Connect tab change signal for auto-applying camera scripts
    connect(ui->tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Initialize button states (start enabled, stop disabled)
    updateExperimentButtonStates();
    
    // Initialize tab states (all tabs enabled initially since no experiment is active)
    updateTabStates();

    // Auto-connect on startup (camera at 400 ms, then nanopositioner after camera completes)
    initManager_->start();

    // Quiet update check on startup (only prompts if an update is available)
    if (!autoUpdateDisabled)
    {
        QTimer::singleShot(1500, this, [this]() {
            if (updater_) updater_->checkForUpdates(false);
        });
    }

    // Issue #358: one restoration path (default when nothing valid is saved).
    restoreWindowGeometry();
}

MainWindow::~MainWindow() {
    backend_.setBackgroundCaptureCallback({});
    // Block on any in-flight async flush before members are destroyed; the
    // watcher's own destructor would not wait for the running task.
    if (flushWatcher_ && flushWatcher_->isRunning()) {
        flushWatcher_->waitForFinished();
    }
    if (finalizeWatcher_ && finalizeWatcher_->isRunning()) {
        finalizeWatcher_->waitForFinished();
    }
    // Stop all timers that access backend_ via callbacks before the UI is
    // torn down. The OverviewTab 50fps timer fires onTick() which calls
    // backend_.playback().fetchLatest() — if the timer fires after the
    // widget tree is partially destroyed, that's a use-after-free.
    if (statsTimer_) {
        statsTimer_->stop();
    }
    delete ui;
}

void MainWindow::setupSidebar()
{
    // Remove the existing tabs widget from the central widget layout
    ui->verticalLayout->removeWidget(ui->tabs);

    // The splitter is the single owner of sidebar geometry (issue #359).
    mainSplitter_ = new QSplitter(Qt::Horizontal, ui->centralwidget);
    mainSplitter_->setObjectName(QStringLiteral("mainSplitter"));
    mainSplitter_->setChildrenCollapsible(false);

    sidebarWidget_ = new frontend::SidebarWidget(backend_, mainSplitter_);
    mainSplitter_->addWidget(sidebarWidget_);
    mainSplitter_->addWidget(ui->tabs);
    mainSplitter_->setStretchFactor(0, 0);
    mainSplitter_->setStretchFactor(1, 1);
    ui->tabs->setMinimumWidth(frontend::geometry::kWorkspaceMinWidth);

    loadSidebarPreference();
    sidebarWidget_->setVisible(sidebarUserVisible_);
    mainSplitter_->setSizes({sidebarPreferredWidth_, 1000});
    connect(mainSplitter_, &QSplitter::splitterMoved, this, &MainWindow::onSplitterMoved);

    sidebarPersistTimer_ = new QTimer(this);
    sidebarPersistTimer_->setSingleShot(true);
    sidebarPersistTimer_->setInterval(300);
    connect(sidebarPersistTimer_, &QTimer::timeout, this, &MainWindow::saveSidebarPreference);
    layoutAdjustTimer_ = new QTimer(this);
    layoutAdjustTimer_->setSingleShot(true);
    layoutAdjustTimer_->setInterval(120);
    connect(layoutAdjustTimer_, &QTimer::timeout, this, &MainWindow::applySidebarLayout);

    ui->verticalLayout->addWidget(mainSplitter_);
}

QTabWidget* MainWindow::mainTabs() const { return ui->tabs; }

void MainWindow::loadSidebarPreference()
{
    QSettings settings;
    const int version = settings.value(QStringLiteral("Sidebar/LayoutVersion"), 0).toInt();
    if (version >= frontend::geometry::kSidebarLayoutVersion) {
        sidebarUserVisible_ = settings.value(QStringLiteral("Sidebar/Visible"), true).toBool();
        sidebarPreferredWidth_ = frontend::geometry::sanitizeSidebarPreferredWidth(
            settings.value(QStringLiteral("Sidebar/PreferredWidth")));
        return;
    }
    // One-time migration of the legacy keys written by SidebarWidget.
    sidebarUserVisible_ = !settings.value(QStringLiteral("Sidebar/Collapsed"), false).toBool();
    sidebarPreferredWidth_ = frontend::geometry::sanitizeSidebarPreferredWidth(
        settings.value(QStringLiteral("Sidebar/ExpandedWidth")));
    saveSidebarPreference();
}

void MainWindow::saveSidebarPreference()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Sidebar/LayoutVersion"), frontend::geometry::kSidebarLayoutVersion);
    settings.setValue(QStringLiteral("Sidebar/Visible"), sidebarUserVisible_);
    settings.setValue(QStringLiteral("Sidebar/PreferredWidth"), sidebarPreferredWidth_);
}

bool MainWindow::isHardwarePanelVisible() const
{
    return sidebarWidget_ && sidebarWidget_->isVisible();
}

void MainWindow::updateHardwarePanelAction()
{
    if (!hardwarePanelAct_) return;
    const bool visible = isHardwarePanelVisible();
    const QSignalBlocker block(hardwarePanelAct_);
    hardwarePanelAct_->setChecked(visible);
    hardwarePanelAct_->setText(visible ? tr("Hide hardware panel") : tr("Show hardware panel"));
    hardwarePanelAct_->setToolTip(visible ? tr("Hide the hardware panel (statistics, background, nanopositioner, pump)")
                                          : tr("Show the hardware panel (statistics, background, nanopositioner, pump)"));
    if (hardwarePanelBtn_) {
        hardwarePanelBtn_->setText(visible ? QStringLiteral("◀") : QStringLiteral("▶"));
        hardwarePanelBtn_->setToolTip(hardwarePanelAct_->toolTip());
        hardwarePanelBtn_->setAccessibleName(hardwarePanelAct_->text());
    }
}

void MainWindow::setHardwarePanelVisible(bool visible)
{
    if (!sidebarWidget_ || !mainSplitter_) return;
    sidebarUserVisible_ = visible;
    if (!visible) {
        // Capture the actual expanded width as the preference (never the
        // collapsed/forced-narrow value), then hide through the splitter.
        if (sidebarWidget_->isVisible() && !sidebarHiddenForSpace_) {
            const QList<int> sizes = mainSplitter_->sizes();
            if (!sizes.isEmpty() && sizes[0] >= frontend::geometry::kSidebarMinWidth) {
                sidebarPreferredWidth_ = std::clamp(sizes[0], frontend::geometry::kSidebarMinWidth,
                                                    frontend::geometry::kSidebarMaxWidth);
            }
        }
        const bool hadFocus = sidebarWidget_->isAncestorOf(QApplication::focusWidget());
        {
            frontend::detail::ScopedFlag guard(applyingSidebarLayout_);
            sidebarWidget_->hide();
        }
        sidebarHiddenForSpace_ = false;
        if (hadFocus && hardwarePanelBtn_) hardwarePanelBtn_->setFocus(Qt::OtherFocusReason);
    } else {
        applySidebarLayout();
    }
    updateHardwarePanelAction();
    if (sidebarPersistTimer_) sidebarPersistTimer_->start();
}

void MainWindow::applySidebarLayout()
{
    if (!sidebarWidget_ || !mainSplitter_ || applyingSidebarLayout_) return;
    frontend::detail::ScopedFlag guard(applyingSidebarLayout_);
    if (!sidebarUserVisible_) {
        if (sidebarWidget_->isVisible()) sidebarWidget_->hide();
        return;
    }
    const int contents = mainSplitter_->contentsRect().width();
    const int handle = mainSplitter_->handleWidth();
    const auto fit = frontend::geometry::fitSidebarWidth(sidebarPreferredWidth_, contents, handle);
    if (!fit.fits) {
        // Even the compact panel would push the workspace below its minimum:
        // keep the workspace usable, remember the intent, re-show when space
        // returns (resizeEvent). Never enlarge the outer window.
        if (sidebarWidget_->isVisible()) sidebarWidget_->hide();
        sidebarHiddenForSpace_ = true;
        statusBar()->showMessage(tr("Window too narrow for the hardware panel; enlarge the window to show it."), 4000);
        updateHardwarePanelAction();
        return;
    }
    sidebarHiddenForSpace_ = false;
    if (!sidebarWidget_->isVisible()) sidebarWidget_->show();
    QList<int> sizes = mainSplitter_->sizes();
    if (sizes.size() < 2) return;
    const int total = sizes[0] + sizes[1];
    if (sizes[0] != fit.width) {
        sizes[0] = fit.width;
        sizes[1] = std::max(0, total - fit.width);
        mainSplitter_->setSizes(sizes);
    }
    updateHardwarePanelAction();
}

void MainWindow::onSplitterMoved(int pos, int index)
{
    Q_UNUSED(pos);
    Q_UNUSED(index);
    if (applyingSidebarLayout_ || !sidebarWidget_ || !sidebarWidget_->isVisible() || sidebarHiddenForSpace_) return;
    const QList<int> sizes = mainSplitter_->sizes();
    if (sizes.isEmpty() || sizes[0] < frontend::geometry::kSidebarCompactWidth) return;
    // A user-driven drag defines the preference (clamped, never 0).
    sidebarPreferredWidth_ = std::clamp(sizes[0], frontend::geometry::kSidebarMinWidth,
                                        frontend::geometry::kSidebarMaxWidth);
    if (sidebarPersistTimer_) sidebarPersistTimer_->start();
}

// ---- Issue #358: window geometry ------------------------------------------

void MainWindow::setAvailableGeometryOverrideForTests(const QRect& available)
{
    availableGeometryOverride_ = available;
}

QRect MainWindow::availableDesktopForWindow() const
{
    if (availableGeometryOverride_.isValid()) return availableGeometryOverride_;
    const QScreen* s = screen();
    if (!s && windowHandle()) s = windowHandle()->screen();
    if (!s) s = QGuiApplication::primaryScreen();
    return s ? s->availableGeometry() : QRect();
}

void MainWindow::restoreWindowGeometry()
{
    QSettings settings;
    const int version = settings.value(QStringLiteral("Window/LayoutVersion"), 0).toInt();
    std::optional<QRect> saved;
    if (settings.contains(QStringLiteral("Window/Rect"))) {
        const QRect r = settings.value(QStringLiteral("Window/Rect")).toRect();
        if (r.isValid()) saved = r;
    }
    QList<QRect> screens;
    if (availableGeometryOverride_.isValid()) {
        screens.push_back(availableGeometryOverride_);
    } else {
        for (const QScreen* s : QGuiApplication::screens()) screens.push_back(s->availableGeometry());
    }
    const auto decision = frontend::geometry::resolveWindowGeometry(saved, version, screens);
    restoredGeometryFromSettings_ = decision.usedSaved;
    // decision.geometry is a frame rectangle; apply the client size and
    // position (the frame margin is validated after show).
    resize(decision.geometry.size());
    move(decision.geometry.topLeft());
    if (decision.usedSaved && settings.value(QStringLiteral("Window/Maximized"), false).toBool()) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
    SPDLOG_INFO("MainWindow: geometry {} {}x{}@{},{} (screen {}{})", decision.usedSaved ? "restored" : "default",
                decision.geometry.width(), decision.geometry.height(), decision.geometry.x(), decision.geometry.y(),
                decision.screenIndex, decision.clamped ? ", clamped" : "");
}

void MainWindow::saveWindowGeometry()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Window/LayoutVersion"), frontend::geometry::kWindowLayoutVersion);
    const QRect rect = isMaximized() ? normalGeometry() : frameGeometry();
    settings.setValue(QStringLiteral("Window/Rect"), rect);
    settings.setValue(QStringLiteral("Window/Maximized"), isMaximized());
}

void MainWindow::ensureWindowFitsScreen()
{
    if (fittingWindow_ || isMaximized() || isFullScreen()) return;
    const QRect available = availableDesktopForWindow();
    if (!available.isValid()) return;
    const QRect frame = frameGeometry();
    if (available.contains(frame)) return;
    frontend::detail::ScopedFlag guard(fittingWindow_);
    const QSize frameMargin = frame.size() - size();
    const QSize minimum = minimumSizeHint().expandedTo(minimumSize()) + frameMargin;
    const QRect fitted = frontend::geometry::clampToAvailable(frame, available, minimum);
    SPDLOG_INFO("MainWindow: window {}x{}@{},{} exceeds the available desktop {}x{}@{},{}; fitting to {}x{}@{},{}",
                frame.width(), frame.height(), frame.x(), frame.y(), available.width(), available.height(),
                available.x(), available.y(), fitted.width(), fitted.height(), fitted.x(), fitted.y());
    resize(fitted.size() - frameMargin);
    move(fitted.topLeft());
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (firstShowDone_) return;
    firstShowDone_ = true;
    // Validate the actual decorated geometry once the window exists, then
    // follow screen changes with a coalesced adjustment.
    QTimer::singleShot(0, this, [this]() {
        ensureWindowFitsScreen();
        applySidebarLayout();
    });
    if (QWindow* handle = windowHandle()) {
        // One coalesced adjustment per screen change; each screen is hooked
        // once (tracked by object name set on a per-window property).
        auto hookScreen = [this](QScreen* s) {
            if (!s) return;
            const QString key = QStringLiteral("mib_screen_hooked_%1").arg(reinterpret_cast<quintptr>(this));
            if (s->property(key.toUtf8().constData()).toBool()) return;
            s->setProperty(key.toUtf8().constData(), true);
            connect(s, &QScreen::availableGeometryChanged, this, [this](const QRect&) {
                QTimer::singleShot(250, this, [this]() { ensureWindowFitsScreen(); });
            });
        };
        hookScreen(handle->screen());
        connect(handle, &QWindow::screenChanged, this, [hookScreen](QScreen* s) { hookScreen(s); });
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // Coalesced sidebar re-fit (may re-show a panel hidden for space or
    // clamp one that no longer fits); never resizes the outer window.
    if (layoutAdjustTimer_ && !applyingSidebarLayout_) layoutAdjustTimer_->start();
}

void MainWindow::setupCornerWidgets() {
    // Create corner widget for experiment controls (on same row as tabs)
    auto *experimentControlsWidget = new QWidget(this);
    auto *experimentControlsLayout = new QHBoxLayout(experimentControlsWidget);
    experimentControlsLayout->setContentsMargins(5, 0, 5, 0);
    experimentControlsLayout->setSpacing(5);
    
    // Run lifecycle state as text + glyph (issue #363), never color alone.
    runStatusWidget_ = new frontend::RunStatusWidget(experimentControlsWidget);
    runStatusWidget_->bind(runStatusModel_);
    experimentControlsLayout->addWidget(runStatusWidget_);
    
    // ROI display label
    roiLabel_ = new QLabel(tr("ROI: --"), experimentControlsWidget);
    roiLabel_->setStyleSheet("font-weight: bold; padding: 0 8px;");
    experimentControlsLayout->addWidget(roiLabel_);

    // Create push buttons and connect them to actions
    startExperimentBtn_ = new QPushButton(startExperimentAct_->text(), experimentControlsWidget);
    startExperimentBtn_->setObjectName(QStringLiteral("startExperimentBtn"));
    stopExperimentBtn_ = new QPushButton(stopExperimentAct_->text(), experimentControlsWidget);
    stopExperimentBtn_->setObjectName(QStringLiteral("stopExperimentBtn"));
    connect(startExperimentBtn_, &QPushButton::clicked, startExperimentAct_, &QAction::trigger);
    connect(stopExperimentBtn_, &QPushButton::clicked, stopExperimentAct_, &QAction::trigger);
    experimentControlsLayout->addWidget(startExperimentBtn_);
    experimentControlsLayout->addWidget(stopExperimentBtn_);
    
    // Add controls widget to the corner of the tab bar (same row as tabs)
    experimentTabs_->setCornerWidget(experimentControlsWidget, Qt::TopRightCorner);
    
    // Create corner widget for camera controls (on same row as main tabs)
    auto *cameraControlsWidget = new QWidget(this);
    auto *cameraControlsLayout = new QHBoxLayout(cameraControlsWidget);
    cameraControlsLayout->setContentsMargins(5, 0, 5, 0);
    cameraControlsLayout->setSpacing(5);
    
    // Buttons are pure presentations of the controller's shared actions:
    // text/enabled/tooltip follow the action, clicks trigger the action.
    QAction* startAct = cameraController_->startAction();
    QAction* stopAct = cameraController_->stopAction();
    startCameraBtn_ = new QPushButton(startAct->text(), cameraControlsWidget);
    startCameraBtn_->setObjectName(QStringLiteral("startCameraBtn"));
    stopCameraBtn_ = new QPushButton(stopAct->text(), cameraControlsWidget);
    stopCameraBtn_->setObjectName(QStringLiteral("stopCameraBtn"));
    connect(startCameraBtn_, &QPushButton::clicked, startAct, &QAction::trigger);
    connect(stopCameraBtn_, &QPushButton::clicked, stopAct, &QAction::trigger);
    auto bindButton = [](QPushButton* button, QAction* action) {
        button->setEnabled(action->isEnabled());
        button->setToolTip(action->toolTip());
        QObject::connect(action, &QAction::changed, button, [button, action]() {
            button->setEnabled(action->isEnabled());
            button->setToolTip(action->toolTip());
            button->setText(action->text());
        });
    };
    bindButton(startCameraBtn_, startAct);
    bindButton(stopCameraBtn_, stopAct);
    cameraControlsLayout->addWidget(startCameraBtn_);
    cameraControlsLayout->addWidget(stopCameraBtn_);
    
    // Add controls widget to the corner of the main tab bar (same row as tabs)
    ui->tabs->setCornerWidget(cameraControlsWidget, Qt::TopRightCorner);

    // Issue #359: stable reopen/hide affordance for the hardware panel in the
    // main chrome (left tab-bar corner), backed by one checkable action.
    hardwarePanelAct_ = new QAction(this);
    hardwarePanelAct_->setObjectName(QStringLiteral("hardwarePanelAct"));
    hardwarePanelAct_->setCheckable(true);
    hardwarePanelAct_->setShortcut(QKeySequence(tr("Ctrl+Shift+H")));
    connect(hardwarePanelAct_, &QAction::triggered, this, [this](bool checked) { setHardwarePanelVisible(checked); });
    ui->settingsMenu->addSeparator();
    ui->settingsMenu->addAction(hardwarePanelAct_);
    hardwarePanelBtn_ = new QToolButton(this);
    hardwarePanelBtn_->setObjectName(QStringLiteral("hardwarePanelBtn"));
    hardwarePanelBtn_->setDefaultAction(hardwarePanelAct_);
    hardwarePanelBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    hardwarePanelBtn_->setAutoRaise(true);
    hardwarePanelBtn_->setFocusPolicy(Qt::StrongFocus);
    ui->tabs->setCornerWidget(hardwarePanelBtn_, Qt::TopLeftCorner);
    updateHardwarePanelAction();
    // The button shows only the glyph; the action keeps the full text for
    // menus/accessibility.
    connect(hardwarePanelAct_, &QAction::changed, this, [this]() {
        if (hardwarePanelBtn_ && hardwarePanelAct_)
            hardwarePanelBtn_->setText(hardwarePanelAct_->isChecked() ? QStringLiteral("◀") : QStringLiteral("▶"));
    });
    hardwarePanelBtn_->setText(hardwarePanelAct_->isChecked() ? QStringLiteral("◀") : QStringLiteral("▶"));
}

void MainWindow::updateExperimentButtonStates()
{
    if (startExperimentAct_ && stopExperimentAct_)
    {
        // Disable start when experiment is active, enable when inactive
        startExperimentAct_->setEnabled(!experimentActive_);
        // Disable stop when experiment is inactive, enable when active
        stopExperimentAct_->setEnabled(experimentActive_);
    }
    
    // Update button enabled states to match actions
    if (startExperimentBtn_)
    {
        startExperimentBtn_->setEnabled(startExperimentAct_ ? startExperimentAct_->isEnabled() : false);
    }
    if (stopExperimentBtn_)
    {
        stopExperimentBtn_->setEnabled(stopExperimentAct_ ? stopExperimentAct_->isEnabled() : false);
    }

    // Stop is a single in-flight operation: no second stop while finalizing.
    if (stopExperimentBtn_ && stopInProgress_) stopExperimentBtn_->setEnabled(false);
    if (startExperimentBtn_ && stopInProgress_) startExperimentBtn_->setEnabled(false);

    // Update tab states
    updateTabStates();
}

void MainWindow::updateTabStates()
{
    if (!ui->tabs)
        return;

    // Disable Overview tab (index 1) and Review tab (index 3) during experiment
    ui->tabs->setTabEnabled(1, !experimentActive_); // Overview
    ui->tabs->setTabEnabled(3, !experimentActive_); // Review
}

void MainWindow::onStartCapture()
{
    // Thin wrapper kept for the screenshot tour and legacy callers: the
    // controller owns guards, duplicate protection, and failure reporting.
    const auto r = cameraController_->requestStart();
    if (r.outcome == frontend::CameraCommandResult::Outcome::AlreadyInState)
    {
        QMessageBox::information(this, tr("Start Camera"), r.message);
    }
}

void MainWindow::onStopCapture()
{
    const auto r = cameraController_->requestStop();
    if (r.outcome == frontend::CameraCommandResult::Outcome::AlreadyInState)
    {
        QMessageBox::information(this, tr("Stop Camera"), r.message);
    }
}

void MainWindow::onCameraStateChanged(const frontend::CameraActionState& state)
{
    using Phase = frontend::CameraActionState::Phase;
    // Stats sampling + flush scheduling (onUpdateStats) must run whenever a
    // session is active, whichever route started it (issue #360: the old
    // Preview overlay path never armed this timer).
    const bool active = state.phase == Phase::Starting || state.phase == Phase::Running;
    if (active && !statsTimer_->isActive())
    {
        statsTimer_->start();
    }
    else if (!active && statsTimer_->isActive() && !experimentActive_ && !flushInProgress_)
    {
        statsTimer_->stop();
    }

    if (!statusLabel_) return;
    switch (state.phase)
    {
    case Phase::Failed: {
        const QString message = state.failureMessage.isEmpty() ? tr("Camera start failed")
                                                               : tr("Camera failed: %1").arg(state.failureMessage);
        statusLabel_->setText(message);
        // Actionable and persistent (issue #363): survives every stats tick.
        if (alertModel_)
            alertModel_->raise(QStringLiteral("camera.start"), frontend::AlertSeverity::Error, message,
                               tr("Check the camera connection/selection in Connect, then Start Live View again."));
        if (runStatusModel_ && !experimentActive_) runStatusModel_->setIdlePhase(frontend::RunPhase::Idle, tr("camera failed"));
        break;
    }
    case Phase::Idle:
        // onUpdateStats stopped: leave the operator a definite state.
        statusLabel_->setText(state.phaseText());
        if (runStatusModel_ && !experimentActive_) runStatusModel_->setIdlePhase(frontend::RunPhase::Idle);
        break;
    case Phase::Running:
        statusLabel_->setText(state.phaseText());
        if (alertModel_) alertModel_->resolve(QStringLiteral("camera.start"));
        if (runStatusModel_ && !experimentActive_) runStatusModel_->setIdlePhase(frontend::RunPhase::CameraRunning);
        break;
    default:
        statusLabel_->setText(state.phaseText());
        break;
    }
}

// ---- Issue #363: status surfaces ---------------------------------------------

void MainWindow::setupStatusSurfaces()
{
    alertModel_ = new frontend::UiAlertModel(this);
    runStatusModel_ = new frontend::RunStatusModel(this);
    alertBanner_ = new frontend::AlertBanner(ui->centralwidget);
    alertBanner_->bind(alertModel_);
    // Above the workspace splitter (inserted before setupSidebar adds it), so
    // it wraps across the full width and is never covered by a tab.
    ui->verticalLayout->insertWidget(0, alertBanner_);

    auto* diagnosticsBtn = new QToolButton(this);
    diagnosticsBtn->setObjectName(QStringLiteral("diagnosticsBtn"));
    diagnosticsBtn->setText(tr("Diagnostics…"));
    diagnosticsBtn->setToolTip(tr("Detailed acquisition/processing/transport telemetry and identities"));
    diagnosticsBtn->setAutoRaise(true);
    diagnosticsBtn->setFocusPolicy(Qt::StrongFocus);
    connect(diagnosticsBtn, &QToolButton::clicked, this, &MainWindow::showDiagnostics);
    ui->statusbar->addPermanentWidget(diagnosticsBtn);
    auto* diagnosticsAct = new QAction(tr("Diagnostics…"), this);
    diagnosticsAct->setObjectName(QStringLiteral("diagnosticsAct"));
    connect(diagnosticsAct, &QAction::triggered, this, &MainWindow::showDiagnostics);
    ui->helpMenu->addAction(diagnosticsAct);
}

void MainWindow::showDiagnostics()
{
    if (!diagnosticsDialog_) {
        diagnosticsDialog_ = new QDialog(this);
        diagnosticsDialog_->setObjectName(QStringLiteral("diagnosticsDialog"));
        diagnosticsDialog_->setWindowTitle(tr("Diagnostics"));
        diagnosticsDialog_->setModal(false);
        diagnosticsDialog_->resize(560, 420);
        auto* layout = new QVBoxLayout(diagnosticsDialog_);
        diagnosticsText_ = new QPlainTextEdit(diagnosticsDialog_);
        diagnosticsText_->setObjectName(QStringLiteral("diagnosticsText"));
        diagnosticsText_->setReadOnly(true);
        layout->addWidget(diagnosticsText_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, diagnosticsDialog_);
        connect(buttons, &QDialogButtonBox::rejected, diagnosticsDialog_, &QDialog::hide);
        layout->addWidget(buttons);
    }
    diagnosticsDialog_->show();
    diagnosticsDialog_->raise();
    refreshDiagnostics(sampleStats());
}

QString MainWindow::compactStatusText() const { return compactStatus_; }

void MainWindow::refreshDiagnostics(const frontend::StatisticsData& data)
{
    if (!diagnosticsDialog_ || !diagnosticsDialog_->isVisible() || !diagnosticsText_) return;
    const auto& cap = backend_.capture();
    const auto telemetry = cap.telemetrySnapshot();
    const auto core = backend_.processing().activeProcessingCoreIdentity();
    const auto lifecycle = cap.lifecycleSnapshot();
    QStringList lines;
    lines << tr("Capture session: %1 (generation %2)").arg(QLatin1String(backend::services::toString(lifecycle.state))).arg(lifecycle.generation);
    lines << tr("Delivery mode: requested %1, confirmed %2")
                 .arg(QLatin1String(camera::common::toString(cap.activeDeliveryMode())),
                      cap.stats().deliveryModeConfirmed.load() ? tr("yes") : tr("no"));
    lines << tr("Camera frame rate: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.captureFrameRate));
    lines << tr("Camera data rate (MB/s): %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.captureDataRateMBps));
    lines << tr("Frames delivered: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.framesDelivered));
    lines << tr("Transport lost frames: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.transportLostFrames));
    lines << tr("Intentionally discarded (LatestFrame): %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.intentionallyDiscardedFrames));
    lines << tr("Buffer underruns: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.bufferUnderruns));
    lines << tr("SDK completed queue depth: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.sdkCompletedQueueDepth));
    lines << tr("SDK input buffers: %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.sdkInputBufferCount));
    lines << tr("Frame age (µs): %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.frameAgeUs));
    lines << tr("Publish latency (µs): %1").arg(frontend::StatsDisplayManager::formatMetric(telemetry.publishLatencyUs));
    lines << tr("Timestamps: %1").arg(QString::fromStdString(camera::common::describe(cap.timestampDescriptor())));
    lines << QString();
    lines << tr("Display: %1 fps").arg(QString::number(data.displayFps, 'f', 1));
    lines << tr("Algorithm: %1 µs (age %2 ms)").arg(QString::number(data.algoAvgUs, 'f', 1)).arg(QString::number(data.algoAvgUsAgeMs, 'f', 0));
    lines << tr("Valid: %1/s   Invalid: %2/s   Flushed (valid): %3").arg(QString::number(data.validFps, 'f', 1), QString::number(data.invalidFps, 'f', 1)).arg(static_cast<qulonglong>(data.totalValidFlushed));
    lines << tr("Ring width (median): %1 (age %2 ms)").arg(QString::number(data.meanRingRatio, 'f', 3)).arg(QString::number(data.meanRingRatioAgeMs, 'f', 0));
    lines << tr("Experiment: %1; buffered valid=%2 invalid=%3; flushing=%4; runtime %5 s")
                 .arg(data.experimentActive ? tr("active") : tr("inactive"))
                 .arg(static_cast<qulonglong>(data.validBuffered)).arg(static_cast<qulonglong>(data.invalidBuffered))
                 .arg(data.flushInProgress ? tr("yes") : tr("no")).arg(QString::number(data.experimentRuntimeSeconds, 'f', 0));
    lines << QString();
    lines << tr("Processing core: %1 (contract %2, source %3)").arg(QString::fromStdString(core.version)).arg(core.contractVersion).arg(QString::fromStdString(core.source));
    lines << tr("Core artifact sha256: %1").arg(QString::fromStdString(core.artifactSha256));
    lines << tr("Process memory: %1 MB").arg(QString::number(backend::Tools::getProcessMemoryMB(), 'f', 1));

    // Issue #370: byte-budgeted owners + presentation counters (distinct from loss).
    lines << QString();
    lines << tr("Memory owners (current / peak / budget; unknown vendor memory is reported as unknown):");
    const auto budget = backend_.memoryBudgetSnapshot();
    auto mb = [](uint64_t bytes) { return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1); };
    for (const auto& o : budget.owners) {
        QString line = QStringLiteral("  %1: ").arg(QString::fromStdString(o.name));
        if (o.knowledge == backend::diagnostics::MemoryKnowledge::Unknown) {
            line += tr("unknown");
        } else {
            line += tr("%1 / %2 MB").arg(mb(o.currentBytes), mb(o.peakBytes));
            if (o.capacityBytes > 0) line += tr(" / budget %1 MB%2").arg(mb(o.capacityBytes), o.overBudget() ? tr(" OVER") : QString());
            line += tr(", %1 / %2 items").arg(static_cast<qulonglong>(o.currentCount)).arg(static_cast<qulonglong>(o.peakCount));
            if (o.capacityCount > 0) line += tr(" / cap %1").arg(static_cast<qulonglong>(o.capacityCount));
            if (o.evictedByBudget > 0) line += tr(", evicted/replaced %1").arg(static_cast<qulonglong>(o.evictedByBudget));
            if (o.knowledge == backend::diagnostics::MemoryKnowledge::Estimated) line += tr(" (estimated)");
        }
        lines << line;
    }
    lines << tr("Accounted host memory: %1 MB across %2 owners%3")
                 .arg(mb(budget.accountedBytes()))
                 .arg(budget.owners.size())
                 .arg(budget.hasUnknownOwner() ? tr(" (plus unknown vendor buffers)") : QString());
    if (experimentTabs_ && experimentTabs_->count() > 0) {
        if (auto* previewPage = qobject_cast<frontend::PreviewPage*>(experimentTabs_->widget(0))) {
            if (auto* panel = previewPage->getPlaybackPanel()) {
                lines << tr("Display: presented %1 frames, skipped by display %2 (presentation only; not acquisition, processing or persistence loss)")
                             .arg(static_cast<qulonglong>(panel->displayFramesPresented()))
                             .arg(static_cast<qulonglong>(panel->displayFramesSkipped()));
            }
        }
    }
    diagnosticsText_->setPlainText(lines.join(QLatin1Char('\n')));
}

namespace {
QString gateStatusLabel(backend::app::GateStatus status)
{
    switch (status) {
    case backend::app::GateStatus::Pass: return QStringLiteral("OK");
    case backend::app::GateStatus::Warn: return QStringLiteral("Warning");
    case backend::app::GateStatus::Fail: return QStringLiteral("Blocked");
    case backend::app::GateStatus::Unavailable: return QStringLiteral("Unknown");
    case backend::app::GateStatus::NotRequired: return QStringLiteral("Not required");
    }
    return QStringLiteral("?");
}
} // namespace

bool MainWindow::explainReadiness(const backend::app::ExperimentReadinessSnapshot& readiness)
{
    if (readiness.ready)
        return true;
    QStringList lines;
    bool onlyFault = true;
    for (const auto& g : readiness.gates)
    {
        if (!g.blocksStart())
            continue;
        if (g.id != "lifecycle.fault")
            onlyFault = false;
        QString line = QStringLiteral("%1 — %2: %3")
                           .arg(gateStatusLabel(g.status), QString::fromStdString(g.id),
                                QString::fromStdString(g.reason));
        if (!g.remediation.empty())
            line += QStringLiteral("\n    → %1").arg(QString::fromStdString(g.remediation));
        lines << line;
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("Start Experiment"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("The experiment cannot start until every readiness check passes."));
    box.setInformativeText(lines.join(QStringLiteral("\n\n")));
    QPushButton* ackBtn = nullptr;
    if (onlyFault)
        ackBtn = box.addButton(tr("Acknowledge fault and re-check"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (ackBtn && box.clickedButton() == ackBtn)
    {
        backend_.experiment().clearUnresolvedFault();
        SPDLOG_INFO("MainWindow: operator acknowledged the unresolved experiment fault");
    }
    statusLabel_->setText(tr("Experiment not ready: %1")
                              .arg(QString::fromStdString([&] {
                                  std::string ids;
                                  for (const auto& id : readiness.blockingGateIds())
                                      ids += (ids.empty() ? "" : ", ") + id;
                                  return ids;
                              }())));
    return false;
}

void MainWindow::onStartExperiment()
{
    if (experimentActive_ || stopInProgress_)
    {
        QMessageBox::information(this, tr("Experiment"),
                                 tr("Experiment is already running"));
        return;
    }

    auto& coordinator = backend_.experiment();
    auto &processing = backend_.processing();

    // Multi-image series capture requires inline realtime processing; decide
    // this before the preflight so the frozen snapshot records the mode the
    // run actually uses.
    restoreRealtimeModeAfterExperiment_ = false;
    realtimeModeBeforeExperiment_ =
        static_cast<int>(processing.getRealtimeProcessingMode());
    const auto processingConfig = processing.getProcessingConfig();
    const bool multiImageSeriesEnabled =
        processingConfig.multi_image_enabled && processingConfig.multi_image_count > 1;
    const bool needsInlineForSeries =
        multiImageSeriesEnabled &&
        processing.getRealtimeProcessingMode() ==
            backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch;

    // Preflight (issue #369): the backend evaluates every gate against the
    // actual state. Blocking gates are explained with their remediation; a
    // destination-less evaluation only leaves storage.output unknown.
    {
        auto preflight = coordinator.evaluateReadiness();
        std::vector<backend::app::ReadinessGate> blockers;
        for (const auto& g : preflight.gates)
            if (g.blocksStart() && g.id != "storage.output") blockers.push_back(g);
        if (!blockers.empty())
        {
            preflight.ready = false;
            preflight.gates = blockers;
            explainReadiness(preflight);
            return;
        }
    }

    // Guard: Latest Frame intentionally discards frames, so a recording made in
    // that mode can be incomplete. Require an explicit acknowledgement (#332).
    bool acknowledgeLatestFrameDrops = false;
    if (backend_.capture().activeDeliveryMode() ==
        camera::common::FrameDeliveryMode::LatestFrame)
    {
        QMessageBox box(this);
        box.setWindowTitle(tr("Start Experiment"));
        box.setIcon(QMessageBox::Warning);
        box.setText(tr("Latest Frame prioritizes low latency and may intentionally discard frames. "
                       "The recorded sequence may be incomplete."));
        QPushButton* switchBtn = box.addButton(tr("Switch to Every Frame"), QMessageBox::AcceptRole);
        QPushButton* continueBtn = box.addButton(tr("Continue with Latest Frame"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(switchBtn);
        box.exec();
        if (box.clickedButton() == switchBtn)
        {
            // Same setConfig + persist path as the ConnectTab combo; the
            // running capture is not restarted, so the new mode takes effect
            // at the next capture start (the badge keeps showing the
            // backend-confirmed mode until then).
            if (connectTab_)
                connectTab_->setDeliveryMode(camera::common::FrameDeliveryMode::EveryFrame);
            updateDeliveryModeBadge();
            acknowledgeLatestFrameDrops = true; // this session still runs LatestFrame
        }
        else if (box.clickedButton() == continueBtn)
        {
            acknowledgeLatestFrameDrops = true;
        }
        else
        {
            return; // Cancel: abort experiment start
        }
    }

    // Show file dialog to select HDF5 save location
    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Save Experiment Data"),
        "",
        tr("HDF5 Files (*.h5 *.hdf5);;All Files (*)"));

    if (filePath.isEmpty())
    {
        // User cancelled
        return;
    }

    if (needsInlineForSeries)
    {
        processing.setRealtimeProcessingMode(
            backend::services::ProcessingService::RealtimeProcessingMode::Inline);
        restoreRealtimeModeAfterExperiment_ = true;
        QMessageBox::information(
            this,
            tr("Start Experiment"),
            tr("Multi-image series capture requires inline realtime processing.\n"
               "This experiment will run in inline mode so series images remain reviewable.\n"
               "Your previous realtime mode will be restored when the experiment stops."));
        SPDLOG_INFO("MainWindow: switched realtime mode async_batch -> inline for multi-image experiment");
    }

    // Final readiness with the destination, then the Start transaction with
    // that exact generation. The backend opens the file, freezes and
    // persists the run snapshot, and only then enters Running; any change
    // between the two calls is refused as stale rather than raced.
    backend::app::ExperimentStartRequest request;
    request.outputPath = filePath.toStdString();
    request.acknowledgeLatestFrameDrops = acknowledgeLatestFrameDrops;
    const auto readiness = coordinator.evaluateReadiness(request.outputPath, request.profileId);
    if (!explainReadiness(readiness))
    {
        restoreRealtimeModeIfNeeded();
        return;
    }
    request.readinessGeneration = readiness.generation;
    const auto result = coordinator.start(request);
    switch (result.outcome)
    {
    case backend::app::ExperimentStartOutcome::Started:
        break;
    case backend::app::ExperimentStartOutcome::StaleReadiness:
        restoreRealtimeModeIfNeeded();
        QMessageBox::warning(this, tr("Start Experiment"),
                             tr("The configuration changed while the experiment was being prepared, "
                                "so the readiness check is no longer valid.\n\n%1\n\nPlease start again.")
                                 .arg(QString::fromStdString(result.message)));
        statusLabel_->setText(tr("Experiment start refused: readiness changed"));
        return;
    case backend::app::ExperimentStartOutcome::NotReady:
        restoreRealtimeModeIfNeeded();
        explainReadiness(result.readiness);
        return;
    case backend::app::ExperimentStartOutcome::StorageFailed:
    case backend::app::ExperimentStartOutcome::ProvenanceFailed:
        restoreRealtimeModeIfNeeded();
        QMessageBox::critical(this, tr("Error"),
                              tr("Failed to prepare the experiment file:\n%1\n\n%2")
                                  .arg(filePath, QString::fromStdString(result.message)));
        statusLabel_->setText(tr("Experiment start failed: %1").arg(QString::fromStdString(result.message)));
        return;
    case backend::app::ExperimentStartOutcome::AlreadyActive:
    case backend::app::ExperimentStartOutcome::Busy:
        restoreRealtimeModeIfNeeded();
        QMessageBox::information(this, tr("Experiment"), QString::fromStdString(result.message));
        return;
    }

    // Record experiment start time from the frozen snapshot.
    experimentStartTimeNs_ = result.run.startWallClockNs;

    experimentActive_ = true;
    runOperationId_ = runStatusModel_->beginOperation(frontend::RunPhase::Starting, QFileInfo(filePath).fileName());
    runStatusModel_->setPhase(frontend::RunPhase::Running, runOperationId_, QFileInfo(filePath).fileName());
    statusLabel_->setText(result.run.camera.simulated
                              ? tr("Experiment started (simulated camera)")
                              : tr("Experiment started"));
    updateExperimentButtonStates(); // This will also call updateTabStates() to disable Overview and Review tabs
}

void MainWindow::restoreRealtimeModeIfNeeded()
{
    if (!restoreRealtimeModeAfterExperiment_)
        return;
    const auto restoreMode =
        realtimeModeBeforeExperiment_ ==
                static_cast<int>(backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch)
            ? backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch
            : backend::services::ProcessingService::RealtimeProcessingMode::Inline;
    backend_.processing().setRealtimeProcessingMode(restoreMode);
    restoreRealtimeModeAfterExperiment_ = false;
    SPDLOG_INFO("MainWindow: restored realtime mode to {}",
                restoreMode == backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                    ? "async_batch"
                    : "inline");
}

void MainWindow::onStopExperiment()
{
    if (!experimentActive_)
    {
        QMessageBox::information(this, tr("Experiment"),
                                 tr("No experiment is currently running"));
        return;
    }
    if (stopInProgress_) return; // one finalization only

    // Issue #363: stop is a two-phase, single-flight operation. Phase 1
    // (here) marks Stopping, waits for the in-flight auto-flush, then drains
    // the buffered frames on a worker (Saving) so the GUI keeps rendering the
    // state; phase 2 (finishStopExperiment) writes metadata/provenance,
    // closes the file and reports Complete or Failed.
    using stop_clock = std::chrono::steady_clock;
    auto sinceMs = [](stop_clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(stop_clock::now() - t0).count();
    };
    stopInProgress_ = true;
    finalizeHandled_ = false;
    runStatusModel_->setPhase(frontend::RunPhase::Stopping, runOperationId_);
    updateExperimentButtonStates();
    statusLabel_->setText(tr("Stopping experiment…"));

    // Wait for any ongoing flush to complete
    {
        const auto t0 = stop_clock::now();
        const bool wasInProgress = flushInProgress_;
        if (flushInProgress_ && flushWatcher_)
        {
            flushWatcher_->waitForFinished();
        }
        SPDLOG_INFO("stop-lag: waitForFinished(async flush) took {:.3f} ms (inProgress={})",
                    sinceMs(t0), wasInProgress);
    }

    if (!backend_.hdf5().isFileOpen())
    {
        finishStopExperiment(true);
        return;
    }

    runStatusModel_->setPhase(frontend::RunPhase::Saving, runOperationId_);
    statusLabel_->setText(tr("Saving experiment data…"));
    if (!finalizeWatcher_) {
        finalizeWatcher_ = new QFutureWatcher<bool>(this);
        connect(finalizeWatcher_, &QFutureWatcher<bool>::finished, this, [this]() {
            if (finalizeHandled_) return;
            finishStopExperiment(finalizeWatcher_->result());
        });
    }
    // The worker owns only the backend pointer (outlives this window).
    auto* backend = &backend_;
    finalizeWatcher_->setFuture(QtConcurrent::run([backend]() {
        auto& hdf5 = backend->hdf5();
        auto& proc = backend->processing();
        const size_t flushed = proc.flushBufferedFrames(hdf5);
        if (flushed > 0) SPDLOG_INFO("Final flush: {} frames submitted to HDF5 write queue", flushed);
        // Drain the async write queue so the writer thread has stopped before the
        // direct appendFrames in finishStopExperiment (no two writers on the file).
        return proc.finishFlush();
    }));
}

void MainWindow::finishStopExperiment(bool flushOk)
{
    if (finalizeHandled_) return;
    finalizeHandled_ = true;
    using stop_clock = std::chrono::steady_clock;
    const auto tStopBegin = stop_clock::now();
    auto sinceMs = [](stop_clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(stop_clock::now() - t0).count();
    };
    auto &processing = backend_.processing();
    auto &hdf5 = backend_.hdf5();
    if (!flushOk)
    {
        backend_.experiment().reportUnresolvedFault(
            "experiment.flushFailed", "a save error occurred while flushing experiment data to disk");
        alertModel_->raise(QStringLiteral("save.flush"), frontend::AlertSeverity::Error,
                           tr("A save error occurred while flushing experiment data to disk."),
                           tr("The run is incomplete; check the destination and the log."));
        runStatusModel_->latchFailure(runOperationId_, tr("flush failed"));
        QMessageBox::warning(this, tr("Save Error"),
                             tr("A save error occurred while flushing experiment data to disk."));
    }

    {
        const auto t0 = stop_clock::now();
        processing.endExperiment();
        backend_.processing().resetRealtimeMetrics();
        SPDLOG_INFO("stop-lag: endExperiment+resetRealtimeMetrics took {:.3f} ms", sinceMs(t0));
    }

    // Get final frame counts (should be empty after flush, but check anyway)
    const auto tGetFramesStart = stop_clock::now();
    auto validFrames = processing.getValidFrames();
    auto invalidFrames = processing.getInvalidFrames();
    SPDLOG_INFO("stop-lag: get{{Valid,Invalid}}Frames took {:.3f} ms (valid={}, invalid={})",
                sinceMs(tGetFramesStart), validFrames.size(), invalidFrames.size());

    // Record experiment end time
    uint64_t experimentEndTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();

    // Save any remaining frames and write experiment info
    if (hdf5.isFileOpen())
    {
        if (!validFrames.empty() || !invalidFrames.empty())
        {
            const auto t0 = stop_clock::now();
            // Save any remaining frames that weren't flushed
            const bool appendOk = hdf5.appendFrames(validFrames, invalidFrames);
            SPDLOG_INFO("stop-lag: appendFrames(remaining) took {:.3f} ms "
                        "(valid={}, invalid={})",
                        sinceMs(t0), validFrames.size(), invalidFrames.size());
            if (!appendOk)
            {
                QMessageBox::warning(this, tr("Warning"),
                                     tr("Failed to save remaining frames to HDF5"));
            }
        }

        // Write experiment metadata (including background image for reproducibility if set)
        size_t totalValid = validFrames.size();
        size_t totalInvalid = invalidFrames.size();
        // Note: We can't easily track total frames written via append, so we use current counts
        // In a production system, you'd want to track cumulative counts
        if (!hdf5.flush())
        {
            SPDLOG_WARN("stop-lag: H5Fflush before writeExperimentInfo failed");
        }
        auto processingConfig = processing.getProcessingConfig();
        auto roi = processing.getRealtimeRoi();
        cv::Mat bg = processing.getRealtimeBackgroundGray();
        const auto processingCore = processing.activeProcessingCoreIdentity();
        bool metadataOk = false;
        {
            const auto t0 = stop_clock::now();
            metadataOk = hdf5.writeExperimentInfo(
                experimentStartTimeNs_, experimentEndTimeNs, totalValid, totalInvalid,
                processingConfig, roi, bg.empty() ? nullptr : &bg, &processingCore);
            SPDLOG_INFO("stop-lag: writeExperimentInfo took {:.3f} ms", sinceMs(t0));
        }
        if (!metadataOk) {
            SPDLOG_ERROR("Experiment metadata/provenance write failed");
            backend_.experiment().reportUnresolvedFault(
                "experiment.provenanceFailed",
                "mandatory metadata/processing-core provenance could not be saved for the last run");
            alertModel_->raise(QStringLiteral("save.metadata"), frontend::AlertSeverity::Error,
                               tr("Experiment metadata/processing-core provenance could not be saved."),
                               tr("The file is not complete; keep the log and free space/permissions before the next run."));
            runStatusModel_->latchFailure(runOperationId_, tr("metadata/provenance write failed"));
            QMessageBox::critical(
                this, tr("Save Error"),
                tr("Experiment frame data was written, but mandatory metadata and "
                   "processing-core provenance could not be saved."));
        }

        // Persist the reconciled frame accounting (issue #367). A run whose
        // required accounting does not reconcile is stored as failed, never
        // complete; the completion state is surfaced to the operator below.
        const auto accounting = processing.experimentAccountingSnapshot();
        if (metadataOk && !hdf5.writeRunAccounting(accounting)) {
            SPDLOG_ERROR("Experiment run accounting could not be persisted");
        }
        if (metadataOk && !hdf5.writeAcquisitionProvenance(backend_.capture().timestampDescriptor(),
                                                           backend_.capture().telemetrySnapshot())) {
            SPDLOG_ERROR("Experiment acquisition provenance could not be persisted");
        }
        SPDLOG_INFO("Experiment accounting: completion={} ({}); admitted={} empty={} processed={} "
                    "rejected={} processingFailed={} storeLoss={} persisted={}/{} failed={}",
                    backend::recording::toString(accounting.completion), accounting.completionReason,
                    accounting.admitted, accounting.empty, accounting.processed,
                    accounting.scientificallyRejected, accounting.processingFailed,
                    accounting.storeOverwritten + accounting.storeNotCommitted + accounting.storeMalformed,
                    accounting.persistenceCommitted, accounting.persistenceAdmitted,
                    accounting.persistenceFailed);
        if (accounting.completion != backend::recording::RunCompletionState::Complete &&
            accounting.completion != backend::recording::RunCompletionState::IntentionallyPartial) {
            alertModel_->raise(QStringLiteral("run.accounting"), frontend::AlertSeverity::Warning,
                               tr("The run is recorded as '%1': %2")
                                   .arg(QString::fromLatin1(backend::recording::toString(accounting.completion)),
                                        QString::fromStdString(accounting.completionReason)),
                               tr("Review the frame accounting in the Review tab before using this run."));
            runStatusModel_->latchFailure(runOperationId_,
                                          QString::fromLatin1(backend::recording::toString(accounting.completion)));
            QMessageBox::warning(
                this, tr("Experiment Accounting"),
                tr("The run is recorded as '%1': %2\n\nEmpty %3 · processed %4 · rejected %5 · "
                   "processing failed %6 · store loss %7 · persisted %8/%9 · persistence failed %10")
                    .arg(QString::fromLatin1(backend::recording::toString(accounting.completion)))
                    .arg(QString::fromStdString(accounting.completionReason))
                    .arg(static_cast<qulonglong>(accounting.empty))
                    .arg(static_cast<qulonglong>(accounting.processed))
                    .arg(static_cast<qulonglong>(accounting.scientificallyRejected))
                    .arg(static_cast<qulonglong>(accounting.processingFailed))
                    .arg(static_cast<qulonglong>(accounting.storeOverwritten + accounting.storeNotCommitted +
                                                 accounting.storeMalformed))
                    .arg(static_cast<qulonglong>(accounting.persistenceCommitted))
                    .arg(static_cast<qulonglong>(accounting.persistenceAdmitted))
                    .arg(static_cast<qulonglong>(accounting.persistenceFailed)));
        }

        // Save full config.json content for backtracking
        std::string configJson = backend_.getLastConfigJson();
        if (metadataOk && !configJson.empty()) {
            const auto t0 = stop_clock::now();
            hdf5.writeConfigJson(configJson);
            SPDLOG_INFO("stop-lag: writeConfigJson took {:.3f} ms (bytes={})",
                        sinceMs(t0), configJson.size());
        }

        // Note: Chart snapshots are no longer saved during experiment stop.
        // Charts are now generated on-demand from HDF5 data in the Review tab.

        statusLabel_->setText(
            metadataOk
                ? QString("Experiment saved: %1 valid, %2 invalid frames")
                      .arg(totalValid)
                      .arg(totalInvalid)
                : tr("Experiment save incomplete: metadata/provenance failed"));
        {
            const auto t0 = stop_clock::now();
            hdf5.closeFile();
            SPDLOG_INFO("stop-lag: closeFile (H5Fclose) took {:.3f} ms", sinceMs(t0));
        }
    }
    else
    {
        statusLabel_->setText("Experiment stopped (HDF5 file not open)");
    }

    // Release the frozen run snapshot; the coordinator returns to Idle so the
    // next preflight evaluates a fresh state (issue #369).
    if (auto finished = backend_.experiment().finish())
    {
        SPDLOG_INFO("MainWindow: experiment run {} finished (readiness gen {}, capture gen {})",
                    finished->startGeneration, finished->readinessGeneration, finished->captureGeneration);
    }

    experimentActive_ = false;
    stopInProgress_ = false;
    // Complete becomes Failed automatically when a failure was latched.
    runStatusModel_->setPhase(frontend::RunPhase::Complete, runOperationId_);
    restoreRealtimeModeIfNeeded();
    updateExperimentButtonStates(); // This will also call updateTabStates() to enable Overview and Review tabs

    const auto cfgAtStop = processing.getProcessingConfig();
    const double stopTotalMs = sinceMs(tStopBegin);
    SPDLOG_INFO("stop-lag: finishStopExperiment total {:.3f} ms (multiImage={}/{})",
                stopTotalMs,
                cfgAtStop.multi_image_enabled, cfgAtStop.multi_image_count);
    {
        std::ostringstream data;
        data << "{\"multi_image_enabled\":" << (cfgAtStop.multi_image_enabled ? 1 : 0)
             << ",\"multi_image_count\":" << cfgAtStop.multi_image_count << "}";
        backend::services::CrashReporter::capturePerformanceTransaction(
            "experiment.stop", "ui.action", stopTotalMs, data.str());
    }
}
void MainWindow::onUpdateStats()
{
    // Sampling + persistence scheduling always run while a session is
    // active; rendering is a separate step (issue #363) and never touches
    // alerts or the run state.
    const frontend::StatisticsData data = sampleStats();
    renderStats(data);
    refreshDiagnostics(data);
    // Keep the acquisition-mode badge in sync with the backend-confirmed mode
    // (the confirmation lands shortly after capture start).
    updateDeliveryModeBadge();
}

frontend::StatisticsData MainWindow::sampleStats()
{
    const auto &cap = backend_.capture();
    const auto &s = cap.stats();
    auto &proc = backend_.processing();
    const uint64_t tFetchStartUs = backend::Tools::getTimestamp();
    const auto bufferedFrames = proc.getBufferedFrameCounts();
    const uint64_t tFetchEndUs = backend::Tools::getTimestamp();
    const double fetchMs = static_cast<double>(tFetchEndUs - tFetchStartUs) / 1000.0;

    double displayFps = 0.0;
    if (experimentTabs_ && experimentTabs_->count() > 0) {
        auto* previewPage = qobject_cast<frontend::PreviewPage*>(experimentTabs_->widget(0));
        if (previewPage) {
            auto* playbackPanel = previewPage->getPlaybackPanel();
            if (playbackPanel) {
                displayFps = playbackPanel->getDisplayFps();
            }
        }
    }
    const double algoAvgUs = proc.getAlgoAvgUs1s();
    const double validFps = proc.getValidFps1s();
    const double invalidFps = proc.getInvalidFps1s();
    const uint64_t totalValidFlushed = proc.getTotalValidFlushed();

    const uint64_t nowUs = backend::Tools::getTimestamp();
    const double algoAvgUsAgeMs = (nowUs - proc.getAlgoAvgUs1sUpdatedUs()) / 1000.0;
    const double meanRingRatioAgeMs = (nowUs - backend_.autofocus().getLastRingRatioUpdateUs()) / 1000.0;

    double experimentRuntimeSeconds = 0.0;
    if (experimentActive_ && experimentStartTimeNs_ > 0) {
        uint64_t currentTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        experimentRuntimeSeconds = static_cast<double>(currentTimeNs - experimentStartTimeNs_) / 1e9;
    }

    frontend::StatisticsData data;
    data.displayFps = displayFps;
    data.algoAvgUs = algoAvgUs;
    data.validFps = validFps;
    data.invalidFps = invalidFps;
    data.totalValidFlushed = totalValidFlushed;
    data.cameraRunning = cap.isRunning();
    {
        const auto telemetry = cap.telemetrySnapshot();
        data.cameraFps = s.lastFrameRate.load();
        data.cameraDataRateMBps = s.lastDataRateMBps.load();
        data.cameraFpsText = frontend::StatsDisplayManager::formatMetric(telemetry.captureFrameRate);
        data.cameraDataRateText = frontend::StatsDisplayManager::formatMetric(telemetry.captureDataRateMBps);
    }
    data.meanRingRatio = backend_.autofocus().getMedianRingRatio();
    data.experimentActive = experimentActive_;
    data.validBuffered = bufferedFrames.valid;
    data.invalidBuffered = bufferedFrames.invalid;
    data.flushInProgress = flushInProgress_;
    data.experimentRuntimeSeconds = experimentRuntimeSeconds;
    data.algoAvgUsAgeMs = algoAvgUsAgeMs;
    data.meanRingRatioAgeMs = meanRingRatioAgeMs;

    if (experimentActive_ && !stopInProgress_)
    {
        size_t totalBuffered = bufferedFrames.total();

        // Check if we need to flush (round-robin buffer)
        // Only start a new flush if one isn't already in progress
        size_t flushNeeded = proc.getFlushInterval();
        if (flushNeeded > 0 && totalBuffered >= flushNeeded && !flushInProgress_)
        {
            // Flush frames to disk asynchronously to avoid blocking UI
            flushInProgress_ = true;
            // Capture the backend pointer, NOT `this`: QFutureWatcher's
            // destructor does not block on a running future, so the task can
            // outlive this window. The backend itself outlives the window
            // (constructed before it in main()).
            auto* backend = &backend_;
            QFuture<size_t> future = QtConcurrent::run([backend]()
                                                       {
#ifdef _WIN32
                // Lower OS thread priority and optionally set affinity to a non-critical core
                // Background mode (Vista+); falls back to BELOW_NORMAL if unavailable
                if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                }
                const unsigned int cores = std::thread::hardware_concurrency();
                if (cores > 1) {
                    // Prefer the last core
                    const DWORD_PTR mask = (cores >= (sizeof(DWORD_PTR) * 8))
                        ? (static_cast<DWORD_PTR>(1) << ((sizeof(DWORD_PTR) * 8) - 1))
                        : (static_cast<DWORD_PTR>(1) << (cores - 1));
                    SetThreadAffinityMask(GetCurrentThread(), mask);
                }
#endif
                auto& hdf5 = backend->hdf5();
                auto& proc = backend->processing();
                return proc.flushBufferedFrames(hdf5); });
            flushWatcher_->setFuture(future);
        }

        // Throttled diagnostic log (~1 Hz)
        static uint64_t lastDiagLogUs = 0;
        if (nowUs - lastDiagLogUs >= 1'000'000ULL) {
            uint64_t earliest = 0, latest = 0;
            size_t count = 0;
            backend_.playback().queryRange(earliest, latest, count);
            const double memMB = backend::Tools::getProcessMemoryMB();
            SPDLOG_INFO("MainWindow stats: buffer_fetch_ms={:.3f}, valid={}, invalid={}, total={}, playback_range=[{},{}] count={}, flush_interval={}, max_buffered={}, flushing={}, mem_mb={:.1f}",
                        fetchMs, bufferedFrames.valid, bufferedFrames.invalid, totalBuffered, earliest, latest, count, flushNeeded, proc.getMaxBufferedFrames(), flushInProgress_ ? 1 : 0, memMB);
            lastDiagLogUs = nowUs;
        }
    }
    return data;
}

void MainWindow::renderStats(const frontend::StatisticsData& data)
{
    if (sidebarWidget_ && sidebarWidget_->statisticsPanel()) {
        sidebarWidget_->statisticsPanel()->updateStatistics(data);
    }
    // Bounded operator metrics only (issue #363): camera rate, valid/invalid
    // rate, algorithm headline, persistence health. Verbose transport values
    // live in Diagnostics. The label is elided, so this never changes the
    // window's required width, and it never carries alerts.
    QString compact = data.cameraRunning
                          ? tr("Camera %1 fps").arg(data.cameraFpsText.isEmpty() ? QString::number(data.cameraFps, 'f', 0) : data.cameraFpsText)
                          : tr("Camera stopped");
    compact += tr(" · Valid %1/s · Invalid %2/s").arg(QString::number(data.validFps, 'f', 1), QString::number(data.invalidFps, 'f', 1));
    compact += tr(" · Algo %1 µs").arg(QString::number(data.algoAvgUs, 'f', 0));
    if (data.experimentActive) {
        compact += tr(" · Run %1 s · buffered %2%3")
                       .arg(QString::number(data.experimentRuntimeSeconds, 'f', 0))
                       .arg(static_cast<qulonglong>(data.validBuffered + data.invalidBuffered))
                       .arg(data.flushInProgress ? tr(" (flushing)") : QString());
    }
    compactStatus_ = compact;
    if (!stopInProgress_) statusLabel_->setText(compact);
}

void MainWindow::updateDeliveryModeBadge()
{
    if (!deliveryModeLabel_)
        return;
    const auto& cap = backend_.capture();
    const bool latest =
        cap.activeDeliveryMode() == camera::common::FrameDeliveryMode::LatestFrame;
    QString text = latest ? tr("⏩ LATEST FRAME · drops stale frames")
                          : tr("▶ EVERY FRAME · sequence preserved");
    if (!cap.stats().deliveryModeConfirmed.load(std::memory_order_acquire))
    {
        text += tr(" (requested)");
    }
    deliveryModeLabel_->setText(text);
    // Color is supplementary only; the glyph + text fully identify the mode.
    deliveryModeLabel_->setStyleSheet(latest ? QStringLiteral("color: #b06a00;")
                                             : QStringLiteral("color: #2e7d32;"));
}

void MainWindow::startExperimentServices()
{
    if (experimentServicesActive_) {
        return; // Already active
    }

    auto frameStore = backend_.getFrameStore();
    if (!frameStore) {
        SPDLOG_ERROR("MainWindow::startExperimentServices: FrameStore is null");
        return;
    }

    backend_.processing().startRealtime(frameStore);
    backend_.playback().play();
    experimentServicesActive_ = true;
    SPDLOG_INFO("MainWindow: Experiment services started");
}

void MainWindow::stopExperimentServices()
{
    if (!experimentServicesActive_) {
        return; // Already stopped
    }

    backend_.processing().stopRealtime();
    backend_.playback().pause();
    experimentServicesActive_ = false;
    SPDLOG_INFO("MainWindow: Experiment services stopped");
}

void MainWindow::onNoCamerasFound()
{
    if (ui->tabs)
    {
        ui->tabs->setCurrentIndex(0); // Connect tab
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("No camera found"));
    box.setIcon(QMessageBox::Information);
    box.setText(tr("No camera was found.\n\n"
                   "Please check that the camera is powered on and that the camera LED is green (ready).\n"
                   "Then try connecting again."));

    auto *tryAgainBtn = box.addButton(tr("Try again"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Ok);

    box.exec();

    if (box.clickedButton() == tryAgainBtn)
    {
        // Defer to avoid re-entrancy if discovery immediately emits noCamerasFound().
        QTimer::singleShot(0, this, [this]() {
            if (connectTab_) connectTab_->tryAutoConnect();
        });
    }
}

void MainWindow::onTabChanged(int index)
{
    // Guard: Once experiment started, overview is disabled until end of experiment
    if (index == 1 && experimentActive_) {
        QMessageBox::warning(this, tr("Overview Tab"),
                             tr("Overview tab is disabled while an experiment is active. Please stop the experiment first."));
        // Switch back to previous tab (or Experiment tab)
        if (ui->tabs) {
            ui->tabs->setCurrentIndex(2); // Switch to Experiment tab
        }
        return;
    }

    // Guard: Cannot review when there is an experiment going on
    if (index == 3 && experimentActive_) {
        QMessageBox::warning(this, tr("Review Tab"),
                             tr("Review tab is disabled while an experiment is active. Please stop the experiment first."));
        // Switch back to previous tab (or Experiment tab)
        if (ui->tabs) {
            ui->tabs->setCurrentIndex(2); // Switch to Experiment tab
        }
        return;
    }

    // Handle service lifecycle for Overview (index 1) + Experiment (index 2)
    // Both tabs rely on playback/processing to show live frames.
    const int OVERVIEW_TAB_INDEX = 1;
    const int EXPERIMENT_TAB_INDEX = 2;
    
    if (index == OVERVIEW_TAB_INDEX || index == EXPERIMENT_TAB_INDEX) {
        // Switching TO Overview/Experiment: start services
        startExperimentServices();
        // Only run processing pipeline when Experiment tab is visible. When Overview is visible,
        // disable processing to avoid wrong processing times, slowdown, and overview frames
        // being used as auto-background.
        backend_.processing().setRealtimeEnabled(index == EXPERIMENT_TAB_INDEX);
    } else if (experimentServicesActive_) {
        // Switching away from Overview/Experiment: stop services
        stopExperimentServices();
    }

    // Auto-start camera when navigating to Overview
    if (index == OVERVIEW_TAB_INDEX)
    {
        auto &cap = backend_.capture();
        if (!cap.isRunning() && backend_.isCameraConfigured())
        {
            SPDLOG_INFO("MainWindow: auto-starting camera on Overview navigation");
            if (cap.start())
            {
                if (statsTimer_)
                    statsTimer_->start();
                if (statusLabel_)
                    statusLabel_->setText("Camera running");
            }
            else
            {
                QMessageBox::warning(this, tr("Start Camera"),
                                     tr("Failed to start camera. Please check camera connection and try again."));
                if (statusLabel_)
                    statusLabel_->setText("Camera start failed");
            }
        }
    }

    // Guard: Camera script application - skip during experiment
    // Only handle script application for switches between Overview (index 1) and Experiment (index 2)
    if (index != 1 && index != 2) {
        return;
    }

    // Skip script application if experiment is active
    if (experimentActive_) {
        SPDLOG_WARN("MainWindow::onTabChanged: Skipping script application during active experiment");
        return;
    }

    if (backend_.isMindVisionCameraSelected()) {
        SPDLOG_DEBUG("MainWindow::onTabChanged: Skipping EGrabber script application for MindVision camera");
        return;
    }

    // Check if camera is currently running
    bool wasRunning = backend_.capture().isRunning();

    QString scriptPath;
    
    if (index == 1) {
        // Overview tab
        if (!overviewTab_) {
            SPDLOG_WARN("MainWindow::onTabChanged: overviewTab_ is null");
            return;
        }
        scriptPath = overviewTab_->currentJsPath();
    } else if (index == 2) {
        // Experiment tab
        if (!experimentTabs_) {
            SPDLOG_WARN("MainWindow::onTabChanged: experimentTabs_ is null");
            return;
        }
        // Get PreviewPage from Experiment tab (index 0)
        auto* previewPage = qobject_cast<frontend::PreviewPage*>(experimentTabs_->widget(0));
        if (!previewPage) {
            SPDLOG_WARN("MainWindow::onTabChanged: PreviewPage is null");
            return;
        }
        auto* configTabs = previewPage->getConfigTabs();
        if (!configTabs) {
            SPDLOG_WARN("MainWindow::onTabChanged: ConfigTabs is null");
            return;
        }
        scriptPath = configTabs->currentJsPath();
    }

    // Verify the script file exists
    if (scriptPath.isEmpty()) {
        SPDLOG_WARN("MainWindow::onTabChanged: Script path is empty");
        return;
    }

    QFileInfo fileInfo(scriptPath);
    if (!fileInfo.exists()) {
        SPDLOG_WARN("MainWindow::onTabChanged: Script file does not exist: {}", scriptPath.toStdString());
        return;
    }

    // Apply the script
    SPDLOG_INFO("MainWindow::onTabChanged: Auto-applying camera script from {}", scriptPath.toStdString());
    std::string backendErr;
    if (!backend_.applyCameraScriptFromFile(scriptPath.toStdString(), &backendErr)) {
        SPDLOG_ERROR("MainWindow::onTabChanged: Failed to apply script: {}", backendErr);
        return;
    }

    // If camera was running, restart it through the shared command path
    // (applyCameraScriptFromFile is a documented backend transaction that
    // stops capture itself; the restart is an ordinary start request).
    if (wasRunning) {
        SPDLOG_INFO("MainWindow::onTabChanged: Restarting camera after script application");
        const auto r = cameraController_->requestStart();
        if (!r.accepted()) {
            SPDLOG_ERROR("MainWindow::onTabChanged: Failed to restart camera after script application: {}",
                         r.message.toStdString());
            statusLabel_->setText(tr("Camera script applied, but restart failed: %1").arg(r.message));
        } else {
            SPDLOG_INFO("MainWindow::onTabChanged: Camera restart requested");
        }
    } else {
        SPDLOG_INFO("MainWindow::onTabChanged: Camera script applied (camera was not running)");
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Guard: Warn if closing during active experiment
    if (experimentActive_) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            tr("Close Application"),
            tr("An experiment is currently active. Closing the application may result in data loss.\n\nDo you want to close anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    // A finalization in flight completes before the window goes away
    // (bounded: the worker only drains the write queue).
    if (stopInProgress_ && finalizeWatcher_) {
        finalizeWatcher_->waitForFinished();
        if (!finalizeHandled_) finishStopExperiment(finalizeWatcher_->result());
    }

    // Ensure experiment services are stopped before closing
    if (experimentServicesActive_) {
        stopExperimentServices();
    }

    // Stop the capture service so the camera hardware is released and no
    // new frames are pushed into FrameStore while the window destructs.
    if (backend_.capture().isRunning()) {
        backend_.capture().stop();
    }

    // Issue #358: persist geometry only when the close is accepted.
    saveWindowGeometry();
    saveSidebarPreference();
    QMainWindow::closeEvent(event);
}
