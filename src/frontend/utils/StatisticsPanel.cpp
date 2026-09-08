#include "frontend/utils/StatisticsPanel.h"

#include <QLabel>
#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QFrame>

namespace frontend
{

    namespace {
        QString formatRuntime(double seconds)
        {
            if (seconds <= 0.0)
            {
                return "00:00:00";
            }
            
            int totalSeconds = static_cast<int>(seconds);
            int hours = totalSeconds / 3600;
            int minutes = (totalSeconds % 3600) / 60;
            int secs = totalSeconds % 60;
            
            return QString("%1:%2:%3")
                .arg(hours, 2, 10, QChar('0'))
                .arg(minutes, 2, 10, QChar('0'))
                .arg(secs, 2, 10, QChar('0'));
        }
    }

    StatisticsPanel::StatisticsPanel(QWidget* parent)
        : QWidget(parent)
        , displayFpsLabel_(nullptr)
        , displayFpsValue_(nullptr)
        , algoTimeLabel_(nullptr)
        , algoTimeValue_(nullptr)
        , validFpsLabel_(nullptr)
        , validFpsValue_(nullptr)
        , invalidFpsLabel_(nullptr)
        , invalidFpsValue_(nullptr)
        , flushedLabel_(nullptr)
        , flushedValue_(nullptr)
        , cameraStatusLabel_(nullptr)
        , cameraStatusValue_(nullptr)
        , cameraFpsLabel_(nullptr)
        , cameraFpsValue_(nullptr)
        , cameraDataRateLabel_(nullptr)
        , cameraDataRateValue_(nullptr)
        , ringwidthLabel_(nullptr)
        , ringwidthValue_(nullptr)
        , experimentStatusLabel_(nullptr)
        , experimentStatusValue_(nullptr)
        , validBufferedLabel_(nullptr)
        , validBufferedValue_(nullptr)
        , invalidBufferedLabel_(nullptr)
        , invalidBufferedValue_(nullptr)
        , flushStatusLabel_(nullptr)
        , flushStatusValue_(nullptr)
        , validImagesSavedLabel_(nullptr)
        , validImagesSavedValue_(nullptr)
        , experimentRuntimeLabel_(nullptr)
        , experimentRuntimeValue_(nullptr)
    {
        setupUI();
    }

    StatisticsPanel::~StatisticsPanel() = default;

    void StatisticsPanel::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(6, 6, 6, 6);
        mainLayout->setSpacing(8);

        // Display metrics group
        auto* displayGroup = new QGroupBox(tr("Display"), this);
        auto* displayLayout = new QFormLayout(displayGroup);
        displayFpsLabel_ = new QLabel(tr("FPS:"), displayGroup);
        displayFpsValue_ = new QLabel("0.0", displayGroup);
        displayFpsValue_->setStyleSheet("font-weight: bold;");
        displayLayout->addRow(displayFpsLabel_, displayFpsValue_);
        mainLayout->addWidget(displayGroup);

        // Processing metrics group
        auto* processingGroup = new QGroupBox(tr("Processing"), this);
        auto* processingLayout = new QFormLayout(processingGroup);
        algoTimeLabel_ = new QLabel(tr("Algo Time:"), processingGroup);
        algoTimeValue_ = new QLabel("0.0 us", processingGroup);
        algoTimeValue_->setStyleSheet("font-weight: bold;");
        processingLayout->addRow(algoTimeLabel_, algoTimeValue_);
        validFpsLabel_ = new QLabel(tr("Valid FPS:"), processingGroup);
        validFpsValue_ = new QLabel("0.0", processingGroup);
        validFpsValue_->setStyleSheet("font-weight: bold;");
        processingLayout->addRow(validFpsLabel_, validFpsValue_);
        invalidFpsLabel_ = new QLabel(tr("Invalid FPS:"), processingGroup);
        invalidFpsValue_ = new QLabel("0.0", processingGroup);
        invalidFpsValue_->setStyleSheet("font-weight: bold;");
        processingLayout->addRow(invalidFpsLabel_, invalidFpsValue_);
        flushedLabel_ = new QLabel(tr("Flushed:"), processingGroup);
        flushedValue_ = new QLabel("0", processingGroup);
        flushedValue_->setStyleSheet("font-weight: bold;");
        processingLayout->addRow(flushedLabel_, flushedValue_);
        mainLayout->addWidget(processingGroup);

