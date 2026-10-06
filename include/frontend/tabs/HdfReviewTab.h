#pragma once

#include <QWidget>
#include <QImage>
#include <QCache>
#include <optional>
#include <vector>
#include <cstdint>
#include <memory>
#include <QString>
#include <QPointF>
#include <utility>

namespace cv
{
    class Mat;
}
namespace backend
{
    class AppBackend;
}
namespace backend::services
{
    class Hdf5Service;
    struct ProcessedFrame;
}

#include "backend/processing/ProcessingService.h"
#include "backend/recording/HdfExportService.h"
#include "frontend/utils/OverlayRenderer.h"
#include "frontend/utils/ScatterHitTest.h"

#include <functional>
#include <map>

class QPushButton;
class QComboBox;
class QLabel;
class QTabWidget;
class QTableView;
class QGridLayout;
class QScrollArea;
class QVBoxLayout;
class QHBoxLayout;
class QCheckBox;
class QSpacerItem;
class QChartView;
class QChart;
class QScatterSeries;
class QLineSeries;
class QValueAxis;
class QProgressDialog;
class QToolButton;
class QAction;
class QSplitter;
class QStackedWidget;
namespace frontend { class ElidingLabel; class FrameViewerDialog; class ZoomableChartView; }
namespace backend::monitoring { struct KdeCoreRecord; }
template<typename T> class QFutureWatcher;
#if __has_include(<QHistogramSeries>)
class QHistogramSeries;
#else
class QBarSeries;
class QBarCategoryAxis;
#endif

namespace frontend
{
    class HdfMetricsModel;
}

namespace Ui { class HdfReviewTab; }

namespace frontend
{

    class HdfReviewTab : public QWidget
    {
        Q_OBJECT
    public:
        explicit HdfReviewTab(backend::AppBackend &backend, QWidget *parent = nullptr);
        ~HdfReviewTab() override;

        // Test hooks: open a file as "Select File" would, and inspect the
        // stored KDE core contours drawn on the scatter (full-run solid,
        // live/provisional dashed).
        void loadHdfFileForTests(const QString &filePath) { loadHdfFile(filePath); }
        const std::vector<QLineSeries*> &storedKdeContourSeriesForTests() const { return storedKdeSeries_; }
        bool hasStoredKdeLive() const { return !storedKdeLive_.empty(); }
        bool hasStoredKdeAnalysis() const { return !storedKdeAnalysis_.empty(); }
        // Full-run core contour (scatter context menu "Compute core contour
        // from full run"): computed on a worker from the file's valid cells,
        // saved as /analysis @kde_core_json after confirming an overwrite.
        void computeFullRunCoreForTests() { startFullRunCoreComputation(); }
        bool fullRunCoreJobInFlight() const { return coreWatcher_ != nullptr; }
        // nullopt: ask with a dialog (default); true/false: answer for tests.
        void setOverwriteAnswerForTests(std::optional<bool> answer) { overwriteAnswerForTests_ = answer; }
        QString statusTextForTests() const;
        // The px→µm the tab uses for the open file (recorded, else live).
        double pixelToMicronForTests() const { return filePixelToMicron(); }
        QAction *computeCoreAction() const { return computeCoreAction_; }

        // Scatter interaction (issue #467): click a point to show the cell in
        // the frame pane docked beside the scatter.
        ZoomableChartView *scatterViewForTests() const { return scatterPlotView_; }
        QScatterSeries *scatterHighlightForTests() const { return scatterHighlight_; }
        QWidget *framePaneForTests() const;
        // Frame index + dataset (true = valid) the pane shows, if any.
        std::optional<std::pair<int, bool>> framePaneFrameForTests() const;
        const std::vector<int> &scatterPointToFrameForTests() const { return scatterPointToFrame_; }
        // Pixel position of scatter point `point` in scatter-view coordinates.
        QPointF scatterPointViewPosForTests(int point) const;
        // Replaces the modal viewer that "Open in window…" would exec().
        void setFrameViewerSinkForTests(std::function<void(int, bool)> sink) { frameViewerSinkForTests_ = std::move(sink); }
        void stepScatterSelectionForTests(int delta) { stepScatterSelection(delta); }
        int selectedFrameForTests() const { return selectedFrameIndex_; }
        // The chart snapshots Export All / Export Charts would write.
        std::map<std::string, cv::Mat> renderChartSnapshotsForTests();

