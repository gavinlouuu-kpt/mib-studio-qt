#pragma once

#include "frontend/models/ProcessingConfigDraft.h"
#include "frontend/models/TargetGatePreview.h"
#include "backend/processing/EModulusLut.h"
#include "backend/processing/MonitoringDensity.h"
#include "backend/services/MonitoringDensityService.h"
#include "frontend/utils/OverlayRenderer.h"
#include "frontend/utils/ScatterHitTest.h"

#include <QWidget>
#include <QImage>
#include <QPointer>
#include <QPointF>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cv { class Mat; }
namespace backend { class AppBackend; }
namespace backend::services { struct ProcessedFrame; struct FilterResult; }

class QTimer;
class QChartView;
namespace frontend { class ZoomableChartView; class FrameViewerDialog; class GateOverlayItem; }
class QScatterSeries;
class QLineSeries;
class QBarSeries;
class QBarSet;
class QHistogramSeries;
class QBarCategoryAxis;
class QChart;
class QValueAxis;
class QScrollArea;
class QGridLayout;
class QWidget;
class QLabel;
class QCheckBox;
class QPushButton;
class QHBoxLayout;
class QVBoxLayout;
class QShowEvent;
class QHideEvent;
class QSpinBox;
class QDoubleSpinBox;
class QGroupBox;
class QFormLayout;
class QSplitter;

namespace Ui { class ExperimentMonitoringTab; }

