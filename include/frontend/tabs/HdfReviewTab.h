#pragma once

#include <QWidget>
#include <QImage>
#include <QCache>
#include <vector>
#include <cstdint>
#include <memory>
#include <QString>

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
namespace frontend { class ElidingLabel; }
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
        void setSelectedFrame(int frameIndex);
        void onScrollValueChanged(int value);
        QImage drawRoiOverlay(const QImage &image, int imgWidth, int imgHeight) const;
        void showFrameViewer(int frameIndex);
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
        QChartView *scatterPlotView_ = nullptr;
        QChart *scatterPlotChart_ = nullptr;
        QScatterSeries *scatterSeries_ = nullptr;
        QValueAxis *scatterXAxis_ = nullptr;
        QValueAxis *scatterYAxis_ = nullptr;
        std::vector<QLineSeries*> isoelasticCurves_;
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
        bool isShowingValid_ = true;
        OverlayMode overlayMode_{OverlayMode::None};
        bool showRoiOverlay_ = false;
        bool isRecordingMode_ = false;
        bool recordingMultiImageEnabled_ = false;
        size_t recordingMultiImageCount_ = 1;
        backend::services::ProcessingService::Roi roi_{0, 0, 0, 0};
        QString loadedHdfFilePath_;
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