    private slots:
        void onSelectFile();
        void onCloseFile();
        void onExportMetrics();
        void onExportAll();
        void onBatchExportMetrics();
        void onBatchExportAll();
        void onExportCharts();
        void onOverlayModeChanged(int index);
        void onToggleRoiOverlay(bool enabled);
        void onTabChanged(int index);
        void onThumbnailClicked(int frameIndex);
        void onThumbnailDoubleClicked(int frameIndex);
        void onTableSelectionChanged();
        void onViewFrameDetails(int frameIndex);
        void onRegenerateMasks();

    private:
        QString accountingSummary() const; // issue #367 Review summary
        void setupCharts();
        void loadHdfFile(const QString &filePath);
        void populateFrames(const std::vector<backend::services::ProcessedFrame> &frames, bool isValid);
        void clearDisplay();
        void updateImageGrid(const std::vector<backend::services::ProcessedFrame> &frames);
        void updateMetricsTable(const std::vector<backend::services::ProcessedFrame> &frames);
        void loadThumbnailsBatch(const std::vector<backend::services::ProcessedFrame> &frames,
                                 size_t startIndex, size_t count, bool isValid);
        QImage matToQImage(const cv::Mat &mat) const;
        // `valid` names the dataset explicitly: isShowingValid_ is false on
        // the Charts tab, where the scatter selects valid frames.
        void setSelectedFrame(int frameIndex, bool valid);
        void onScrollValueChanged(int value);
        QImage drawRoiOverlay(const QImage &image, int imgWidth, int imgHeight) const;
        void showFrameViewer(int frameIndex, bool valid);
        // Image, mask and multi-image series of one frame, read on demand.
        backend::services::ProcessedFrame loadFrameForDisplay(int frameIndex, bool valid) const;
        // Scatter interaction + docked frame pane (issue #467).
        void setupChartsLayout();
        void onScatterClicked(QPointF viewPos, Qt::MouseButton button);
        void onScatterDoubleClicked(QPointF viewPos);
        void onScatterHover(QPointF viewPos);
        std::optional<std::size_t> scatterPointAt(QPointF viewPos) const;
        scatterhit::Viewport scatterViewport() const;
        void selectScatterFrame(int frameIndex);
        void stepScatterSelection(int delta);
        void setScatterHighlight(int frameIndex);
        void raiseScatterHighlight();
        void refreshFramePane();
        bool chartsTabVisible() const;
        struct ScatterViewState {
            double x0{0}, x1{0}, y0{0}, y1{0};
            bool userZoomed{false};
            int highlightFrame{-1};
        };
        ScatterViewState saveScatterView() const;
        void restoreScatterView(const ScatterViewState& state);
        // Carousel/refresh helpers
        void computeVisibleRange(bool isValid, size_t &outStartIndex, size_t &outEndIndex) const;
        void refreshVisibleThumbnails(bool isValid);
        void pruneOffscreenThumbnails(bool isValid);
        QImage buildThumbnailForIndex(size_t index, bool isValid);
        void loadRecordingSeriesWindow(size_t frameIndex, backend::services::ProcessedFrame& frame) const;
        // Dataset-path helpers that route to /recorded_frames/* when
        // isRecordingMode_ is true, else to /valid_frames/* or /invalid_frames/*.
        // masksPath() returns "" in recording mode (no masks written).
        std::string imagesPath(bool isValid) const;
        std::string masksPath(bool isValid) const;
        // Issue #344: asynchronous, single-flight export through the Qt-free
        // backend::recording::HdfExportService (own reader per job, progress +
        // cancellation, transactional output). Batch jobs are chained on the
        // GUI thread without swapping the live reader/model state.
        struct BatchExportState {
            QStringList sources;
            QStringList destinations;
            QString root;
            bool metricsOnly{false};
            int index{0};
            int exported{0};
            QStringList failures;
        };
        bool exportInProgress() const { return exportWatcher_ != nullptr; }
        // Issue #358: bounded file row — secondary actions live in a native
        // "More" menu, the path is elided (full value in tooltip/copy).
        void setupBoundedFileRow();
        void updateSecondaryActionState();
        void setFilePathText(const QString& text);
        bool beginExportJob(backend::recording::HdfExportRequest request, const QString& title,
                            std::function<void(const backend::recording::HdfExportResult&)> onDone);
        void onExportProgress(const backend::recording::HdfExportProgress& progress);
        void onExportJobFinished();
        void finishExportUi();
        void setExportControlsEnabled(bool enabled);
        QString exportSummary(const backend::recording::HdfExportResult& result) const;
        void reportExportNotCompleted(const QString& title, const backend::recording::HdfExportResult& result);
        std::map<std::string, cv::Mat> renderChartSnapshots(const std::vector<backend::services::ProcessedFrame>& validFrames);
        void continueBatchExport();
        void updateCharts();
        void generateScatterPlot(const std::vector<backend::services::ProcessedFrame>& validFrames);
        void generateHistogram(const std::vector<backend::services::ProcessedFrame>& validFrames);
        void loadIsoelasticCurves();
        QPixmap chartToPixmap(QChartView* chartView) const;
        QString metricsExportDir() const;
        QString exportAllRootDir() const;
        void rememberMetricsExportDir(const QString& dirPath);
        void rememberExportAllRootDir(const QString& dirPath);