        // Camera metrics group
        auto* cameraGroup = new QGroupBox(tr("Camera"), this);
        auto* cameraLayout = new QFormLayout(cameraGroup);
        cameraStatusLabel_ = new QLabel(tr("Status:"), cameraGroup);
        cameraStatusValue_ = new QLabel(tr("Stopped"), cameraGroup);
        cameraStatusValue_->setStyleSheet("font-weight: bold;");
        cameraLayout->addRow(cameraStatusLabel_, cameraStatusValue_);
        cameraFpsLabel_ = new QLabel(tr("FPS:"), cameraGroup);
        cameraFpsValue_ = new QLabel("0.0", cameraGroup);
        cameraFpsValue_->setStyleSheet("font-weight: bold;");
        cameraLayout->addRow(cameraFpsLabel_, cameraFpsValue_);
        cameraDataRateLabel_ = new QLabel(tr("Data Rate:"), cameraGroup);
        cameraDataRateValue_ = new QLabel("0.0 MB/s", cameraGroup);
        cameraDataRateValue_->setStyleSheet("font-weight: bold;");
        cameraLayout->addRow(cameraDataRateLabel_, cameraDataRateValue_);
        mainLayout->addWidget(cameraGroup);

        // Autofocus metrics group
        auto* autofocusGroup = new QGroupBox(tr("Autofocus"), this);
        auto* autofocusLayout = new QFormLayout(autofocusGroup);
        ringwidthLabel_ = new QLabel(tr("Ring width:"), autofocusGroup);
        ringwidthValue_ = new QLabel("0.000", autofocusGroup);
        ringwidthValue_->setStyleSheet("font-weight: bold;");
        autofocusLayout->addRow(ringwidthLabel_, ringwidthValue_);
        mainLayout->addWidget(autofocusGroup);

        // Experiment metrics group
        auto* experimentGroup = new QGroupBox(tr("Experiment"), this);
        auto* experimentLayout = new QFormLayout(experimentGroup);
        experimentStatusLabel_ = new QLabel(tr("Status:"), experimentGroup);
        experimentStatusValue_ = new QLabel(tr("Inactive"), experimentGroup);
        experimentStatusValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(experimentStatusLabel_, experimentStatusValue_);
        validBufferedLabel_ = new QLabel(tr("Valid Buffered:"), experimentGroup);
        validBufferedValue_ = new QLabel("0", experimentGroup);
        validBufferedValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(validBufferedLabel_, validBufferedValue_);
        invalidBufferedLabel_ = new QLabel(tr("Invalid Buffered:"), experimentGroup);
        invalidBufferedValue_ = new QLabel("0", experimentGroup);
        invalidBufferedValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(invalidBufferedLabel_, invalidBufferedValue_);
        flushStatusLabel_ = new QLabel(tr("Flush Status:"), experimentGroup);
        flushStatusValue_ = new QLabel(tr("Idle"), experimentGroup);
        flushStatusValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(flushStatusLabel_, flushStatusValue_);
        validImagesSavedLabel_ = new QLabel(tr("Valid Images Saved:"), experimentGroup);
        validImagesSavedValue_ = new QLabel("0", experimentGroup);
        validImagesSavedValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(validImagesSavedLabel_, validImagesSavedValue_);
        experimentRuntimeLabel_ = new QLabel(tr("Runtime:"), experimentGroup);
        experimentRuntimeValue_ = new QLabel("00:00:00", experimentGroup);
        experimentRuntimeValue_->setStyleSheet("font-weight: bold;");
        experimentLayout->addRow(experimentRuntimeLabel_, experimentRuntimeValue_);
        mainLayout->addWidget(experimentGroup);

