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
#include "backend/recording/RecordingAccounting.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/dialogs/BatchMaskDialog.h"
#include "frontend/dialogs/FrameViewerDialog.h"
#include "frontend/models/HdfMetricsModel.h"
#include "frontend/utils/OverlayRenderer.h"
#include "frontend/utils/HdfReviewExportPaths.h"
#include "backend/recording/HdfExportService.h"
#include "frontend/utils/ElidingLabel.h"

#include <QToolButton>
#include <QMenu>
#include <QAction>

#include <QFutureWatcher>
#include <QPointer>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrent>

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
    
    scatterPlotView_ = new QChartView(scatterPlotChart_, ui->chartsTab);
    scatterPlotView_->setRenderHint(QPainter::Antialiasing);
    scatterPlotView_->setMinimumHeight(300);
    // Replace placeholder with actual chart view
    int scatterIndex = ui->chartsLayout->indexOf(ui->scatterPlotViewPlaceholder);
    ui->chartsLayout->removeWidget(ui->scatterPlotViewPlaceholder);
    ui->scatterPlotViewPlaceholder->deleteLater();
    ui->chartsLayout->insertWidget(scatterIndex, scatterPlotView_, 1);
    
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
    histogramView_->setMinimumHeight(300);
    // Replace placeholder with actual chart view
    int histogramIndex = ui->chartsLayout->indexOf(ui->histogramViewPlaceholder);
    ui->chartsLayout->removeWidget(ui->histogramViewPlaceholder);
    ui->histogramViewPlaceholder->deleteLater();
    ui->chartsLayout->insertWidget(histogramIndex, histogramView_, 1);

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
    validFrames_.clear();
    invalidFrames_.clear();
    selectedFrameIndex_ = -1;
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
    setSelectedFrame(frameIndex);
}

void HdfReviewTab::onThumbnailDoubleClicked(int frameIndex) {
    showFrameViewer(frameIndex);
}

void HdfReviewTab::onViewFrameDetails(int frameIndex) {
    showFrameViewer(frameIndex);
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
    QTableView* table = isShowingValid_ ? ui->validMetricsTable : ui->invalidMetricsTable;
    if (!table || !table->selectionModel()) return;
    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (!rows.isEmpty()) {
        setSelectedFrame(rows.first().row());
    }
}