        Ui::HdfReviewTab* ui;
        backend::AppBackend &backend_;
        std::unique_ptr<backend::services::Hdf5Service> hdfReader_;

        // Charts (created in C++)
        ZoomableChartView *scatterPlotView_ = nullptr;
        QChart *scatterPlotChart_ = nullptr;
        QScatterSeries *scatterSeries_ = nullptr;
        QValueAxis *scatterXAxis_ = nullptr;
        QValueAxis *scatterYAxis_ = nullptr;
        std::vector<QLineSeries*> isoelasticCurves_;
        // One point drawn on top of every other series: the selected cell.
        QScatterSeries *scatterHighlight_ = nullptr;
        // Scatter points (µm², deformability) and the frame each stands for;
        // point k is not frame k (frames failing validation have no point).
        std::vector<scatterhit::Point> scatterPoints_;
        std::vector<int> scatterPointToFrame_;
        std::vector<int> frameToScatterPoint_;
        // False while the scatter shows another file's data (batch export
        // snapshots); clicks then select nothing.
        bool scatterShowsLiveFile_ = false;
        int highlightFrame_ = -1;
        std::optional<ScatterViewState> batchScatterRestore_;
        // Charts view: scatter | (frame pane over histogram).
        QSplitter *chartsSplitter_ = nullptr;
        QSplitter *chartsRightSplitter_ = nullptr;
        QStackedWidget *framePaneStack_ = nullptr;
        FrameViewerDialog *framePane_ = nullptr;
        QLabel *framePaneTitle_ = nullptr;
        int paneFrame_ = -1; // frame the pane currently shows (valid set)
        bool paneStale_ = false;      // a selection changed behind the modal viewer
        bool modalViewerOpen_ = false;
        bool settingSelection_ = false;
        std::function<void(int, bool)> frameViewerSinkForTests_;
        // Stored KDE core contours of the open file (loops in µm² /
        // deformability) and the series drawing them on the scatter.
        std::vector<std::vector<std::pair<double, double>>> storedKdeLive_;
        std::vector<std::vector<std::pair<double, double>>> storedKdeAnalysis_;
        double storedKdeLiveFraction_ = 0.0;
        double storedKdeAnalysisFraction_ = 0.0;
        std::vector<QLineSeries*> storedKdeSeries_;
        void readStoredKdeRecords();
        void drawStoredKdeContours();
        // Recorded factor of the open file, else the live processing factor.
        double filePixelToMicron() const;
        double pixelToMicronOf(const backend::services::Hdf5Service &reader) const;
        void startFullRunCoreComputation();
        void onFullRunCoreFinished();
        void updateComputeCoreActionState();
        QAction *computeCoreAction_ = nullptr;
        QFutureWatcher<backend::monitoring::KdeCoreRecord> *coreWatcher_ = nullptr;
        QString coreJobPath_;
        std::optional<bool> overwriteAnswerForTests_;
        QChartView *histogramView_ = nullptr;
        QChart *histogramChart_ = nullptr;
#if __has_include(<QHistogramSeries>)
        QHistogramSeries *histogramSeries_ = nullptr;
#else
        QBarSeries *histogramBarSeries_ = nullptr;
        QBarCategoryAxis *histogramCategoryAxis_ = nullptr;
#endif
        QValueAxis *histogramXAxis_ = nullptr;
        QValueAxis *histogramYAxis_ = nullptr;

