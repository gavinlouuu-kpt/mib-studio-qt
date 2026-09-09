#pragma once

#include <QWidget>
#include <cstdint>
#include <cstddef>

namespace backend { class AppBackend; }
class QTabWidget;
class QLabel;

namespace frontend
{

    struct StatisticsData
    {
        double displayFps = 0.0;
        double algoAvgUs = 0.0;
        double validFps = 0.0;
        double invalidFps = 0.0;
        uint64_t totalValidFlushed = 0;
        bool cameraRunning = false;
        double cameraFps = 0.0;
        double cameraDataRateMBps = 0.0;
        // Truthful renderings (issue #368): empty = fall back to the numeric
        // fields; otherwise shown verbatim ("n/a", "unsupported", "12 (stale 4.0 s)").
        QString cameraFpsText;
        QString cameraDataRateText;
        double meanRingRatio = 0.0;
        bool experimentActive = false;
        size_t validBuffered = 0;
        size_t invalidBuffered = 0;
        bool flushInProgress = false;
        double experimentRuntimeSeconds = 0.0;
        // Age in ms since last backend update (for stale indicator); 0 = just updated, large = stale
        double algoAvgUsAgeMs = 0.0;
        double meanRingRatioAgeMs = 0.0;
    };

    class StatisticsPanel : public QWidget
    {
        Q_OBJECT
    public:
        explicit StatisticsPanel(QWidget* parent = nullptr);
        ~StatisticsPanel();

        void updateStatistics(const StatisticsData& data);

    private:
        void setupUI();

        // Display metrics
        QLabel* displayFpsLabel_;
        QLabel* displayFpsValue_;

        // Processing metrics
        QLabel* algoTimeLabel_;
        QLabel* algoTimeValue_;
        QLabel* validFpsLabel_;
        QLabel* validFpsValue_;
        QLabel* invalidFpsLabel_;
        QLabel* invalidFpsValue_;
        QLabel* flushedLabel_;
        QLabel* flushedValue_;

        // Camera metrics
        QLabel* cameraStatusLabel_;
        QLabel* cameraStatusValue_;
        QLabel* cameraFpsLabel_;
        QLabel* cameraFpsValue_;
        QLabel* cameraDataRateLabel_;
        QLabel* cameraDataRateValue_;

        // Autofocus metrics
        QLabel* ringwidthLabel_;
        QLabel* ringwidthValue_;

        // Experiment metrics
        QLabel* experimentStatusLabel_;
        QLabel* experimentStatusValue_;
        QLabel* validBufferedLabel_;
        QLabel* validBufferedValue_;
        QLabel* invalidBufferedLabel_;
        QLabel* invalidBufferedValue_;
        QLabel* flushStatusLabel_;
        QLabel* flushStatusValue_;
        QLabel* validImagesSavedLabel_;
        QLabel* validImagesSavedValue_;
        QLabel* experimentRuntimeLabel_;
        QLabel* experimentRuntimeValue_;
    };

} // namespace frontend