void HdfReviewTab::setSelectedFrame(int frameIndex) {
    if (frameIndex < 0) {
        selectedFrameIndex_ = -1;
        return;
    }

    const auto& frames = isShowingValid_ ? validFrames_ : invalidFrames_;
    if (frameIndex >= static_cast<int>(frames.size())) {
        return;
    }

    selectedFrameIndex_ = frameIndex;

    // Update thumbnail selection
    QGridLayout* grid = isShowingValid_ ? ui->validImageGrid : ui->invalidImageGrid;
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
    QTableView* table = isShowingValid_ ? ui->validMetricsTable : ui->invalidMetricsTable;
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

void HdfReviewTab::showFrameViewer(int frameIndex) {
    const auto& framesMeta = isShowingValid_ ? validFrames_ : invalidFrames_;
    if (frameIndex < 0 || frameIndex >= static_cast<int>(framesMeta.size())) {
        return;
    }
    SPDLOG_INFO("HdfReviewTab: showFrameViewer index={} ({})", frameIndex, isShowingValid_ ? "valid" : "invalid");

    // Build a full ProcessedFrame by fetching images on demand
    backend::services::ProcessedFrame initialFrame = framesMeta[frameIndex];
    const std::string imgPath = imagesPath(isShowingValid_);
    const std::string maskPath = masksPath(isShowingValid_);

    if (hdfReader_) {
        cv::Mat original, mask;
        if (hdfReader_->readImageByIndex(imgPath, static_cast<size_t>(frameIndex), original)) {
            initialFrame.originalImage = original;
            SPDLOG_TRACE("HdfReviewTab: viewer loaded original {}[{}] ({}x{}x{})",
                         imgPath, frameIndex, original.cols, original.rows, original.channels());
        }
        if (!maskPath.empty() && hdfReader_->readImageByIndex(maskPath, static_cast<size_t>(frameIndex), mask)) {
            initialFrame.processedImage = mask;
            SPDLOG_TRACE("HdfReviewTab: viewer loaded mask {}[{}] ({}x{}x{})",
                         maskPath, frameIndex, mask.cols, mask.rows, mask.channels());
        }
        // Load multi-image series data if available.
        if (isShowingValid_) {
            if (isRecordingMode_) {
                loadRecordingSeriesWindow(static_cast<size_t>(frameIndex), initialFrame);
            } else {
                std::vector<cv::Mat> seriesImages;
                if (hdfReader_->readSeriesImagesByIndex(static_cast<size_t>(frameIndex), seriesImages) && !seriesImages.empty()) {
                    initialFrame.seriesImages = std::move(seriesImages);
                    SPDLOG_DEBUG("HdfReviewTab: loaded {} series images for frame {}", initialFrame.seriesImages.size(), frameIndex);
                }
            }
        }
    }

    // Create dialog with current overlay mode and ROI overlay state
    auto* dialog = new FrameViewerDialog(initialFrame, roi_, overlayMode_, showRoiOverlay_, this);
    
    // Store current index in a way that can be modified by lambdas
    struct NavigationState {
        int currentIndex;
        bool isValidSet;
    };

    // shared_ptr: each lambda co-owns the state, so its lifetime no longer
    // depends on the destroyed-signal connect ordering (a hand-rolled
    // new/delete-in-connect was one refactor away from a double free / leak).
    auto navState = std::make_shared<NavigationState>(NavigationState{frameIndex, isShowingValid_});
    
    // Connect navigation signals
    // Helper lambda to load series images for a frame.
    auto loadSeriesImages = [this, navState](backend::services::ProcessedFrame& pf, int idx) {
        if (navState->isValidSet && hdfReader_) {
            if (isRecordingMode_) {
                loadRecordingSeriesWindow(static_cast<size_t>(idx), pf);
            } else {
                std::vector<cv::Mat> seriesImages;
                if (hdfReader_->readSeriesImagesByIndex(static_cast<size_t>(idx), seriesImages) && !seriesImages.empty()) {
                    pf.seriesImages = std::move(seriesImages);
                }
            }
        }
    };

    connect(dialog, &FrameViewerDialog::requestPreviousFrame, this, [this, dialog, navState, loadSeriesImages]() {
        const auto& frames = navState->isValidSet ? validFrames_ : invalidFrames_;
        if (frames.empty()) return;
        navState->currentIndex = navState->currentIndex - 1;
        if (navState->currentIndex < 0) {
            navState->currentIndex = static_cast<int>(frames.size()) - 1; // Wrap to last
        }
        if (navState->currentIndex >= 0 && navState->currentIndex < static_cast<int>(frames.size())) {
            // Fetch images on demand
            backend::services::ProcessedFrame pf = frames[navState->currentIndex];
            const std::string imgPath2 = imagesPath(navState->isValidSet);
            const std::string maskPath2 = masksPath(navState->isValidSet);
            if (hdfReader_) {
                cv::Mat original2, mask2;
                if (hdfReader_->readImageByIndex(imgPath2, static_cast<size_t>(navState->currentIndex), original2)) {
                    pf.originalImage = original2;
                }
                if (!maskPath2.empty() && hdfReader_->readImageByIndex(maskPath2, static_cast<size_t>(navState->currentIndex), mask2)) {
                    pf.processedImage = mask2;
                }
            }
            loadSeriesImages(pf, navState->currentIndex);
            dialog->setFrame(pf);
            // Update selected frame in main view
            setSelectedFrame(navState->currentIndex);
        }
    });

    connect(dialog, &FrameViewerDialog::requestNextFrame, this, [this, dialog, navState, loadSeriesImages]() {
        const auto& frames = navState->isValidSet ? validFrames_ : invalidFrames_;
        if (frames.empty()) return;
        navState->currentIndex = navState->currentIndex + 1;
        if (navState->currentIndex >= static_cast<int>(frames.size())) {
            navState->currentIndex = 0; // Wrap to first
        }
        if (navState->currentIndex >= 0 && navState->currentIndex < static_cast<int>(frames.size())) {
            backend::services::ProcessedFrame pf = frames[navState->currentIndex];
            const std::string imgPath2 = imagesPath(navState->isValidSet);
            const std::string maskPath2 = masksPath(navState->isValidSet);
            if (hdfReader_) {
                cv::Mat original2, mask2;
                if (hdfReader_->readImageByIndex(imgPath2, static_cast<size_t>(navState->currentIndex), original2)) {
                    pf.originalImage = original2;
                }
                if (!maskPath2.empty() && hdfReader_->readImageByIndex(maskPath2, static_cast<size_t>(navState->currentIndex), mask2)) {
                    pf.processedImage = mask2;
                }
            }
            loadSeriesImages(pf, navState->currentIndex);
            dialog->setFrame(pf);
            // Update selected frame in main view
            setSelectedFrame(navState->currentIndex);
        }
    });
    
    // Show dialog
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->exec();
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
    generateScatterPlot(validFrames);
    generateHistogram(validFrames);
    auto toBgr = [](const QPixmap& pixmap) {
        cv::Mat bgr;
        if (pixmap.isNull()) return bgr;
        QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
        cv::Mat rgba(image.height(), image.width(), CV_8UC4, const_cast<uchar*>(image.constBits()),
                     static_cast<size_t>(image.bytesPerLine()));
        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR); // deep copy; independent of the QImage
        return bgr;
    };
    snapshots["scatter_plot.tiff"] = toBgr(chartToPixmap(scatterPlotView_));
    snapshots["ring_width_histogram.tiff"] = toBgr(chartToPixmap(histogramView_));
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
    }
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

    QDir dir(dirPath);
    bool success = true;
    
    // Generate charts from current data
    generateScatterPlot(validFrames_);
    generateHistogram(validFrames_);
    
    // Export scatter plot
    QString scatterPath = dir.filePath("scatter_plot.tiff");
    QPixmap scatterPixmap = chartToPixmap(scatterPlotView_);
    if (!scatterPixmap.isNull()) {
        QImage scatterImage = scatterPixmap.toImage();
        // Convert to RGB32 format for consistent handling
        scatterImage = scatterImage.convertToFormat(QImage::Format_RGB32);
        cv::Mat scatterMat(scatterImage.height(), scatterImage.width(), CV_8UC4, 
                          const_cast<uchar*>(scatterImage.constBits()), 
                          scatterImage.bytesPerLine());
        cv::Mat scatterBGR;
        cv::cvtColor(scatterMat, scatterBGR, cv::COLOR_RGBA2BGR);
        if (!cv::imwrite(scatterPath.toStdString(), scatterBGR)) {
            SPDLOG_WARN("Failed to write scatter plot TIFF: {}", scatterPath.toStdString());
            success = false;
        } else {
            SPDLOG_INFO("Exported scatter plot {}x{} to {}", scatterBGR.cols, scatterBGR.rows, scatterPath.toStdString());
        }
    } else {
        success = false;
    }
    
    // Export histogram
    QString histogramPath = dir.filePath("ring_width_histogram.tiff");
    QPixmap histogramPixmap = chartToPixmap(histogramView_);
    if (!histogramPixmap.isNull()) {
        QImage histogramImage = histogramPixmap.toImage();
        // Convert to RGB32 format for consistent handling
        histogramImage = histogramImage.convertToFormat(QImage::Format_RGB32);
        cv::Mat histogramMat(histogramImage.height(), histogramImage.width(), CV_8UC4, 
                            const_cast<uchar*>(histogramImage.constBits()), 
                            histogramImage.bytesPerLine());
        cv::Mat histogramBGR;
        cv::cvtColor(histogramMat, histogramBGR, cv::COLOR_RGBA2BGR);
        if (!cv::imwrite(histogramPath.toStdString(), histogramBGR)) {
            SPDLOG_WARN("Failed to write histogram TIFF: {}", histogramPath.toStdString());
            success = false;
        } else {
            SPDLOG_INFO("Exported histogram {}x{} to {}", histogramBGR.cols, histogramBGR.rows, histogramPath.toStdString());
        }
    } else {
        success = false;
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

void HdfReviewTab::generateScatterPlot(const std::vector<backend::services::ProcessedFrame>& validFrames) {
    if (!scatterSeries_ || !scatterXAxis_ || !scatterYAxis_) {
        return;
    }

    scatterSeries_->clear();

    if (validFrames.empty()) {
        scatterXAxis_->setRange(0, 1000);
        scatterYAxis_->setRange(0, 1);
        return;
    }

    // Get conversion factor from backend (pixels to microns)
    const double conversionFactor = backend_.processing().getPixelToMicronFactor();
    // Area conversion: pixels² to microns² = pixels² * (microns/pixel)²
    const double areaConversionFactor = conversionFactor * conversionFactor;

    // Collect points
    std::vector<std::pair<double, double>> points;
    double minArea = std::numeric_limits<double>::max();
    double maxArea = std::numeric_limits<double>::lowest();
    double minDeform = std::numeric_limits<double>::max();
    double maxDeform = std::numeric_limits<double>::lowest();

    for (const auto& frame : validFrames) {
        if (frame.validation.isValid) {
            // Convert area from pixels² to microns²
            double areaPixels = frame.validation.area;
            double areaMicrons = areaPixels * areaConversionFactor;
            double deform = frame.validation.deformability;
            points.push_back({areaMicrons, deform});

            minArea = std::min(minArea, areaMicrons);
            maxArea = std::max(maxArea, areaMicrons);
            minDeform = std::min(minDeform, deform);
            maxDeform = std::max(maxDeform, deform);
        }
    }

    if (points.empty()) {
        scatterXAxis_->setRange(0, 1000);
        scatterYAxis_->setRange(0, 1);
        return;
    }

    // Add scatter points
    for (const auto& p : points) {
        scatterSeries_->append(p.first, p.second);
    }

    // Set axis ranges with padding
    if (minArea < maxArea) {
        double areaPadding = (maxArea - minArea) * 0.1;
        scatterXAxis_->setRange(minArea - areaPadding, maxArea + areaPadding);
    } else {
        scatterXAxis_->setRange(0, 1000);
    }

    if (minDeform < maxDeform) {
        double deformPadding = (maxDeform - minDeform) * 0.1;
        scatterYAxis_->setRange(minDeform - deformPadding, maxDeform + deformPadding);
    } else {
        scatterYAxis_->setRange(0, 1);
    }
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
    
    SPDLOG_INFO("Loaded {} isoelastic curves from {}", curvesByModulus.size(), filePath.toStdString());
}

} // namespace frontend

// Include moc file for ThumbnailLabel class (defined in this .cpp file with Q_OBJECT)
#include "HdfReviewTab.moc"