namespace frontend {

class ExperimentMonitoringTab : public QWidget {
    Q_OBJECT
public:
    explicit ExperimentMonitoringTab(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~ExperimentMonitoringTab() override;
    
    // Scatter density (KDE) colouring. While enabled, every valid point in the
    // Deformability-vs-Area scatter is coloured by its normalised population
    // density. The estimate is owned by the backend
    // (backend::services::MonitoringDensityService: lowest-priority worker,
    // load back-off, compute budget); this tab pushes its settings and chart
    // axes to it, polls for a new result every kKdePollMs and re-routes the
    // points. The three settings persist in QSettings (Monitoring/Kde*).
    static constexpr int kKdePollMs = 100;
    using KdeSettings = backend::services::MonitoringDensitySettings;
    static constexpr double kKdeBandwidthFactorMin = KdeSettings::kBandwidthFactorMin;
    static constexpr double kKdeBandwidthFactorMax = KdeSettings::kBandwidthFactorMax;
    static constexpr double kKdeBandwidthFactorDefault = 1.0;
    static constexpr int kKdeIntervalMsMin = KdeSettings::kIntervalMsMin;
    static constexpr int kKdeIntervalMsMax = KdeSettings::kIntervalMsMax;
    static constexpr int kKdeIntervalMsDefault = 2000;
    bool kdeEnabled() const { return kdeEnabled_; }
    void setKdeEnabled(bool enabled);
    double kdeBandwidthFactor() const { return kdeBandwidthFactor_; }
    void setKdeBandwidthFactor(double factor);
    int kdeIntervalMs() const { return kdeIntervalMs_; }
    void setKdeIntervalMs(int ms);
    // Ask the backend for an estimate now; a no-op while disabled. The
    // service skips it when the ring is unchanged or the pipeline is loaded.
    void requestKdeUpdate();
    bool kdeJobInFlight() const; // the service has a pending or running tick
    bool kdeTimerActive() const; // result poll timer (enabled and visible)
    uint64_t kdeGeneration() const { return kdeGeneration_; } // adopted estimates
    int lastKdeComputeMs() const { return lastKdeComputeMs_; }
    std::size_t lastKdePointCount() const { return lastKdePointCount_; }
    // Core contour: share of the population enclosed by the solid contour
    // drawn on the scatter while KDE is on (Monitoring Settings, persisted).
    static constexpr double kKdeCoreFractionMin = KdeSettings::kCoreFractionMin;
    static constexpr double kKdeCoreFractionMax = KdeSettings::kCoreFractionMax;
    static constexpr double kKdeCoreFractionDefault = 0.9;
    static constexpr int kKdeGridNx = backend::services::MonitoringDensityService::kGridNx;
    static constexpr int kKdeGridNy = backend::services::MonitoringDensityService::kGridNy;
    double kdeCoreFraction() const { return kdeCoreFraction_; }
    void setKdeCoreFraction(double fraction);
    const std::vector<std::vector<backend::monitoring::DensityPoint>>& lastKdeContours() const { return lastKdeContours_; }
    double lastKdeCoreLevel() const { return lastKdeCoreLevel_; }
    std::size_t lastKdeCoreCount() const { return lastKdeCoreCount_; }
    // Reference contour (dashed) compared against the live one. Pin copies
    // the current live contour; set takes loops from elsewhere (a file, later).
    bool hasKdeReference() const { return !kdeReference_.empty(); }
    void pinKdeReference();
    void setKdeReference(std::vector<std::vector<backend::monitoring::DensityPoint>> loops, const QString& label);
    void clearKdeReference();
    // Reference from a previous experiment file: the full-run analysis record
    // when present, else the provisional live record. The path persists
    // (Monitoring/KdeReferencePath) and is reloaded at startup.
    bool loadKdeReferenceFromFile(const QString& path, QString* error = nullptr);
    QString kdeReferenceLabel() const { return kdeReferenceLabel_; }
    QString kdeReferencePath() const { return kdeReferencePath_; }
    // Provisional record (JSON, backend/processing/KdeCoreRecord.h) of the
    // estimate on screen; empty before the first estimate or while off. The
    // backend hands the same record to the experiment coordinator itself.
    std::string lastCoreRecordJson() const;
    const std::vector<QLineSeries*>& kdeContourSeriesForTests() const { return kdeContourSeries_; }
    const std::vector<QLineSeries*>& kdeReferenceSeriesForTests() const { return kdeReferenceSeries_; }
    QCheckBox* kdeToggle() const;
    QScatterSeries* scatterSeriesForTests() const { return scatterSeries_; }
    QScatterSeries* targetGroupSeriesForTests() const { return targetGroupSeries_; }
    // While KDE is on, points are routed into one series per density level
    // (index 0 = sparsest) instead of Qt's per-point configuration, which
    // rebuilds one graphics item per point on every refresh (~200 ms for
    // 1000 points on the GUI thread, measured in integration.monitoring_kde_e2e).
    static constexpr int kKdeLevels = 8;
    static int kdeLevelForDensity(double density);
    const std::vector<QScatterSeries*>& kdeLevelSeriesForTests() const { return kdeLevelSeries_; }
    const std::vector<QScatterSeries*>& kdeTargetLevelSeriesForTests() const { return kdeTargetLevelSeries_; }
    // Density of a frame from the last completed estimate; false if unknown.
    bool kdeDensityForFrame(uint64_t frameIndex, double& density) const;
    // Append frames to the rolling buffer as if they had been polled from the
    // backend and redraw (tests drive the charts without a running pipeline).
    void injectMonitoringFramesForTests(const std::vector<backend::services::ProcessedFrame>& frames);

    // Fixed chart axis ranges (user-definable via Monitoring Settings)
    double getScatterXMin() const { return scatterXMin_; }
    double getScatterXMax() const { return scatterXMax_; }
    double getScatterYMin() const { return scatterYMin_; }
    double getScatterYMax() const { return scatterYMax_; }
    void setScatterXRange(double minVal, double maxVal);
    void setScatterYRange(double minVal, double maxVal);

    double getHistogramXMin() const { return histogramXMin_; }
    double getHistogramXMax() const { return histogramXMax_; }
    double getHistogramYMax() const { return histogramYMax_; }
    double getHistogramBinWidth() const { return histogramBinWidth_; }
    void setHistogramXRange(double minVal, double maxVal);
    void setHistogramYMax(double maxVal);
    void setHistogramBinWidth(double width);

    /** Redraw scatter and histogram with current data and axis ranges (e.g. after settings change). */
    void refreshCharts();

    // Scatter interaction during a run (same rule as the Review tab, issue
    // #467): a click on a point shows that cell in the frame pane docked over
    // the histogram. The pane keeps its own copy of the frame, so the image
    // stays when the point later leaves the rolling buffer.
    ZoomableChartView* scatterViewForTests() const { return scatterplotView_; }
    QScatterSeries* scatterHighlightForTests() const { return scatterHighlight_; }
    QWidget* framePaneForTests() const { return framePaneContainer_; }
    FrameViewerDialog* framePaneViewerForTests() const { return framePane_; }
    FrameViewerDialog* frameWindowForTests() const;
    QString framePaneTitleForTests() const;
    // Frame index (ProcessedFrame::index) of the selected cell, if any.
    std::optional<uint64_t> selectedFrameIndex() const;
    // Frame index behind every scatter point, in point order (oldest first).
    const std::vector<uint64_t>& scatterPointFramesForTests() const { return scatterPointFrames_; }
    // Pixel position of scatter point `point` in scatter-view coordinates.
    QPointF scatterPointViewPosForTests(std::size_t point) const;
    void stepScatterSelectionForTests(int delta) { stepScatterSelection(delta); }

    // Target-group gate on the scatter (see "Gate overlay" below). Everything
    // here reads or drives the overlay the way an operator would.
    struct GateOverlayInfo {
        bool shown{false};            // overlay item visible (draft gate on)
        QString label;                // "Target group" / "Target group (unapplied)"
        QString notice;               // E gate on without a table; empty otherwise
        QRectF plotPx;                // plot rectangle, chart coordinates
        QRectF boxPx;                 // area/deformability box, chart coordinates (may overhang the plot)
        int maskCells{0};             // sampled cells over the plot
        int maskOn{0};                // ... that the draft gate would accept
        QStringList contourLabels;    // "E ≥ 1.00 kPa", "E ≤ 3.00 kPa"
        int contourLines{0};          // polylines drawn
        gate::HandleKind hovered{gate::HandleKind::None};
        gate::HandleKind dragging{gate::HandleKind::None};
    };
    GateOverlayInfo gateOverlayInfoForTests() const;
    QLabel* gatePreviewLabelForTests() const { return gatePreviewLabel_; }
    // The E-modulus table the overlay uses (a private copy); null when none is loaded.
    const backend::EModulusLut* gateLutForTests() const { return lutSnapshot_.get(); }
    // Representative position of a handle in scatter-view coordinates (NaN
    // coordinates when it is not visible).
    QPointF gateHandleViewPosForTests(gate::HandleKind kind) const;
    // The handle under a scatter-view position (None when there is none).
    gate::HandleKind gateHandleAtForTests(QPointF viewPos) const;
    // Scatter-view position of a data point (area um^2, deformability).
    QPointF scatterDataViewPosForTests(double areaUm, double deformability) const;

public slots:
    void updateRoiDisplay(int offsetX, int offsetY, int width, int height);

    // Chart snapshot capture for HDF5 saving
    bool captureChartSnapshots(cv::Mat& histogramImage, cv::Mat& scatterPlotImage) const;

    // Chart export to TIFF
    bool exportChartAsTiff(QChartView* chartView, const QString& filePath) const;
    bool exportHistogramAsTiff(const QString& filePath) const;
    bool exportScatterPlotAsTiff(const QString& filePath) const;

public:
    // Issue #364: tune panel surfaces for tests and the main window.
    static constexpr int kTunePanelMinWidth = 220;
    static constexpr int kTunePanelMaxWidth = 280;
    ProcessingConfigDraft& tuneDraft() { return draft_; }
    const ProcessingConfigDraft& tuneDraft() const { return draft_; }
    QWidget* tunePanel() const;
    QScrollArea* tuneScrollArea() const { return tuneScrollArea_; }
    QWidget* tuneFooter() const { return tuneFooter_; }
    QPushButton* tuneApplyButton() const { return tuneApplyBtn_; }
    QPushButton* tuneRevertButton() const { return tuneRevertBtn_; }
    QString tuneStateText() const;
    QWidget* tuneFieldWidget(TuneField field) const;
    // Simulate an operator edit through the bound control (signals live).
    bool setTuneFieldForTests(TuneField field, const QVariant& value);

signals:
    // Issue #364: the panel never mutates the backend or the file itself; it
    // asks the config coordinator (AppConfigWatcher) and waits for the
    // result delivered to onApplyResult().
    void applyRequested(const frontend::ApplyProcessingDraftRequest& request);
    // Dirty / conflict / applying / error state changed (for status surfaces).
    void tuneStateChanged();

private slots:
    void onUpdate();
    void onToggleOverlay(bool enabled);
    void onClearBuffer();
    void onApplyParams();
    void onRevertParams();
    void onSortTrigger();
    void onPeriodicTriggerToggled(bool checked);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

public slots:
    // Reload the authoritative config (runtime ProcessingService) into the
    // draft baseline. Local edits are retained; an exposed field that differs
    // puts the panel in Conflict instead of silently overwriting.
    void loadCurrentConfig();
    // Same, recording the document fingerprint the baseline corresponds to
    // (sent with the next apply request so a stale document is refused).
    void loadCurrentConfig(const QByteArray& documentFingerprint);
    void onApplyResult(const frontend::ConfigApplyResult& result);

private:
    void setupCharts();
    void setupTuneParamsPanel();
    void loadIsoelasticCurves();
    void updateScatterplot(const std::vector<backend::services::ProcessedFrame>& validFrames);
    void updateHistogram(const std::vector<backend::services::ProcessedFrame>& validFrames);
    void updateValidFramesGrid(const std::vector<backend::services::ProcessedFrame>& validFrames);
    void updateInvalidFramesGrid(const std::vector<backend::services::ProcessedFrame>& invalidFrames);
    QImage extractRoiImage(const cv::Mat& image, int x, int y, int w, int h) const;
    QImage matToQImage(const cv::Mat& mat) const;
    void clearGrid(QGridLayout* grid);
    QImage createOverlayImage(const cv::Mat& original, const cv::Mat& mask, const backend::services::FilterResult* validation = nullptr) const;
    // KDE colouring (see the public block above).
    void pollKdeResult();
    void pushKdeSettings(); // visibility from isVisible()
    void pushKdeSettings(bool visible);
    void setupKdeLevelSeries();
    void hideKdeLegendMarkers();
    void setKdeModeVisuals(bool on);
    void loadKdePreferences();
    void saveKdePreferences();
    // Scatter click -> frame pane (see the public block above).
    void setupFramePane();
    void onScatterClicked(QPointF viewPos, Qt::MouseButton button);
    void onScatterDoubleClicked(QPointF viewPos);
    void onScatterHover(QPointF viewPos);
    std::optional<std::size_t> scatterPointAt(QPointF viewPos) const;

    // Gate overlay: the draft target-group gate drawn on the scatter and edited
    // there. The overlay (GateOverlayItem, a child item of the chart) is drawn
    // from the tune draft, so an unapplied edit previews live; dragging a box
    // edge or an iso-E contour writes the same spin box the panel has, so the
    // draft, the `*` markers and the footer follow and Apply stays required.
    void setupGateOverlay();
    void refreshGateOverlay();
    void refreshGatePreview();
    void refreshLutSnapshot();
    std::shared_ptr<const gate::Contour> contourFor(double level) const;
    gate::GateSpec gateSpec() const; // the gate of the tune draft
    // The draft gate with what is needed to hit-test or draw it right now.
    struct GateGeometry {
        gate::GateSpec spec;
        gate::Viewport viewport;
        const backend::EModulusLut* lut{nullptr};
        std::shared_ptr<const gate::Contour> eMin, eMax; // only with the E gate on and a table
        gate::EContours contours() const { return {eMin.get(), eMax.get()}; }
    };
    GateGeometry gateGeometry() const;
    QPointF chartPosFromView(QPointF viewPos) const;
    gate::HandleHit gateHandleHit(QPointF viewPos) const;
    bool gateHandleEditable(gate::HandleKind kind) const;
    bool beginGateEdit(const QPointF& viewPos);
    void onGateDragStarted();
    void onGateDragMoved(const QPointF& viewPos);
    void onGateDragFinished(const QPointF& viewPos, bool completed);
    void setGateHover(gate::HandleKind kind);
    bool writeGateField(TuneField field, double value);
    gate::Limits gateLimits(TuneField field) const;
    QString gateHandleToolTip(gate::HandleKind kind, bool dragging) const;
    bool showGateHover(QPointF viewPos);
    scatterhit::Viewport scatterViewport() const;
    const backend::services::ProcessedFrame* recentValidFrame(uint64_t frameIndex) const;
    void selectFrame(const backend::services::ProcessedFrame& frame);
    void stepScatterSelection(int delta);
    void clearSelection();
    void refreshScatterHighlight();
    void openSelectedFrameInWindow();
    backend::services::ProcessedFrame frameForDisplay(const backend::services::ProcessedFrame& frame) const;
    OverlayMode paneOverlayMode() const;

    Ui::ExperimentMonitoringTab* ui;
    backend::AppBackend& backend_;
    QTimer* updateTimer_ = nullptr;
    QTimer* periodicTriggerTimer_ = nullptr;
    uint64_t periodicTriggerPulseCount_ = 0;
    QLabel* roiLabel_ = nullptr;

    // Panel 1: Scatterplot
    ZoomableChartView* scatterplotView_ = nullptr;
    QChart* scatterplotChart_ = nullptr;
    QScatterSeries* scatterSeries_ = nullptr;
    QScatterSeries* targetGroupSeries_ = nullptr;
    QValueAxis* scatterXAxis_ = nullptr;
    QValueAxis* scatterYAxis_ = nullptr;
    std::vector<QLineSeries*> isoelasticCurves_;
    // Plotted points (µm², deformability) and the frame index each stands
    // for, rebuilt with the series on every refresh; hit testing uses these,
    // so it works the same with KDE level series or the plain ones.
    std::vector<scatterhit::Point> scatterPoints_;
    std::vector<uint64_t> scatterPointFrames_;
    std::vector<double> scatterPointE_; // stored Young's modulus per plotted point (NaN = none)
    QScatterSeries* scatterHighlight_ = nullptr; // the selected cell, drawn on top

    // Gate overlay state (see setupGateOverlay).
    QWidget* scatterContainer_ = nullptr;   // scatter view + preview label
    QLabel* gatePreviewLabel_ = nullptr;
    GateOverlayItem* gateOverlay_ = nullptr; // owned by the chart
    // Private copy of the processing service's E-modulus table (it is loaded once
    // at backend bootstrap and read-only afterwards; the copy is taken on the GUI
    // thread and re-taken only when the service's lock-protected table identity
    // changes), so the GUI never reads the live object while it is being loaded.
    std::shared_ptr<const backend::EModulusLut> lutSnapshot_;
    std::string lutSnapshotKey_;
    uint64_t lutGeneration_ = 0;
    mutable std::map<double, std::shared_ptr<const gate::Contour>> contourCache_; // by level; cleared with the table
    struct GatePress {
        gate::HandleHit hit;
        gate::DragSession session;
    };
    std::optional<GatePress> gatePress_; // a claimed press (drag not necessarily started)
    bool gateDragging_ = false;
    gate::HandleKind gateHover_ = gate::HandleKind::None;

    // Frame pane (over the histogram) showing the selected cell.
    QSplitter* chartsRightSplitter_ = nullptr;
    QWidget* framePaneContainer_ = nullptr;
    QLabel* framePaneTitle_ = nullptr;
    FrameViewerDialog* framePane_ = nullptr;
    QPointer<FrameViewerDialog> frameWindow_; // "Open in window…", non-modal
    std::unique_ptr<backend::services::ProcessedFrame> selectedFrame_; // copy as clicked

    // Panel 2: Histogram
    ZoomableChartView* histogramView_ = nullptr;
    QChart* histogramChart_ = nullptr;
#ifndef MIB_HAS_QHISTOGRAMSERIES
#if __has_include(<QHistogramSeries>)
#define MIB_HAS_QHISTOGRAMSERIES 1
#else
#define MIB_HAS_QHISTOGRAMSERIES 0
#endif
#endif
#if MIB_HAS_QHISTOGRAMSERIES
    QHistogramSeries* histogramSeries_ = nullptr;
#else
    QBarSeries* barSeries_ = nullptr;
    QBarCategoryAxis* histogramCategoryAxis_ = nullptr;
#endif
    QValueAxis* histogramYAxis_ = nullptr;
    QValueAxis* histogramXAxis_ = nullptr;
    
    // Overlay state
    bool showValidOverlay_ = false;
    bool showInvalidOverlay_ = false;
    
    // KDE colouring state
    bool kdeEnabled_ = false;
    double kdeBandwidthFactor_ = kKdeBandwidthFactorDefault;
    int kdeIntervalMs_ = kKdeIntervalMsDefault;
    QTimer* kdeTimer_ = nullptr; // polls the backend service for a new result
    std::unordered_map<uint64_t, double> kdeDensityByIndex_; // adopted estimate
    uint64_t kdeGeneration_ = 0;
    uint64_t kdeServiceGeneration_ = 0; // service generation last looked at
    int lastKdeComputeMs_ = 0;
    std::size_t lastKdePointCount_ = 0;
    double lastKdeBandwidthX_ = 0.0;
    double lastKdeBandwidthY_ = 0.0;
    // Core contour state.
    double kdeCoreFraction_ = kKdeCoreFractionDefault;
    std::vector<std::vector<backend::monitoring::DensityPoint>> lastKdeContours_;
    double lastKdeCoreLevel_ = 0.0;
    std::size_t lastKdeCoreCount_ = 0;
    std::vector<std::vector<backend::monitoring::DensityPoint>> kdeReference_;
    QString kdeReferenceLabel_;
    QString kdeReferencePath_; // file the reference came from; empty when pinned/none
    std::shared_ptr<const backend::services::MonitoringDensityResult> kdeResult_; // adopted estimate
    void setKdeReferencePath(const QString& path);
    std::vector<QLineSeries*> kdeContourSeries_;   // solid, live
    std::vector<QLineSeries*> kdeReferenceSeries_; // dashed, reference
    void redrawKdeContours(std::vector<QLineSeries*>& family,
                           const std::vector<std::vector<backend::monitoring::DensityPoint>>& loops, bool reference);
    void refreshKdeTooltip();
    // One scatter series per density level (circles) and per level for the
    // target group (rectangles); hidden and empty while KDE is off.
    std::vector<QScatterSeries*> kdeLevelSeries_;
    std::vector<QScatterSeries*> kdeTargetLevelSeries_;

    // Fixed chart axis ranges (user-definable)
    double scatterXMin_ = 0.0;
    double scatterXMax_ = 1000.0;
    double scatterYMin_ = 0.0;
    double scatterYMax_ = 1.0;
    double histogramXMin_ = 15.0;
    double histogramXMax_ = 25.0;
    double histogramYMax_ = 100.0;
    double histogramBinWidth_ = 0.5;

    // Rolling buffers to maintain recent frames even after flush
    std::vector<backend::services::ProcessedFrame> recentValidFrames_;
    std::vector<backend::services::ProcessedFrame> recentInvalidFrames_;
    uint64_t lastValidFrameIndex_ = 0;
    uint64_t lastInvalidFrameIndex_ = 0;

    static constexpr int THUMBNAIL_SIZE = 100;
    static constexpr int GRID_COLUMNS = 5;
    static constexpr int MAX_FRAMES_TO_SHOW = 25;
    static constexpr int MAX_RECENT_FRAMES = 1000; // Keep more frames for scatterplot/histogram
    static constexpr int UPDATE_INTERVAL_MS = 500;

    // Tune params panel (issue #364): every exposed field is bound once;
    // the draft model owns baseline/changed/dirty/conflict state.
    struct TuneBinding {
        TuneField field;
        QWidget* widget{nullptr};
        QWidget* rowLabel{nullptr};
        std::function<QVariant()> read;
        std::function<void(const QVariant&)> write;
    };
    void bindTuneField(TuneField field, QWidget* widget, QWidget* rowLabel,
                       std::function<QVariant()> read, std::function<void(const QVariant&)> write);
    void onTuneFieldEdited(TuneField field, const QVariant& value);
    void populateTuneWidgetsFromDraft();
    void refreshTuneFooter();
    void refreshTuneChangeMarkers();

    QWidget* tunePanelContent_ = nullptr;
    QScrollArea* tuneScrollArea_ = nullptr;
    QWidget* tuneFooter_ = nullptr;
    QLabel* tuneStateLabel_ = nullptr;
    QLabel* tuneValidationLabel_ = nullptr;
    QPushButton* tuneApplyBtn_ = nullptr;
    QPushButton* tuneRevertBtn_ = nullptr;
    std::vector<TuneBinding> tuneBindings_;
    ProcessingConfigDraft draft_;
    QByteArray documentFingerprint_;
    bool tuneLoading_ = false;
};

} // namespace frontend