        mainLayout->addStretch();
    }

    void StatisticsPanel::updateStatistics(const StatisticsData& data)
    {
        // Display metrics
        displayFpsValue_->setText(QString::number(data.displayFps, 'f', 1));

        // Processing metrics
        algoTimeValue_->setText(QString::number(data.algoAvgUs, 'f', 1) + " us");
        if (data.algoAvgUsAgeMs > 1000.0) {
            algoTimeValue_->setStyleSheet("font-weight: bold; color: gray;");
            algoTimeValue_->setToolTip(tr("Stale: last update %1 ms ago").arg(static_cast<int>(data.algoAvgUsAgeMs)));
        } else {
            algoTimeValue_->setStyleSheet("font-weight: bold;");
            algoTimeValue_->setToolTip(QString());
        }
        validFpsValue_->setText(QString::number(data.validFps, 'f', 1));
        invalidFpsValue_->setText(QString::number(data.invalidFps, 'f', 1));
        flushedValue_->setText(QString::number(static_cast<qulonglong>(data.totalValidFlushed)));

        // Camera metrics
        if (data.cameraRunning)
        {
            cameraStatusValue_->setText(tr("Running"));
            cameraStatusValue_->setStyleSheet("font-weight: bold; color: green;");
            cameraFpsValue_->setText(data.cameraFpsText.isEmpty()
                                         ? QString::number(data.cameraFps, 'f', 1)
                                         : data.cameraFpsText);
            cameraDataRateValue_->setText((data.cameraDataRateText.isEmpty()
                                               ? QString::number(data.cameraDataRateMBps, 'f', 1)
                                               : data.cameraDataRateText) + " MB/s");
        }
        else
        {
            cameraStatusValue_->setText(tr("Stopped"));
            cameraStatusValue_->setStyleSheet("font-weight: bold; color: gray;");
            cameraFpsValue_->setText("0.0");
            cameraDataRateValue_->setText("0.0 MB/s");
        }

        // Autofocus metrics
        ringwidthValue_->setText(QString::number(data.meanRingRatio, 'f', 3));
        if (data.meanRingRatioAgeMs > 1000.0) {
            ringwidthValue_->setStyleSheet("font-weight: bold; color: gray;");
            ringwidthValue_->setToolTip(tr("Stale: last update %1 ms ago").arg(static_cast<int>(data.meanRingRatioAgeMs)));
        } else {
            ringwidthValue_->setStyleSheet("font-weight: bold;");
            ringwidthValue_->setToolTip(QString());
        }

        // Experiment metrics
        if (data.experimentActive)
        {
            experimentStatusValue_->setText(tr("Active"));
            experimentStatusValue_->setStyleSheet("font-weight: bold; color: green;");
            validBufferedValue_->setText(QString::number(data.validBuffered));
            invalidBufferedValue_->setText(QString::number(data.invalidBuffered));
            if (data.flushInProgress)
            {
                flushStatusValue_->setText(tr("Flushing..."));
                flushStatusValue_->setStyleSheet("font-weight: bold; color: orange;");
            }
            else
            {
                flushStatusValue_->setText(tr("Idle"));
                flushStatusValue_->setStyleSheet("font-weight: bold; color: gray;");
            }
            validImagesSavedValue_->setText(QString::number(static_cast<qulonglong>(data.totalValidFlushed)));
            experimentRuntimeValue_->setText(formatRuntime(data.experimentRuntimeSeconds));
        }
        else
        {
            experimentStatusValue_->setText(tr("Inactive"));
            experimentStatusValue_->setStyleSheet("font-weight: bold; color: gray;");
            validBufferedValue_->setText("0");
            invalidBufferedValue_->setText("0");
            flushStatusValue_->setText(tr("Idle"));
            flushStatusValue_->setStyleSheet("font-weight: bold; color: gray;");
            validImagesSavedValue_->setText("0");
            experimentRuntimeValue_->setText("00:00:00");
        }
    }

} // namespace frontend
