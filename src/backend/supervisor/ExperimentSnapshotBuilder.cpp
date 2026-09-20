#include "backend/supervisor/ExperimentSnapshotBuilder.h"

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/app/Tools.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"

#include <algorithm>
#include <chrono>

namespace backend::supervisor {

namespace {

Metric fromSample(const services::MetricSample& s)
{
    return s.hasValue() ? Metric::of(static_cast<double>(s.value)) : Metric::unknown();
}

} // namespace

ExperimentSnapshot buildExperimentSnapshot(AppBackend& backend, const SnapshotBuildContext& context)
{
    using services::ProcessingService;
    ExperimentSnapshot s;
    s.sequence = context.sequence;
    s.hostTimeUs = Tools::getTimestamp();
    s.wallClockNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch()).count());
    s.objective = context.objective;
    s.targetValidObjects = context.targetValidObjects;
    s.priorRecommendations = context.priorRecommendations;

    // ---- experiment lifecycle (authoritative: ExperimentCoordinator) ------
    auto& coordinator = backend.experiment();
    const app::ExperimentStatus status = coordinator.status();
    s.experimentState = app::toString(status.state);
    s.startGeneration = status.startGeneration;
    s.runId = !status.outputPath.empty()
                  ? status.outputPath
                  : ("generation-" + std::to_string(status.startGeneration));
    if (status.startWallClockNs > 0 && status.state == app::ExperimentRunState::Active) {
        s.elapsedSeconds = static_cast<double>(s.wallClockNs - std::min(s.wallClockNs, status.startWallClockNs)) / 1e9;
    }
    s.recording.state = s.experimentState;
    s.recording.outputPath = status.outputPath;
    s.recording.storageReady = status.state == app::ExperimentRunState::Active && status.faultCode.empty();
    s.recording.persistenceAdmitted = Metric::of(static_cast<double>(status.persistenceAdmitted));
    s.recording.persistenceCommitted = Metric::of(static_cast<double>(status.persistenceCommitted));
    s.recording.persistenceFailed = Metric::of(static_cast<double>(status.persistenceFailed));
    s.recording.validBuffered = Metric::of(static_cast<double>(status.validBuffered));
    s.recording.invalidBuffered = Metric::of(static_cast<double>(status.invalidBuffered));
    s.recording.faultCode = status.faultCode;
    s.recording.faultMessage = status.faultMessage;

    // ---- configuration (frozen run snapshot when active, else live) ---------
    const auto run = coordinator.activeRun();
    const app::CameraSourceInfo camera = backend.cameraSourceInfo();
    s.configuration.cameraSource = camera.effective;
    s.configuration.simulated = camera.simulated;
    if (run) {
        s.configuration.deliveryMode = run->deliveryModeActive;
        s.configuration.roiWidth = run->roiW;
        s.configuration.roiHeight = run->roiH;
        s.configuration.processingCoreVersion = run->processingCore.version;
        s.configuration.configSha256 = run->processingConfigSha256;
        s.configuration.triggerEnabled = run->triggerRequired;
    }
    // Exposure / requested FPS are camera-profile values the backend does not
    // expose as live metrics today; they stay unknown rather than guessed.
    s.configuration.requestedFps = Metric::unknown();
    s.configuration.exposureUs = Metric::unknown();

    // ---- acquisition (CaptureService telemetry, per-metric validity) ---------
    auto& capture = backend.capture();
    const services::CaptureLifecycleSnapshot lifecycle = capture.lifecycleSnapshot();
    s.acquisition.captureState = services::toString(lifecycle.state);
    s.acquisition.cameraReady = lifecycle.cameraReady;
    s.acquisition.lastFailure = lifecycle.isActive() ? "none" : services::toString(lifecycle.lastFailure);
    if (lifecycle.isActive() && lifecycle.lastFailure != services::CaptureFailureKind::None &&
        lifecycle.lastFailureGeneration == lifecycle.generation) {
        s.acquisition.lastFailure = services::toString(lifecycle.lastFailure);
    }
    const services::AcquisitionTelemetrySnapshot telemetry = capture.telemetrySnapshot();
    s.acquisition.measuredFps = fromSample(telemetry.captureFrameRate);
    s.acquisition.framesDelivered = fromSample(telemetry.framesDelivered);
    s.acquisition.transportLostFrames = fromSample(telemetry.transportLostFrames);
    s.acquisition.intentionallyDiscardedFrames = fromSample(telemetry.intentionallyDiscardedFrames);
    s.acquisition.bufferUnderruns = fromSample(telemetry.bufferUnderruns);
    s.acquisition.sdkQueueDepth = fromSample(telemetry.sdkCompletedQueueDepth);
    s.acquisition.sdkInputBuffers = fromSample(telemetry.sdkInputBufferCount);
    s.acquisition.frameAgeUs = fromSample(telemetry.frameAgeUs);
    s.acquisition.publishLatencyUs = fromSample(telemetry.publishLatencyUs);
    if (!s.configuration.deliveryMode.empty()) {
        // keep the frozen value
    } else {
        s.configuration.deliveryMode = telemetry.sessionActive ? "active" : "";
    }

    // ---- detection (ProcessingService counters + monitoring window) ----------
    auto& processing = backend.processing();
    const services::ProcessingConfig cfg = processing.getProcessingConfig();
    s.configuration.detectionThreshold = cfg.bg_subtract_threshold;
    s.configuration.minAreaUm2 = cfg.area_threshold_min;
    s.configuration.maxAreaUm2 = cfg.area_threshold_max;
    s.configuration.areaRangeCheck = cfg.enable_area_range_check;
    if (!run) s.configuration.triggerEnabled = cfg.enable_target_group;
    if (s.configuration.roiWidth == 0) {
        const auto roi = processing.getRealtimeRoi();
        s.configuration.roiWidth = roi.w;
        s.configuration.roiHeight = roi.h;
    }
    const auto buffered = processing.getBufferedFrameCounts();
    const double validTotal = static_cast<double>(processing.getTotalValidFlushed() + buffered.valid);
    const double invalidTotal = static_cast<double>(processing.getTotalInvalidFlushed() + buffered.invalid);
    const double droppedValid = static_cast<double>(processing.getDroppedValidFrames());
    const double droppedInvalid = static_cast<double>(processing.getDroppedInvalidFrames());
    s.detection.validObjects = Metric::of(validTotal);
    s.detection.invalidObjects = Metric::of(invalidTotal);
    s.detection.droppedValid = Metric::of(droppedValid);
    s.detection.droppedInvalid = Metric::of(droppedInvalid);
    s.detection.processingFailures = Metric::of(static_cast<double>(processing.getProcessingFailureCount()));
    // Frames available to the realtime loop: what capture delivered to the
    // FrameStore (the loop consumes or explicitly drops each one). Object
    // counts above are kept separate from frame counts on purpose.
    s.detection.framesProcessed = s.acquisition.framesDelivered;
    s.detection.algoFps = Metric::of(processing.getAlgoFps1s());
    s.detection.validPerSecond = Metric::of(processing.getValidFps1s());
    s.detection.invalidPerSecond = Metric::of(processing.getInvalidFps1s());
    s.detection.processingTimeUs = Metric::of(processing.getAlgoAvgUs1s());
    s.detection.laplacianVarianceMean = Metric::unknown(); // not produced by the core

    if (processing.isMonitoringActive() && context.statisticsWindow > 0) {
        const auto frames = processing.getMonitoringValidFrames();
        const size_t n = std::min(frames.size(), context.statisticsWindow);
        if (n > 0) {
            double areaSum = 0.0, areaMin = 1e300, areaMax = -1e300, contrastSum = 0.0, q2Sum = 0.0, q4Sum = 0.0;
            for (size_t i = frames.size() - n; i < frames.size(); ++i) {
                const auto& v = frames[i].validation;
                areaSum += v.area;
                areaMin = std::min(areaMin, v.area);
                areaMax = std::max(areaMax, v.area);
                contrastSum += v.brightness.q3 - v.brightness.q1;
                q2Sum += v.brightness.q2;
                q4Sum += v.brightness.q4;
            }
            const double dn = static_cast<double>(n);
            s.detection.windowObjects = n;
            s.detection.areaMeanUm2 = Metric::of(areaSum / dn);
            s.detection.areaMinUm2 = Metric::of(areaMin);
            s.detection.areaMaxUm2 = Metric::of(areaMax);
            s.detection.contrastMean = Metric::of(contrastSum / dn);
            s.detection.brightnessMedianMean = Metric::of(q2Sum / dn);
            s.detection.brightnessMaxMean = Metric::of(q4Sum / dn);
        }
        const auto invalid = processing.getMonitoringInvalidFrames();
        uint64_t border = 0, area = 0, ring = 0, inner = 0, other = 0;
        for (const auto& f : invalid) {
            const auto& v = f.validation;
            if (v.touchesBorder) ++border;
            else if (cfg.enable_area_range_check && !v.inRange) ++area;
            else if (cfg.require_single_inner_contour && !v.hasSingleInnerContour) ++inner;
            else if (cfg.enable_ring_ratio_check && (v.ringRatio < cfg.ring_ratio_min || v.ringRatio > cfg.ring_ratio_max)) ++ring;
            else ++other;
        }
        if (!invalid.empty()) {
            s.detection.rejectionReasons = {"border", "area", "inner_contour", "ring_ratio", "other"};
            s.detection.rejectionCounts = {border, area, inner, ring, other};
        }
    }

    // ---- trigger (TriggerService counters) --------------------------------------
    auto& trigger = backend.trigger();
    s.trigger.cameraBound = trigger.hasCamera();
    const double issued = static_cast<double>(trigger.getTriggerCount());
    const double suppressed = static_cast<double>(trigger.getDroppedRequestCount());
    const double droppedPulses = static_cast<double>(trigger.getDroppedPulseCount());
    const double stale = static_cast<double>(trigger.getDroppedStaleRequestCount());
    // Every request the trigger service saw is an eligible object; pulses
    // actually driven are issued minus post-dequeue losses.
    s.trigger.eligibleObjects = Metric::of(issued + suppressed + stale);
    s.trigger.triggersIssued = Metric::of(std::max(0.0, issued - droppedPulses));
    s.trigger.suppressedRequests = Metric::of(suppressed);
    s.trigger.droppedPulses = Metric::of(droppedPulses);
    s.trigger.staleRequests = Metric::of(stale);
    s.trigger.lastOnsetUs = issued > 0 ? Metric::of(trigger.getLastOnsetUs()) : Metric::unknown();

    return s;
}

} // namespace backend::supervisor