        // Models
        HdfMetricsModel *validMetricsModel_ = nullptr;
        HdfMetricsModel *invalidMetricsModel_ = nullptr;

        // Spacers for grid layout
        QSpacerItem *validBottomSpacer_ = nullptr;
        QSpacerItem *validTopSpacer_ = nullptr;
        QSpacerItem *invalidBottomSpacer_ = nullptr;
        QSpacerItem *invalidTopSpacer_ = nullptr;

        // Data
        std::vector<backend::services::ProcessedFrame> validFrames_;
        std::vector<backend::services::ProcessedFrame> invalidFrames_;
        int selectedFrameIndex_ = -1;
        bool selectedFrameValid_ = true;
        bool isShowingValid_ = true;
        OverlayMode overlayMode_{OverlayMode::None};
        bool showRoiOverlay_ = false;
        bool isRecordingMode_ = false;
        bool recordingMultiImageEnabled_ = false;
        size_t recordingMultiImageCount_ = 1;
        backend::services::ProcessingService::Roi roi_{0, 0, 0, 0};
        QString loadedHdfFilePath_;
        // TD-17: px→µm of the open file (its run snapshot's factor; the live
        // processing factor only for files that record none). See filePixelToMicron().
        double filePixelToMicron_ = 0.0;
        QString lastExportDir_;
        QFutureWatcher<backend::recording::HdfExportResult>* exportWatcher_ = nullptr;
        QProgressDialog* exportProgress_ = nullptr;
        backend::recording::HdfExportCancelToken exportCancel_;
        std::function<void(const backend::recording::HdfExportResult&)> exportDone_;
        std::unique_ptr<BatchExportState> batch_;
        QToolButton* moreActionsBtn_ = nullptr;
        QAction* batchMetricsAct_ = nullptr;
        QAction* batchAllAct_ = nullptr;
        QAction* exportChartsAct_ = nullptr;
        QAction* regenerateMasksAct_ = nullptr;
        frontend::ElidingLabel* filePathLabel_ = nullptr;

        static constexpr int THUMBNAIL_SIZE = 128;
        static constexpr int GRID_COLUMNS = 5;
        static constexpr size_t INITIAL_THUMBNAIL_COUNT = 200; // Load first 200 thumbnails
        static constexpr size_t BATCH_THUMBNAIL_COUNT = 100;   // Load 100 more when scrolling

        size_t validThumbnailsLoaded_ = 0;
        size_t invalidThumbnailsLoaded_ = 0;

        // Thumbnail cache (key encodes frame type + index)
        QCache<qulonglong, QImage> thumbnailCache_;

        // Preserve scroll positions per tab
        int validScrollValue_ = 0;
        int invalidScrollValue_ = 0;
    };

} // namespace frontend
