#include "frontend/tabs/HdfReviewTab.h"
#include "ui_HdfReviewTab.h"

#include <memory>

#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QFrame>
#include <QSpacerItem>
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QInputDialog>
#include <QSettings>
#include <QStringList>
#include <QScrollBar>
#include <QEventLoop>
#include <QComboBox>
#include <QChartView>
#include <QLineSeries>
#include <QLegendMarker>
#include <QPen>
#include <QCoreApplication>
#include <QDir>
#include <map>
#include <QScatterSeries>
#include <QChart>
#include <QValueAxis>
#include <algorithm>
#include <limits>
#ifndef MIB_HAS_QHISTOGRAMSERIES
#if __has_include(<QHistogramSeries>)
#define MIB_HAS_QHISTOGRAMSERIES 1
#else
#define MIB_HAS_QHISTOGRAMSERIES 0
#endif
#endif
#if MIB_HAS_QHISTOGRAMSERIES
#include <QHistogramSeries>
#else
#include <QBarSeries>
#include <QBarSet>
#include <QBarCategoryAxis>
#endif

#include "backend/app/AppBackend.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/KdeCoreRecord.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/dialogs/BatchMaskDialog.h"
#include "frontend/dialogs/FrameViewerDialog.h"
#include "frontend/models/HdfMetricsModel.h"
#include "frontend/utils/OverlayRenderer.h"
#include "frontend/utils/HdfReviewExportPaths.h"
#include "backend/recording/HdfExportService.h"
#include "frontend/utils/ElidingLabel.h"
#include "frontend/widgets/ZoomableChartView.h"

#include <QCursor>
#include <QLabel>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolTip>

#include <QToolButton>
#include <QMenu>
#include <QAction>

#include <QFutureWatcher>
#include <QPointer>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrent>
#include <chrono>
#include <cmath>

#include <spdlog/spdlog.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kLastExportDirSetting = "HdfReviewTab/lastExportDir";

struct HdfReviewLoadData {
    std::unique_ptr<backend::services::Hdf5Service> reader;
    std::vector<backend::services::ProcessedFrame> validFrames;
    std::vector<backend::services::ProcessedFrame> invalidFrames;
    bool isRecordingMode{false};
    bool recordingMultiImageEnabled{false};
    size_t recordingMultiImageCount{1};
};

bool loadHdfReviewData(const QString& filePath, HdfReviewLoadData& outData, QString* errorMessage)
{
    HdfReviewLoadData loaded;
    loaded.reader = std::make_unique<backend::services::Hdf5Service>();
    if (!loaded.reader->loadFile(filePath.toStdString())) {
        if (errorMessage) {
            *errorMessage = QFile::exists(filePath)
                ? QObject::tr("File exists but could not be opened as HDF5")
                : QObject::tr("File not found");
        }
        return false;
    }

    loaded.isRecordingMode = loaded.reader->isRecordingFile();
    if (loaded.isRecordingMode) {
        uint64_t startTimeNs = 0;
        uint64_t endTimeNs = 0;
        uint64_t totalFrames = 0;
        uint64_t filteredFrames = 0;
        bool multiImageEnabled = false;
        uint64_t multiImageCount = 1;
        loaded.reader->readRecordingInfo(startTimeNs,
                                         endTimeNs,
                                         totalFrames,
                                         filteredFrames,
                                         &multiImageEnabled,
                                         &multiImageCount);
        loaded.recordingMultiImageEnabled = multiImageEnabled;
        loaded.recordingMultiImageCount = static_cast<size_t>(std::max<uint64_t>(multiImageCount, 1));
        if (!loaded.reader->readRecordingMetadata(loaded.validFrames)) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Failed to read recording metadata");
            }
            return false;
        }
    } else {
        if (!loaded.reader->readValidMetadata(loaded.validFrames)) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Failed to read valid-frame metadata");
            }
            return false;
        }
        if (!loaded.reader->readInvalidMetadata(loaded.invalidFrames)) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Failed to read invalid-frame metadata");
            }
            return false;
        }
    }

    outData = std::move(loaded);
    return true;
}

QString trimmedFailureList(const QStringList& failures)
{
    constexpr int kMaxShown = 8;
    QStringList shown = failures.mid(0, kMaxShown);
    if (failures.size() > kMaxShown) {
        shown << QObject::tr("...and %1 more").arg(failures.size() - kMaxShown);
    }
    return shown.join(QStringLiteral("\n"));
}

struct SeriesExportSelection {
    bool exportSeriesImages{false};
    size_t startInclusive{0}; // zero-based
    size_t endInclusive{0};   // zero-based
};

bool promptSeriesExportSelection(QWidget* parent,
                                 size_t seriesCount,
                                 SeriesExportSelection& outSelection) {
    outSelection = SeriesExportSelection{};
    if (seriesCount == 0) {
        return true;
    }

    const int maxSeriesInt = static_cast<int>(std::min(
        seriesCount, static_cast<size_t>(std::numeric_limits<int>::max())));
    if (maxSeriesInt <= 0) {
        return true;
    }

    QMessageBox modeDialog(parent);
    modeDialog.setIcon(QMessageBox::Question);
    modeDialog.setWindowTitle(QObject::tr("Series Export Options"));
    modeDialog.setText(QObject::tr("Multi-image mode has %1 frames per detection.")
                           .arg(maxSeriesInt));
    modeDialog.setInformativeText(
        QObject::tr("Choose how series frames should be exported "
                    "(example custom range: 9 to 15)."));

    auto* allButton = modeDialog.addButton(
        QObject::tr("All %1 Frames").arg(maxSeriesInt), QMessageBox::AcceptRole);
    auto* customButton = modeDialog.addButton(
        QObject::tr("Custom Range..."), QMessageBox::ActionRole);
    auto* skipButton = modeDialog.addButton(
        QObject::tr("Skip Series Frames"), QMessageBox::DestructiveRole);
    modeDialog.addButton(QMessageBox::Cancel);
    modeDialog.exec();

    if (modeDialog.clickedButton() == allButton) {
        outSelection.exportSeriesImages = true;
        outSelection.startInclusive = 0;
        outSelection.endInclusive = static_cast<size_t>(maxSeriesInt - 1);
        return true;
    }

    if (modeDialog.clickedButton() == customButton) {
        bool startOk = false;
        const int startFrame = QInputDialog::getInt(
            parent,
            QObject::tr("Series Range"),
            QObject::tr("Start frame (1-based):"),
            1,
            1,
            maxSeriesInt,
            1,
            &startOk);
        if (!startOk) {
            return false;
        }

        bool endOk = false;
        const int endFrame = QInputDialog::getInt(
            parent,
            QObject::tr("Series Range"),
            QObject::tr("End frame (1-based):"),
            maxSeriesInt,
            startFrame,
            maxSeriesInt,
            1,
            &endOk);
        if (!endOk) {
            return false;
        }

        outSelection.exportSeriesImages = true;
        outSelection.startInclusive = static_cast<size_t>(startFrame - 1);
        outSelection.endInclusive = static_cast<size_t>(endFrame - 1);
        return true;
    }

    if (modeDialog.clickedButton() == skipButton) {
        outSelection.exportSeriesImages = false;
        return true;
    }

    return false;
}

} // namespace

namespace frontend {

class ThumbnailLabel : public QLabel {
    Q_OBJECT
public:
    explicit ThumbnailLabel(int frameIndex, int thumbnailSize, QWidget* parent = nullptr)
        : QLabel(parent), frameIndex_(frameIndex) {
        setAlignment(Qt::AlignCenter);
        setFrameStyle(QFrame::Box);
        setLineWidth(2);
        setStyleSheet("QLabel { border: 2px solid gray; }");
        setMinimumSize(thumbnailSize, thumbnailSize);
        setMaximumSize(thumbnailSize, thumbnailSize);
        setScaledContents(false);
    }

    void setSelected(bool selected) {
        if (selected) {
            setStyleSheet("QLabel { border: 3px solid blue; background-color: lightblue; }");
        } else {
            setStyleSheet("QLabel { border: 2px solid gray; }");
        }
    }

    int frameIndex() const { return frameIndex_; }

signals:
    void clicked(int frameIndex);
    void doubleClicked(int frameIndex);

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            emit clicked(frameIndex_);
        }
        QLabel::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            emit doubleClicked(frameIndex_);
        }
        QLabel::mouseDoubleClickEvent(event);
    }

private:
    int frameIndex_;
};

// Issue #367: Review distinguishes empty / scientifically rejected /
// processing failed / store loss / persistence failure and shows the
// recorded completion state. Legacy files without accounting say so
// explicitly instead of being presented as complete.
QString HdfReviewTab::accountingSummary() const
{
    if (!hdfReader_) return {};
    backend::recording::RecordingAccountingSnapshot a;
    if (!hdfReader_->readRunAccounting(a)) {
        return tr(" · accounting: not recorded (legacy file)");
    }
    const uint64_t storeLoss = a.storeOverwritten + a.storeNotCommitted + a.storeMalformed;
    QString text = tr(" · run %1").arg(QString::fromLatin1(backend::recording::toString(a.completion)));
    if (!a.reconciled) text += tr(" (accounting does not reconcile)");
    text += tr(" — empty %1, rejected %2, processing failed %3, store loss %4, persisted %5/%6, persistence failed %7")
                .arg(static_cast<qulonglong>(a.empty))
                .arg(static_cast<qulonglong>(a.scientificallyRejected))
                .arg(static_cast<qulonglong>(a.processingFailed))
                .arg(static_cast<qulonglong>(storeLoss))
                .arg(static_cast<qulonglong>(a.persistenceCommitted))
                .arg(static_cast<qulonglong>(a.persistenceAdmitted))
                .arg(static_cast<qulonglong>(a.persistenceFailed));
    return text;
}

HdfReviewTab::HdfReviewTab(backend::AppBackend& backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::HdfReviewTab), backend_(backend) {
    ui->setupUi(this);

    QSettings settings;
    lastExportDir_ = settings.value(kLastExportDirSetting, QDir::homePath()).toString();

    // Configure thumbnail cache (store up to ~2048 thumbnails)
    thumbnailCache_.setMaxCost(2048);
    SPDLOG_INFO("HdfReviewTab: thumbnail cache size set to {}", 2048);

    // Connect button signals
    connect(ui->selectFileBtn, &QPushButton::clicked, this, &HdfReviewTab::onSelectFile);
    connect(ui->closeFileBtn, &QPushButton::clicked, this, &HdfReviewTab::onCloseFile);
    connect(ui->exportMetricsBtn, &QPushButton::clicked, this, &HdfReviewTab::onExportMetrics);
    connect(ui->exportAllBtn, &QPushButton::clicked, this, &HdfReviewTab::onExportAll);
    connect(ui->batchExportMetricsBtn, &QPushButton::clicked, this, &HdfReviewTab::onBatchExportMetrics);
    connect(ui->batchExportAllBtn, &QPushButton::clicked, this, &HdfReviewTab::onBatchExportAll);
    connect(ui->exportChartsBtn, &QPushButton::clicked, this, &HdfReviewTab::onExportCharts);
    connect(ui->regenerateMasksBtn, &QPushButton::clicked, this, &HdfReviewTab::onRegenerateMasks);
    setupBoundedFileRow();
    connect(ui->overlayModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HdfReviewTab::onOverlayModeChanged);
    connect(ui->roiOverlayCheck, &QCheckBox::toggled, this, &HdfReviewTab::onToggleRoiOverlay);

    // Setup valid frames tab widgets
    connect(ui->validImageScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &HdfReviewTab::onScrollValueChanged);
    ui->validMetricsTable->horizontalHeader()->setStretchLastSection(true);
    validMetricsModel_ = new HdfMetricsModel(ui->validMetricsTable);
    validMetricsModel_->setPixelToMicronFactor(backend_.processing().getPixelToMicronFactor());
    validMetricsModel_->setSource(&validFrames_);
    ui->validMetricsTable->setModel(validMetricsModel_);

    // Setup invalid frames tab widgets
    connect(ui->invalidImageScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &HdfReviewTab::onScrollValueChanged);
    ui->invalidMetricsTable->horizontalHeader()->setStretchLastSection(true);
    invalidMetricsModel_ = new HdfMetricsModel(ui->invalidMetricsTable);
    invalidMetricsModel_->setPixelToMicronFactor(backend_.processing().getPixelToMicronFactor());
    invalidMetricsModel_->setSource(&invalidFrames_);
    ui->invalidMetricsTable->setModel(invalidMetricsModel_);

    // Add bottom spacers to grids
    validBottomSpacer_ = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
    ui->validImageGrid->addItem(validBottomSpacer_, 0, 0, 1, GRID_COLUMNS);
    invalidBottomSpacer_ = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
    ui->invalidImageGrid->addItem(invalidBottomSpacer_, 0, 0, 1, GRID_COLUMNS);

    // Charts tab - replace placeholders with actual chart views
    // Left side: Scatter plot chart
    scatterPlotChart_ = new QChart();
    scatterSeries_ = new QScatterSeries();
    scatterSeries_->setMarkerSize(6.0);
    scatterSeries_->setName("Valid Frames");
    scatterPlotChart_->addSeries(scatterSeries_);
    scatterPlotChart_->setTitle("Deformability vs Area (μm²)");
    scatterPlotChart_->legend()->setVisible(false);
    
    scatterXAxis_ = new QValueAxis();
    scatterXAxis_->setTitleText("Area (μm²)");
    scatterYAxis_ = new QValueAxis();
    scatterYAxis_->setTitleText("Deformability");
    scatterPlotChart_->addAxis(scatterXAxis_, Qt::AlignBottom);
    scatterPlotChart_->addAxis(scatterYAxis_, Qt::AlignLeft);
    scatterSeries_->attachAxis(scatterXAxis_);
    scatterSeries_->attachAxis(scatterYAxis_);
    
    // Load isoelastic curves overlay
    loadIsoelasticCurves();
    
    // The selected cell: one point drawn above every other series.
    scatterHighlight_ = new QScatterSeries();
    scatterHighlight_->setObjectName(QStringLiteral("scatterHighlight"));
    scatterHighlight_->setName(tr("Selected cell"));
    scatterHighlight_->setMarkerSize(13.0);
    scatterHighlight_->setColor(QColor(0xf2, 0x8e, 0x2b));
    scatterHighlight_->setBorderColor(Qt::white);
    scatterHighlight_->setVisible(false);
    scatterPlotChart_->addSeries(scatterHighlight_);
    scatterHighlight_->attachAxis(scatterXAxis_);
    scatterHighlight_->attachAxis(scatterYAxis_);
    raiseScatterHighlight();

    // Wheel zoom, drag pan, single click selects a cell (issue #467).
    scatterPlotView_ = new ZoomableChartView(scatterPlotChart_, ui->chartsTab);
    scatterPlotView_->setObjectName(QStringLiteral("reviewScatterView"));
    scatterPlotView_->setRenderHint(QPainter::Antialiasing);
    // A double-click on a point is two selects, not a zoom reset.
    scatterPlotView_->setResetOnDoubleClick(false);
    scatterPlotView_->setDefaultRange(scatterXAxis_, 0, 1000);
    scatterPlotView_->setDefaultRange(scatterYAxis_, 0, 1);
    // Right-click: compute the authoritative (full-run) KDE core contour.
    scatterPlotView_->setContextMenuPolicy(Qt::ActionsContextMenu);
    computeCoreAction_ = new QAction(tr("Compute core contour from full run"), scatterPlotView_);
    computeCoreAction_->setObjectName(QStringLiteral("computeCoreAction"));
    computeCoreAction_->setEnabled(false);
    scatterPlotView_->addAction(computeCoreAction_);
    scatterPlotView_->addAction(scatterPlotView_->resetZoomAction());
    connect(computeCoreAction_, &QAction::triggered, this, &HdfReviewTab::startFullRunCoreComputation);
    connect(scatterPlotView_, &ZoomableChartView::plotClicked, this, &HdfReviewTab::onScatterClicked);
    connect(scatterPlotView_, &ZoomableChartView::plotDoubleClicked, this, &HdfReviewTab::onScatterDoubleClicked);
    connect(scatterPlotView_, &ZoomableChartView::hoverMoved, this, &HdfReviewTab::onScatterHover);
    scatterPlotView_->setMinimumHeight(300);
    
    // Right side: Histogram chart
    histogramChart_ = new QChart();
    histogramChart_->setTitle("Ring Width Distribution");
    histogramChart_->legend()->setVisible(false);
    
    histogramYAxis_ = new QValueAxis();
    histogramYAxis_->setTitleText("Frequency");
    histogramChart_->addAxis(histogramYAxis_, Qt::AlignLeft);
    
#if MIB_HAS_QHISTOGRAMSERIES
    histogramSeries_ = new QHistogramSeries();
    histogramSeries_->setName("Ring Width");
    histogramChart_->addSeries(histogramSeries_);
    histogramXAxis_ = new QValueAxis();
    histogramXAxis_->setLabelsAngle(-90);
    histogramXAxis_->setLabelFormat("%.2f");
    histogramChart_->addAxis(histogramXAxis_, Qt::AlignBottom);
    histogramSeries_->attachAxis(histogramXAxis_);
    histogramSeries_->attachAxis(histogramYAxis_);
#else
    histogramBarSeries_ = new QBarSeries();
    histogramChart_->addSeries(histogramBarSeries_);
    histogramCategoryAxis_ = new QBarCategoryAxis();
    histogramCategoryAxis_->setLabelsAngle(-90);
    histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
    histogramBarSeries_->attachAxis(histogramCategoryAxis_);
    histogramBarSeries_->attachAxis(histogramYAxis_);
    histogramXAxis_ = nullptr;
#endif
    
    histogramView_ = new QChartView(histogramChart_, ui->chartsTab);
    histogramView_->setRenderHint(QPainter::Antialiasing);
    histogramView_->setMinimumHeight(200);
    setupChartsLayout();

    // Connect tab and table signals
    connect(ui->frameTypeTabs, QOverload<int>::of(&QTabWidget::currentChanged), 
            this, &HdfReviewTab::onTabChanged);
    connect(ui->validMetricsTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &HdfReviewTab::onTableSelectionChanged);
    connect(ui->invalidMetricsTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &HdfReviewTab::onTableSelectionChanged);
    connect(ui->validMetricsTable, &QTableView::doubleClicked,
            this, [this](const QModelIndex& idx) {
                if (idx.isValid()) onViewFrameDetails(idx.row());
            });
    connect(ui->invalidMetricsTable, &QTableView::doubleClicked,
            this, [this](const QModelIndex& idx) {
                if (idx.isValid()) onViewFrameDetails(idx.row());
            });
}

