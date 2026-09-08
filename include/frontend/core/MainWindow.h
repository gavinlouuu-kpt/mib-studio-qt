#pragma once

#include <QMainWindow>

#include "backend/app/ExperimentReadiness.h"

#include <chrono>

class QLabel;
class QTimer;
class PlaybackPanel;
class QTabWidget;
class QSplitter;
class QSpinBox;
class QAction;
class QPushButton;
class QToolButton;
class QShowEvent;
class QResizeEvent;
class QCloseEvent;

namespace backend { class AppBackend; }
namespace backend::app { struct ExperimentReadinessSnapshot; }
namespace frontend { class OverviewTab; }
namespace frontend { class ConnectTab; }
namespace frontend { class AutoUpdater; }
namespace frontend { class SidebarWidget; }
namespace frontend { class DeviceInitManager; }
namespace frontend { class CameraController; }
namespace frontend { class ElidingLabel; }
namespace frontend { class RunStatusModel; class UiAlertModel; class RunStatusWidget; class AlertBanner; struct StatisticsData; }
class QDialog;
class QPlainTextEdit;
namespace frontend { struct CameraActionState; }
namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onStartCapture();
    void onStopCapture();
    void onStartExperiment();
    void onStopExperiment();
    void onUpdateStats();
    void onTabChanged(int index);
    void onNoCamerasFound();
    void onCameraStateChanged(const frontend::CameraActionState& state);
    // Issue #363: detailed identities/telemetry live in a non-modal dialog,
    // never in the status bar. Invokable so tests can open it by name.
    void showDiagnostics();

public:
    // Single authoritative camera command path (issue #360). Every camera
    // Start/Stop presentation in the application dispatches through it.
    frontend::CameraController* cameraController() const { return cameraController_; }

    // Issue #359: the main splitter is the single owner of the hardware
    // panel (sidebar) geometry; this window owns the user's visibility/width
    // preference and commands the splitter. The checkable action is the
    // stable reopen affordance (tab-bar corner, keyboard accessible).
    QAction* hardwarePanelAction() const { return hardwarePanelAct_; }
    bool isHardwarePanelVisible() const;
    void setHardwarePanelVisible(bool visible);
    int hardwarePanelPreferredWidth() const { return sidebarPreferredWidth_; }
    QSplitter* mainSplitter() const { return mainSplitter_; }
    frontend::SidebarWidget* sidebar() const { return sidebarWidget_; }
    QTabWidget* mainTabs() const;
    QTabWidget* experimentTabs() const { return experimentTabs_; }

    // Issue #358: one window-geometry restoration path. Tests and the
    // screenshot tour supply the available desktop explicitly so decisions
    // never depend on the (800x600) offscreen platform screen.
    void setAvailableGeometryOverrideForTests(const QRect& available);

    // Issue #363: run state, alerts and metrics are separate surfaces.
    frontend::UiAlertModel* alertModel() const { return alertModel_; }
    frontend::RunStatusModel* runStatusModel() const { return runStatusModel_; }
    frontend::AlertBanner* alertBanner() const { return alertBanner_; }
    frontend::RunStatusWidget* runStatusWidget() const { return runStatusWidget_; }
    QString compactStatusText() const;
    bool stopInProgress() const { return stopInProgress_; }
    bool restoredWindowGeometryFromSettings() const { return restoredGeometryFromSettings_; }
    void ensureWindowFitsScreen();
    void saveWindowGeometry();

private:
    void updateExperimentButtonStates();
    // Issue #369: explain blocking readiness gates (with remediation) and
    // return false; true when the snapshot is ready.
    bool explainReadiness(const backend::app::ExperimentReadinessSnapshot& readiness);
    // Shared-backend lifecycle (issue #372): the coordinator owns flush,
    // stop and finalization; this slot renders each status transition
    // (queued to the GUI thread from the coordinator's callback).
    void onExperimentStatus(const backend::app::ExperimentStatus& status);
    // Issue #363
    void setupStatusSurfaces();
    frontend::StatisticsData sampleStats();
    void renderStats(const frontend::StatisticsData& data);
    void refreshDiagnostics(const frontend::StatisticsData& data);
    void updateTabStates();
    void updateDeliveryModeBadge();
    void startExperimentServices();
    void stopExperimentServices();
    void setupCornerWidgets();
    void setupSidebar();
    void restoreWindowGeometry();
    QRect availableDesktopForWindow() const;
    void applySidebarLayout();
    void onSplitterMoved(int pos, int index);
    void loadSidebarPreference();
    void saveSidebarPreference();
    void updateHardwarePanelAction();
    
    Ui::MainWindow* ui;
    backend::AppBackend& backend_;
    frontend::ElidingLabel* statusLabel_ = nullptr;
    QLabel* processingCoreLabel_ = nullptr;
    QLabel* deliveryModeLabel_ = nullptr;
    QTimer* statsTimer_ = nullptr;
    PlaybackPanel* playbackPanel_ = nullptr;
    QTabWidget* experimentTabs_ = nullptr;
    frontend::ConnectTab* connectTab_ = nullptr;
    frontend::OverviewTab* overviewTab_ = nullptr;
    frontend::AutoUpdater* updater_ = nullptr;
    frontend::SidebarWidget* sidebarWidget_ = nullptr;
    frontend::DeviceInitManager* initManager_ = nullptr;
    frontend::CameraController* cameraController_ = nullptr;
    QSplitter* mainSplitter_ = nullptr;
    uint64_t experimentStartTimeNs_{0};
    bool experimentActive_{false};
    bool experimentServicesActive_{false};
    bool flushInProgress_{false}; // mirrors ExperimentStatus::flushing
    QAction* startExperimentAct_ = nullptr;
    QAction* stopExperimentAct_ = nullptr;
    QPushButton* startExperimentBtn_ = nullptr;
    QPushButton* stopExperimentBtn_ = nullptr;
    frontend::RunStatusWidget* runStatusWidget_ = nullptr;
    frontend::UiAlertModel* alertModel_ = nullptr;
    frontend::RunStatusModel* runStatusModel_ = nullptr;
    frontend::AlertBanner* alertBanner_ = nullptr;
    QDialog* diagnosticsDialog_ = nullptr;
    QPlainTextEdit* diagnosticsText_ = nullptr;
    bool stopInProgress_{false};
    std::chrono::steady_clock::time_point stopRequestedAt_{};
    uint64_t runOperationId_{0};
    QString compactStatus_;
    QLabel* roiLabel_ = nullptr;
    QPushButton* startCameraBtn_ = nullptr;
    QPushButton* stopCameraBtn_ = nullptr;
    // Issue #359 sidebar preference/state (never a hard minimum size).
    QAction* hardwarePanelAct_ = nullptr;
    QToolButton* hardwarePanelBtn_ = nullptr;
    int sidebarPreferredWidth_{300};
    bool sidebarUserVisible_{true};
    bool sidebarHiddenForSpace_{false};
    bool applyingSidebarLayout_{false};
    QTimer* sidebarPersistTimer_ = nullptr;
    QTimer* layoutAdjustTimer_ = nullptr;
    // Issue #358 window geometry.
    QRect availableGeometryOverride_;
    bool restoredGeometryFromSettings_{false};
    bool fittingWindow_{false};
    bool firstShowDone_{false};
};
