#include "frontend/utils/StatsDisplayManager.h"

#include <QString>
#include <QTabWidget>
#include <spdlog/spdlog.h>

#include <algorithm>

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/playback/PlaybackService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/TriggerService.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/app/Tools.h"
#include "frontend/tabs/PreviewPage.h"
#include "frontend/system/PlaybackPanel.h"

namespace frontend
{

    StatsDisplayManager::StatsDisplayManager(backend::AppBackend &backend, QObject *parent)
        : QObject(parent), backend_(backend)
    {
    }

    QString StatsDisplayManager::collectAndFormatStats(bool experimentActive, QTabWidget *experimentTabs, bool flushInProgress)
    {
        const auto &cap = backend_.capture();
        const auto &s = cap.stats();
        auto &proc = backend_.processing();
        const uint64_t tFetchStartUs = backend::Tools::getTimestamp();
        const auto bufferedFrames = proc.getBufferedFrameCounts();
        const uint64_t tFetchEndUs = backend::Tools::getTimestamp();
        lastFetchTimeMs_ = static_cast<double>(tFetchEndUs - tFetchStartUs) / 1000.0;

        QString status;
        // Display / algo / classification rates and totals
        double displayFps = 0.0;
        if (experimentTabs && experimentTabs->count() > 0) {
            auto* previewPage = qobject_cast<PreviewPage*>(experimentTabs->widget(0));
            if (previewPage) {
                auto* playbackPanel = previewPage->getPlaybackPanel();
                if (playbackPanel) {
                    displayFps = playbackPanel->getDisplayFps();
                }
            }
        }
        const double algoAvgUs = proc.getAlgoAvgUs1s();
        const double validFps = proc.getValidFps1s();
        const double invalidFps = proc.getInvalidFps1s();
        const uint64_t totalValidFlushed = proc.getTotalValidFlushed();

        status = QString("Display=%1 fps | Algo=%2 us | Valid=%3/s | Invalid=%4/s | Flushed(valid)=%5")
                     .arg(QString::number(displayFps, 'f', 1))
                     .arg(QString::number(algoAvgUs, 'f', 1))
                     .arg(QString::number(validFps, 'f', 1))
                     .arg(QString::number(invalidFps, 'f', 1))
                     .arg(QString::number(static_cast<qulonglong>(totalValidFlushed)));

        // Camera transport stats (issue #368: unknown is never shown as 0)
        if (cap.isRunning()) {
            const auto telemetry = cap.telemetrySnapshot();
            status += QString(" | Camera=%1 fps, %2 MB/s")
                          .arg(formatMetric(telemetry.captureFrameRate))
                          .arg(formatMetric(telemetry.captureDataRateMBps));
        } else {
            status += " | Camera: stopped";
        }

        // Append live ring width (median from AutofocusService, same value used by autofocus)
        {
            const double ringWidth = backend_.autofocus().getMedianRingRatio();
            status += QString(" | Ring width=%1").arg(QString::number(ringWidth, 'f', 3));
        }

        // Target-identification loss + sort latency. Conversion is the fraction
        // of valid detections that entered the sort target group; served is the
        // fraction of those that actually got a pulse (the rest were extra
        // targets in a frame, evicted requests, or undriven pulses). Latency is
        // the live acquisition->pulse EWMA, visible without MIB_PIPELINE_TIMING.
        {
            const auto ident = proc.getIdentificationCounters();
            const auto& trig = backend_.trigger();
            auto& timing = backend::diagnostics::PipelineTimingRecorder::instance();

            const uint64_t targets = ident.targetGroupObjects;
            const uint64_t lostSort = ident.unservedTargetGroupObjects +
                                      trig.getDroppedRequestCount() + trig.getDroppedPulseCount();
            const double conversionPct =
                ident.validObjects > 0
                    ? 100.0 * static_cast<double>(targets) / static_cast<double>(ident.validObjects)
                    : 0.0;
            const double servedPct =
                targets > 0
                    ? 100.0 * static_cast<double>(targets - std::min(lostSort, targets)) /
                          static_cast<double>(targets)
                    : 100.0;

            status += QString(" | Targets=%1 (%2% of valid), served=%3%, lost=%4")
                          .arg(QString::number(static_cast<qulonglong>(targets)))
                          .arg(QString::number(conversionPct, 'f', 1))
                          .arg(QString::number(servedPct, 'f', 1))
                          .arg(QString::number(static_cast<qulonglong>(lostSort)));
            status += QString(" | SortLat=%1 us (max %2)")
                          .arg(QString::number(timing.avgTargetLatencyUs(), 'f', 0))
                          .arg(QString::number(static_cast<qulonglong>(timing.maxTargetLatencyUs())));
            if (proc.getDroppedValidFrames() > 0) {
                status += QString(" | DroppedValid=%1")
                              .arg(QString::number(
                                  static_cast<qulonglong>(proc.getDroppedValidFrames())));
            }
        }

        if (experimentActive)
        {
            size_t totalBuffered = bufferedFrames.total();

            status += QString(" | Experiment: active | buffered: valid=%1, invalid=%2")
                          .arg(bufferedFrames.valid)
                          .arg(bufferedFrames.invalid);
            if (flushInProgress)
            {
                status += " (flushing...)";
            }

            // Throttled diagnostic log (~1 Hz)
            static uint64_t lastDiagLogUs = 0;
            const uint64_t nowUs = backend::Tools::getTimestamp();
            if (nowUs - lastDiagLogUs >= 1'000'000ULL) {
                uint64_t earliest = 0, latest = 0;
                size_t count = 0;
                backend_.playback().queryRange(earliest, latest, count);
                const double memMB = backend::Tools::getProcessMemoryMB();
                size_t flushNeeded = proc.getFlushInterval();
                SPDLOG_DEBUG("MainWindow stats: buffer_fetch_ms={:.3f}, valid={}, invalid={}, total={}, playback_range=[{},{}] count={}, flush_interval={}, max_buffered={}, flushing={}, mem_mb={:.1f}",
                             lastFetchTimeMs_, bufferedFrames.valid, bufferedFrames.invalid, totalBuffered, earliest, latest, count, flushNeeded, proc.getMaxBufferedFrames(), flushInProgress ? 1 : 0, memMB);
                lastDiagLogUs = nowUs;
            }
        }
        else
        {
            status += " | Experiment: inactive";
        }

        return status;
    }

QString StatsDisplayManager::formatMetric(const backend::services::MetricSample& m)
{
    using backend::services::MetricValidity;
    switch (m.validity) {
    case MetricValidity::Valid: return QString::number(static_cast<qulonglong>(m.value));
    case MetricValidity::Stale:
        return QStringLiteral("%1 (stale %2 s)")
            .arg(static_cast<qulonglong>(m.value))
            .arg(QString::number(static_cast<double>(m.ageUs) / 1e6, 'f', 1));
    case MetricValidity::Unavailable: return QStringLiteral("n/a");
    case MetricValidity::Unsupported: return QStringLiteral("unsupported");
    case MetricValidity::Error: return QStringLiteral("error");
    }
    return QStringLiteral("?");
}

} // namespace frontend
