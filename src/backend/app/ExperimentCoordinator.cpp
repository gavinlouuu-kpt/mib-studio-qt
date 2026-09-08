#include "backend/app/ExperimentCoordinator.h"

#include "backend/app/AppBackend.h"
#include "backend/app/Tools.h"
#include "backend/camera/common/TimestampValue.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace backend::app {

namespace {

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string sha256Of(const std::string& bytes)
{
    if (bytes.empty()) return {};
    return backend::processing::processingCoreBytesSha256(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

std::string canonicalProcessingConfig(const backend::services::ProcessingConfig& c)
{
    std::ostringstream o;
    o.precision(17);
    o << "gaussian_blur_size=" << c.gaussian_blur_size << ";bg_subtract_threshold=" << c.bg_subtract_threshold
      << ";morph_kernel_size=" << c.morph_kernel_size << ";morph_iterations=" << c.morph_iterations
      << ";area_threshold_min=" << c.area_threshold_min << ";area_threshold_max=" << c.area_threshold_max
      << ";deformability_threshold_min=" << c.deformability_threshold_min
      << ";deformability_threshold_max=" << c.deformability_threshold_max
      << ";enable_border_check=" << c.enable_border_check
      << ";enable_area_range_check=" << c.enable_area_range_check
      << ";enable_deformability_range_check=" << c.enable_deformability_range_check
      << ";area_ratio_threshold_max=" << c.area_ratio_threshold_max
      << ";enable_area_ratio_check=" << c.enable_area_ratio_check
      << ";ring_ratio_min=" << c.ring_ratio_min << ";ring_ratio_max=" << c.ring_ratio_max
      << ";enable_ring_ratio_check=" << c.enable_ring_ratio_check
      << ";require_single_inner_contour=" << c.require_single_inner_contour
      << ";empty_frame_pixel_threshold=" << c.empty_frame_pixel_threshold
      << ";auto_background_enabled=" << c.auto_background_enabled
      << ";auto_background_empty_frames=" << c.auto_background_empty_frames
      << ";auto_background_cooldown_frames=" << c.auto_background_cooldown_frames
      << ";enable_target_group=" << c.enable_target_group
      << ";target_group_area_min=" << c.target_group_area_min
      << ";target_group_area_max=" << c.target_group_area_max
      << ";target_group_deformability_min=" << c.target_group_deformability_min
      << ";target_group_deformability_max=" << c.target_group_deformability_max
      << ";enable_target_group_emodulus=" << c.enable_target_group_emodulus
      << ";target_group_emodulus_min=" << c.target_group_emodulus_min
      << ";target_group_emodulus_max=" << c.target_group_emodulus_max
      << ";multi_image_enabled=" << c.multi_image_enabled
      << ";multi_image_count=" << c.multi_image_count;
    return o.str();
}

ReadinessGate gate(const char* id, GateStatus status, std::string reason = {},
                   std::string remediation = {}, std::string detail = {})
{
    ReadinessGate g;
    g.id = id;
    g.status = status;
    g.reason = std::move(reason);
    g.remediation = std::move(remediation);
    g.detail = std::move(detail);
    return g;
}

// Probe that the destination directory exists and is writable without
// touching the destination file itself.
bool outputWritable(const std::string& path, std::string& reason)
{
    if (path.empty()) {
        reason = "no output path selected";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path p(path);
    const auto dir = p.has_parent_path() ? p.parent_path() : std::filesystem::current_path(ec);
    if (!std::filesystem::exists(dir, ec)) {
        // Hdf5Service::openFile creates parents; verify the nearest existing
        // ancestor is a writable directory.
        auto probe = dir;
        while (!probe.empty() && !std::filesystem::exists(probe, ec)) probe = probe.parent_path();
        if (probe.empty() || !std::filesystem::is_directory(probe, ec)) {
            reason = "output directory cannot be created: " + dir.string();
            return false;
        }
    } else if (!std::filesystem::is_directory(dir, ec)) {
        reason = "output parent is not a directory: " + dir.string();
        return false;
    }
    if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
        reason = "output path is a directory";
        return false;
    }
    // Writability probe: create and remove a temp file next to the target.
    auto probeDir = std::filesystem::exists(dir, ec) ? dir : dir;
    while (!probeDir.empty() && !std::filesystem::exists(probeDir, ec)) probeDir = probeDir.parent_path();
    const auto probeFile = probeDir / (".mib_write_probe_" + std::to_string(Tools::getTimestamp()));
    {
        std::ofstream f(probeFile, std::ios::binary);
        if (!f.good()) {
            reason = "output directory is not writable: " + probeDir.string();
            return false;
        }
    }
    std::filesystem::remove(probeFile, ec);
    const auto space = std::filesystem::space(probeDir, ec);
    if (!ec && space.available < (64ULL << 20)) {
        reason = "less than 64 MiB free at " + probeDir.string();
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON serializers
// ---------------------------------------------------------------------------

std::string runSnapshotToJson(const RunConfigurationSnapshot& s)
{
    std::ostringstream o;
    o.precision(17);
    o << "{"
      << "\"schema_version\":" << RunConfigurationSnapshot::kSchemaVersion
      << ",\"readiness_generation\":" << s.readinessGeneration
      << ",\"start_generation\":" << s.startGeneration
      << ",\"capture_generation\":" << s.captureGeneration
      << ",\"start_host_time_us\":" << s.startHostTimeUs
      << ",\"start_wall_clock_ns\":" << s.startWallClockNs
      << ",\"camera\":{\"requested\":" << q(s.camera.requested)
      << ",\"effective\":" << q(s.camera.effective) << ",\"label\":" << q(s.camera.label)
      << ",\"simulated\":" << (s.camera.simulated ? "true" : "false")
      << ",\"fallback\":" << (s.camera.fallback ? "true" : "false")
      << ",\"fallback_reason\":" << q(s.camera.fallbackReason) << "}"
      << ",\"camera_ready\":" << (s.cameraReady ? "true" : "false")
      << ",\"delivery_mode_requested\":" << q(s.deliveryModeRequested)
      << ",\"delivery_mode_active\":" << q(s.deliveryModeActive)
      << ",\"delivery_mode_confirmed\":" << (s.deliveryModeConfirmed ? "true" : "false")
      << ",\"timestamp_descriptor\":" << q(s.timestampDescriptor)
      << ",\"roi\":{\"x\":" << s.roiX << ",\"y\":" << s.roiY << ",\"w\":" << s.roiW << ",\"h\":" << s.roiH << "}"
      << ",\"frame\":{\"width\":" << s.frameWidth << ",\"height\":" << s.frameHeight
      << ",\"pixel_format\":" << s.pixelFormat << ",\"known\":" << (s.frameGeometryKnown ? "true" : "false") << "}"
      << ",\"processing_core\":{\"version\":" << q(s.processingCore.version)
      << ",\"contract_version\":" << s.processingCore.contractVersion
      << ",\"engine_abi_version\":" << s.processingCore.engineAbiVersion
      << ",\"sha256\":" << q(s.processingCore.artifactSha256)
      << ",\"source\":" << q(s.processingCore.source)
      << ",\"pin_satisfied\":" << (s.processingCorePinSatisfied ? "true" : "false") << "}"
      << ",\"processing_config_version\":" << s.processingConfigVersion
      << ",\"processing_config_sha256\":" << q(s.processingConfigSha256)
      << ",\"config_json_sha256\":" << q(s.configJsonSha256)
      << ",\"profile_id\":" << q(s.profileId)
      << ",\"pixel_to_micron\":" << s.pixelToMicron
      << ",\"background\":{\"present\":" << (s.backgroundPresent ? "true" : "false")
      << ",\"generation\":" << s.backgroundGeneration << ",\"sha256\":" << q(s.backgroundSha256) << "}"
      << ",\"trigger\":{\"required\":" << (s.triggerRequired ? "true" : "false")
      << ",\"bound\":" << (s.triggerBound ? "true" : "false") << ",\"generation\":" << s.triggerGeneration << "}"
      << ",\"output_path\":" << q(s.outputPath)
      << ",\"realtime_mode\":" << q(s.realtimeMode)
      << ",\"application\":{\"version\":" << q(s.applicationVersion) << ",\"build_id\":" << q(s.buildId)
      << ",\"os\":" << q(s.operatingSystem) << "}"
      << "}";
    return o.str();
}

std::string readinessToJson(const ExperimentReadinessSnapshot& r)
{
    std::ostringstream o;
    o << "{\"generation\":" << r.generation << ",\"evaluated_host_time_us\":" << r.evaluatedHostTimeUs
      << ",\"ready\":" << (r.ready ? "true" : "false") << ",\"gates\":[";
    bool first = true;
    for (const auto& g : r.gates) {
        if (!first) o << ",";
        first = false;
        o << "{\"id\":" << q(g.id) << ",\"status\":" << q(toString(g.status)) << ",\"reason\":" << q(g.reason)
          << ",\"remediation\":" << q(g.remediation) << ",\"detail\":" << q(g.detail) << "}";
    }
    o << "]}";
    return o.str();
}

// ---------------------------------------------------------------------------

bool ExperimentCoordinator::InvalidationKey::operator==(const InvalidationKey& o) const
{
    return captureGeneration == o.captureGeneration && cameraReady == o.cameraReady &&
           cameraSource == o.cameraSource && cameraFallback == o.cameraFallback &&
           deliveryMode == o.deliveryMode && processingConfigVersion == o.processingConfigVersion &&
           configJsonSha256 == o.configJsonSha256 && coreVersion == o.coreVersion &&
           coreSha256 == o.coreSha256 && corePinSatisfied == o.corePinSatisfied &&
           backgroundGeneration == o.backgroundGeneration && roiX == o.roiX && roiY == o.roiY &&
           roiW == o.roiW && roiH == o.roiH && pixelToMicron == o.pixelToMicron &&
           outputPath == o.outputPath && profileId == o.profileId && faulted == o.faulted;
}

ExperimentCoordinator::ExperimentCoordinator(AppBackend& backend) : backend_(backend) {}

void ExperimentCoordinator::setApplicationIdentity(std::string version, std::string buildId, std::string os)
{
    std::lock_guard<std::mutex> lk(mutex_);
    appVersion_ = std::move(version);
    buildId_ = std::move(buildId);
    os_ = std::move(os);
}

void ExperimentCoordinator::reportUnresolvedFault(const std::string& code, const std::string& message)
{
    std::lock_guard<std::mutex> lk(mutex_);
    faultActive_ = true;
    faultCode_ = code;
    faultMessage_ = message;
}

void ExperimentCoordinator::clearUnresolvedFault()
{
    std::lock_guard<std::mutex> lk(mutex_);
    faultActive_ = false;
    faultCode_.clear();
    faultMessage_.clear();
}

bool ExperimentCoordinator::hasUnresolvedFault() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return faultActive_;
}

ExperimentRunState ExperimentCoordinator::state() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return state_;
}

std::optional<RunConfigurationSnapshot> ExperimentCoordinator::activeRun() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return activeRun_;
}

RunConfigurationSnapshot ExperimentCoordinator::candidateLocked(const std::string& outputPath,
                                                                const std::string& profileId) const
{
    RunConfigurationSnapshot s;
    auto& cap = backend_.capture();
    auto& proc = backend_.processing();
    const auto lifecycle = cap.lifecycleSnapshot();
    s.captureGeneration = lifecycle.generation;
    s.cameraReady = lifecycle.cameraReady && lifecycle.state == services::CaptureLifecycleState::Running;
    s.camera = backend_.cameraSourceInfo();
    s.deliveryModeRequested = ::camera::common::toString(cap.stats().deliveryModeConfirmed.load()
                                                             ? cap.activeDeliveryMode()
                                                             : cap.activeDeliveryMode());
    s.deliveryModeActive = ::camera::common::toString(cap.activeDeliveryMode());
    s.deliveryModeConfirmed = cap.stats().deliveryModeConfirmed.load(std::memory_order_acquire);
    s.timestampDescriptor = ::camera::common::describe(cap.timestampDescriptor());

    const auto roi = proc.getRealtimeRoi();
    s.roiX = roi.x; s.roiY = roi.y; s.roiW = roi.w; s.roiH = roi.h;
    if (auto store = backend_.getFrameStore()) {
        playback::Frame f;
        if (store->getLatest(f)) {
            s.frameWidth = f.width;
            s.frameHeight = f.height;
            s.pixelFormat = f.pixelFormat;
            s.frameGeometryKnown = true;
        }
    }

    s.processingCore = proc.activeProcessingCoreIdentity();
    s.processingCorePinSatisfied = proc.isProcessingCorePinSatisfied();
    s.processingConfigVersion = proc.getConfigVersion();
    s.processingConfigSha256 = sha256Of(canonicalProcessingConfig(proc.getProcessingConfig()));
    s.configJsonSha256 = sha256Of(backend_.getLastConfigJson());
    s.profileId = profileId;
    s.pixelToMicron = proc.getPixelToMicronFactor();

    const auto bg = proc.getRealtimeBackgroundGrayShared();
    s.backgroundPresent = bg && !bg->empty();
    s.backgroundGeneration = proc.backgroundGeneration();
    s.backgroundSha256 = proc.backgroundSha256();

    s.triggerRequired = proc.getProcessingConfig().enable_target_group;
    s.triggerGeneration = backend_.trigger().boundGeneration();
    s.triggerBound = s.triggerGeneration != 0 && s.triggerGeneration == lifecycle.generation;

    s.outputPath = outputPath;
    s.realtimeMode = proc.getRealtimeProcessingMode() ==
                             services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                         ? "async_batch"
                         : "inline";
    s.applicationVersion = appVersion_;
    s.buildId = buildId_;
    s.operatingSystem = os_;
    return s;
}

ExperimentCoordinator::InvalidationKey
ExperimentCoordinator::currentKeyLocked(const std::string& outputPath, const std::string& profileId) const
{
    const auto c = candidateLocked(outputPath, profileId);
    InvalidationKey k;
    k.captureGeneration = c.captureGeneration;
    k.cameraReady = c.cameraReady;
    k.cameraSource = c.camera.effective;
    k.cameraFallback = c.camera.fallback;
    k.deliveryMode = c.deliveryModeActive;
    k.processingConfigVersion = c.processingConfigVersion;
    k.configJsonSha256 = c.configJsonSha256;
    k.coreVersion = c.processingCore.version;
    k.coreSha256 = c.processingCore.artifactSha256;
    k.corePinSatisfied = c.processingCorePinSatisfied;
    k.backgroundGeneration = c.backgroundGeneration;
    k.roiX = c.roiX; k.roiY = c.roiY; k.roiW = c.roiW; k.roiH = c.roiH;
    k.pixelToMicron = c.pixelToMicron;
    k.outputPath = outputPath;
    k.profileId = profileId;
    k.faulted = faultActive_;
    return k;
}

ExperimentReadinessSnapshot ExperimentCoordinator::evaluateLocked(const std::string& outputPath,
                                                                  const std::string& profileId)
{
    ExperimentReadinessSnapshot r;
    r.evaluatedHostTimeUs = Tools::getTimestamp();
    r.candidate = candidateLocked(outputPath, profileId);
    const auto& c = r.candidate;
    const auto key = currentKeyLocked(outputPath, profileId);
    if (!haveLastKey_ || key != lastKey_) {
        readinessGeneration_.fetch_add(1);
        lastKey_ = key;
        haveLastKey_ = true;
    }
    r.generation = readinessGeneration_.load();
    r.candidate.readinessGeneration = r.generation;

    // --- camera session / hardware-vs-mock --------------------------------
    const auto lifecycle = backend_.capture().lifecycleSnapshot();
    if (c.cameraReady) {
        r.gates.push_back(gate("camera.session", GateStatus::Pass, {}, {},
                               "generation " + std::to_string(c.captureGeneration)));
    } else {
        std::string reason = std::string("camera is ") + services::toString(lifecycle.state);
        if (lifecycle.lastFailure != services::CaptureFailureKind::None &&
            lifecycle.lastFailureGeneration == lifecycle.generation) {
            reason += ": " + lifecycle.lastFailureMessage;
        }
        r.gates.push_back(gate("camera.session", GateStatus::Fail, reason,
                               "Start Live View and wait until the camera reports Running"));
    }
    if (c.camera.fallback) {
        r.gates.push_back(gate("camera.source", GateStatus::Fail,
                               "requested " + c.camera.requested + " camera is unavailable; " +
                                   c.camera.fallbackReason,
                               "select the mock camera explicitly, or install/connect the hardware",
                               "effective=" + c.camera.effective));
    } else if (c.camera.simulated) {
        r.gates.push_back(gate("camera.source", GateStatus::Warn, "simulated (mock) camera selected",
                               "expected for development/tests; not a hardware run",
                               "effective=mock label=" + c.camera.label));
    } else if (c.camera.effective == "unknown") {
        r.gates.push_back(gate("camera.source", GateStatus::Unavailable, "no camera source configured",
                               "connect a camera or configure the mock camera"));
    } else {
        r.gates.push_back(gate("camera.source", GateStatus::Pass, {}, {},
                               c.camera.effective + " " + c.camera.label));
    }
    if (!c.cameraReady) {
        r.gates.push_back(gate("camera.deliveryMode", GateStatus::Unavailable,
                               "delivery mode is confirmed only by a running camera"));
    } else if (!c.deliveryModeConfirmed) {
        r.gates.push_back(gate("camera.deliveryMode", GateStatus::Unavailable,
                               "backend has not confirmed the delivery mode"));
    } else if (c.deliveryModeActive == "latestFrame") {
        r.gates.push_back(gate("camera.deliveryMode", GateStatus::Warn,
                               "Latest Frame intentionally discards frames; the recording may be incomplete",
                               "switch to Every Frame or acknowledge the policy at Start",
                               c.deliveryModeActive));
    } else {
        r.gates.push_back(gate("camera.deliveryMode", GateStatus::Pass, {}, {}, c.deliveryModeActive));
    }
    if (c.frameGeometryKnown) {
        r.gates.push_back(gate("camera.geometry", GateStatus::Pass, {}, {},
                               std::to_string(c.frameWidth) + "x" + std::to_string(c.frameHeight) +
                                   " pf=0x" + [&] { char b[20]; std::snprintf(b, sizeof(b), "%llx", (unsigned long long)c.pixelFormat); return std::string(b); }()));
    } else {
        r.gates.push_back(gate("camera.geometry", GateStatus::Unavailable, "no frame has been received yet",
                               "wait for the first frame"));
    }
    if (c.roiW > 0 && c.roiH > 0) {
        r.gates.push_back(gate("processing.roi", GateStatus::Pass, {}, {},
                               std::to_string(c.roiW) + "x" + std::to_string(c.roiH) + "@" +
                                   std::to_string(c.roiX) + "," + std::to_string(c.roiY)));
    } else {
        r.gates.push_back(gate("processing.roi", GateStatus::Warn, "no ROI set; full frame is processed"));
    }

    // --- processing core / config / calibration ---------------------------
    if (c.processingCorePinSatisfied) {
        r.gates.push_back(gate("processing.core", GateStatus::Pass, {}, {},
                               c.processingCore.version + " contract " +
                                   std::to_string(c.processingCore.contractVersion)));
    } else {
        r.gates.push_back(gate("processing.core", GateStatus::Fail,
                               "required processing core " +
                                   backend_.processing().requiredProcessingCoreVersion() + " is not active",
                               "activate the pinned core in Settings > Processing Core"));
    }
    r.gates.push_back(gate("processing.config", GateStatus::Pass, {}, {},
                           "version " + std::to_string(c.processingConfigVersion) + " sha " +
                               c.processingConfigSha256.substr(0, 12)));
    if (c.pixelToMicron > 0.0) {
        r.gates.push_back(gate("calibration.pixelToMicron", GateStatus::Pass, {}, {},
                               std::to_string(c.pixelToMicron)));
    } else {
        r.gates.push_back(gate("calibration.pixelToMicron", GateStatus::Fail,
                               "pixel-to-micron factor is not positive",
                               "set the conversion factor in Settings"));
    }
    if (c.backgroundPresent) {
        r.gates.push_back(gate("processing.background", GateStatus::Pass, {}, {},
                               "generation " + std::to_string(c.backgroundGeneration) + " sha " +
                                   c.backgroundSha256.substr(0, 12)));
    } else {
        r.gates.push_back(gate("processing.background", GateStatus::Warn,
                               "no background image set; detection uses frame thresholds only",
                               "capture a background (Set Background / calibration)"));
    }

    // --- trigger / strobe --------------------------------------------------
    if (!c.triggerRequired) {
        r.gates.push_back(gate("trigger.output", GateStatus::NotRequired, "target-group sorting disabled"));
    } else if (c.triggerBound) {
        r.gates.push_back(gate("trigger.output", GateStatus::Pass, {}, {},
                               "bound to session " + std::to_string(c.triggerGeneration)));
    } else {
        r.gates.push_back(gate("trigger.output", GateStatus::Fail,
                               "sorting is enabled but the trigger service is not bound to the running camera",
                               "restart the camera; check the trigger wiring"));
    }

    // --- output / storage --------------------------------------------------
    if (outputPath.empty()) {
        r.gates.push_back(gate("storage.output", GateStatus::Unavailable, "no output path chosen yet",
                               "choose an HDF5 destination"));
    } else {
        std::string why;
        if (outputWritable(outputPath, why)) {
            r.gates.push_back(gate("storage.output", GateStatus::Pass, {}, {}, outputPath));
        } else {
            r.gates.push_back(gate("storage.output", GateStatus::Fail, why,
                                   "choose a writable destination with free space", outputPath));
        }
    }
    if (backend_.hdf5().isFileOpen()) {
        r.gates.push_back(gate("storage.hdf5", GateStatus::Fail, "an HDF5 file is already open",
                               "finish or close the current file first"));
    } else {
        r.gates.push_back(gate("storage.hdf5", GateStatus::Pass));
    }
    if (backend_.isFrameRecording()) {
        r.gates.push_back(gate("lifecycle.recording", GateStatus::Fail, "raw frame recording is active",
                               "stop recording first"));
    } else {
        r.gates.push_back(gate("lifecycle.recording", GateStatus::Pass));
    }
    if (state_ != ExperimentRunState::Idle) {
        r.gates.push_back(gate("lifecycle.experiment", GateStatus::Fail,
                               std::string("experiment is ") + toString(state_)));
    } else {
        r.gates.push_back(gate("lifecycle.experiment", GateStatus::Pass));
    }
    if (faultActive_) {
        r.gates.push_back(gate("lifecycle.fault", GateStatus::Fail, faultMessage_,
                               "resolve/acknowledge the fault before starting", faultCode_));
    } else {
        r.gates.push_back(gate("lifecycle.fault", GateStatus::Pass));
    }

    // --- telemetry capability relevant to a hard gate -----------------------
    {
        const auto t = backend_.capture().telemetrySnapshot();
        if (!c.cameraReady) {
            r.gates.push_back(gate("telemetry.transportLoss", GateStatus::Unavailable,
                                   "no active session"));
        } else if (t.transportLostFrames.validity == services::MetricValidity::Unsupported) {
            r.gates.push_back(gate("telemetry.transportLoss", GateStatus::Warn,
                                   "this camera backend cannot report transport loss",
                                   "loss will be unobservable in provenance", "unsupported"));
        } else if (t.transportLostFrames.validity != services::MetricValidity::Valid) {
            r.gates.push_back(gate("telemetry.transportLoss", GateStatus::Warn,
                                   "transport loss has not been reported for this session",
                                   "loss may be unobservable in provenance",
                                   std::string(services::toString(t.transportLostFrames.validity))));
        } else {
            r.gates.push_back(gate("telemetry.transportLoss", GateStatus::Pass, {}, {},
                                   "lost=" + std::to_string(t.transportLostFrames.value)));
        }
    }

    r.ready = true;
    for (const auto& g : r.gates) {
        if (g.blocksStart()) r.ready = false;
    }
    return r;
}

ExperimentReadinessSnapshot ExperimentCoordinator::evaluateReadiness(const std::string& outputPath,
                                                                     const std::string& profileId)
{
    std::lock_guard<std::mutex> lk(mutex_);
    return evaluateLocked(outputPath, profileId);
}

ExperimentStartResult ExperimentCoordinator::start(const ExperimentStartRequest& request)
{
    ExperimentStartResult result;
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) {
        result.outcome = ExperimentStartOutcome::Busy;
        result.message = "another experiment transaction is in progress";
        return result;
    }
    if (state_ != ExperimentRunState::Idle) {
        result.outcome = state_ == ExperimentRunState::Active ? ExperimentStartOutcome::AlreadyActive
                                                                : ExperimentStartOutcome::Busy;
        result.message = std::string("experiment is ") + toString(state_);
        return result;
    }

    std::string path = request.outputPath;
    if (path.size() < 3 || (path.substr(path.size() - 3) != ".h5" &&
                            (path.size() < 5 || path.substr(path.size() - 5) != ".hdf5"))) {
        path += ".h5";
    }

    // 1-3. Re-evaluate now and compare with the presented generation. The
    // evaluation bumps the generation iff an invalidation input changed, so a
    // mismatch means the preflight is stale (reconnect, config/background/
    // core/ROI/output change, or new fault).
    result.readiness = evaluateLocked(path, request.profileId);
    if (request.readinessGeneration != result.readiness.generation) {
        result.outcome = ExperimentStartOutcome::StaleReadiness;
        result.message = "readiness generation " + std::to_string(request.readinessGeneration) +
                         " is stale (current " + std::to_string(result.readiness.generation) +
                         "); re-run the preflight";
        SPDLOG_WARN("ExperimentCoordinator: start refused — {}", result.message);
        return result;
    }
    if (!result.readiness.ready) {
        result.outcome = ExperimentStartOutcome::NotReady;
        std::string ids;
        for (const auto& id : result.readiness.blockingGateIds()) ids += (ids.empty() ? "" : ", ") + id;
        result.message = "not ready: " + ids;
        SPDLOG_WARN("ExperimentCoordinator: start refused — {}", result.message);
        return result;
    }
    if (result.readiness.candidate.deliveryModeActive == "latestFrame" &&
        !request.acknowledgeLatestFrameDrops) {
        result.outcome = ExperimentStartOutcome::NotReady;
        result.message = "Latest Frame delivery discards frames; acknowledge the policy or switch to Every Frame";
        return result;
    }

    state_ = ExperimentRunState::Starting;
    status_ = ExperimentStatus{};
    publishLocked(lk, "starting");

    // 4. Freeze the run snapshot from the evaluated candidate.
    RunConfigurationSnapshot run = result.readiness.candidate;
    run.startGeneration = ++startCounter_;
    run.startHostTimeUs = Tools::getTimestamp();
    run.startWallClockNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    run.outputPath = path;

    // 5. Persistence resources.
    auto& hdf5 = backend_.hdf5();
    if (!hdf5.openFile(path)) {
        state_ = ExperimentRunState::Idle;
        result.outcome = ExperimentStartOutcome::StorageFailed;
        result.message = "failed to open HDF5 file: " + path;
        return result;
    }
    if (!hdf5.initializeDatasets()) {
        hdf5.closeFile();
        state_ = ExperimentRunState::Idle;
        result.outcome = ExperimentStartOutcome::StorageFailed;
        result.message = "failed to initialize HDF5 datasets in " + path;
        return result;
    }
    // Provenance first: a run without its frozen snapshot cannot be Complete.
    if (!hdf5.writeRunSnapshotJson(runSnapshotToJson(run), readinessToJson(result.readiness))) {
        hdf5.closeFile();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        state_ = ExperimentRunState::Idle;
        result.outcome = ExperimentStartOutcome::ProvenanceFailed;
        result.message = "failed to persist the run configuration snapshot";
        return result;
    }

    // 6-7. Acquire processing ownership and enter Running.
    auto& proc = backend_.processing();
    proc.setExperimentAccountingContext(run.captureGeneration, run.deliveryModeActive == "latestFrame");
    proc.startExperiment();
    activeRun_ = run;
    state_ = ExperimentRunState::Active;
    result.outcome = ExperimentStartOutcome::Started;
    result.message = "experiment started";
    result.run = run;
    SPDLOG_INFO("ExperimentCoordinator: started run {} (readiness gen {}, capture gen {}, camera {}{}) -> {}",
                run.startGeneration, run.readinessGeneration, run.captureGeneration, run.camera.effective,
                run.camera.simulated ? " [simulated]" : "", path);
    publishLocked(lk, "active");
    return result;
}

void ExperimentCoordinator::setStatusCallback(StatusCallback cb)
{
    std::lock_guard<std::mutex> lk(callbackMutex_);
    statusCallback_ = std::move(cb);
}

ExperimentStatus ExperimentCoordinator::snapshotLocked() const
{
    ExperimentStatus s = status_;
    s.state = state_;
    if (activeRun_) {
        s.startGeneration = activeRun_->startGeneration;
        s.readinessGeneration = activeRun_->readinessGeneration;
        s.captureGeneration = activeRun_->captureGeneration;
        s.outputPath = activeRun_->outputPath;
        s.startWallClockNs = activeRun_->startWallClockNs;
    }
    const auto& proc = backend_.processing();
    const auto counts = proc.getBufferedFrameCounts();
    s.validBuffered = counts.valid;
    s.invalidBuffered = counts.invalid;
    const auto acc = proc.experimentAccountingSnapshot();
    s.persistenceAdmitted = acc.persistenceAdmitted;
    s.persistenceCommitted = acc.persistenceCommitted;
    s.persistenceFailed = acc.persistenceFailed;
    s.faultCode = faultActive_ ? faultCode_ : std::string{};
    s.faultMessage = faultActive_ ? faultMessage_ : std::string{};
    return s;
}

ExperimentStatus ExperimentCoordinator::status() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return snapshotLocked();
}

void ExperimentCoordinator::publishLocked(std::unique_lock<std::mutex>& lk, const char* message)
{
    status_.message = message ? message : "";
    ExperimentStatus s = snapshotLocked();
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> clk(callbackMutex_);
        cb = statusCallback_;
    }
    lk.unlock();
    if (cb) cb(s);
    lk.lock();
}

std::optional<RunConfigurationSnapshot> ExperimentCoordinator::finish()
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto run = activeRun_;
    activeRun_.reset();
    state_ = ExperimentRunState::Idle;
    return run;
}

} // namespace backend::app