HdfReviewTab::~HdfReviewTab() {
    if (chartsSplitter_ && chartsRightSplitter_) {
        QSettings settings;
        settings.setValue(QStringLiteral("Review/ChartsSplitter"), chartsSplitter_->saveState());
        settings.setValue(QStringLiteral("Review/ChartsRightSplitter"), chartsRightSplitter_->saveState());
    }
    // The core job owns its input by value; only make sure it cannot call back.
    if (coreWatcher_) {
        coreWatcher_->disconnect(this);
        coreWatcher_->waitForFinished();
        coreWatcher_ = nullptr;
    }
    // Issue #344: never destroy the tab under a running export job. The job
    // owns its own reader/request, so cancelling makes it stop within one
    // frame; the wait here is bounded by that.
    exportCancel_.cancel();
    if (exportWatcher_) {
        exportWatcher_->disconnect(this);
        exportWatcher_->waitForFinished();
        exportWatcher_ = nullptr;
    }
    // Clean up isoelastic curve line series
    for (auto it = isoelasticCurves_.begin(); it != isoelasticCurves_.end(); ++it) {
        QLineSeries* series = *it;
        if (series) {
            delete series;
        }
    }
    isoelasticCurves_.clear();
    delete ui;
}

void HdfReviewTab::onSelectFile() {
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("Open HDF File"),
        "",
        tr("HDF5 Files (*.h5 *.hdf5);;All Files (*)")
    );

    if (!filePath.isEmpty()) {
        loadHdfFile(filePath);
    }
}

void HdfReviewTab::onCloseFile() {
    clearDisplay();
    hdfReader_.reset();
    loadedHdfFilePath_.clear();
    setFilePathText(tr("No file selected"));
    ui->statusLabel->setText(tr("Ready"));
    ui->closeFileBtn->setEnabled(false);
    ui->overlayModeLabel->setEnabled(false);
    ui->overlayModeCombo->setEnabled(false);
    ui->overlayModeCombo->setCurrentIndex(0);
    overlayMode_ = OverlayMode::None;
    SPDLOG_INFO("HdfReviewTab: file closed by user");
}

void HdfReviewTab::loadHdfFile(const QString& filePath) {
    ui->statusLabel->setText(tr("Loading..."));
    setFilePathText(filePath);
    clearDisplay();

    SPDLOG_INFO("HdfReviewTab: opening file '{}'", filePath.toStdString());
    // Open and retain HDF5 file for the lifetime of this review session
    hdfReader_.reset();
    hdfReader_ = std::make_unique<backend::services::Hdf5Service>();
    if (!hdfReader_->loadFile(filePath.toStdString())) {
        const bool exists = QFile::exists(filePath);
        const QString detail = exists
            ? tr("The file exists but its HDF5 metadata is corrupt, likely caused by "
                 "an interrupted write (crash or forced close during an experiment).\n\n"
                 "Frame data may be partially recoverable using the h5recover tool "
                 "from the HDF5 utilities package.")
            : tr("File not found.");
        QMessageBox::critical(this, tr("Cannot Open HDF5 File"),
                              tr("Failed to open:\n%1\n\n%2").arg(filePath).arg(detail));
        ui->statusLabel->setText(tr("Error loading file"));
        hdfReader_.reset();
        loadedHdfFilePath_.clear();
        return;
    }

    loadedHdfFilePath_ = filePath;
    readStoredKdeRecords();

    // Detect recording-mode file. Recording files have no valid/invalid
    // categorization, no masks, no per-frame metrics — just raw frames
    // with index/timestamp metadata.
    isRecordingMode_ = hdfReader_->isRecordingFile();

    size_t validImagesCount = 0;
    size_t invalidImagesCount = 0;
    size_t totalValid = 0, totalInvalid = 0;

    if (isRecordingMode_) {
        // Recording mode: hide the invalid tab, relabel the valid tab as "Frames".
        ui->frameTypeTabs->setTabText(0, tr("Frames"));
        ui->frameTypeTabs->setTabVisible(1, false);
        ui->frameTypeTabs->setCurrentIndex(0);
        isShowingValid_ = true;
        roi_ = {0, 0, 0, 0};
        ui->roiOverlayCheck->setEnabled(false);

        uint64_t startTimeNs = 0, endTimeNs = 0;
        uint64_t totalFrames = 0, filteredFrames = 0;
        bool multiImageEnabled = false;
        uint64_t multiImageCount = 1;
        if (hdfReader_->readRecordingInfo(startTimeNs,
                                          endTimeNs,
                                          totalFrames,
                                          filteredFrames,
                                          &multiImageEnabled,
                                          &multiImageCount)) {
            recordingMultiImageEnabled_ = multiImageEnabled;
            recordingMultiImageCount_ = std::max<size_t>(1, static_cast<size_t>(multiImageCount));
            ui->statusLabel->setText(tr("Recording: %1 frames, %2 empty skipped")
                                     .arg(static_cast<qulonglong>(totalFrames))
                                     .arg(static_cast<qulonglong>(filteredFrames)) +
                                     accountingSummary());
            SPDLOG_INFO("HdfReviewTab: recording multi-image enabled={}, count={}",
                        recordingMultiImageEnabled_,
                        recordingMultiImageCount_);
        }

        size_t count = 0; int h = 0, w = 0, c = 0;
        if (hdfReader_->getDatasetInfo("/recorded_frames/images", count, h, w, c)) {
            SPDLOG_INFO("Dataset /recorded_frames/images: count={}, H={}, W={}, C={}", count, h, w, c);
            validImagesCount = count;
        }

        if (!hdfReader_->readRecordingMetadata(validFrames_)) {
            SPDLOG_WARN("Failed to read recording metadata");
            validFrames_.clear();
        }
        invalidFrames_.clear();
    } else {
        // Experiment mode: restore default tab labels/visibility in case a
        // recording file was previously loaded in this session.
        ui->frameTypeTabs->setTabText(0, tr("Valid Frames"));
        ui->frameTypeTabs->setTabVisible(1, true);

        uint64_t startTimeNs = 0, endTimeNs = 0;
        backend::services::ProcessingService::Roi loadedRoi{0, 0, 0, 0};
        if (hdfReader_->readExperimentInfo(startTimeNs, endTimeNs, totalValid, totalInvalid, &loadedRoi)) {
            ui->statusLabel->setText(QString("Valid: %1, Invalid: %2")
                                 .arg(totalValid).arg(totalInvalid) + accountingSummary());
            roi_ = loadedRoi;
            SPDLOG_INFO("Loaded ROI from HDF5: x={}, y={}, w={}, h={}", roi_.x, roi_.y, roi_.w, roi_.h);
            ui->roiOverlayCheck->setEnabled(true);
        } else {
            roi_ = {0, 0, 0, 0};
            SPDLOG_WARN("Failed to read experiment info or ROI not found in HDF5 file");
            ui->roiOverlayCheck->setEnabled(false);
        }

        size_t count = 0; int h = 0, w = 0, c = 0;
        if (hdfReader_->getDatasetInfo("/valid_frames/images", count, h, w, c)) {
            SPDLOG_INFO("Dataset /valid_frames/images: count={}, H={}, W={}, C={}", count, h, w, c);
            validImagesCount = count;
        }
        if (hdfReader_->getDatasetInfo("/valid_frames/masks", count, h, w, c)) {
            SPDLOG_INFO("Dataset /valid_frames/masks:  count={}, H={}, W={}, C={}", count, h, w, c);
        }
        if (hdfReader_->getDatasetInfo("/invalid_frames/images", count, h, w, c)) {
            SPDLOG_INFO("Dataset /invalid_frames/images: count={}, H={}, W={}, C={}", count, h, w, c);
            invalidImagesCount = count;
        }
        if (hdfReader_->getDatasetInfo("/invalid_frames/masks", count, h, w, c)) {
            SPDLOG_INFO("Dataset /invalid_frames/masks:  count={}, H={}, W={}, C={}", count, h, w, c);
        }

        if (!hdfReader_->readValidMetadata(validFrames_)) {
            SPDLOG_WARN("Failed to read valid metadata or none found");
            validFrames_.clear();
        }

        if (!hdfReader_->readInvalidMetadata(invalidFrames_)) {
            SPDLOG_WARN("Failed to read invalid metadata or none found");
            invalidFrames_.clear();
        }
    }

    // Keep file open in hdfReader_ for subsequent on-demand reads (thumbnails/viewer)

    // Populate UI
    updateImageGrid(validFrames_);
    updateMetricsTable(validFrames_);
    updateImageGrid(invalidFrames_);
    updateMetricsTable(invalidFrames_);

    // Enable export buttons if we have any data. Recording files have no
    // metrics or charts, so the metrics/charts exports and mask regeneration
    // are meaningless — disable them.
    bool hasData = !validFrames_.empty() || !invalidFrames_.empty();
    ui->exportMetricsBtn->setEnabled(hasData && !isRecordingMode_);
    ui->exportAllBtn->setEnabled(hasData);
    ui->exportChartsBtn->setEnabled(hasData && !isRecordingMode_);
    ui->closeFileBtn->setEnabled(hasData);
    ui->regenerateMasksBtn->setEnabled(hasData);
    updateSecondaryActionState();

    // Enable overlay controls if we have frames (not in recording mode — no masks/ROI)
    if (hasData && !isRecordingMode_) {
        ui->overlayModeLabel->setEnabled(true);
        ui->overlayModeCombo->setEnabled(true);
        ui->roiOverlayCheck->setEnabled(true);
    } else if (isRecordingMode_) {
        ui->overlayModeLabel->setEnabled(false);
        ui->overlayModeCombo->setEnabled(false);
        ui->roiOverlayCheck->setEnabled(false);
    }

    // Update charts tab with snapshots from HDF5
    updateCharts();

    // Prefer actual dataset/metadata counts for status display (experiment info may be stale)
    if (!isRecordingMode_) {
        const size_t shownValid = !validFrames_.empty() ? validFrames_.size()
                                 : (validImagesCount > 0 ? validImagesCount : totalValid);
        const size_t shownInvalid = !invalidFrames_.empty() ? invalidFrames_.size()
                                   : (invalidImagesCount > 0 ? invalidImagesCount : totalInvalid);
        ui->statusLabel->setText(QString("Valid: %1, Invalid: %2")
                              .arg(static_cast<qulonglong>(shownValid))
                              .arg(static_cast<qulonglong>(shownInvalid)));
    }

    SPDLOG_INFO("Loaded HDF file: {} valid frames, {} invalid frames", 
               validFrames_.size(), invalidFrames_.size());
}

void HdfReviewTab::populateFrames(const std::vector<backend::services::ProcessedFrame>& frames, bool isValid) {
    // This method is kept for potential future use but currently not needed
    // as frames are stored directly in loadHdfFile
    if (isValid) {
        validFrames_ = frames;
        updateImageGrid(validFrames_);
        updateMetricsTable(validFrames_);
    } else {
        invalidFrames_ = frames;
        updateImageGrid(invalidFrames_);
        updateMetricsTable(invalidFrames_);
    }
}

