#pragma once

#include "frontend/models/ProcessingConfigDraft.h"

#include <QWidget>
#include <QImage>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace cv { class Mat; }
namespace backend { class AppBackend; }
namespace backend::services { struct ProcessedFrame; struct FilterResult; }

class QTimer;
template <typename T> class QFutureWatcher;
class QChartView;
namespace frontend { class ZoomableChartView; }
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

namespace Ui { class ExperimentMonitoringTab; }

namespace frontend {

class ExperimentMonitoringTab : public QWidget {
    Q_OBJECT
public:
    explicit ExperimentMonitoringTab(backend::AppBackend& backend, QWidget* parent = nullptr);
    ~ExperimentMonitoringTab() override;
    
    // Scatter density (KDE) colouring. While enabled, every valid point in the
    // Deformability-vs-Area scatter is coloured by its normalised population
    // density; the estimate is recomputed periodically on a worker thread
    // (QtConcurrent) from a snapshot of the rolling buffer and never blocks
    // the 500 ms chart refresh, which only re-applies the last result. The
    // three settings persist in QSettings (Monitoring/Kde*).
    struct KdeResult {
        std::vector<uint64_t> frameIndices;
        std::vector<double> density; // normalised [0, 1], parallel to frameIndices
        double bandwidthX{0.0};      // µm²
        double bandwidthY{0.0};      // deformability
        int computeMs{0};
    };
    static constexpr double kKdeBandwidthFactorMin = 0.2;
    static constexpr double kKdeBandwidthFactorMax = 5.0;
    static constexpr double kKdeBandwidthFactorDefault = 1.0;
    static constexpr int kKdeIntervalMsMin = 500;
    static constexpr int kKdeIntervalMsMax = 60000;
    static constexpr int kKdeIntervalMsDefault = 2000;
    bool kdeEnabled() const { return kdeEnabled_; }
    void setKdeEnabled(bool enabled);
    double kdeBandwidthFactor() const { return kdeBandwidthFactor_; }
    void setKdeBandwidthFactor(double factor);
    int kdeIntervalMs() const { return kdeIntervalMs_; }
    void setKdeIntervalMs(int ms);
    // Compute now (what the periodic timer does); a no-op while disabled, while
    // a job is already running, or when the buffer is unchanged since the last
    // completed estimate.
    void requestKdeUpdate();
    bool kdeJobInFlight() const { return kdeWatcher_ != nullptr; }
    bool kdeTimerActive() const;
    uint64_t kdeGeneration() const { return kdeGeneration_; } // completed estimates
    int lastKdeComputeMs() const { return lastKdeComputeMs_; }
    std::size_t lastKdePointCount() const { return lastKdePointCount_; }
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
    void onKdeJobFinished();
    void setupKdeLevelSeries();
    void hideKdeLegendMarkers();
    void setKdeModeVisuals(bool on);
    void loadKdePreferences();
    void saveKdePreferences();

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
    QTimer* kdeTimer_ = nullptr;
    QFutureWatcher<KdeResult>* kdeWatcher_ = nullptr;
    std::unordered_map<uint64_t, double> kdeDensityByIndex_; // last completed estimate
    uint64_t kdeGeneration_ = 0;
    int lastKdeComputeMs_ = 0;
    std::size_t lastKdePointCount_ = 0;
    struct KdeFingerprint {
        std::size_t count{0};
        uint64_t firstIndex{0};
        uint64_t lastIndex{0};
        double bandwidthFactor{0.0};
        double areaConversion{0.0};
        bool operator==(const KdeFingerprint& o) const
        {
            return count == o.count && firstIndex == o.firstIndex && lastIndex == o.lastIndex
                   && bandwidthFactor == o.bandwidthFactor && areaConversion == o.areaConversion;
        }
    };
    KdeFingerprint kdeFingerprint_; // input of the last launched estimate
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

