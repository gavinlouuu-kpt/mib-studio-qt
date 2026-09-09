#pragma once

#include "frontend/models/ProcessingConfigDraft.h"

#include <QWidget>
#include <QImage>
#include <cstdint>
#include <functional>
#include <vector>

namespace cv { class Mat; }
namespace backend { class AppBackend; }
namespace backend::services { struct ProcessedFrame; struct FilterResult; }

class QTimer;
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
    
    // Settings accessors
    double getKdeBandwidth() const { return kdeBandwidth_; }
    int getKdeGridResolution() const { return kdeGridResolution_; }
    void setKdeBandwidth(double bandwidth);
    void setKdeGridResolution(int resolution);

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
    std::vector<std::vector<double>> computeKDE(const std::vector<std::pair<double, double>>& points,
                                                 int gridX, int gridY, double bandwidth) const;

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
    
    // KDE settings
    double kdeBandwidth_ = 50.0;
    int kdeGridResolution_ = 50;

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