void HdfReviewTab::clearDisplay() {
    scatterPoints_.clear();
    scatterPointToFrame_.clear();
    frameToScatterPoint_.clear();
    scatterShowsLiveFile_ = false;
    setScatterHighlight(-1);
    paneFrame_ = -1;
    if (framePaneStack_) framePaneStack_->setCurrentIndex(0);
    storedKdeLive_.clear();
    storedKdeAnalysis_.clear();
    drawStoredKdeContours(); // removes the previous file's contours
    validFrames_.clear();
    invalidFrames_.clear();
    selectedFrameIndex_ = -1;
    selectedFrameValid_ = true;
    validThumbnailsLoaded_ = 0;
    invalidThumbnailsLoaded_ = 0;
    roi_ = {0, 0, 0, 0};
    showRoiOverlay_ = false;
    thumbnailCache_.clear();
    validScrollValue_ = 0;
    invalidScrollValue_ = 0;
    isRecordingMode_ = false;
    recordingMultiImageEnabled_ = false;
    recordingMultiImageCount_ = 1;

    // Restore the default tab labels/visibility that recording-mode loads
    // may have overridden.
    ui->frameTypeTabs->setTabText(0, tr("Valid Frames"));
    ui->frameTypeTabs->setTabVisible(1, true);

    // Disable export buttons and ROI overlay when no data
    ui->exportMetricsBtn->setEnabled(false);
    ui->exportAllBtn->setEnabled(false);
    ui->exportChartsBtn->setEnabled(false);
    ui->regenerateMasksBtn->setEnabled(false);
    updateSecondaryActionState();
    ui->roiOverlayCheck->setEnabled(false);
    ui->roiOverlayCheck->setChecked(false);

    // Clear valid frames grid
    QLayoutItem* item;
    while ((item = ui->validImageGrid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    // After clearing, the spacer pointer may be dangling; reset it
    validBottomSpacer_ = nullptr;
    validTopSpacer_ = nullptr;

    // Clear invalid frames grid
    while ((item = ui->invalidImageGrid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    // After clearing, the spacer pointer may be dangling; reset it
    invalidBottomSpacer_ = nullptr;
    invalidTopSpacer_ = nullptr;

    if (validMetricsModel_) validMetricsModel_->setSource(&validFrames_);
    if (invalidMetricsModel_) invalidMetricsModel_->setSource(&invalidFrames_);

    // Clear charts
    if (scatterSeries_) {
        scatterSeries_->clear();
    }
    // Clear isoelastic curves (they will be reloaded when charts are regenerated)
    for (auto it = isoelasticCurves_.begin(); it != isoelasticCurves_.end(); ++it) {
        QLineSeries* series = *it;
        if (series) {
            scatterPlotChart_->removeSeries(series);
            delete series;
        }
    }
    isoelasticCurves_.clear();
    if (scatterXAxis_ && scatterYAxis_) {
        scatterXAxis_->setRange(0, 1000);
        scatterYAxis_->setRange(0, 1);
    }
    if (scatterPlotView_) {
        scatterPlotView_->setDefaultRange(scatterXAxis_, 0, 1000);
        scatterPlotView_->setDefaultRange(scatterYAxis_, 0, 1);
        scatterPlotView_->resetZoom();
    }
#if MIB_HAS_QHISTOGRAMSERIES
    if (histogramSeries_) {
        histogramSeries_->clear();
    }
#else
    if (histogramBarSeries_) {
        histogramBarSeries_->clear();
    }
#endif
    if (histogramYAxis_) {
        histogramYAxis_->setRange(0, 1);
    }
}

void HdfReviewTab::updateImageGrid(const std::vector<backend::services::ProcessedFrame>& frames) {
    // Determine which grid to use based on which frames vector we're updating
    bool isValid = (&frames == &validFrames_);
    QGridLayout* grid = isValid ? ui->validImageGrid : ui->invalidImageGrid;
    SPDLOG_DEBUG("HdfReviewTab: updateImageGrid {} frames={}",
                 isValid ? "valid" : "invalid", frames.size());
    
    // Clear existing thumbnails
    QLayoutItem* item;
    while ((item = grid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    // Reset spacer pointer for this grid since all items were removed
    if (isValid) {
        validBottomSpacer_ = nullptr;
    } else {
        invalidBottomSpacer_ = nullptr;
    }

    // Reset loaded count
    if (isValid) {
        validThumbnailsLoaded_ = 0;
    } else {
        invalidThumbnailsLoaded_ = 0;
    }

    // Only load initial batch of thumbnails to avoid memory issues
    size_t initialCount = std::min(frames.size(), INITIAL_THUMBNAIL_COUNT);
    loadThumbnailsBatch(frames, 0, initialCount, isValid);
    
    // Update loaded count
    if (isValid) {
        validThumbnailsLoaded_ = initialCount;
    } else {
        invalidThumbnailsLoaded_ = initialCount;
    }

    // Virtualize remaining space: adjust bottom spacer height instead of creating thousands of placeholders
    const size_t totalRows = (frames.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
    const size_t loadedRows = (initialCount + GRID_COLUMNS - 1) / GRID_COLUMNS;
    const int cellH = THUMBNAIL_SIZE + 8; // approximate spacing/margins
    const int remainingRows = static_cast<int>(totalRows > loadedRows ? (totalRows - loadedRows) : 0);
    const int spacerH = remainingRows * cellH;
    if (isValid) {
        if (validBottomSpacer_) {
            ui->validImageGrid->removeItem(validBottomSpacer_);
            delete validBottomSpacer_;
        }
        validBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->validImageGrid->addItem(validBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    } else {
        if (invalidBottomSpacer_) {
            ui->invalidImageGrid->removeItem(invalidBottomSpacer_);
            delete invalidBottomSpacer_;
        }
        invalidBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->invalidImageGrid->addItem(invalidBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    }
}

void HdfReviewTab::loadThumbnailsBatch(const std::vector<backend::services::ProcessedFrame>& frames,
                                        size_t startIndex, size_t count, bool isValid) {
    QGridLayout* grid = isValid ? ui->validImageGrid : ui->invalidImageGrid;
    size_t endIndex = std::min(startIndex + count, frames.size());
    SPDLOG_DEBUG("HdfReviewTab: loadThumbnailsBatch {} start={} count={} end={}",
                 isValid ? "valid" : "invalid", startIndex, count, endIndex);
#ifdef _WIN32
    {
        MEMORYSTATUSEX st;
        st.dwLength = sizeof(st);
        if (GlobalMemoryStatusEx(&st)) {
            SPDLOG_INFO("Mem before batch: load={}%, avail_phys_MB={}, avail_page_MB={}",
                        st.dwMemoryLoad,
                        static_cast<unsigned long long>(st.ullAvailPhys / (1024 * 1024)),
                        static_cast<unsigned long long>(st.ullAvailPageFile / (1024 * 1024)));
        }
    }
#endif
    
    for (size_t i = startIndex; i < endIndex; ++i) {
        // Cache key: [valid_flag (1 bit)] [reserved (15 bits)] [index (48 bits)]
        const qulonglong key = (static_cast<qulonglong>(isValid ? 1 : 0) << 63)
                             | (static_cast<qulonglong>(i) & 0x0000FFFFFFFFFFFFull);

        QImage* cached = thumbnailCache_.object(key);
        QImage thumbImage;
        if (cached) {
            thumbImage = *cached;
        } else {
            // Dataset paths (routed to /recorded_frames/* when in recording mode)
            const std::string imgPath = imagesPath(isValid);
            const std::string maskPath = masksPath(isValid);

            // Read original image by dataset position (i), not by frame.index.
            // Fall back to the in-memory ProcessedFrame when the HDF5 reader is
            // unavailable (e.g. results came from a folder-sourced batch).
            const auto& framesRef = isValid ? validFrames_ : invalidFrames_;
            cv::Mat original;
            if (!hdfReader_ || !hdfReader_->readImageByIndex(imgPath, i, original)) {
                if (i < framesRef.size() && !framesRef[i].originalImage.empty()) {
                    original = framesRef[i].originalImage;
                } else {
                    SPDLOG_WARN("HdfReviewTab: failed to read original image {}[{}]", imgPath, i);
                    continue;
                }
            }

            // Optional processing overlay when overlay mode is not None and masks exist.
            const backend::services::FilterResult* validation = (i < framesRef.size()) ? &framesRef[i].validation : nullptr;
            if (overlayMode_ != OverlayMode::None && !maskPath.empty()) {
                cv::Mat mask;
                bool maskOk = hdfReader_ && hdfReader_->readImageByIndex(maskPath, i, mask) && !mask.empty();
                if (!maskOk && i < framesRef.size() && !framesRef[i].processedImage.empty()) {
                    mask = framesRef[i].processedImage;
                    maskOk = true;
                }
                if (maskOk) {
                    thumbImage = createProcessingOverlay(original, mask, validation, overlayMode_);
                } else {
                    SPDLOG_DEBUG("HdfReviewTab: mask not available for {}[{}] (overlay on)", maskPath, i);
                    thumbImage = matToQImage(original);
                }
            } else {
                thumbImage = matToQImage(original);
            }

            // ROI rectangle overlay if enabled
            if (showRoiOverlay_ && !thumbImage.isNull() && roi_.w > 0 && roi_.h > 0) {
                thumbImage = drawRoiOverlay(thumbImage, original.cols, original.rows);
            }

            // Scale and cache
            if (!thumbImage.isNull()) {
                QImage scaled = thumbImage.scaled(THUMBNAIL_SIZE, THUMBNAIL_SIZE, 
                                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
                auto* stored = new QImage(scaled);
                thumbnailCache_.insert(key, stored, 1);
                thumbImage = scaled;
                SPDLOG_TRACE("HdfReviewTab: cached thumbnail key={} ({}), size={}x{}",
                             key, isValid ? "valid" : "invalid",
                             scaled.width(), scaled.height());
            }
        }
        
        // Scale to thumbnail size
        QImage scaled = thumbImage; // already scaled if newly created; if from cache, should be scaled too
        
        auto* label = new ThumbnailLabel(static_cast<int>(i), THUMBNAIL_SIZE, grid->parentWidget());
        label->setPixmap(QPixmap::fromImage(scaled));
        label->setToolTip(QString("Frame %1\nDouble-click to view details").arg(i));
        
        connect(label, &ThumbnailLabel::clicked, this, &HdfReviewTab::onThumbnailClicked);
        connect(label, &ThumbnailLabel::doubleClicked, this, &HdfReviewTab::onThumbnailDoubleClicked);
        
        int row = static_cast<int>(i) / GRID_COLUMNS;
        int col = static_cast<int>(i) % GRID_COLUMNS;
        
        // Remove placeholder if exists
        QLayoutItem* existingItem = grid->itemAtPosition(row, col);
        if (existingItem && existingItem->widget()) {
            QWidget* existingWidget = existingItem->widget();
            if (qobject_cast<QLabel*>(existingWidget) && 
                !qobject_cast<ThumbnailLabel*>(existingWidget)) {
                grid->removeWidget(existingWidget);
                existingWidget->deleteLater();
            }
        }
        
        grid->addWidget(label, row, col);

        SPDLOG_DEBUG("HdfReviewTab: loaded thumbnail {} ({})", i, isValid ? "valid" : "invalid");
    }

    // Adjust spacer height to reflect newly loaded rows
    const size_t totalRows = (frames.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
    const size_t loadedRows = (endIndex + GRID_COLUMNS - 1) / GRID_COLUMNS;
    const int cellH = THUMBNAIL_SIZE + 8;
    const int remainingRows = static_cast<int>(totalRows > loadedRows ? (totalRows - loadedRows) : 0);
    const int spacerH = remainingRows * cellH;
    if (isValid) {
        if (validBottomSpacer_) {
            ui->validImageGrid->removeItem(validBottomSpacer_);
            delete validBottomSpacer_;
        }
        validBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->validImageGrid->addItem(validBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    } else {
        if (invalidBottomSpacer_) {
            ui->invalidImageGrid->removeItem(invalidBottomSpacer_);
            delete invalidBottomSpacer_;
        }
        invalidBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->invalidImageGrid->addItem(invalidBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    }
#ifdef _WIN32
    {
        MEMORYSTATUSEX st;
        st.dwLength = sizeof(st);
        if (GlobalMemoryStatusEx(&st)) {
            SPDLOG_INFO("Mem after batch: load={}%, avail_phys_MB={}, avail_page_MB={}",
                        st.dwMemoryLoad,
                        static_cast<unsigned long long>(st.ullAvailPhys / (1024 * 1024)),
                        static_cast<unsigned long long>(st.ullAvailPageFile / (1024 * 1024)));
        }
    }
#endif
}

void HdfReviewTab::onScrollValueChanged(int value) {
    QScrollArea* scrollArea = isShowingValid_ ? ui->validImageScroll : ui->invalidImageScroll;
    const auto& frames = isShowingValid_ ? validFrames_ : invalidFrames_;
    size_t& loadedCount = isShowingValid_ ? validThumbnailsLoaded_ : invalidThumbnailsLoaded_;
    
    if (frames.empty() || loadedCount >= frames.size()) {
        // Still ensure visible items reflect current overlay state
        refreshVisibleThumbnails(isShowingValid_);
        pruneOffscreenThumbnails(isShowingValid_);
        return;
    }
    
    // Trigger loading when near the bottom of the CURRENT content (post-pruning).
    // Using scrollbar maximum ensures we don't depend on internal loaded counters.
    QScrollBar* scrollBar = scrollArea->verticalScrollBar();
    const int cellH = THUMBNAIL_SIZE + 8; // keep in sync with grid estimation
    int threshold = std::max(0, scrollBar->maximum() - (cellH * 2));

    if (value >= threshold && loadedCount < frames.size()) {
        // Load next batch
        size_t batchSize = std::min(BATCH_THUMBNAIL_COUNT, frames.size() - loadedCount);
        loadThumbnailsBatch(frames, loadedCount, batchSize, isShowingValid_);
        loadedCount += batchSize;
        
        SPDLOG_DEBUG("Loaded thumbnail batch: {} total loaded out of {}", loadedCount, frames.size());
    }

    // Always refresh visible thumbnails (ensures overlay changes apply lazily)
    refreshVisibleThumbnails(isShowingValid_);
    pruneOffscreenThumbnails(isShowingValid_);
}

void HdfReviewTab::updateMetricsTable(const std::vector<backend::services::ProcessedFrame>& frames) {
    // Determine which model to use based on which frames vector we're updating
    bool isValid = (&frames == &validFrames_);
    if (isValid) {
        if (validMetricsModel_) {
            validMetricsModel_->setSource(&validFrames_);
            ui->validMetricsTable->resizeColumnsToContents();
        }
    } else {
        if (invalidMetricsModel_) {
            invalidMetricsModel_->setSource(&invalidFrames_);
            ui->invalidMetricsTable->resizeColumnsToContents();
        }
    }
}

std::string HdfReviewTab::imagesPath(bool isValid) const {
    if (isRecordingMode_) return "/recorded_frames/images";
    return isValid ? "/valid_frames/images" : "/invalid_frames/images";
}

std::string HdfReviewTab::masksPath(bool isValid) const {
    if (isRecordingMode_) return {};
    return isValid ? "/valid_frames/masks" : "/invalid_frames/masks";
}

QImage HdfReviewTab::matToQImage(const cv::Mat& mat) const {
    if (mat.empty()) {
        return QImage();
    }

    if (mat.type() == CV_8UC1) {
        // Grayscale
        QImage img(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_Grayscale8);
        return img.copy();
    } else if (mat.type() == CV_8UC3) {
        // BGR to RGB
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        QImage img(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888);
        return img.copy();
    } else if (mat.type() == CV_8UC4) {
        // BGRA to RGBA
        cv::Mat rgba;
        cv::cvtColor(mat, rgba, cv::COLOR_BGRA2RGBA);
        QImage img(rgba.data, rgba.cols, rgba.rows, static_cast<int>(rgba.step), QImage::Format_RGBA8888);
        return img.copy();
    }

    // Fallback: convert to grayscale
    cv::Mat gray;
    cv::cvtColor(mat, gray, cv::COLOR_BGR2GRAY);
    QImage img(gray.data, gray.cols, gray.rows, static_cast<int>(gray.step), QImage::Format_Grayscale8);
    return img.copy();
}

void HdfReviewTab::onTabChanged(int index) {
    // Save previous tab's scroll position
    {
        QScrollArea* prevScroll = isShowingValid_ ? ui->validImageScroll : ui->invalidImageScroll;
        if (prevScroll && prevScroll->verticalScrollBar()) {
            int prevVal = prevScroll->verticalScrollBar()->value();
            if (isShowingValid_) {
                validScrollValue_ = prevVal;
            } else {
                invalidScrollValue_ = prevVal;
            }
        }
    }

    isShowingValid_ = (index == 0);
    if (chartsTabVisible()) refreshFramePane();

    // Do not rebuild image grids on tab switch; just refresh metrics view
    if (isShowingValid_) {
        updateMetricsTable(validFrames_);
    } else {
        updateMetricsTable(invalidFrames_);
    }

    // Restore saved scroll position for the new tab
    {
        QScrollArea* currScroll = isShowingValid_ ? ui->validImageScroll : ui->invalidImageScroll;
        if (currScroll && currScroll->verticalScrollBar()) {
            int targetVal = isShowingValid_ ? validScrollValue_ : invalidScrollValue_;
            currScroll->verticalScrollBar()->setValue(targetVal);
        }
    }
}

void HdfReviewTab::onThumbnailClicked(int frameIndex) {
    setSelectedFrame(frameIndex, isShowingValid_);
}

void HdfReviewTab::onThumbnailDoubleClicked(int frameIndex) {
    showFrameViewer(frameIndex, isShowingValid_);
}

void HdfReviewTab::onViewFrameDetails(int frameIndex) {
    showFrameViewer(frameIndex, isShowingValid_);
}

void HdfReviewTab::onRegenerateMasks() {
    QString loadedPath;
    if (hdfReader_) {
        const QString label = ui->filePathLabel->text();
        if (label != tr("No file selected")) loadedPath = label;
    }

    BatchMaskDialog dlg(backend_, loadedPath, this);
    dlg.exec();

    const QString savedPath = dlg.savedHdf5Path();
    if (savedPath.isEmpty()) return;

    loadHdfFile(savedPath);
}

void HdfReviewTab::onTableSelectionChanged() {
    // The table that changed, not the visible tab: the scatter selects valid
    // rows while the Charts tab (isShowingValid_ == false) is showing.
    const bool valid = sender() != ui->invalidMetricsTable->selectionModel();
    QTableView* table = valid ? ui->validMetricsTable : ui->invalidMetricsTable;
    if (!table || !table->selectionModel()) return;
    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (!rows.isEmpty()) {
        setSelectedFrame(rows.first().row(), valid);
    }
}

void HdfReviewTab::setSelectedFrame(int frameIndex, bool valid) {
    // Selecting the table row below re-enters through selectionChanged.
    if (settingSelection_) return;
    settingSelection_ = true;
    struct Reset { bool& f; ~Reset() { f = false; } } reset{settingSelection_};

    if (frameIndex < 0) {
        selectedFrameIndex_ = -1;
        return;
    }

    const auto& frames = valid ? validFrames_ : invalidFrames_;
    if (frameIndex >= static_cast<int>(frames.size())) {
        return;
    }

    selectedFrameIndex_ = frameIndex;
    selectedFrameValid_ = valid;
    // The scatter highlight and the frame pane show the last *valid* cell
    // chosen; an invalid-set selection changes neither.
    if (valid && !isRecordingMode_) {
        setScatterHighlight(frameIndex);
        refreshFramePane();
    }

    // Update thumbnail selection
    QGridLayout* grid = valid ? ui->validImageGrid : ui->invalidImageGrid;
    for (int i = 0; i < grid->count(); ++i) {
        QLayoutItem* item = grid->itemAt(i);
        if (item && item->widget()) {
            auto* label = qobject_cast<ThumbnailLabel*>(item->widget());
            if (label) {
                label->setSelected(label->frameIndex() == frameIndex);
            }
        }
    }

    // Update table selection
    QTableView* table = valid ? ui->validMetricsTable : ui->invalidMetricsTable;
    if (table && table->model()) {
        QModelIndex idx = table->model()->index(frameIndex, 0);
        if (idx.isValid() && table->selectionModel()) {
            table->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            table->scrollTo(idx);
        }
    }
}

void HdfReviewTab::onExportMetrics() {
    if (exportInProgress()) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return;
    }
    if (loadedHdfFilePath_.isEmpty() || (validFrames_.empty() && invalidFrames_.empty())) {
        QMessageBox::information(this, tr("Export Metrics"),
                                 tr("No metrics data available to export."));
        return;
    }

    const QString initialPath = frontend::hdfreviewexport::metricsCsvPath(
        loadedHdfFilePath_, metricsExportDir());
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Export Metrics to CSV"),
        initialPath,
        tr("CSV Files (*.csv);;All Files (*)")
    );

    if (filePath.isEmpty()) {
        return;
    }

    backend::recording::HdfExportRequest request;
    request.sourcePath = loadedHdfFilePath_.toStdString();
    request.outputRoot = QFileInfo(filePath).absolutePath().toStdString();
    request.format = backend::recording::HdfExportFormat::MetricsCsv;
    request.conversionFactor = backend_.processing().getPixelToMicronFactor();
    request.explicitDestination = filePath.toStdString();
    beginExportJob(std::move(request), tr("Export Metrics"), [this, filePath](const backend::recording::HdfExportResult& r) {
        finishExportUi();
        if (r.completed()) {
            rememberMetricsExportDir(QFileInfo(filePath).absolutePath());
            QMessageBox::information(this, tr("Export Complete"),
                                     tr("Exported %1 frames (Valid: %2, Invalid: %3) to:\n%4")
                                         .arg(static_cast<qulonglong>(r.validCount + r.invalidCount))
                                         .arg(static_cast<qulonglong>(r.validCount))
                                         .arg(static_cast<qulonglong>(r.invalidCount))
                                         .arg(QString::fromStdString(r.finalPath)));
        } else {
            reportExportNotCompleted(tr("Export Metrics"), r);
        }
    });
}

void HdfReviewTab::onBatchExportMetrics() {
    if (exportInProgress()) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return;
    }
    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("Select HDF Files for Metrics Export"),
        loadedHdfFilePath_.isEmpty() ? QString() : QFileInfo(loadedHdfFilePath_).absolutePath(),
        tr("HDF5 Files (*.h5 *.hdf5);;All Files (*)")
    );
    if (filePaths.isEmpty()) {
        return;
    }

    const QString dirPath = QFileDialog::getExistingDirectory(
        this,
        tr("Select Directory for Metrics CSV Files"),
        metricsExportDir(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dirPath.isEmpty()) {
        return;
    }

    auto batch = std::make_unique<BatchExportState>();
    batch->sources = filePaths;
    batch->destinations = frontend::hdfreviewexport::batchMetricsCsvPaths(filePaths, dirPath);
    batch->root = dirPath;
    batch->metricsOnly = true;
    batch_ = std::move(batch);
    continueBatchExport();
}

void HdfReviewTab::onOverlayModeChanged(int index) {
    overlayMode_ = static_cast<OverlayMode>(index);
    if (framePane_) framePane_->setOverlayMode(overlayMode_);
    SPDLOG_INFO("Overlay mode changed to index {} (OverlayMode={})", index, static_cast<int>(overlayMode_));
    thumbnailCache_.clear();

    if (ui->validImageScroll && ui->validImageScroll->verticalScrollBar()) {
        validScrollValue_ = ui->validImageScroll->verticalScrollBar()->value();
    }
    if (ui->invalidImageScroll && ui->invalidImageScroll->verticalScrollBar()) {
        invalidScrollValue_ = ui->invalidImageScroll->verticalScrollBar()->value();
    }
    refreshVisibleThumbnails(true);
    refreshVisibleThumbnails(false);
    pruneOffscreenThumbnails(true);
    pruneOffscreenThumbnails(false);
    if (ui->validImageScroll && ui->validImageScroll->verticalScrollBar()) {
        ui->validImageScroll->verticalScrollBar()->setValue(validScrollValue_);
    }
    if (ui->invalidImageScroll && ui->invalidImageScroll->verticalScrollBar()) {
        ui->invalidImageScroll->verticalScrollBar()->setValue(invalidScrollValue_);
    }
}

void HdfReviewTab::onToggleRoiOverlay(bool enabled) {
    showRoiOverlay_ = enabled;
    if (framePane_) framePane_->setShowRoiOverlay(enabled);
    SPDLOG_INFO("ROI overlay toggled: {}, ROI: x={}, y={}, w={}, h={}", 
                enabled, roi_.x, roi_.y, roi_.w, roi_.h);
    thumbnailCache_.clear();

    // Preserve current scroll positions
    if (ui->validImageScroll && ui->validImageScroll->verticalScrollBar()) {
        validScrollValue_ = ui->validImageScroll->verticalScrollBar()->value();
    }
    if (ui->invalidImageScroll && ui->invalidImageScroll->verticalScrollBar()) {
        invalidScrollValue_ = ui->invalidImageScroll->verticalScrollBar()->value();
    }

    // Refresh only what is visible in each tab (carousel-like behavior)
    refreshVisibleThumbnails(true);
    refreshVisibleThumbnails(false);
    pruneOffscreenThumbnails(true);
    pruneOffscreenThumbnails(false);

    // Restore scroll positions
    if (ui->validImageScroll && ui->validImageScroll->verticalScrollBar()) {
        ui->validImageScroll->verticalScrollBar()->setValue(validScrollValue_);
    }
    if (ui->invalidImageScroll && ui->invalidImageScroll->verticalScrollBar()) {
        ui->invalidImageScroll->verticalScrollBar()->setValue(invalidScrollValue_);
    }
}

QImage HdfReviewTab::drawRoiOverlay(const QImage& image, int imgWidth, int imgHeight) const {
    if (image.isNull() || roi_.w <= 0 || roi_.h <= 0) {
        return image;
    }

    // Create a copy to draw on
    QImage overlayImage = image.copy();
    QPainter painter(&overlayImage);
    painter.setRenderHint(QPainter::Antialiasing);

    // Calculate ROI rectangle in image coordinates
    // ROI is in original image coordinates, need to scale to current image size
    double scaleX = static_cast<double>(image.width()) / static_cast<double>(imgWidth);
    double scaleY = static_cast<double>(image.height()) / static_cast<double>(imgHeight);
    
    int roiX = static_cast<int>(roi_.x * scaleX);
    int roiY = static_cast<int>(roi_.y * scaleY);
    int roiW = static_cast<int>(roi_.w * scaleX);
    int roiH = static_cast<int>(roi_.h * scaleY);

    // Clamp ROI to image bounds
    roiX = std::max(0, std::min(roiX, image.width() - 1));
    roiY = std::max(0, std::min(roiY, image.height() - 1));
    roiW = std::max(1, std::min(roiW, image.width() - roiX));
    roiH = std::max(1, std::min(roiH, image.height() - roiY));

    // Draw rectangle with red border (thicker for visibility)
    QPen pen(QColor(255, 0, 0), 3); // Red, 3px width for better visibility
    painter.setPen(pen);
    painter.drawRect(roiX, roiY, roiW, roiH);

    return overlayImage;
}

void HdfReviewTab::computeVisibleRange(bool isValid, size_t &outStartIndex, size_t &outEndIndex) const {
    const auto& frames = isValid ? validFrames_ : invalidFrames_;
    outStartIndex = 0;
    outEndIndex = 0;
    if (frames.empty()) return;

    const QScrollArea* scrollArea = isValid ? ui->validImageScroll : ui->invalidImageScroll;
    if (!scrollArea || !scrollArea->verticalScrollBar()) return;

    const int cellH = THUMBNAIL_SIZE + 8;
    const int value = scrollArea->verticalScrollBar()->value();
    const int viewportH = scrollArea->viewport()->height();

    int startRow = value / cellH;
    startRow = std::max(0, startRow - 1); // buffer one row above
    int rowsVisible = (viewportH + cellH - 1) / cellH + 2; // buffer two rows
    int endRow = startRow + rowsVisible;

    const size_t totalRows = (frames.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
    endRow = std::min<int>(endRow, static_cast<int>(totalRows));

    outStartIndex = static_cast<size_t>(startRow) * GRID_COLUMNS;
    outEndIndex = std::min(frames.size(), static_cast<size_t>(endRow) * GRID_COLUMNS);
}

QImage HdfReviewTab::buildThumbnailForIndex(size_t index, bool isValid) {
    // Cache key: [valid_flag (1 bit)] [reserved (15 bits)] [index (48 bits)]
    const qulonglong key = (static_cast<qulonglong>(isValid ? 1 : 0) << 63)
                         | (static_cast<qulonglong>(index) & 0x0000FFFFFFFFFFFFull);

    if (QImage* cached = thumbnailCache_.object(key)) {
        return *cached;
    }

    const std::string imgPath = imagesPath(isValid);
    const std::string maskPath = masksPath(isValid);

    QImage thumbImage;
    cv::Mat original;
    if (!hdfReader_ || !hdfReader_->readImageByIndex(imgPath, index, original)) {
        SPDLOG_DEBUG("buildThumbnailForIndex: missing original {}[{}]", imgPath, index);
        return thumbImage;
    }

    const auto& framesRef = isValid ? validFrames_ : invalidFrames_;
    const backend::services::FilterResult* validation = (index < framesRef.size()) ? &framesRef[index].validation : nullptr;
    if (overlayMode_ != OverlayMode::None && !maskPath.empty()) {
        cv::Mat mask;
        if (hdfReader_->readImageByIndex(maskPath, index, mask) && !mask.empty()) {
            thumbImage = createProcessingOverlay(original, mask, validation, overlayMode_);
        } else {
            thumbImage = matToQImage(original);
        }
    } else {
        thumbImage = matToQImage(original);
    }
    if (showRoiOverlay_ && !thumbImage.isNull() && roi_.w > 0 && roi_.h > 0) {
        thumbImage = drawRoiOverlay(thumbImage, original.cols, original.rows);
    }

    if (!thumbImage.isNull()) {
        QImage scaled = thumbImage.scaled(THUMBNAIL_SIZE, THUMBNAIL_SIZE,
                                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto* stored = new QImage(scaled);
        thumbnailCache_.insert(key, stored, 1);
        return scaled;
    }
    return thumbImage;
}

void HdfReviewTab::refreshVisibleThumbnails(bool isValid) {
    const auto& frames = isValid ? validFrames_ : invalidFrames_;
    if (frames.empty()) return;

    size_t startIndex = 0, endIndex = 0;
    computeVisibleRange(isValid, startIndex, endIndex);
    if (endIndex <= startIndex) return;

    QGridLayout* grid = isValid ? ui->validImageGrid : ui->invalidImageGrid;

    // Remove existing thumbnail labels
    QVector<QWidget*> toRemove;
    for (int i = 0; i < grid->count(); ++i) {
        QLayoutItem* it = grid->itemAt(i);
        if (!it) continue;
        QWidget* w = it->widget();
        if (w && qobject_cast<ThumbnailLabel*>(w)) {
            toRemove.push_back(w);
        }
    }
    for (QWidget* w : toRemove) {
        grid->removeWidget(w);
        w->deleteLater();
    }

    // Update top spacer height for rows before startIndex
    const int cellH = THUMBNAIL_SIZE + 8;
    const size_t totalRows = (frames.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
    const size_t startRow = startIndex / GRID_COLUMNS;
    const size_t visibleRows = ((endIndex - startIndex) + GRID_COLUMNS - 1) / GRID_COLUMNS;

    if (isValid) {
        if (validTopSpacer_) {
            ui->validImageGrid->removeItem(validTopSpacer_);
            delete validTopSpacer_;
        }
        validTopSpacer_ = new QSpacerItem(0, static_cast<int>(startRow) * cellH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->validImageGrid->addItem(validTopSpacer_, 0, 0, 1, GRID_COLUMNS);
    } else {
        if (invalidTopSpacer_) {
            ui->invalidImageGrid->removeItem(invalidTopSpacer_);
            delete invalidTopSpacer_;
        }
        invalidTopSpacer_ = new QSpacerItem(0, static_cast<int>(startRow) * cellH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->invalidImageGrid->addItem(invalidTopSpacer_, 0, 0, 1, GRID_COLUMNS);
    }

    // Add visible thumbnails as a contiguous block after the top spacer
    int localRowBase = 1; // row 0 is reserved for top spacer
    for (size_t i = startIndex; i < endIndex; ++i) {
        int localRow = localRowBase + static_cast<int>((i - startIndex) / GRID_COLUMNS);
        int col = static_cast<int>(i % GRID_COLUMNS);
        auto* label = new ThumbnailLabel(static_cast<int>(i), THUMBNAIL_SIZE, grid->parentWidget());
        QImage img = buildThumbnailForIndex(i, isValid);
        if (!img.isNull()) {
            label->setPixmap(QPixmap::fromImage(img));
        }
        grid->addWidget(label, localRow, col);
        connect(label, &ThumbnailLabel::clicked, this, &HdfReviewTab::onThumbnailClicked);
        connect(label, &ThumbnailLabel::doubleClicked, this, &HdfReviewTab::onThumbnailDoubleClicked);
    }

    // Adjust bottom spacer for rows after endIndex
    const size_t remainingRows = (totalRows > (startRow + visibleRows)) ? (totalRows - (startRow + visibleRows)) : 0;
    const int bottomH = static_cast<int>(remainingRows) * cellH;
    if (isValid) {
        if (validBottomSpacer_) {
            ui->validImageGrid->removeItem(validBottomSpacer_);
            delete validBottomSpacer_;
        }
        validBottomSpacer_ = new QSpacerItem(0, bottomH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->validImageGrid->addItem(validBottomSpacer_, localRowBase + static_cast<int>(visibleRows), 0, 1, GRID_COLUMNS);
    } else {
        if (invalidBottomSpacer_) {
            ui->invalidImageGrid->removeItem(invalidBottomSpacer_);
            delete invalidBottomSpacer_;
        }
        invalidBottomSpacer_ = new QSpacerItem(0, bottomH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->invalidImageGrid->addItem(invalidBottomSpacer_, localRowBase + static_cast<int>(visibleRows), 0, 1, GRID_COLUMNS);
    }
}

void HdfReviewTab::pruneOffscreenThumbnails(bool isValid) {
    const auto& frames = isValid ? validFrames_ : invalidFrames_;
    if (frames.empty()) return;

    size_t keepStart = 0, keepEnd = 0;
    computeVisibleRange(isValid, keepStart, keepEnd);
    if (keepEnd <= keepStart) return;

    QGridLayout* grid = isValid ? ui->validImageGrid : ui->invalidImageGrid;

    // Collect labels to remove (outside keep range)
    QVector<QWidget*> toRemove;
    for (int i = 0; i < grid->count(); ++i) {
        QLayoutItem* it = grid->itemAt(i);
        if (!it) continue;
        QWidget* w = it->widget();
        if (!w) continue; // skip non-widget items like QSpacerItem
        auto* label = qobject_cast<ThumbnailLabel*>(w);
        if (!label) continue;
        const size_t idx = static_cast<size_t>(label->frameIndex());
        if (idx < keepStart || idx >= keepEnd) {
            toRemove.push_back(w);
        }
    }
    for (QWidget* w : toRemove) {
        grid->removeWidget(w);
        w->deleteLater();
    }

    // Recompute bottom spacer height based on max index currently present
    size_t maxIndexPresent = 0;
    bool any = false;
    for (int i = 0; i < grid->count(); ++i) {
        QLayoutItem* it = grid->itemAt(i);
        if (!it) continue;
        QWidget* w = it->widget();
        auto* label = qobject_cast<ThumbnailLabel*>(w);
        if (!label) continue;
        any = true;
        size_t idx = static_cast<size_t>(label->frameIndex());
        if (idx > maxIndexPresent) maxIndexPresent = idx;
    }
    const size_t totalRows = (frames.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
    size_t loadedRows = any ? ((maxIndexPresent + 1 + GRID_COLUMNS - 1) / GRID_COLUMNS) : 0;
    const int cellH = THUMBNAIL_SIZE + 8;
    const int remainingRows = static_cast<int>(totalRows > loadedRows ? (totalRows - loadedRows) : 0);
    const int spacerH = remainingRows * cellH;
    if (isValid) {
        if (validBottomSpacer_) {
            ui->validImageGrid->removeItem(validBottomSpacer_);
            delete validBottomSpacer_;
        }
        validBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->validImageGrid->addItem(validBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    } else {
        if (invalidBottomSpacer_) {
            ui->invalidImageGrid->removeItem(invalidBottomSpacer_);
            delete invalidBottomSpacer_;
        }
        invalidBottomSpacer_ = new QSpacerItem(0, spacerH, QSizePolicy::Minimum, QSizePolicy::Fixed);
        ui->invalidImageGrid->addItem(invalidBottomSpacer_, static_cast<int>(loadedRows), 0, 1, GRID_COLUMNS);
    }
}

void HdfReviewTab::loadRecordingSeriesWindow(size_t frameIndex,
                                             backend::services::ProcessedFrame& frame) const {
    if (!hdfReader_ || !isRecordingMode_ || !recordingMultiImageEnabled_ || recordingMultiImageCount_ <= 1) {
        return;
    }

    std::vector<cv::Mat> seriesImages;
    if (hdfReader_->readImagesRange("/recorded_frames/images",
                                    frameIndex,
                                    recordingMultiImageCount_,
                                    seriesImages) &&
        seriesImages.size() > 1) {
        frame.seriesImages = std::move(seriesImages);
        SPDLOG_DEBUG("HdfReviewTab: loaded recording series window for frame {} (count={})",
                     frameIndex,
                     frame.seriesImages.size());
    }
}

backend::services::ProcessedFrame HdfReviewTab::loadFrameForDisplay(int frameIndex, bool valid) const {
    const auto& framesMeta = valid ? validFrames_ : invalidFrames_;
    backend::services::ProcessedFrame frame = framesMeta[static_cast<size_t>(frameIndex)];
    if (!hdfReader_) return frame;
    const std::string imgPath = imagesPath(valid);
    const std::string maskPath = masksPath(valid);
    cv::Mat original, mask;
    if (hdfReader_->readImageByIndex(imgPath, static_cast<size_t>(frameIndex), original)) {
        frame.originalImage = original;
        SPDLOG_TRACE("HdfReviewTab: viewer loaded original {}[{}] ({}x{}x{})",
                     imgPath, frameIndex, original.cols, original.rows, original.channels());
    }
    if (!maskPath.empty() && hdfReader_->readImageByIndex(maskPath, static_cast<size_t>(frameIndex), mask)) {
        frame.processedImage = mask;
        SPDLOG_TRACE("HdfReviewTab: viewer loaded mask {}[{}] ({}x{}x{})",
                     maskPath, frameIndex, mask.cols, mask.rows, mask.channels());
    }
    // Multi-image series data (valid set only).
    if (valid) {
        if (isRecordingMode_) {
            loadRecordingSeriesWindow(static_cast<size_t>(frameIndex), frame);
        } else {
            std::vector<cv::Mat> seriesImages;
            if (hdfReader_->readSeriesImagesByIndex(static_cast<size_t>(frameIndex), seriesImages) && !seriesImages.empty()) {
                frame.seriesImages = std::move(seriesImages);
                SPDLOG_DEBUG("HdfReviewTab: loaded {} series images for frame {}", frame.seriesImages.size(), frameIndex);
            }
        }
    }
    return frame;
}

void HdfReviewTab::showFrameViewer(int frameIndex, bool valid) {
    const auto& framesMeta = valid ? validFrames_ : invalidFrames_;
    if (frameIndex < 0 || frameIndex >= static_cast<int>(framesMeta.size())) {
        return;
    }
    SPDLOG_INFO("HdfReviewTab: showFrameViewer index={} ({})", frameIndex, valid ? "valid" : "invalid");
    if (frameViewerSinkForTests_) {
        frameViewerSinkForTests_(frameIndex, valid);
        return;
    }
    if (scatterPlotView_) scatterPlotView_->cancelGesture();

    // Create dialog with current overlay mode and ROI overlay state
    auto* dialog = new FrameViewerDialog(loadFrameForDisplay(frameIndex, valid), roi_, overlayMode_, showRoiOverlay_, this);

    // shared_ptr: each lambda co-owns the state, so its lifetime no longer
    // depends on the destroyed-signal connect ordering (a hand-rolled
    // new/delete-in-connect was one refactor away from a double free / leak).
    struct NavigationState {
        int currentIndex;
        bool isValidSet;
    };
    auto navState = std::make_shared<NavigationState>(NavigationState{frameIndex, valid});

    // Prev/next wrap around the dataset the viewer was opened on.
    auto step = [this, dialog, navState](int delta) {
        const auto& frames = navState->isValidSet ? validFrames_ : invalidFrames_;
        if (frames.empty()) return;
        const int n = static_cast<int>(frames.size());
        navState->currentIndex = ((navState->currentIndex + delta) % n + n) % n;
        dialog->setFrame(loadFrameForDisplay(navState->currentIndex, navState->isValidSet));
        // Update selected frame in main view
        setSelectedFrame(navState->currentIndex, navState->isValidSet);
    };
    connect(dialog, &FrameViewerDialog::requestPreviousFrame, this, [step]() { step(-1); });
    connect(dialog, &FrameViewerDialog::requestNextFrame, this, [step]() { step(+1); });

    // Show dialog. The pane (if the Charts tab is showing) catches up once.
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    modalViewerOpen_ = true;
    dialog->exec();
    modalViewerOpen_ = false;
    if (paneStale_) refreshFramePane();
}

void HdfReviewTab::onExportAll() {
    if (exportInProgress()) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return;
    }
    if (!hdfReader_ || (validFrames_.empty() && invalidFrames_.empty())) {
        QMessageBox::warning(this, tr("Export Error"),
                            tr("No data available to export."));
        return;
    }

    const QString rootPath = QFileDialog::getExistingDirectory(this,
        tr("Select Export Root Directory"),
        exportAllRootDir(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (rootPath.isEmpty()) {
        return;
    }

    // Series range + chart snapshots are decided/rendered on the GUI thread
    // before the job exists; the worker never touches widgets.
    backend::recording::HdfExportRequest request;
    request.sourcePath = loadedHdfFilePath_.toStdString();
    request.outputRoot = rootPath.toStdString();
    request.format = backend::recording::HdfExportFormat::All;
    request.conversionFactor = backend_.processing().getPixelToMicronFactor();
    size_t seriesCount = 0, seriesRecords = 0;
    int seriesH = 0, seriesW = 0;
    if (!isRecordingMode_ && hdfReader_->getSeriesImageInfo(seriesRecords, seriesCount, seriesH, seriesW)) {
        SeriesExportSelection selection;
        if (!promptSeriesExportSelection(this, seriesCount, selection)) {
            SPDLOG_INFO("HdfReviewTab: export-all cancelled while selecting series range");
            return;
        }
        request.series.exportSeries = selection.exportSeriesImages;
        request.series.startInclusive = selection.startInclusive;
        request.series.endInclusive = selection.endInclusive;
    }
    if (!isRecordingMode_) {
        request.supplementalImages = renderChartSnapshots(validFrames_);
    }
    beginExportJob(std::move(request), tr("Export All"), [this, rootPath](const backend::recording::HdfExportResult& r) {
        finishExportUi();
        if (r.completed()) {
            rememberExportAllRootDir(rootPath);
            QMessageBox::information(this, tr("Export Complete"), exportSummary(r));
        } else {
            reportExportNotCompleted(tr("Export All"), r);
        }
    });
}

void HdfReviewTab::onBatchExportAll() {
    if (exportInProgress()) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return;
    }
    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("Select HDF Files for Batch Export All"),
        loadedHdfFilePath_.isEmpty() ? QString() : QFileInfo(loadedHdfFilePath_).absolutePath(),
        tr("HDF5 Files (*.h5 *.hdf5);;All Files (*)")
    );
    if (filePaths.isEmpty()) {
        return;
    }

    const QString rootPath = QFileDialog::getExistingDirectory(
        this,
        tr("Select Export Root Directory"),
        exportAllRootDir(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (rootPath.isEmpty()) {
        return;
    }

    auto batch = std::make_unique<BatchExportState>();
    batch->sources = filePaths;
    batch->destinations = frontend::hdfreviewexport::batchExportAllDirectoryPaths(filePaths, rootPath);
    batch->root = rootPath;
    batch->metricsOnly = false;
    batch_ = std::move(batch);
    // Batch snapshots draw other files on this scatter; bring the live
    // file's view back when the batch ends.
    if (hdfReader_ && !isRecordingMode_) batchScatterRestore_ = saveScatterView();
    continueBatchExport();
}

// Issue #344: one job at a time, run off the GUI thread by the Qt-free
// HdfExportService with its own read-only reader. The worker callable owns
// everything it needs (request, cancel token, service) and never captures a
// widget pointer; progress is re-dispatched to the GUI thread through a
// QPointer + queued invocation.
bool HdfReviewTab::beginExportJob(backend::recording::HdfExportRequest request, const QString& title,
                                  std::function<void(const backend::recording::HdfExportResult&)> onDone) {
    using backend::recording::HdfExportProgress;
    using backend::recording::HdfExportResult;
    if (exportWatcher_) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return false;
    }
    exportCancel_ = backend::recording::HdfExportCancelToken{};
    const auto token = exportCancel_;
    exportDone_ = std::move(onDone);

    if (!exportProgress_) {
        exportProgress_ = new QProgressDialog(this);
        exportProgress_->setWindowModality(Qt::WindowModal);
        exportProgress_->setAutoClose(false);
        exportProgress_->setAutoReset(false);
        exportProgress_->setMinimumDuration(0);
        exportProgress_->setCancelButtonText(tr("Cancel"));
        connect(exportProgress_, &QProgressDialog::canceled, this, [this]() {
            exportCancel_.cancel();
            if (exportProgress_) exportProgress_->setLabelText(tr("Cancelling..."));
        });
    }
    exportProgress_->setWindowTitle(title);
    exportProgress_->setLabelText(tr("Starting export..."));
    exportProgress_->setRange(0, 0);
    exportProgress_->show();
    setExportControlsEnabled(false);

    QPointer<HdfReviewTab> self(this);
    auto progressFn = [self](const HdfExportProgress& p) {
        if (QObject* ctx = self.data()) {
            QMetaObject::invokeMethod(ctx, [self, p]() { if (self) self->onExportProgress(p); }, Qt::QueuedConnection);
        }
    };
    auto service = std::make_shared<backend::recording::HdfExportService>();
    exportWatcher_ = new QFutureWatcher<HdfExportResult>(this);
    connect(exportWatcher_, &QFutureWatcher<HdfExportResult>::finished, this, &HdfReviewTab::onExportJobFinished);
    exportWatcher_->setFuture(QtConcurrent::run([service, request = std::move(request), token, progressFn]() {
        return service->run(request, token, progressFn);
    }));
    SPDLOG_INFO("HdfReviewTab: export job launched ({})", title.toStdString());
    return true;
}

void HdfReviewTab::onExportProgress(const backend::recording::HdfExportProgress& p) {
    if (!exportWatcher_ || !exportProgress_) return;
    const QString phase = QString::fromLatin1(backend::recording::toString(p.phase));
    if (p.total > 0) {
        exportProgress_->setRange(0, static_cast<int>(std::min<uint64_t>(p.total, 1000000)));
        exportProgress_->setValue(static_cast<int>(std::min<uint64_t>(p.completed, 1000000)));
        exportProgress_->setLabelText(tr("%1 (%2 / %3)").arg(phase)
                                          .arg(static_cast<qulonglong>(p.completed))
                                          .arg(static_cast<qulonglong>(p.total)));
    } else {
        exportProgress_->setLabelText(phase);
    }
}

void HdfReviewTab::onExportJobFinished() {
    auto* watcher = exportWatcher_;
    exportWatcher_ = nullptr;
    if (!watcher) return;
    const backend::recording::HdfExportResult result = watcher->result();
    watcher->deleteLater();
    auto done = std::move(exportDone_);
    exportDone_ = {};
    if (done) done(result);
}

void HdfReviewTab::finishExportUi() {
    if (exportProgress_) {
        exportProgress_->hide();
    }
    setExportControlsEnabled(true);
}

void HdfReviewTab::setExportControlsEnabled(bool enabled) {
    ui->exportMetricsBtn->setEnabled(enabled);
    ui->exportAllBtn->setEnabled(enabled);
    ui->batchExportMetricsBtn->setEnabled(enabled);
    ui->batchExportAllBtn->setEnabled(enabled);
    ui->exportChartsBtn->setEnabled(enabled);
    ui->regenerateMasksBtn->setEnabled(enabled && !loadedHdfFilePath_.isEmpty());
    updateSecondaryActionState();
}

void HdfReviewTab::setupBoundedFileRow() {
    // Secondary/batch actions move into one native menu so the primary row
    // (open/close/export) stays bounded at 1366 px (issue #358). The hidden
    // buttons keep their enable logic; the actions mirror it.
    moreActionsBtn_ = new QToolButton(this);
    moreActionsBtn_->setObjectName(QStringLiteral("reviewMoreActionsBtn"));
    moreActionsBtn_->setText(tr("More…"));
    moreActionsBtn_->setToolTip(tr("Batch exports, chart export and mask regeneration"));
    moreActionsBtn_->setPopupMode(QToolButton::InstantPopup);
    moreActionsBtn_->setFocusPolicy(Qt::StrongFocus);
    auto* menu = new QMenu(moreActionsBtn_);
    batchMetricsAct_ = menu->addAction(ui->batchExportMetricsBtn->text(), this, &HdfReviewTab::onBatchExportMetrics);
    batchMetricsAct_->setToolTip(ui->batchExportMetricsBtn->toolTip());
    batchAllAct_ = menu->addAction(ui->batchExportAllBtn->text(), this, &HdfReviewTab::onBatchExportAll);
    batchAllAct_->setToolTip(ui->batchExportAllBtn->toolTip());
    exportChartsAct_ = menu->addAction(ui->exportChartsBtn->text(), this, &HdfReviewTab::onExportCharts);
    menu->addSeparator();
    regenerateMasksAct_ = menu->addAction(ui->regenerateMasksBtn->text(), this, &HdfReviewTab::onRegenerateMasks);
    regenerateMasksAct_->setToolTip(ui->regenerateMasksBtn->toolTip());
    moreActionsBtn_->setMenu(menu);
    for (QPushButton* hidden : {ui->batchExportMetricsBtn, ui->batchExportAllBtn, ui->exportChartsBtn, ui->regenerateMasksBtn}) {
        hidden->hide();
    }
    const int insertAt = ui->fileRowLayout->indexOf(ui->exportAllBtn) + 1;
    ui->fileRowLayout->insertWidget(insertAt, moreActionsBtn_);
    ui->fileRowLayout->addStretch(1);

    // Path: elided display, full value in tooltip / copy action.
    filePathLabel_ = new frontend::ElidingLabel(ui->filePathLabel->text(), this);
    filePathLabel_->setObjectName(QStringLiteral("reviewFilePathLabel"));
    filePathLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui->fileInfoRowLayout->replaceWidget(ui->filePathLabel, filePathLabel_);
    ui->filePathLabel->hide();
    ui->overlayLegendLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    updateSecondaryActionState();
}

void HdfReviewTab::updateSecondaryActionState() {
    if (!moreActionsBtn_) return;
    batchMetricsAct_->setEnabled(ui->batchExportMetricsBtn->isEnabled());
    batchAllAct_->setEnabled(ui->batchExportAllBtn->isEnabled());
    exportChartsAct_->setEnabled(ui->exportChartsBtn->isEnabled());
    regenerateMasksAct_->setEnabled(ui->regenerateMasksBtn->isEnabled());
}

void HdfReviewTab::setFilePathText(const QString& text) {
    ui->filePathLabel->setText(text);
    if (filePathLabel_) filePathLabel_->setText(text);
}

QString HdfReviewTab::exportSummary(const backend::recording::HdfExportResult& r) const {
    QString message = tr("Export complete:\n");
    if (!r.recordingMode) {
        message += tr("- CSV: %1\n").arg(r.metricsWritten ? tr("Yes") : tr("No"));
    }
    message += tr("- Images: %1\n").arg(static_cast<qulonglong>(r.imagesExported));
    if (r.seriesExported > 0) {
        message += tr("- Series Images: %1\n").arg(static_cast<qulonglong>(r.seriesExported));
    }
    if (!r.recordingMode) {
        message += tr("- Charts: %1\n").arg(static_cast<qulonglong>(r.chartsExported));
    }
    if (!r.warnings.empty()) {
        message += tr("- Warnings: %1\n").arg(static_cast<qulonglong>(r.warnings.size()));
    }
    message += tr("\nLocation: %1").arg(QString::fromStdString(r.finalPath));
    return message;
}

void HdfReviewTab::reportExportNotCompleted(const QString& title, const backend::recording::HdfExportResult& r) {
    if (r.status == backend::recording::HdfExportStatus::Cancelled) {
        QMessageBox::information(this, title, tr("Export cancelled. Partial output was discarded."));
        return;
    }
    QString text = tr("Export failed: %1").arg(QString::fromStdString(r.error));
    if (!r.retainedPartialPath.empty()) {
        text += tr("\n\nPartial output was retained at:\n%1").arg(QString::fromStdString(r.retainedPartialPath));
    }
    QMessageBox::critical(this, title, text);
}

std::map<std::string, cv::Mat> HdfReviewTab::renderChartSnapshots(
    const std::vector<backend::services::ProcessedFrame>& validFrames) {
    std::map<std::string, cv::Mat> snapshots;
    // Snapshots are full extent without the selection; the live file's view
    // (zoom, highlight) comes back afterwards. Batch snapshots of other files
    // are restored once at the end of the batch (continueBatchExport).
    const bool liveFile = (&validFrames == &validFrames_);
    const ScatterViewState saved = saveScatterView();
    generateScatterPlot(validFrames);
    generateHistogram(validFrames);
    if (scatterHighlight_) scatterHighlight_->setVisible(false);
    auto toBgr = [](const QPixmap& pixmap) {
        cv::Mat bgr;
        if (pixmap.isNull()) return bgr;
        QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
        // Format_RGB32 is 0xAARRGGBB, i.e. B,G,R,A bytes in memory: BGRA to
        // OpenCV. Converting it as RGBA swapped red and blue in every
        // exported chart (blue points came out orange) until
        // integration.review_scatter_e2e looked at the snapshot.
        cv::Mat bgra(image.height(), image.width(), CV_8UC4, const_cast<uchar*>(image.constBits()),
                     static_cast<size_t>(image.bytesPerLine()));
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR); // deep copy; independent of the QImage
        return bgr;
    };
    snapshots["scatter_plot.tiff"] = toBgr(chartToPixmap(scatterPlotView_));
    snapshots["ring_width_histogram.tiff"] = toBgr(chartToPixmap(histogramView_));
    if (liveFile) restoreScatterView(saved);
    return snapshots;
}

void HdfReviewTab::continueBatchExport() {
    if (!batch_) return;
    while (batch_->index < batch_->sources.size()) {
        const int i = batch_->index++;
        const QString& filePath = batch_->sources[i];
        backend::recording::HdfExportRequest request;
        request.sourcePath = filePath.toStdString();
        request.outputRoot = batch_->root.toStdString();
        request.conversionFactor = backend_.processing().getPixelToMicronFactor();
        request.explicitDestination = batch_->destinations[i].toStdString();
        if (batch_->metricsOnly) {
            request.format = backend::recording::HdfExportFormat::MetricsCsv;
            // Recording files carry no metrics; reject up front like before.
            backend::services::Hdf5Service probe;
            if (probe.loadFile(request.sourcePath) && probe.isRecordingFile()) {
                batch_->failures << tr("%1: recording files do not contain metrics").arg(QFileInfo(filePath).fileName());
                continue;
            }
        } else {
            request.format = backend::recording::HdfExportFormat::All;
            // Per-file series prompt + chart snapshots on the GUI thread,
            // from a separate reader — the live tab state is untouched.
            HdfReviewLoadData data;
            QString error;
            if (!loadHdfReviewData(filePath, data, &error)) {
                batch_->failures << tr("%1: %2").arg(QFileInfo(filePath).fileName(), error);
                continue;
            }
            if (data.validFrames.empty() && data.invalidFrames.empty()) {
                batch_->failures << tr("%1: no exportable frame data found").arg(QFileInfo(filePath).fileName());
                continue;
            }
            size_t seriesCount = 0, seriesRecords = 0;
            int seriesH = 0, seriesW = 0;
            if (!data.isRecordingMode && data.reader->getSeriesImageInfo(seriesRecords, seriesCount, seriesH, seriesW)) {
                SeriesExportSelection selection;
                if (!promptSeriesExportSelection(this, seriesCount, selection)) {
                    batch_->failures << tr("%1: cancelled").arg(QFileInfo(filePath).fileName());
                    continue;
                }
                request.series.exportSeries = selection.exportSeriesImages;
                request.series.startInclusive = selection.startInclusive;
                request.series.endInclusive = selection.endInclusive;
            }
            if (!data.isRecordingMode) {
                request.supplementalImages = renderChartSnapshots(data.validFrames);
            }
        }
        const QString title = batch_->metricsOnly ? tr("Batch Metrics Export (%1/%2)") : tr("Batch Export All (%1/%2)");
        if (!beginExportJob(std::move(request), title.arg(i + 1).arg(batch_->sources.size()),
                            [this, filePath](const backend::recording::HdfExportResult& r) {
                                if (!batch_) return;
                                if (r.completed()) {
                                    ++batch_->exported;
                                    SPDLOG_INFO("Batch exported {} -> {}", filePath.toStdString(), r.finalPath);
                                } else {
                                    batch_->failures << tr("%1: %2").arg(QFileInfo(filePath).fileName(),
                                                                         QString::fromStdString(r.error));
                                    if (r.status == backend::recording::HdfExportStatus::Cancelled) {
                                        batch_->index = batch_->sources.size(); // stop the chain
                                    }
                                }
                                continueBatchExport();
                            })) {
            batch_->failures << tr("%1: could not start").arg(QFileInfo(filePath).fileName());
            continue;
        }
        return; // the completion callback resumes the chain
    }

    // Batch finished.
    auto batch = std::move(batch_);
    batch_.reset();
    finishExportUi();
    if (!batch->metricsOnly && hdfReader_ && (!validFrames_.empty() || !invalidFrames_.empty())) {
        updateCharts(); // restore the live file's charts after batch snapshots
        if (batchScatterRestore_) restoreScatterView(*batchScatterRestore_);
    }
    batchScatterRestore_.reset();
    if (batch->exported > 0) {
        if (batch->metricsOnly) rememberMetricsExportDir(batch->root);
        else rememberExportAllRootDir(batch->root);
    }
    const QString title = batch->metricsOnly ? tr("Batch Metrics Export Complete") : tr("Batch Export All Complete");
    if (batch->failures.isEmpty()) {
        QMessageBox::information(this, title,
                                 (batch->metricsOnly ? tr("Exported metrics for %1 files to:\n%2")
                                                     : tr("Exported %1 files to source-specific folders under:\n%2"))
                                     .arg(batch->exported)
                                     .arg(batch->root));
    } else {
        QMessageBox::warning(this, title,
                             tr("Exported %1 of %2 files to:\n%3\n\nFailures:\n%4")
                                 .arg(batch->exported)
                                 .arg(batch->sources.size())
                                 .arg(batch->root)
                                 .arg(trimmedFailureList(batch->failures)));
    }
}

void HdfReviewTab::onExportCharts() {
    if (exportInProgress()) {
        QMessageBox::information(this, tr("Export"), tr("An export is already running. Wait for it to finish or cancel it."));
        return;
    }
    if (validFrames_.empty() && invalidFrames_.empty()) {
        QMessageBox::warning(this, tr("Export Error"),
                            tr("No data available to export charts."));
        return;
    }

    QString dirPath = QFileDialog::getExistingDirectory(this, tr("Select Directory to Export Charts"),
                                                        "", QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dirPath.isEmpty()) {
        return;
    }

    // The same snapshots Export All embeds, written as files.
    const QDir dir(dirPath);
    bool success = true;
    for (const auto& [name, image] : renderChartSnapshots(validFrames_)) {
        const QString path = dir.filePath(QString::fromStdString(name));
        if (image.empty() || !cv::imwrite(path.toStdString(), image)) {
            SPDLOG_WARN("Failed to write chart TIFF: {}", path.toStdString());
            success = false;
        } else {
            SPDLOG_INFO("Exported chart {}x{} to {}", image.cols, image.rows, path.toStdString());
        }
    }

    if (success) {
        QMessageBox::information(this, tr("Export Complete"),
                                tr("Charts exported successfully to:\n%1").arg(dirPath));
    } else {
        QMessageBox::warning(this, tr("Export Warning"),
                            tr("Some charts may not have been exported."));
    }
}

QString HdfReviewTab::metricsExportDir() const {
    if (!lastExportDir_.isEmpty()) {
        return lastExportDir_;
    }
    if (!loadedHdfFilePath_.isEmpty()) {
        return QFileInfo(loadedHdfFilePath_).absolutePath();
    }
    return QDir::homePath();
}

QString HdfReviewTab::exportAllRootDir() const {
    return metricsExportDir();
}

void HdfReviewTab::rememberMetricsExportDir(const QString& dirPath) {
    if (dirPath.isEmpty()) {
        return;
    }
    lastExportDir_ = dirPath;
    QSettings settings;
    settings.setValue(kLastExportDirSetting, dirPath);
}

void HdfReviewTab::rememberExportAllRootDir(const QString& dirPath) {
    rememberMetricsExportDir(dirPath);
}

void HdfReviewTab::updateCharts() {
    if (!scatterPlotChart_ || !histogramChart_) {
        SPDLOG_WARN("HdfReviewTab::updateCharts: chart widgets are null");
        return;
    }

    // Recording-mode files have no per-frame metrics; clear any residual
    // chart content from a previous experiment file and bail out.
    if (isRecordingMode_) {
        if (scatterSeries_) scatterSeries_->clear();
        return;
    }

    // Generate charts from loaded frame data
    generateScatterPlot(validFrames_);
    generateHistogram(validFrames_);
    
    // Reload isoelastic curves if they were cleared (e.g., after clearDisplay)
    if (isoelasticCurves_.empty()) {
        loadIsoelasticCurves();
    }
    
    SPDLOG_INFO("HdfReviewTab::updateCharts: Generated charts from {} valid frames", validFrames_.size());
}

void HdfReviewTab::readStoredKdeRecords() {
    storedKdeLive_.clear();
    storedKdeAnalysis_.clear();
    if (!hdfReader_) return;
    auto readInto = [](const std::string& json, const char* which,
                       std::vector<std::vector<std::pair<double, double>>>& out, double& fraction) {
        std::string why;
        const auto record = backend::monitoring::fromJson(json, &why);
        if (!record) {
            SPDLOG_WARN("HdfReviewTab: stored KDE {} record ignored: {}", which, why);
            return;
        }
        fraction = record->coreFraction;
        for (const auto& loop : record->contours) {
            std::vector<std::pair<double, double>> pts;
            pts.reserve(loop.size());
            for (const auto& p : loop) pts.emplace_back(p.x, p.y);
            out.push_back(std::move(pts));
        }
    };
    std::string json;
    if (hdfReader_->readKdeAnalysisJson(json)) readInto(json, "full-run", storedKdeAnalysis_, storedKdeAnalysisFraction_);
    if (hdfReader_->readKdeLiveJson(json)) readInto(json, "live", storedKdeLive_, storedKdeLiveFraction_);
    SPDLOG_INFO("HdfReviewTab: stored KDE core contours: full-run {} loop(s), live {} loop(s)",
                storedKdeAnalysis_.size(), storedKdeLive_.size());
}

void HdfReviewTab::drawStoredKdeContours() {
    if (!scatterPlotChart_) return;
    for (auto* series : storedKdeSeries_) {
        scatterPlotChart_->removeSeries(series);
        delete series;
    }
    storedKdeSeries_.clear();
    // Full-run solid blue, live (provisional) dashed orange: the same slots
    // the Monitoring tab uses for live vs reference. One legend entry per
    // record; further loops of the same record stay out of the legend.
    auto drawFamily = [&](const std::vector<std::vector<std::pair<double, double>>>& loops, double fraction,
                          bool provisional) {
        QPen pen(provisional ? QColor(0xeb, 0x68, 0x34) : QColor(0x2a, 0x78, 0xd6));
        pen.setWidthF(2.0);
        pen.setCosmetic(true);
        if (provisional) pen.setStyle(Qt::DashLine);
        const QString name = provisional ? tr("Core %1% (live, provisional)").arg(std::lround(fraction * 100.0))
                                         : tr("Core %1% (full run)").arg(std::lround(fraction * 100.0));
        bool first = true;
        for (const auto& loop : loops) {
            auto* series = new QLineSeries();
            series->setName(name);
            series->setPen(pen);
            QList<QPointF> pts;
            pts.reserve(static_cast<qsizetype>(loop.size()));
            for (const auto& p : loop) pts.append(QPointF(p.first, p.second));
            series->append(pts);
            scatterPlotChart_->addSeries(series);
            series->attachAxis(scatterXAxis_);
            series->attachAxis(scatterYAxis_);
            if (!first) {
                for (auto* marker : scatterPlotChart_->legend()->markers(series)) marker->setVisible(false);
            }
            first = false;
            storedKdeSeries_.push_back(series);
        }
    };
    drawFamily(storedKdeAnalysis_, storedKdeAnalysisFraction_, false);
    drawFamily(storedKdeLive_, storedKdeLiveFraction_, true);
    raiseScatterHighlight();
}

QString HdfReviewTab::statusTextForTests() const {
    return ui->statusLabel->text();
}

void HdfReviewTab::updateComputeCoreActionState() {
    if (!computeCoreAction_) return;
    bool anyValid = false;
    for (const auto& f : validFrames_) {
        if (f.validation.isValid) { anyValid = true; break; }
    }
    computeCoreAction_->setEnabled(hdfReader_ && !isRecordingMode_ && anyValid && !coreWatcher_);
}

void HdfReviewTab::startFullRunCoreComputation() {
    if (coreWatcher_ || !hdfReader_ || isRecordingMode_ || loadedHdfFilePath_.isEmpty()) return;
    // Same axes as this scatter: area in µm² with the current factor.
    const double factor = backend_.processing().getPixelToMicronFactor();
    const double areaFactor = factor * factor;
    std::vector<backend::monitoring::DensityPoint> points;
    points.reserve(validFrames_.size());
    for (const auto& f : validFrames_) {
        if (f.validation.isValid) points.push_back({f.validation.area * areaFactor, f.validation.deformability});
    }
    if (points.empty()) {
        ui->statusLabel->setText(tr("Core contour: no valid cells in this file"));
        return;
    }
    // The core share follows the Monitoring setting (Monitoring Settings).
    bool ok = false;
    double fraction = QSettings().value(QStringLiteral("Monitoring/KdeCoreFraction"), 0.9).toDouble(&ok);
    if (!ok || !std::isfinite(fraction)) fraction = 0.9;
    fraction = std::clamp(fraction, 0.05, 1.0);
    coreJobPath_ = loadedHdfFilePath_;
    ui->statusLabel->setText(tr("Computing core contour from %1 cells…").arg(points.size()));
    SPDLOG_INFO("HdfReviewTab: full-run core contour started ({} cells, {:.0f}%) for {}", points.size(), fraction * 100.0,
                coreJobPath_.toStdString());
    coreWatcher_ = new QFutureWatcher<backend::monitoring::KdeCoreRecord>(this);
    connect(coreWatcher_, &QFutureWatcher<backend::monitoring::KdeCoreRecord>::finished, this,
            &HdfReviewTab::onFullRunCoreFinished);
    updateComputeCoreActionState();
    coreWatcher_->setFuture(QtConcurrent::run([points = std::move(points), fraction, factor]() {
        auto record = backend::monitoring::computeFullRunCoreRecord(points, fraction, factor);
        record.computedAtNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             std::chrono::system_clock::now().time_since_epoch())
                                                             .count());
        return record;
    }));
}

void HdfReviewTab::onFullRunCoreFinished() {
    auto* watcher = coreWatcher_;
    coreWatcher_ = nullptr;
    if (!watcher) return;
    const backend::monitoring::KdeCoreRecord record = watcher->result();
    watcher->deleteLater();
    updateComputeCoreActionState();
    if (coreJobPath_ != loadedHdfFilePath_ || !hdfReader_) {
        SPDLOG_INFO("HdfReviewTab: full-run core contour dropped (file changed while computing)");
        return;
    }
    if (record.contours.empty()) {
        ui->statusLabel->setText(tr("Core contour: too few cells for a contour (%1 estimated)").arg(record.populationCount));
        return;
    }
    const QString summary = tr("Core %1%: %2 of %3 cells, %4 loop(s)")
                                .arg(std::lround(record.coreFraction * 100.0))
                                .arg(record.cellCount)
                                .arg(record.populationCount)
                                .arg(record.contours.size());
    if (!storedKdeAnalysis_.empty()) {
        bool replace = false;
        if (overwriteAnswerForTests_) {
            replace = *overwriteAnswerForTests_;
        } else {
            replace = QMessageBox::question(this, tr("Replace core contour"),
                                            tr("This experiment already has a full-run core contour.\n"
                                               "Replace it with the new one?\n\n%1").arg(summary),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        }
        if (!replace) {
            ui->statusLabel->setText(tr("Kept the existing full-run core contour"));
            return;
        }
    }
    // Save: the review reader holds the file read-only, so close it, write
    // through a read-write handle, then reopen for review.
    QString notSaved;
    if (exportWatcher_) {
        notSaved = tr("an export is running");
    } else {
        const std::string path = loadedHdfFilePath_.toStdString();
        hdfReader_->closeFile();
        {
            backend::services::Hdf5Service updater;
            if (!updater.openFileForUpdate(path)) {
                notSaved = tr("the file cannot be opened for writing");
            } else {
                if (!updater.writeKdeAnalysisJson(backend::monitoring::toJson(record))) notSaved = tr("the write failed");
                updater.closeFile();
            }
        }
        if (!hdfReader_->loadFile(path)) {
            SPDLOG_ERROR("HdfReviewTab: could not reopen {} after saving the core contour", path);
            ui->statusLabel->setText(tr("Core contour saved, but the file could not be reopened; open it again"));
            hdfReader_.reset();
            updateComputeCoreActionState();
            return;
        }
    }
    storedKdeAnalysis_.clear();
    for (const auto& loop : record.contours) {
        std::vector<std::pair<double, double>> pts;
        pts.reserve(loop.size());
        for (const auto& p : loop) pts.emplace_back(p.x, p.y);
        storedKdeAnalysis_.push_back(std::move(pts));
    }
    storedKdeAnalysisFraction_ = record.coreFraction;
    drawStoredKdeContours();
    if (notSaved.isEmpty()) {
        ui->statusLabel->setText(tr("Full-run core contour saved. %1").arg(summary));
        SPDLOG_INFO("HdfReviewTab: full-run core contour saved to {}", loadedHdfFilePath_.toStdString());
    } else {
        ui->statusLabel->setText(tr("Full-run core contour shown, not saved (%1). %2").arg(notSaved, summary));
        SPDLOG_WARN("HdfReviewTab: full-run core contour not saved to {}: {}", loadedHdfFilePath_.toStdString(),
                    notSaved.toStdString());
    }
}

void HdfReviewTab::generateScatterPlot(const std::vector<backend::services::ProcessedFrame>& validFrames) {
    if (!scatterSeries_ || !scatterXAxis_ || !scatterYAxis_) {
        return;
    }

    scatterSeries_->clear();
    drawStoredKdeContours();
    updateComputeCoreActionState();
    scatterPoints_.clear();
    scatterPointToFrame_.clear();
    frameToScatterPoint_.assign(validFrames.size(), -1);
    scatterShowsLiveFile_ = (&validFrames == &validFrames_);
    // Home ranges for double-click / "Reset zoom"; the data extent below.
    auto setHome = [this](double x0, double x1, double y0, double y1) {
        scatterXAxis_->setRange(x0, x1);
        scatterYAxis_->setRange(y0, y1);
        if (scatterPlotView_) {
            scatterPlotView_->setDefaultRange(scatterXAxis_, x0, x1);
            scatterPlotView_->setDefaultRange(scatterYAxis_, y0, y1);
        }
        if (scatterShowsLiveFile_) setScatterHighlight(highlightFrame_);
        else if (scatterHighlight_) scatterHighlight_->setVisible(false);
    };

    if (validFrames.empty()) {
        setHome(0, 1000, 0, 1);
        return;
    }

    // Get conversion factor from backend (pixels to microns)
    const double conversionFactor = backend_.processing().getPixelToMicronFactor();
    // Area conversion: pixels² to microns² = pixels² * (microns/pixel)²
    const double areaConversionFactor = conversionFactor * conversionFactor;

    // Collect points: scatterPoints_ for hit tests, seriesPoints for the chart.
    QList<QPointF> seriesPoints;
    double minArea = std::numeric_limits<double>::max();
    double maxArea = std::numeric_limits<double>::lowest();
    double minDeform = std::numeric_limits<double>::max();
    double maxDeform = std::numeric_limits<double>::lowest();

    for (size_t i = 0; i < validFrames.size(); ++i) {
        const auto& frame = validFrames[i];
        if (frame.validation.isValid) {
            // Convert area from pixels² to microns²
            double areaPixels = frame.validation.area;
            double areaMicrons = areaPixels * areaConversionFactor;
            double deform = frame.validation.deformability;
            frameToScatterPoint_[i] = static_cast<int>(scatterPoints_.size());
            scatterPointToFrame_.push_back(static_cast<int>(i));
            scatterPoints_.push_back({areaMicrons, deform, static_cast<int>(i)});
            seriesPoints.append(QPointF(areaMicrons, deform));

            minArea = std::min(minArea, areaMicrons);
            maxArea = std::max(maxArea, areaMicrons);
            minDeform = std::min(minDeform, deform);
            maxDeform = std::max(maxDeform, deform);
        }
    }

    if (seriesPoints.isEmpty()) {
        setHome(0, 1000, 0, 1);
        return;
    }

    // One geometry rebuild for the whole set: replace() emits pointsReplaced
    // once, while append() (per point, and QList append on Qt 6.4) emits
    // pointAdded per point and rebuilds the series geometry each time —
    // O(n²); a 20 000-cell file took minutes to open.
    scatterSeries_->replace(seriesPoints);

    // Axis ranges with padding
    double x0 = 0, x1 = 1000, y0 = 0, y1 = 1;
    if (minArea < maxArea) {
        const double areaPadding = (maxArea - minArea) * 0.1;
        x0 = minArea - areaPadding;
        x1 = maxArea + areaPadding;
    }
    if (minDeform < maxDeform) {
        const double deformPadding = (maxDeform - minDeform) * 0.1;
        y0 = minDeform - deformPadding;
        y1 = maxDeform + deformPadding;
    }
    setHome(x0, x1, y0, y1);
}

void HdfReviewTab::generateHistogram(const std::vector<backend::services::ProcessedFrame>& validFrames) {
    // Use config range so the histogram matches the current ring ratio thresholds
    auto cfg = backend_.processing().getProcessingConfig();
    const double HISTOGRAM_MIN = cfg.ring_ratio_min;
    const double HISTOGRAM_MAX = cfg.ring_ratio_max;
    constexpr double HISTOGRAM_BIN_WIDTH = 0.5;
    const int HISTOGRAM_BINS = std::max(1, static_cast<int>((HISTOGRAM_MAX - HISTOGRAM_MIN) / HISTOGRAM_BIN_WIDTH));

    // Reset series
#if MIB_HAS_QHISTOGRAMSERIES
    if (histogramSeries_) {
        histogramSeries_->clear();
    }
#else
    if (histogramBarSeries_) {
        histogramBarSeries_->clear();
    }
#endif

    // Always set fixed x-axis range regardless of data
#if MIB_HAS_QHISTOGRAMSERIES
    if (histogramXAxis_) {
        histogramXAxis_->setRange(HISTOGRAM_MIN, HISTOGRAM_MAX);
        histogramXAxis_->setTickCount(6);
    }
#endif

    // Collect ring ratio values from valid frames
    std::vector<double> ringRatios;
    for (const auto& frame : validFrames) {
        if (frame.validation.isValid && frame.validation.ringRatio > 0.0) {
            ringRatios.push_back(frame.validation.ringRatio);
        }
    }

    // If no data, show empty histogram with fixed range
    if (ringRatios.empty()) {
        if (histogramYAxis_) {
            histogramYAxis_->setRange(0, 1);
        }
#if !MIB_HAS_QHISTOGRAMSERIES
        if (histogramCategoryAxis_) {
            histogramChart_->removeAxis(histogramCategoryAxis_);
            delete histogramCategoryAxis_;
            histogramCategoryAxis_ = nullptr;
        }
        histogramCategoryAxis_ = new QBarCategoryAxis();
        QStringList categories;
        categories.reserve(HISTOGRAM_BINS);
        for (int i = 0; i < HISTOGRAM_BINS; ++i) {
            const double start = HISTOGRAM_MIN + i * HISTOGRAM_BIN_WIDTH;
            const double end = (i == HISTOGRAM_BINS - 1) ? HISTOGRAM_MAX : (start + HISTOGRAM_BIN_WIDTH);
            categories << QString("%1-%2").arg(start, 0, 'f', 1).arg(end, 0, 'f', 1);
        }
        histogramCategoryAxis_->append(categories);
        histogramCategoryAxis_->setLabelsAngle(-90);
        histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
        if (histogramBarSeries_) {
            histogramBarSeries_->attachAxis(histogramCategoryAxis_);
        }
#endif
        return;
    }

    // Count values in each bin
    std::vector<int> binCounts(HISTOGRAM_BINS, 0);
    for (double val : ringRatios) {
        double clampedVal = std::clamp(val, HISTOGRAM_MIN, HISTOGRAM_MAX);
        int binIndex = static_cast<int>((clampedVal - HISTOGRAM_MIN) / HISTOGRAM_BIN_WIDTH);
        if (binIndex >= HISTOGRAM_BINS) {
            binIndex = HISTOGRAM_BINS - 1;
        }
        binIndex = std::clamp(binIndex, 0, HISTOGRAM_BINS - 1);
        binCounts[binIndex]++;
    }

    int maxCount = 0;
    for (int count : binCounts) {
        maxCount = std::max(maxCount, count);
    }

    // Set Y-axis range
    if (histogramYAxis_) {
        const int yMax = std::max(1, static_cast<int>(std::ceil(maxCount * 1.1)));
        histogramYAxis_->setRange(0, yMax);
        histogramYAxis_->applyNiceNumbers();
    }

#if MIB_HAS_QHISTOGRAMSERIES
    // Populate histogram series
    if (histogramSeries_) {
        QVector<qreal> samples;
        samples.reserve(static_cast<int>(ringRatios.size()));
        for (double v : ringRatios) {
            samples.append(static_cast<qreal>(v));
        }
        histogramSeries_->setBinsCount(HISTOGRAM_BINS);
        histogramSeries_->setSamples(samples);
    }
#else
    // Fallback: build bar set and category axis
    auto* barSet = new QBarSet("");
    for (int count : binCounts) {
        *barSet << count;
    }
    if (histogramBarSeries_) {
        histogramBarSeries_->append(barSet);
    }
    
    if (histogramCategoryAxis_) {
        histogramChart_->removeAxis(histogramCategoryAxis_);
        delete histogramCategoryAxis_;
        histogramCategoryAxis_ = nullptr;
    }
    histogramCategoryAxis_ = new QBarCategoryAxis();
    QStringList categories;
    categories.reserve(HISTOGRAM_BINS);
    for (int i = 0; i < HISTOGRAM_BINS; ++i) {
        const double start = HISTOGRAM_MIN + i * HISTOGRAM_BIN_WIDTH;
        const double end = (i == HISTOGRAM_BINS - 1) ? HISTOGRAM_MAX : (start + HISTOGRAM_BIN_WIDTH);
        categories << QString("%1-%2").arg(start, 0, 'f', 1).arg(end, 0, 'f', 1);
    }
    histogramCategoryAxis_->append(categories);
    histogramCategoryAxis_->setLabelsAngle(-90);
    histogramChart_->addAxis(histogramCategoryAxis_, Qt::AlignBottom);
    if (histogramBarSeries_) {
        histogramBarSeries_->attachAxis(histogramCategoryAxis_);
    }
#endif
}

// ---- Charts view: scatter interaction and docked frame pane (issue #467) ----

void HdfReviewTab::setupChartsLayout() {
    for (QWidget* placeholder : {static_cast<QWidget*>(ui->scatterPlotViewPlaceholder),
                                 static_cast<QWidget*>(ui->histogramViewPlaceholder)}) {
        ui->chartsLayout->removeWidget(placeholder);
        placeholder->deleteLater();
    }

    // Frame pane: empty state, or the embedded viewer on the selected cell.
    framePaneStack_ = new QStackedWidget(ui->chartsTab);
    framePaneStack_->setObjectName(QStringLiteral("reviewFramePane"));
    auto* empty = new QLabel(tr("Click a point on the scatter to view the cell"), framePaneStack_);
    empty->setObjectName(QStringLiteral("reviewFramePaneEmpty"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setEnabled(false);
    framePaneStack_->addWidget(empty);

    auto* page = new QWidget(framePaneStack_);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(2);
    framePaneTitle_ = new QLabel(page);
    framePaneTitle_->setObjectName(QStringLiteral("reviewFramePaneTitle"));
    pageLayout->addWidget(framePaneTitle_);
    framePane_ = new FrameViewerDialog(backend::services::ProcessedFrame{}, roi_, overlayMode_, showRoiOverlay_, page);
    framePane_->setEmbedded(true);
    framePane_->setObjectName(QStringLiteral("reviewFramePaneViewer"));
    pageLayout->addWidget(framePane_, 1);
    framePaneStack_->addWidget(page);
    connect(framePane_, &FrameViewerDialog::requestPreviousFrame, this, [this]() { stepScatterSelection(-1); });
    connect(framePane_, &FrameViewerDialog::requestNextFrame, this, [this]() { stepScatterSelection(+1); });
    connect(framePane_, &FrameViewerDialog::requestOpenInWindow, this, [this]() {
        if (highlightFrame_ >= 0) showFrameViewer(highlightFrame_, true);
    });

    // scatter | (frame pane over histogram); the pane never covers the plot.
    chartsRightSplitter_ = new QSplitter(Qt::Vertical, ui->chartsTab);
    chartsRightSplitter_->setObjectName(QStringLiteral("reviewChartsRightSplitter"));
    chartsRightSplitter_->addWidget(framePaneStack_);
    chartsRightSplitter_->addWidget(histogramView_);
    chartsRightSplitter_->setStretchFactor(0, 3);
    chartsRightSplitter_->setStretchFactor(1, 2);
    chartsRightSplitter_->setChildrenCollapsible(false);
    chartsSplitter_ = new QSplitter(Qt::Horizontal, ui->chartsTab);
    chartsSplitter_->setObjectName(QStringLiteral("reviewChartsSplitter"));
    chartsSplitter_->addWidget(scatterPlotView_);
    chartsSplitter_->addWidget(chartsRightSplitter_);
    chartsSplitter_->setStretchFactor(0, 3);
    chartsSplitter_->setStretchFactor(1, 2);
    chartsSplitter_->setChildrenCollapsible(false);
    ui->chartsLayout->addWidget(chartsSplitter_);
    // The scatter is the point of this view: it keeps a usable width and
    // starts with 60 % of it; the pane scrolls its image instead of growing.
    scatterPlotView_->setMinimumWidth(420);
    framePaneStack_->setMinimumWidth(320);
    chartsSplitter_->setSizes({600, 400});
    chartsRightSplitter_->setSizes({550, 300});

    QSettings settings;
    chartsSplitter_->restoreState(settings.value(QStringLiteral("Review/ChartsSplitter")).toByteArray());
    chartsRightSplitter_->restoreState(settings.value(QStringLiteral("Review/ChartsRightSplitter")).toByteArray());
    // Saved once in the destructor; splitterMoved fires per pixel of a drag.
}

bool HdfReviewTab::chartsTabVisible() const {
    return ui->frameTypeTabs->currentWidget() == ui->chartsTab;
}

scatterhit::Viewport HdfReviewTab::scatterViewport() const {
    scatterhit::Viewport v;
    const QRectF plot = scatterPlotChart_->plotArea();
    v.x0 = scatterXAxis_->min();
    v.x1 = scatterXAxis_->max();
    v.y0 = scatterYAxis_->min();
    v.y1 = scatterYAxis_->max();
    v.left = plot.left();
    v.top = plot.top();
    v.width = plot.width();
    v.height = plot.height();
    return v;
}

std::optional<std::size_t> HdfReviewTab::scatterPointAt(QPointF viewPos) const {
    if (!scatterShowsLiveFile_ || scatterPoints_.empty() || !scatterPlotView_) return std::nullopt;
    // View (viewport) -> scene -> chart item coordinates, where plotArea() lives.
    const QPointF chartPos = scatterPlotChart_->mapFromScene(scatterPlotView_->mapToScene(viewPos.toPoint()));
    const double tolerance = std::max(scatterSeries_->markerSize(), 8.0);
    return scatterhit::nearest(scatterPoints_, scatterViewport(), chartPos.x(), chartPos.y(), tolerance);
}

void HdfReviewTab::onScatterClicked(QPointF viewPos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) return;
    // An export redraws the scatter (batch: with other files' data).
    if (exportInProgress() || !hdfReader_ || isRecordingMode_) return;
    const auto hit = scatterPointAt(viewPos);
    if (!hit) return; // empty space: the selection only changes on a point
    const int frame = scatterPointToFrame_[*hit];
    SPDLOG_DEBUG("HdfReviewTab: scatter point {} -> valid frame {}", *hit, frame);
    selectScatterFrame(frame);
}

void HdfReviewTab::onScatterDoubleClicked(QPointF viewPos) {
    // Same guards as a click: an export is redrawing the scatter (batch:
    // with other files' data), and a reset there would also drop the
    // user-zoomed flag the restore relies on.
    if (exportInProgress() || !scatterShowsLiveFile_ || !hdfReader_ || isRecordingMode_) return;
    // Its first click already selected; a double-click on a point never
    // zooms out from under the user.
    if (scatterPointAt(viewPos)) return;
    scatterPlotView_->resetZoom();
}

void HdfReviewTab::onScatterHover(QPointF viewPos) {
    const auto hit = exportInProgress() ? std::nullopt : scatterPointAt(viewPos);
    if (!hit) {
        scatterPlotView_->unsetCursor();
        QToolTip::hideText();
        return;
    }
    scatterPlotView_->setCursor(Qt::PointingHandCursor);
    const auto& p = scatterPoints_[*hit];
    QToolTip::showText(scatterPlotView_->mapToGlobal(viewPos.toPoint()),
                       tr("Frame %1 · %2 µm² · deformability %3")
                           .arg(static_cast<qulonglong>(validFrames_[static_cast<size_t>(p.frame)].index))
                           .arg(p.x, 0, 'f', 1)
                           .arg(p.y, 0, 'f', 4),
                       scatterPlotView_);
}

void HdfReviewTab::selectScatterFrame(int frameIndex) {
    setSelectedFrame(frameIndex, true); // highlight + pane follow valid selections
}

void HdfReviewTab::stepScatterSelection(int delta) {
    if (validFrames_.empty() || isRecordingMode_) return;
    const int n = static_cast<int>(validFrames_.size());
    const int from = highlightFrame_ >= 0 ? highlightFrame_ : (delta > 0 ? -1 : 0);
    selectScatterFrame(((from + delta) % n + n) % n);
}

void HdfReviewTab::setScatterHighlight(int frameIndex) {
    highlightFrame_ = frameIndex;
    if (!scatterHighlight_) return;
    scatterHighlight_->clear();
    const int point = (scatterShowsLiveFile_ && frameIndex >= 0 &&
                       frameIndex < static_cast<int>(frameToScatterPoint_.size()))
                          ? frameToScatterPoint_[static_cast<size_t>(frameIndex)]
                          : -1;
    if (point < 0) {
        scatterHighlight_->setVisible(false); // no point for this frame (failed validation)
        return;
    }
    const auto& p = scatterPoints_[static_cast<size_t>(point)];
    scatterHighlight_->append(p.x, p.y);
    scatterHighlight_->setVisible(true);
}

void HdfReviewTab::raiseScatterHighlight() {
    if (!scatterHighlight_ || !scatterPlotChart_) return;
    // Series draw in insertion order; the selection goes last.
    const auto series = scatterPlotChart_->series();
    if (series.isEmpty() || series.last() != scatterHighlight_) {
        scatterPlotChart_->removeSeries(scatterHighlight_);
        scatterPlotChart_->addSeries(scatterHighlight_);
        scatterHighlight_->attachAxis(scatterXAxis_);
        scatterHighlight_->attachAxis(scatterYAxis_);
    }
    for (auto* marker : scatterPlotChart_->legend()->markers(scatterHighlight_)) marker->setVisible(false);
}

void HdfReviewTab::refreshFramePane() {
    if (!framePane_ || !framePaneStack_) return;
    const bool hasCell = highlightFrame_ >= 0 && highlightFrame_ < static_cast<int>(validFrames_.size()) &&
                         !isRecordingMode_;
    if (!hasCell) {
        paneFrame_ = -1;
        framePaneStack_->setCurrentIndex(0);
        return;
    }
    // One HDF5 read per shown frame, only while the Charts tab shows the pane
    // and no modal viewer hides it (its prev/next would read every frame twice).
    if (!chartsTabVisible() || paneFrame_ == highlightFrame_) return;
    if (modalViewerOpen_) {
        paneStale_ = true;
        return;
    }
    paneFrame_ = highlightFrame_;
    paneStale_ = false;
    framePane_->setRoi(roi_);
    framePane_->setFrame(loadFrameForDisplay(paneFrame_, true));
    // Recorded frame index (what the viewer and the metrics CSV show) first;
    // the 1-based position in the valid set second.
    const auto& frame = validFrames_[static_cast<size_t>(paneFrame_)];
    const bool onScatter = paneFrame_ < static_cast<int>(frameToScatterPoint_.size()) &&
                           frameToScatterPoint_[static_cast<size_t>(paneFrame_)] >= 0;
    QString title = tr("Frame %1 · valid row %2 of %3")
                        .arg(static_cast<qulonglong>(frame.index))
                        .arg(paneFrame_ + 1)
                        .arg(validFrames_.size());
    if (!onScatter) title += tr(" · failed validation, not on the scatter");
    framePaneTitle_->setText(title);
    framePaneStack_->setCurrentIndex(1);
}

HdfReviewTab::ScatterViewState HdfReviewTab::saveScatterView() const {
    ScatterViewState st;
    st.x0 = scatterXAxis_->min();
    st.x1 = scatterXAxis_->max();
    st.y0 = scatterYAxis_->min();
    st.y1 = scatterYAxis_->max();
    st.userZoomed = scatterPlotView_ && scatterPlotView_->isUserZoomed();
    st.highlightFrame = highlightFrame_;
    return st;
}

void HdfReviewTab::restoreScatterView(const ScatterViewState& st) {
    if (st.userZoomed) {
        scatterXAxis_->setRange(st.x0, st.x1);
        scatterYAxis_->setRange(st.y0, st.y1);
        if (scatterPlotView_) scatterPlotView_->markUserZoomed();
    }
    setScatterHighlight(st.highlightFrame);
}

std::map<std::string, cv::Mat> HdfReviewTab::renderChartSnapshotsForTests() {
    return renderChartSnapshots(validFrames_);
}

QWidget* HdfReviewTab::framePaneForTests() const {
    return framePaneStack_;
}

std::optional<std::pair<int, bool>> HdfReviewTab::framePaneFrameForTests() const {
    if (!framePaneStack_ || framePaneStack_->currentIndex() != 1 || paneFrame_ < 0) return std::nullopt;
    return std::make_pair(paneFrame_, true);
}

QPointF HdfReviewTab::scatterPointViewPosForTests(int point) const {
    const auto& p = scatterPoints_.at(static_cast<size_t>(point));
    const QPointF chartPos = scatterPlotChart_->mapToPosition(QPointF(p.x, p.y), scatterSeries_);
    return QPointF(scatterPlotView_->mapFromScene(scatterPlotChart_->mapToScene(chartPos)));
}

QPixmap HdfReviewTab::chartToPixmap(QChartView* chartView) const {
    if (!chartView || !chartView->chart()) {
        return QPixmap();
    }
    
    // Render chart at high resolution for export (square format: 1200x1200 pixels)
    const int exportSize = 1200;
    
    // Save original chart view size and minimum size
    QSize originalSize = chartView->size();
    QSize originalMinSize = chartView->minimumSize();
    
    // Temporarily set minimum size and resize the chart view to match export size
    // This ensures the chart layout is correct for the export dimensions
    chartView->setMinimumSize(exportSize, exportSize);
    chartView->resize(exportSize, exportSize);
    
    // Force layout update and rendering
    chartView->updateGeometry();
    chartView->update();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    
    // Grab the chart at the new size
    QPixmap pixmap = chartView->grab();
    
    // Restore original size and minimum size
    chartView->setMinimumSize(originalMinSize);
    chartView->resize(originalSize);
    chartView->update();
    
    return pixmap;
}

void HdfReviewTab::loadIsoelasticCurves() {
    // Clear any existing curves to avoid duplicates
    for (auto it = isoelasticCurves_.begin(); it != isoelasticCurves_.end(); ++it) {
        QLineSeries* series = *it;
        if (series) {
            scatterPlotChart_->removeSeries(series);
            delete series;
        }
    }
    isoelasticCurves_.clear();
    
    // Find the isoelastic curve data file
    QString appDir = QCoreApplication::applicationDirPath();
    QString filePath = QDir(appDir).absoluteFilePath("../resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
    
    // Try alternative path if file doesn't exist
    if (!QFile::exists(filePath)) {
        filePath = QDir(appDir).absoluteFilePath("resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
    }
    
    // Try source directory path for development
    if (!QFile::exists(filePath)) {
        filePath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt");
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SPDLOG_WARN("Failed to open isoelastic curve file: {}", filePath.toStdString());
        return;
    }

    // Group data points by emodulus value
    std::map<double, std::vector<std::pair<double, double>>> curvesByModulus;

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        
        // Skip empty lines and comments
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        // Parse tab-separated values: area_um, deform, emodulus
        QStringList parts = line.split('\t', Qt::SkipEmptyParts);
        if (parts.size() < 3) {
            continue;
        }

        bool ok1, ok2, ok3;
        double areaUm = parts[0].toDouble(&ok1);
        double deform = parts[1].toDouble(&ok2);
        double emodulus = parts[2].toDouble(&ok3);

        if (ok1 && ok2 && ok3) {
            curvesByModulus[emodulus].push_back({areaUm, deform});
        }
    }

    file.close();

    if (curvesByModulus.empty()) {
        SPDLOG_WARN("No isoelastic curve data found in file: {}", filePath.toStdString());
        return;
    }

    // Create QLineSeries for each modulus value (in reverse order for legend)
    for (auto it = curvesByModulus.rbegin(); it != curvesByModulus.rend(); ++it) {
        const auto& [emodulus, points] = *it;
        QLineSeries* series = new QLineSeries();
        series->setName(QString("%1 kPa").arg(emodulus, 0, 'f', 2));
        
        // Add points to series
        for (const auto& [area, deform] : points) {
            series->append(area, deform);
        }

        // Add series to chart
        scatterPlotChart_->addSeries(series);
        series->attachAxis(scatterXAxis_);
        series->attachAxis(scatterYAxis_);
        
        // Store pointer for cleanup
        isoelasticCurves_.push_back(series);
    }

    // Enable legend to show all series and position it on the right
    scatterPlotChart_->legend()->setVisible(true);
    scatterPlotChart_->legend()->setAlignment(Qt::AlignRight);
    raiseScatterHighlight();
    
    SPDLOG_INFO("Loaded {} isoelastic curves from {}", curvesByModulus.size(), filePath.toStdString());
}

} // namespace frontend

// Include moc file for ThumbnailLabel class (defined in this .cpp file with Q_OBJECT)
#include "HdfReviewTab.moc"
