#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/RecordingTarget.h"
#include "backend/app/WallClock.h"
#include "backend/pz/PzInstrumentControl.h"
#include "backend/processing/IExecutionProvider.h"
#include "backend/processing/pz/PzProfileCompiler.h"
#include "backend/app/SciencePlacement.h"

#include "backend/app/AppBackend.h"
#include "backend/app/MethodProvenance.h"
#include "backend/app/Tools.h"
#include "backend/camera/common/TimestampValue.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/processing/ProcessingService.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/services/RfGeneratorService.h"
#include "backend/services/TriggerService.h"

#include <limits>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
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

// Small format/access probe, NOT a sustained-throughput certification. Cached
// by readiness generation so a UI timer does not continuously write files.
bool probeHdf5Destination(const std::string& output, std::string& reason) {
    auto dir = std::filesystem::path(output).parent_path();
    if (dir.empty()) dir = std::filesystem::current_path();
    std::error_code ec;
    while (!dir.empty() && !std::filesystem::exists(dir, ec))
        dir = dir.parent_path();
    const auto path = dir / (".mib_hdf5_probe_" + std::to_string(Tools::getTimestamp()) + ".h5");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove(path, e);
            std::filesystem::remove(path.string() + ".recovery.h5", e);
        }
    } cleanup{path};
    try {
        services::Hdf5Service hdf;
        std::vector<services::ProcessedFrame> frames(2);
        for (size_t i = 0; i < frames.size(); ++i) {
            auto& f = frames[i];
            f.originalImage = cv::Mat(32, 32, CV_8UC1);
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    f.originalImage.at<uint8_t>(y, x) = static_cast<uint8_t>(x + 7 * y + i);
            f.processedImage = f.originalImage.clone();
        }
        bool ok = hdf.openFile(path.string()) && hdf.appendFrames(frames, {}) && hdf.flush();
        hdf.closeFile();
        std::vector<services::ProcessedFrame> readback;
        ok = ok && hdf.loadFile(path.string()) && hdf.readValidFrames(readback) &&
             readback.size() == frames.size();
        if (ok)
            for (size_t i = 0; i < frames.size(); ++i)
                ok = ok &&
                     cv::norm(frames[i].originalImage, readback[i].originalImage, cv::NORM_INF) ==
                         0 &&
                     cv::norm(frames[i].processedImage, readback[i].processedImage, cv::NORM_INF) ==
                         0;
        hdf.closeFile();
        reason = ok ? "HDF5 write/close/reopen verified; sustained throughput unverified"
                    : "destination HDF5 write/read verification failed";
        return ok;
    } catch (const std::exception& e) {
        reason = std::string("destination HDF5 probe failed: ") + e.what();
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// JSON serializers
// ---------------------------------------------------------------------------

// PL science (YOFO S2): the profile the PL runs, from the current settings (AppBackend::compilePlProfile).
static backend::processing::pz::CompiledProfile compilePlProfile(AppBackend& backend)
{
    return backend.compilePlProfile();
}

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
      << ",\"wall_clock_source\":" << q(s.wallClockSource)
      << ",\"wall_clock_offset_ns\":" << s.wallClockOffsetNs
      << (s.ssdRunId != 0 ? ",\"ssd_run_id\":" + std::to_string(s.ssdRunId) : std::string())
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
      << ",\"method\":" << methodProvenanceToJson(s.method)
      << ",\"pixel_to_micron\":" << s.pixelToMicron
      << ",\"background\":{\"present\":" << (s.backgroundPresent ? "true" : "false")
      << ",\"generation\":" << s.backgroundGeneration << ",\"sha256\":" << q(s.backgroundSha256) << "}"
      << ",\"trigger\":{\"required\":" << (s.triggerRequired ? "true" : "false")
      << ",\"bound\":" << (s.triggerBound ? "true" : "false") << ",\"generation\":" << s.triggerGeneration << "}"
      << ",\"rf_generator\":{\"configured\":" << (s.rfGeneratorConfigured ? "true" : "false")
      << ",\"connected\":" << (s.rfGeneratorConnected ? "true" : "false")
      << ",\"identity\":" << q(s.rfGeneratorIdentity) << ",\"trigger_mode\":" << q(s.rfGeneratorTriggerMode)
      << ",\"trigger_delay_s\":" << s.rfGeneratorTriggerDelayS << ",\"pulse_width_s\":" << s.rfGeneratorPulseWidthS
      << ",\"error\":" << q(s.rfGeneratorError) << ",\"issues\":" << s.rfGeneratorIssues.size() << "}"
      << ",\"output_path\":" << q(s.outputPath)
      << ",\"realtime_mode\":" << q(s.realtimeMode)
      << ",\"science_placement\":" << q(s.sciencePlacement)
      << ",\"execution_provider\":" << q(s.executionProvider)
      << ",\"pl_core\":{\"valid\":" << (s.plCoreValid ? "true" : "false")
      << ",\"abi_version\":" << s.plAbiVersion << ",\"science_profile\":" << s.plScienceProfile
      << ",\"profile_version\":" << s.plProfileVersion << ",\"build_id\":" << q(s.plBuildId)
      << ",\"weights_sha256_prefix\":" << q(s.plWeightsId) << "}"
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
           roiW == o.roiW && roiH == o.roiH && roiPending == o.roiPending && pixelToMicron == o.pixelToMicron &&
           outputPath == o.outputPath && profileId == o.profileId && method == o.method &&
           faulted == o.faulted && frameWidth == o.frameWidth && frameHeight == o.frameHeight &&
           pixelFormat == o.pixelFormat && frameGeometryKnown == o.frameGeometryKnown &&
           bufferBytes == o.bufferBytes && flushInterval == o.flushInterval;
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
    ++faultRevision_;
    faultCode_ = code;
    faultMessage_ = message;
}

void ExperimentCoordinator::clearUnresolvedFault()
{
    std::unique_lock<std::mutex> lk(mutex_);
    faultActive_ = false;
    faultCode_.clear();
    faultMessage_.clear();
    if (state_ == ExperimentRunState::Failed) {
        state_ = ExperimentRunState::Idle;
        publishLocked(lk, "fault cleared");
    }
}

bool ExperimentCoordinator::acknowledgeFault(uint64_t expectedRun, uint64_t expectedFaultRevision,
                                             const std::string& expectedCode,
                                             const std::string& expectedMessage,
                                             std::string& error) {
    std::unique_lock<std::mutex> lk(mutex_);
    if ((state_ != ExperimentRunState::Idle && state_ != ExperimentRunState::Failed) ||
        status_.flushing || activeRun_) {
        error = "Wait for experiment finalization before acknowledging a fault";
        return false;
    }
    if (!faultActive_ || faultRevision_ != expectedFaultRevision ||
        status_.startGeneration != expectedRun || faultCode_ != expectedCode ||
        faultMessage_ != expectedMessage) {
        error = "Fault changed; review the latest status before acknowledging";
        return false;
    }
    faultActive_ = false;
    faultCode_.clear();
    faultMessage_.clear();
    state_ = ExperimentRunState::Idle;
    publishLocked(lk, "Operator acknowledged fault; saved-run outcome is unchanged");
    return true;
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
    s.roiPending = proc.realtimeRoiPending();
    s.roiNotice = proc.pendingRoiNotice();
    s.sciencePlacement = app::sciencePlacement();
    if (auto* provider = backend_.executionProvider()) {
        s.executionProvider = provider->name();
        const auto core = provider->identity();
        s.plCoreValid = core.valid;
        s.plAbiVersion = core.abiVersion;
        s.plScienceProfile = core.scienceProfile;
        s.plProfileVersion = core.profileVersion;
        s.plBuildId = core.buildId;
        s.plWeightsId = core.profileId;
    }
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
    const auto configJson = backend_.getLastConfigJson();
    s.configJsonSha256 = sha256Of(configJson);
    s.profileId = profileId;
    {
        const auto context = backend_.methodContext();
        const auto contextHash = profiles::methodContextHash(context);
        const auto& instrumentName = backend_.instrumentIdentity().name;
        auto& registry = backend_.profileRegistry();
        const auto generation = registry.generation();
        auto& memo = methodMemo_;
        if (!memo.valid || memo.rawConfigSha256 != s.configJsonSha256 ||
            memo.registryGeneration != generation || memo.contextHash != contextHash ||
            memo.instrumentName != instrumentName) {
            const auto canonical =
                configJson.empty() ? std::string{} : profiles::canonicalConfigSha256(configJson);
            memo.method = resolveMethodProvenance(canonical, registry.snapshot(), context,
                                                  instrumentName);
            memo.rawConfigSha256 = s.configJsonSha256;
            memo.registryGeneration = generation;
            memo.contextHash = contextHash;
            memo.instrumentName = instrumentName;
            memo.valid = true;
        }
        s.method = memo.method;
    }
    s.pixelToMicron = proc.getPixelToMicronFactor();

    const auto bg = proc.getRealtimeBackgroundGrayShared();
    s.backgroundPresent = bg && !bg->empty();
    s.backgroundGeneration = proc.backgroundGeneration();
    s.backgroundSha256 = proc.backgroundSha256();

    s.triggerRequired = proc.getProcessingConfig().enable_target_group;
    s.triggerGeneration = backend_.trigger().boundGeneration();
    s.triggerBound = s.triggerGeneration != 0 && s.triggerGeneration == lifecycle.generation;

    // RF sort generator: readback-first. Only consulted when sorting is on
    // and a link is configured; connect attempts back off so a missing
    // instrument does not stall every readiness poll.
    {
        auto& rf = backend_.rfGenerator();
        s.rfGeneratorConfigured = rf.config().enabled;
        if (s.triggerRequired && s.rfGeneratorConfigured) {
            services::RfGeneratorService::State state;
            if (rf.ensureConnected() && rf.readState(state)) {
                s.rfGeneratorConnected = true;
                s.rfGeneratorIdentity = state.identity;
                s.rfGeneratorTriggerMode = state.triggerMode;
                s.rfGeneratorTriggerDelayS = state.triggerDelayS;
                s.rfGeneratorPulseWidthS = state.pulseWidthS;
                for (const auto& issue : services::RfGeneratorService::preflightForSorting(state)) {
                    if (issue.blocking) s.rfGeneratorIssues.push_back(issue.gate + ": " + issue.message);
                }
            } else {
                s.rfGeneratorConnected = false;
                s.rfGeneratorError = rf.lastErrorMessage();
            }
        }
    }

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
    k.roiPending = c.roiPending;
    k.pixelToMicron = c.pixelToMicron;
    k.outputPath = outputPath;
    k.profileId = profileId;
    k.method = methodInvalidationKey(c.method);
    k.faulted = faultActive_;
    k.frameWidth = c.frameWidth;
    k.frameHeight = c.frameHeight;
    k.pixelFormat = c.pixelFormat;
    k.frameGeometryKnown = c.frameGeometryKnown;
    k.bufferBytes = backend_.processing().getMaxBufferedBytes();
    k.flushInterval = backend_.processing().getFlushInterval();
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

    // PZ7035 (#501 P1): a run needs Run mode, where the producer stream is deliberately stopped
    // (the PL takes every frame; previews come from the cell capture). Its gates replace the
    // live-camera ones (session, delivery mode, geometry, overview).
    const bool instrumentModes = backend_.instrumentControlAvailable();
    if (instrumentModes) {
        const auto mode = backend_.instrumentMode();
        if (mode == pz::InstrumentMode::Run) {
            const auto [x, y] = backend_.instrumentRunOffset();
            r.gates.push_back(gate("instrument.mode", GateStatus::Pass, {}, {},
                                   "Run: 512x96 at (" + std::to_string(x) + ", " + std::to_string(y) +
                                       "), 5 kHz, U-Net cell path on"));
        } else {
            r.gates.push_back(gate("instrument.mode", GateStatus::Fail,
                                   std::string("the instrument is in ") +
                                       (mode == pz::InstrumentMode::Align ? "Align" : "no camera mode"),
                                   "Choose the window in Camera & Alignment and switch to Run"));
        }
    } else if (backend_.isCameraOverview()) {
        r.gates.push_back(gate("camera.mode", GateStatus::Fail,
                               "The camera is showing the full sensor overview",
                               "Switch to Experiment to apply the selected camera ROI"));
    }

    // --- camera session / hardware-vs-mock --------------------------------
    const auto lifecycle = backend_.capture().lifecycleSnapshot();
    if (instrumentModes) {
        // live-camera session gates do not apply in Run (see instrument.mode)
    } else if (c.cameraReady) {
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
        r.gates.push_back(gate("camera.source", GateStatus::Warn, "simulated camera source selected",
                               "expected for development/tests; not a hardware run",
                               "effective=" + c.camera.effective + " label=" + c.camera.label));
    } else if (c.camera.effective == "unknown") {
        r.gates.push_back(gate("camera.source", GateStatus::Unavailable, "no camera source configured",
                               "connect a camera or configure the mock camera"));
    } else {
        r.gates.push_back(gate("camera.source", GateStatus::Pass, {}, {},
                               c.camera.effective + " " + c.camera.label));
    }
    if (instrumentModes) {
        // no live-camera delivery in Run
    } else if (!c.cameraReady) {
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
    if (instrumentModes) {
        // the PL frame is the fixed 512x96 U-Net window
    } else if (c.frameGeometryKnown) {
        r.gates.push_back(gate("camera.geometry", GateStatus::Pass, {}, {},
                               std::to_string(c.frameWidth) + "x" + std::to_string(c.frameHeight) +
                                   " pf=0x" + [&] { char b[20]; std::snprintf(b, sizeof(b), "%llx", (unsigned long long)c.pixelFormat); return std::string(b); }()));
    } else {
        r.gates.push_back(gate("camera.geometry", GateStatus::Unavailable, "no frame has been received yet",
                               "wait for the first frame"));
    }
    if (!app::hostProcessingAvailable()) {
        // The PL processes every frame; the host pipeline's gates do not apply. Its
        // results reach the PS through an execution provider (YOFO S1).
        if (auto* provider = backend_.executionProvider()) {
            r.gates.push_back(gate("science.pl", GateStatus::Pass, {}, {},
                                   "results from execution provider '" + provider->name() + "'"));
            // The every-frame ring (#649): a ring that was asked for but does not fit above Linux's RAM refuses the run.
            if (provider->ringFramesWanted() > 0) {
                const std::string why = provider->ringPlacementProblem();
                if (why.empty())
                    r.gates.push_back(gate("ring.placement", GateStatus::Pass, {}, {},
                                           std::to_string(provider->ringFramesWanted()) + " frames"));
                else
                    r.gates.push_back(gate("ring.placement", GateStatus::Fail, why,
                                           "boot the instrument with a smaller mem= (the bundle's bootargs), or run without a ring"));
            }
            // Run stopped to review the frame ring: resume (a new ring) before an experiment.
            if (backend_.runFrozen())
                r.gates.push_back(gate("run.frozen", GateStatus::Fail, "Run is stopped to review the buffered frames",
                                       "resume Run first"));
            // The settings must compile into the PL profile page (S2).
            const auto profile = compilePlProfile(backend_);
            if (profile.ok() && !profile.warnings.empty()) {
                // The profile compiles, but settings the operator changed have no effect on the PL (G7).
                std::string names;
                for (const auto& w : profile.warnings) names += (names.empty() ? "" : ", ") + w;
                r.gates.push_back(gate("processing.profileCompile", GateStatus::Warn,
                                       "these settings are not implemented by the PL and have no effect: " + names,
                                       "reset them to their defaults, or ignore: the run is not affected"));
            } else if (profile.ok()) {
                r.gates.push_back(gate("processing.profileCompile", GateStatus::Pass, {}, {},
                                       "unet_cells_v2" + std::string(profile.table0.empty() ? ", no E-modulus table"
                                                                                           : ", E-modulus table")));
            } else {
                std::string why;
                for (const auto& e : profile.errors) why += (why.empty() ? "" : "; ") + e;
                r.gates.push_back(gate("processing.profileCompile", GateStatus::Fail, why,
                                       "correct the processing settings"));
            }
        } else {
            r.gates.push_back(gate("science.pl", GateStatus::Warn,
                                   "processing runs on the PL; no execution provider brings its results to the PS",
                                   "set MIB_EXECUTION_PROVIDER=pz on the instrument"));
        }
    } else if (c.roiPending) {
        r.gates.push_back(gate("processing.roi", GateStatus::Fail,
                               "the applied ROI is not validated yet; it is applied on the first captured frame",
                               "start live view with realtime processing"));
    } else if (!c.roiNotice.empty()) {
        r.gates.push_back(gate("processing.roi", GateStatus::Warn, c.roiNotice, "set the ROI for this camera window"));
    } else if (c.roiW > 0 && c.roiH > 0) {
        r.gates.push_back(gate("processing.roi", GateStatus::Pass, {}, {},
                               std::to_string(c.roiW) + "x" + std::to_string(c.roiH) + "@" +
                                   std::to_string(c.roiX) + "," + std::to_string(c.roiY)));
    } else {
        r.gates.push_back(gate("processing.roi", GateStatus::Warn, "no ROI set; full frame is processed"));
    }

    // --- processing core / config / calibration ---------------------------
    if (!app::hostProcessingAvailable()) {
        // none of the host pipeline's prerequisites apply
    } else if (c.processingCorePinSatisfied) {
        r.gates.push_back(gate("processing.core", GateStatus::Pass, {}, {},
                               c.processingCore.version + " contract " +
                                   std::to_string(c.processingCore.contractVersion)));
    } else {
        r.gates.push_back(gate("processing.core", GateStatus::Fail,
                               "required processing core " +
                                   backend_.processing().requiredProcessingCoreVersion() + " is not active",
                               "activate the pinned core in Settings > Processing Core"));
    }
    if (app::hostProcessingAvailable())
        r.gates.push_back(gate("processing.config", GateStatus::Pass, {}, {},
                               "version " + std::to_string(c.processingConfigVersion) + " sha " +
                                   c.processingConfigSha256.substr(0, 12)));
    r.gates.push_back(methodRevisionGate(c.method));
    if (c.pixelToMicron > 0.0) {
        r.gates.push_back(gate("calibration.pixelToMicron", GateStatus::Pass, {}, {},
                               std::to_string(c.pixelToMicron)));
    } else {
        r.gates.push_back(gate("calibration.pixelToMicron", GateStatus::Fail,
                               "pixel-to-micron factor is not positive",
                               "set the conversion factor in Settings"));
    }
    if (!app::hostProcessingAvailable()) {
        // backgrounds live in the PL's table bank (B6 profile); checked there
    } else if (c.backgroundPresent) {
        r.gates.push_back(gate("processing.background", GateStatus::Pass, {}, {},
                               "generation " + std::to_string(c.backgroundGeneration) + " sha " +
                                   c.backgroundSha256.substr(0, 12)));
    } else {
        r.gates.push_back(gate("processing.background", GateStatus::Warn,
                               "no background image set; detection uses frame thresholds only",
                               "capture a background (Set Background / calibration)"));
    }

    if (!app::hostProcessingAvailable()) {
        // no host calibration
    } else if (backend_.processing().backgroundCalibrationStatus().state ==
        services::ProcessingService::BackgroundCalibrationState::Running) {
        r.gates.push_back(gate("processing.backgroundCalibration", GateStatus::Fail,
                               "background calibration is still running",
                               "wait for completion or cancel calibration before starting"));
    } else {
        r.gates.push_back(gate("processing.backgroundCalibration", GateStatus::Pass));
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
    // The RF generator behind the trigger line: what it is set to decides
    // whether a pulse becomes a sort burst at all, so a configured link that
    // is down or mis-armed blocks a sorting run.
    if (!c.triggerRequired) {
        r.gates.push_back(gate("rf.generator", GateStatus::NotRequired, "target-group sorting disabled"));
    } else if (!c.rfGeneratorConfigured) {
        r.gates.push_back(gate("rf.generator", GateStatus::NotRequired, "RF generator link disabled or not configured"));
    } else if (!c.rfGeneratorConnected) {
        r.gates.push_back(gate("rf.generator", GateStatus::Fail,
                               "RF generator link is down: " + c.rfGeneratorError,
                               "check the USB/LAN connection and that the instrument is an SSG3000X"));
    } else if (!c.rfGeneratorIssues.empty()) {
        std::string why;
        for (const auto& i : c.rfGeneratorIssues) {
            if (!why.empty()) why += "; ";
            why += i;
        }
        r.gates.push_back(gate("rf.generator", GateStatus::Fail, why,
                               "arm the generator: MOD > PULSE (Pulse State, Pulse Trigger = Ext Trig, Source = Int) and RF ON",
                               c.rfGeneratorIdentity));
    } else {
        std::ostringstream d;
        d.precision(6);
        d << c.rfGeneratorIdentity << " trigger " << c.rfGeneratorTriggerMode << " delay "
          << c.rfGeneratorTriggerDelayS * 1e6 << " us width " << c.rfGeneratorPulseWidthS * 1e6 << " us";
        r.gates.push_back(gate("rf.generator", GateStatus::Pass, {}, {}, d.str()));
    }

    // --- wall clock (G14) --------------------------------------------------
    // A board without an RTC stamps files from the time a client sent; with no client sync the date in the
    // file is whatever the board booted with. Non-blocking: unattended runs still start, and the file says so.
    if (!app::hostProcessingAvailable()) {
        const auto wall = WallClock::status();
        if (wall.synced) {
            r.gates.push_back(gate("clock.wall", GateStatus::Pass, {}, {}, wall.source));
        } else {
            r.gates.push_back(gate("clock.wall", GateStatus::Warn,
                                   "The board has no real-time clock and no client has sent its time: the dates in the saved file will be wrong (marked board_clock_unsynced)",
                                   "keep a browser connected as the controller for a few seconds", wall.source));
        }
    }

    // --- the SATA SSD (#667) -------------------------------------------------
    // With an SSD configured the experiment records to it (S2): the gate blocks until the SSD is READY with no open run (room in the raw area and in the
    // run table are states of pzrec: RAW_FULL and RUN_TABLE_FULL refuse). Without an SSD configured nothing changes: the experiment writes its file as before.
    if (!app::hostProcessingAvailable()) {
        auto& ssd = backend_.ssdStore();
        const auto st = ssd.status();
        if (!ssd.configured()) {
            r.gates.push_back(gate("storage.ssd", GateStatus::NotRequired, st.reason, {}, "no SSD"));
        } else if (!ssd.configProblem().empty()) {
            r.gates.push_back(gate("storage.ssd", GateStatus::Fail, ssd.configProblem(), "fix MIB_SSD_FILTER in the unit", "configuration"));
        } else if (st.state == pz::SsdState::Ready && !st.openRun) {
            r.gates.push_back(gate("storage.ssd", GateStatus::Pass, {}, {}, std::string("READY, ") + std::to_string(st.freeSectors * 512 / 1000000) + " MB free"));
        } else {
            r.gates.push_back(gate("storage.ssd", GateStatus::Fail, st.reason.empty() ? std::string(pz::ssdStateName(st.state)) : st.reason,
                                   "wait for the SSD to be READY, or see the SSD strip", pz::ssdStateName(st.state)));
        }
    }

    // --- output / storage --------------------------------------------------
    if (outputPath.empty()) {
        r.gates.push_back(gate("storage.output", GateStatus::Unavailable, "no output path chosen yet",
                               "choose an HDF5 destination"));
    } else {
        std::string why;
        if (outputWritable(outputPath, why)) {
            r.gates.push_back(gate("storage.output", GateStatus::Pass, {}, {}, outputPath));
            const auto generation = readinessGeneration_.load();
            const auto now = Tools::getTimestamp();
            if (storageProbeGeneration_ != generation || now - storageProbeTimeUs_ > 30'000'000) {
                storageProbeOk_ = probeHdf5Destination(outputPath, storageProbeReason_);
                storageProbeGeneration_ = generation;
                storageProbeTimeUs_ = now;
            }
            r.gates.push_back(
                gate("storage.roundtrip", storageProbeOk_ ? GateStatus::Pass : GateStatus::Fail,
                     storageProbeReason_, storageProbeOk_ ? "" : "choose another destination"));
        } else {
            r.gates.push_back(gate("storage.output", GateStatus::Fail, why,
                                   "choose a writable destination with free space", outputPath));
        }
        // PZ7035 (#501): a RAM-backed destination (today's JTAG RAM root) loses the run at
        // power-off. Non-blocking: the operator sees it at start and copies the data off.
        if (!app::hostProcessingAvailable()) {
            const auto target = app::recordingTarget(outputPath);
            if (target.ram) {
                r.gates.push_back(gate("storage.persistent", GateStatus::Warn, app::recordingTargetWarning(target),
                                       "record to the SATA disk once it is mounted", target.filesystem));
            } else {
                r.gates.push_back(gate("storage.persistent", GateStatus::Pass, {}, {}, target.filesystem));
            }
        }
    }
    if (app::hostProcessingAvailable() && c.frameGeometryKnown) {
        const auto cfg = backend_.processing().getProcessingConfig();
        const uint64_t images = cfg.multi_image_enabled
                                    ? static_cast<uint64_t>(std::max(1, cfg.multi_image_count)) + 1
                                    : 2;
        const uint64_t cap = backend_.processing().getMaxBufferedBytes();
        const uint64_t limit = std::numeric_limits<uint64_t>::max();
        const bool overflow = c.frameHeight == 0 || c.frameWidth == 0 ||
                              c.frameWidth > limit / c.frameHeight ||
                              c.frameWidth * c.frameHeight > limit / images;
        const uint64_t payload = overflow ? limit : c.frameWidth * c.frameHeight * images;
        if (overflow || (cap && payload > cap))
            r.gates.push_back(
                gate("storage.buffer", GateStatus::Fail,
                     "one full-frame image/mask series cannot fit the recording byte budget",
                     "reduce frame size/series length or increase the recording budget"));
        else if (cap && cap / payload < backend_.processing().getFlushInterval())
            r.gates.push_back(gate(
                "storage.buffer", GateStatus::Warn,
                "byte-pressure flushing will run before the configured frame threshold",
                "leave capacity headroom; runtime overflow will fail the experiment explicitly"));
        else
            r.gates.push_back(gate("storage.buffer", GateStatus::Pass));
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
        if (instrumentModes) {
            // Run: the PL records every frame itself; FRAME sequence gaps and the link counters
            // (instrument status) report loss, not the stopped producer stream.
            r.gates.push_back(gate("telemetry.transportLoss", GateStatus::Pass, {}, {}, "PL frame records"));
        } else if (!c.cameraReady) {
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
    // A start during an active run is AlreadyActive however busy the mutex is: answer it before the
    // try-lock, which reports Busy while another thread (status polls, the worker) holds the mutex (#595).
    if (state_.load() == ExperimentRunState::Active) {
        result.outcome = ExperimentStartOutcome::AlreadyActive;
        result.message = std::string("experiment is ") + toString(ExperimentRunState::Active);
        return result;
    }
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) {
        result.outcome = ExperimentStartOutcome::Busy;
        result.message = "another experiment transaction is in progress";
        return result;
    }
    if (state_ != ExperimentRunState::Idle && state_ != ExperimentRunState::Failed) {
        result.outcome = state_ == ExperimentRunState::Active ? ExperimentStartOutcome::AlreadyActive
                                                                : ExperimentStartOutcome::Busy;
        result.message = std::string("experiment is ") + toString(state_);
        return result;
    }
    if (workerExit_) {
        result.outcome = ExperimentStartOutcome::Busy;
        result.message = "experiment coordinator is shut down";
        return result;
    }
    // A stage operation queued before this Start is still running: refuse, never start
    // under a moving stage (#533). Operations queued after are refused by the idle gate.
    if (stageBusyProbe_ && stageBusyProbe_()) {
        result.outcome = ExperimentStartOutcome::Busy;
        result.message = "a Z stage operation is active; stop it or wait for it to finish before starting an experiment";
        SPDLOG_WARN("ExperimentCoordinator: start refused — {}", result.message);
        return result;
    }

    // Writer conflicts must be refused before even changing processing mode.
    // Raw recording acquires its writer under this same coordinator mutex.
    if (backend_.isFrameRecording() || backend_.hdf5().isFileOpen()) {
        result.outcome = ExperimentStartOutcome::NotReady;
        result.message =
            "stop raw recording and close the existing HDF5 file before starting an experiment";
        result.readiness = evaluateLocked(request.outputPath, request.profileId);
        SPDLOG_WARN("ExperimentCoordinator: start refused — {}", result.message);
        return result;
    }

    // Multi-image series capture requires inline realtime processing. Switch
    // before the evaluation so the frozen snapshot records the mode the run
    // actually uses; the finalization restores the previous mode.
    auto& proc = backend_.processing();
    {
        const auto cfg = proc.getProcessingConfig();
        const bool series = cfg.multi_image_enabled && cfg.multi_image_count > 1;
        const bool asyncBatch = proc.getRealtimeProcessingMode() ==
                                services::ProcessingService::RealtimeProcessingMode::AsyncBatch;
        restoreRealtimeMode_ = false;
        if (series && asyncBatch) {
            proc.setRealtimeProcessingMode(services::ProcessingService::RealtimeProcessingMode::Inline);
            restoreRealtimeMode_ = true;
            realtimeModeWasAsyncBatch_ = true;
            SPDLOG_INFO("ExperimentCoordinator: switched realtime mode async_batch -> inline for a multi-image run");
        }
    }
    auto restoreModeOnFailure = [&] {
        if (restoreRealtimeMode_) {
            proc.setRealtimeProcessingMode(services::ProcessingService::RealtimeProcessingMode::AsyncBatch);
            restoreRealtimeMode_ = false;
        }
    };

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
        restoreModeOnFailure();
        return result;
    }
    if (!result.readiness.ready) {
        result.outcome = ExperimentStartOutcome::NotReady;
        std::string ids;
        for (const auto& id : result.readiness.blockingGateIds()) ids += (ids.empty() ? "" : ", ") + id;
        result.message = "not ready: " + ids;
        SPDLOG_WARN("ExperimentCoordinator: start refused — {}", result.message);
        restoreModeOnFailure();
        return result;
    }
    if (result.readiness.candidate.deliveryModeActive == "latestFrame" &&
        !request.acknowledgeLatestFrameDrops) {
        result.outcome = ExperimentStartOutcome::NotReady;
        result.message = "Latest Frame delivery discards frames; acknowledge the policy or switch to Every Frame";
        restoreModeOnFailure();
        return result;
    }

    state_ = ExperimentRunState::Starting;
    status_ = ExperimentStatus{};
    publishLocked(lk, "starting");

    // 4. Freeze the run snapshot from the evaluated candidate.
    RunConfigurationSnapshot run = result.readiness.candidate;
    run.startGeneration = ++startCounter_;
    run.startHostTimeUs = Tools::getTimestamp();
    // Hold the wall clock from the start stamp until the run has stamped and persisted its end (every
    // finalisation path, failed ones included): a resync in between would change the end's offset.
    auto wallHold = WallClock::hold();
    run.startWallClockNs = WallClock::nowNs();
    {
        const auto wall = WallClock::status();
        run.wallClockSource = wall.source;
        run.wallClockOffsetNs = wall.offsetNs;
    }
    run.outputPath = path;

    // 4b. The SSD run (#667 S2): ask pzrec for the id it will give the run, before anything is opened. The id goes into the snapshot, into RUN_ID of the
    // bridge before ARM (the stored records carry it) and is checked against the run pzrec opens after ARM. A refusal names the SSD state.
    pz::SsdStore* ssdRecord = nullptr;
    if (!app::hostProcessingAvailable() && backend_.executionProvider() && backend_.ssdStore().configured()) {
        ssdRecord = &backend_.ssdStore();
        const auto prep = ssdRecord->prepareRun();      // bounded by pzrec's short timeout; the start already holds the coordinator for the provider's longer one
        if (!prep.ok) {
            state_ = ExperimentRunState::Idle;
            result.outcome = ExperimentStartOutcome::NotReady;
            result.message = "SSD: " + prep.why;
            SPDLOG_ERROR("ExperimentCoordinator: {}", result.message);
            restoreModeOnFailure();
            publishLocked(lk, "start failed");
            return result;
        }
        run.ssdRunId = prep.runId;
    }

    // 5. Persistence resources.
    auto& hdf5 = backend_.hdf5();
    if (!hdf5.openFile(path)) {
        state_ = ExperimentRunState::Idle;
        result.outcome = ExperimentStartOutcome::StorageFailed;
        result.message = "failed to open HDF5 file: " + path;
        restoreModeOnFailure();
        publishLocked(lk, "start failed");
        return result;
    }
    if (!hdf5.initializeDatasets()) {
        hdf5.closeFile();
        state_ = ExperimentRunState::Idle;
        result.outcome = ExperimentStartOutcome::StorageFailed;
        result.message = "failed to initialize HDF5 datasets in " + path;
        restoreModeOnFailure();
        publishLocked(lk, "start failed");
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
        restoreModeOnFailure();
        publishLocked(lk, "start failed");
        return result;
    }

    // 6-7. Acquire processing ownership and enter Active.
    proc.setExperimentAccountingContext(run.captureGeneration, run.deliveryModeActive == "latestFrame");
    // Pulse records from before the run (manual/periodic test pulses, a
    // previous run's tail) are not this experiment's: discard them.
    (void)backend_.trigger().drainEvents();
    proc.startExperiment();
    // The shared lifecycle must own a live consumer. Qt previously started it
    // from a visible tab; a headless/Tauri Start otherwise finalized zero work.
    if (app::hostProcessingAvailable()) {
        proc.setRealtimeEnabled(true);
        proc.startRealtime(backend_.getFrameStore());
    } else if (auto* provider = backend_.executionProvider()) {
        // PL science: arm after the run's accounting started, so every frame the
        // PL reports from here on is admitted once.
        // An SSD run's id is the run-table id (RUN_ID of the stored records = the table's id, pz7035 #49); otherwise the start time as before.
        const uint64_t runId = ssdRecord ? static_cast<uint64_t>(run.ssdRunId)
                                         : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                     std::chrono::system_clock::now().time_since_epoch())
                                                                     .count());
        std::string providerError;
        backend_.stopLiveResults(); // the live session (Run without a file) hands the provider to the run (G5)
        const auto profile = compilePlProfile(backend_);
        bool providerOk = profile.ok() && provider->configure(profile, &providerError);
        if (!profile.ok()) providerError = "the settings do not compile into the PL profile";
        // Order of an SSD run: RUN_ID and the store mode with the drain bit before ARM (the provider's start), then `pzrec start` after the ARM. ARM under an
        // open run would abort it, so the run cannot be opened first.
        provider->setRecordToSsd(ssdRecord != nullptr);
        providerOk = providerOk && provider->start(runId, &providerError);
        provider->setRecordToSsd(false);
        if (providerOk && ssdRecord) {
            pz::SsdStartArgs args;
            args.filter = ssdRecord->filter();           // MIB_SSD_FILTER, default valid: the PC rule (frames with a valid cell, plus the invalid sample)
            // the same rate as the HDF5 path (default 100 = the Tauri path, 1 = every invalid-only frame): the file and the SSD run keep the same frames
            args.samplerN = static_cast<uint32_t>(std::min<size_t>(backend_.processing().getInvalidFrameSamplingRate(), 0xFFFFFFFFu));
            args.clientTag = static_cast<uint32_t>(run.startGeneration);
            args.startUnixMs = run.startWallClockNs / 1'000'000ull;
            args.clientSynced = run.wallClockSource == "client_sync";
            std::string ssdWhy;
            bool opened = false;
            if (!ssdRecord->startRun(run.ssdRunId, args, &ssdWhy, &opened)) {
                providerOk = false;
                providerError = "SSD: " + ssdWhy;
                provider->stop();
                if (opened) {
                    // pzrec opened a run we cannot use: abort it. The wait is short because this start holds the coordinator; the store keeps retrying the abort
                    // in the background and the SSD strip shows STOPPING, then what pzrec says.
                    ssdRecord->beginStop(/*abort=*/true);
                    std::string abortWhy;
                    if (!ssdRecord->waitStopped(std::chrono::seconds(2), &abortWhy)) providerError += "; the aborted run is not confirmed closed (" + abortWhy + ")";
                }
            }
        }
        if (!providerOk) {
            // Roll back as for a provenance failure; the results source is a
            // start prerequisite, so the outcome is NotReady with the reason.
            proc.endExperiment();
            hdf5.closeFile();
            std::error_code ec;
            std::filesystem::remove(path, ec);
            state_ = ExperimentRunState::Idle;
            result.outcome = ExperimentStartOutcome::NotReady;
            result.message = "execution provider '" + provider->name() + "' did not start: " + providerError;
            SPDLOG_ERROR("ExperimentCoordinator: {}", result.message);
            restoreModeOnFailure();
            backend_.resumeLiveResults(); // the refused start leaves live monitoring as it was (G5)
            publishLocked(lk, "start failed");
            return result;
        }
        SPDLOG_INFO("ExperimentCoordinator: PL results from '{}' (run id {})", provider->name(), runId);
    }
    activeRun_ = run;
    wallHold_ = std::move(wallHold); // released at the terminal status of finalizeLocked
    lastRun_ = run;
    liveKdeCoreJson_.clear();
    stopRequested_ = cancelRequested_ = fatalRequested_ = false;
    fatalMessage_.clear();
    if (!worker_.joinable()) {
        workerExit_ = false;
        worker_ = std::thread([this] { worker(); });
    }
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

namespace {
// True on a thread that is currently running a status callback. A callback
// that re-registers must not wait for itself.
thread_local bool tlInStatusCallback = false;
}

void ExperimentCoordinator::setStatusCallback(StatusCallback cb)
{
    std::unique_lock<std::mutex> lk(callbackMutex_);
    statusCallback_ = std::move(cb);
    // After this returns, the previous callback is never running or invoked
    // again (except on the calling thread itself, from inside a callback).
    if (!tlInStatusCallback) callbackIdle_.wait(lk, [this] { return callbacksInFlight_ == 0; });
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
    s.validSaved = proc.getTotalValidFlushed();
    s.invalidSaved = proc.getTotalInvalidFlushed();
    s.droppedValid = proc.getDroppedValidFrames();
    s.droppedInvalid = proc.getDroppedInvalidFrames();
    const auto acc = proc.experimentAccountingSnapshot();
    s.persistenceAdmitted = acc.persistenceAdmitted;
    s.persistenceCommitted = acc.persistenceCommitted;
    s.persistenceFailed = acc.persistenceFailed;
    s.faultRevision = faultRevision_;
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
        if (cb) ++callbacksInFlight_;
    }
    lk.unlock();
    if (cb) {
        struct InFlight {
            ExperimentCoordinator& self;
            ~InFlight() {
                tlInStatusCallback = false;
                {
                    std::lock_guard<std::mutex> clk(self.callbackMutex_);
                    --self.callbacksInFlight_;
                }
                self.callbackIdle_.notify_all();
            }
        } inFlight{*this};
        tlInStatusCallback = true;
        try {
            cb(s);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("ExperimentCoordinator: status observer failed: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("ExperimentCoordinator: status observer threw an unknown exception");
        }
    }
    lk.lock();
}

std::optional<RunConfigurationSnapshot> ExperimentCoordinator::finish()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return activeRun_ ? activeRun_ : lastRun_;
}

ExperimentCoordinator::~ExperimentCoordinator()
{
    shutdown();
}

ExperimentStopOutcome ExperimentCoordinator::requestStop(bool cancelled)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (state_ == ExperimentRunState::Stopping || state_ == ExperimentRunState::Starting)
        return ExperimentStopOutcome::Busy;
    if (state_ != ExperimentRunState::Active || !activeRun_) return ExperimentStopOutcome::NotActive;
    if (stopRequested_) return ExperimentStopOutcome::Busy;
    stopRequested_ = true;
    cancelRequested_ = cancelled;
    workerCv_.notify_all();
    return ExperimentStopOutcome::Accepted;
}

void ExperimentCoordinator::setLiveKdeCoreRecord(std::string json)
{
    std::scoped_lock lk(mutex_);
    if (state_ != ExperimentRunState::Active) return; // no run, or already finalizing
    liveKdeCoreJson_ = std::move(json);
}

void ExperimentCoordinator::requestFlush() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (state_ != ExperimentRunState::Active || workerExit_) return;
    flushRequested_ = true;
    workerCv_.notify_all();
}

void ExperimentCoordinator::onFatalSaveError(const std::string& message)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (state_ != ExperimentRunState::Active || !activeRun_) return;
    SPDLOG_ERROR("ExperimentCoordinator: fatal save error during run {}: {}",
                 activeRun_->startGeneration, message);
    fatalRequested_ = true;
    fatalMessage_ = message;
    stopRequested_ = true;
    workerCv_.notify_all();
}

void ExperimentCoordinator::worker()
{
    using clock = std::chrono::steady_clock;
    // Issue #407 time-based backstop: flush any non-empty buffer after this
    // wall-clock interval regardless of count/byte thresholds, so a slow
    // trickle of large frames never sits unwritten.
    static constexpr auto kTimeFlushInterval = std::chrono::seconds(2);
    auto lastFlushTime = clock::now();

    std::unique_lock<std::mutex> lk(mutex_);
    while (true) {
        workerCv_.wait_for(lk, std::chrono::milliseconds(250),
                           [&] { return stopRequested_ || workerExit_ || flushRequested_; });
        flushRequested_ = false;
        if (stopRequested_ && activeRun_) {
            const bool failed = fatalRequested_;
            const std::string msg = fatalMessage_;
            const bool cancelled = cancelRequested_;
            stopRequested_ = cancelRequested_ = fatalRequested_ = false;
            finalizeLocked(lk, cancelled, failed, msg);
            lastFlushTime = clock::now(); // finalize drained everything
            continue;
        }
        stopRequested_ = false;
        if (workerExit_) break;
        if (state_ == ExperimentRunState::Active && activeRun_) {
            auto& proc = backend_.processing();
            // Issue #407: needsFlush() checks both the frame-count interval
            // AND a byte-budget watermark so a flush fires even when the byte
            // budget saturates before the count threshold is reached.
            const bool thresholdReady = proc.needsFlush();
            const auto now = clock::now();
            const bool timeBackstop =
                proc.getBufferedFrameCounts().total() > 0 &&
                (now - lastFlushTime) >= kTimeFlushInterval;
            if ((thresholdReady || timeBackstop) && backend_.hdf5().isFileOpen()) {
                status_.flushing = true;
                lk.unlock();
                const size_t n = proc.flushBufferedFrames(backend_.hdf5());
                if (n > 0) {
                    SPDLOG_DEBUG("ExperimentCoordinator: periodic flush submitted {} frames{}",
                                 n, timeBackstop && !thresholdReady ? " (time backstop)" : "");
                }
                lk.lock();
                status_.flushing = false;
                lastFlushTime = clock::now();
            }
        }
    }
}

namespace {
// How long a finishing SSD run waits for `pzrec stop` (the store retries past it; the end is then reported as "still stopping").
constexpr std::chrono::seconds kSsdStopDeadline{60};
} // namespace

void ExperimentCoordinator::finalizeLocked(std::unique_lock<std::mutex>& lk, bool cancelled, bool failed,
                                           const std::string& failMessage)
{
    using clock = std::chrono::steady_clock;
    const auto tBegin = clock::now();
    auto sinceMs = [](clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    };
    state_ = failed ? ExperimentRunState::Failed : ExperimentRunState::Stopping;
    status_.cancelled = cancelled;
    status_.flushing = true;
    const RunConfigurationSnapshot run = *activeRun_;
    publishLocked(lk, failed ? "fatal save error; finalizing" : "stopping");
    auto& proc = backend_.processing();
    auto& hdf5 = backend_.hdf5();
    const bool restoreMode = restoreRealtimeMode_;
    restoreRealtimeMode_ = false;
    const std::string liveKdeCoreJson = std::move(liveKdeCoreJson_);
    liveKdeCoreJson_.clear();
    lk.unlock();

    // PL science: stop the provider first. It delivers what the device wrote
    // before STOP, so the run's last frames are ingested before the drain.
    // SSD run (#667 S2): close the SSD run first or at the same time? Both: `pzrec stop` (graceful: the drain finishes the records that passed, the trailer is
    // written) starts on its own thread, the bridge STOP follows, and the end waits for pzrec under a deadline. The stop never blocks the UI (this is the
    // coordinator's worker, the bridge reports "stopping") and it tolerates a slow or retried pzrec stop: past the deadline the run is finalised with a fault
    // that names the SSD run, which pzrec may still be closing (the strip shows STOPPING until it has).
    bool ssdStopOk = true;
    std::string ssdStopWhy;
    bool ssdStopPending = false;
    if (run.ssdRunId != 0) backend_.ssdStore().beginStop(/*abort=*/cancelled);       // a save error of the HDF5 file does not make the SSD data bad: graceful
    if (!app::hostProcessingAvailable()) {
        if (auto* provider = backend_.executionProvider()) {
            provider->stop();
            const auto st = provider->status();
            SPDLOG_INFO("ExperimentCoordinator: provider '{}' stopped: {} frames, {} results, {} incomplete, "
                        "{} decode errors, {} sequence gaps, {} overruns",
                        provider->name(), st.frames, st.results, st.incompleteFrames, st.decodeErrors,
                        st.sequenceGaps, st.overruns);
        }
    }
    if (run.ssdRunId != 0) {
        ssdStopOk = backend_.ssdStore().waitStopped(kSsdStopDeadline, &ssdStopWhy, &ssdStopPending);
        if (!ssdStopOk) {
            SPDLOG_ERROR("ExperimentCoordinator: SSD run {} {}: {}", run.ssdRunId, ssdStopPending ? "is still stopping" : "was not confirmed closed", ssdStopWhy);
        } else {
            SPDLOG_INFO("ExperimentCoordinator: SSD run {} closed", run.ssdRunId);
        }
    }

    bool ok = ssdStopOk;
    bool flushOk = true;
    const bool fileOpen = hdf5.isFileOpen();
    // 2. Drain the async write queue (writer thread stopped afterwards).
    if (fileOpen) {
        const auto t0 = clock::now();
        const size_t submitted = proc.flushBufferedFrames(hdf5);
        if (!proc.finishFlush()) flushOk = false;
        SPDLOG_INFO("ExperimentCoordinator: final flush submitted {} frames, ok={} ({:.3f} ms)",
                    submitted, flushOk, sinceMs(t0));
    }
    // 3. Stop accumulating.
    if (!proc.endExperiment()) flushOk = false;
    proc.resetRealtimeMetrics();
    // 4. Remainder that arrived between 2 and 3 goes through the same flush
    // path so the accounting credits it as committed (bench, 2026-09-08).
    const auto remainder = proc.getBufferedFrameCounts();
    if (fileOpen && remainder.total() > 0) {
        const auto t0 = clock::now();
        const size_t submitted = proc.flushBufferedFrames(hdf5);
        const bool remOk = proc.finishFlush();
        if (!remOk) flushOk = false;
        SPDLOG_INFO("ExperimentCoordinator: remainder flush submitted {} frames (valid={}, invalid={}), ok={} ({:.3f} ms)",
                    submitted, remainder.valid, remainder.invalid, remOk, sinceMs(t0));
    }
    if (!flushOk) ok = false;
    // Pulses fired after the last batch drain (writer is stopped now, so this
    // append is single-threaded like the metadata writes below).
    if (fileOpen) {
        auto tail = backend_.trigger().drainEvents();
        if (!tail.empty() && !hdf5.appendTriggerEvents(tail)) {
            SPDLOG_WARN("ExperimentCoordinator: {} trailing trigger event(s) not persisted", tail.size());
        }
    }
    const uint64_t endNs = WallClock::nowNs();
    auto accounting = proc.experimentAccountingSnapshot();
    // The queue is gone after finishFlush(), so preserve a fatal flush result
    // in the run snapshot before persisting and caching accounting.
    if (failed || !flushOk) {
        const std::string message =
            !failMessage.empty() ? failMessage
                                 : "a save error occurred while flushing experiment data to disk";
        accounting.fatalError = true;
        accounting.fatalMessage = message;
        accounting = recording::reconcile(std::move(accounting));
        ok = false;
    }
    // 5-6. Metadata, accounting, provenance, config JSON; close.
    bool metadataOk = true;
    if (fileOpen) {
        if (!hdf5.flush()) SPDLOG_WARN("ExperimentCoordinator: H5Fflush before metadata failed");
        const auto cfg = proc.getEffectiveProcessingConfig();
        const auto roi = proc.getRealtimeRoi();
        cv::Mat bg = proc.getRealtimeBackgroundGray();
        const auto core = proc.activeProcessingCoreIdentity();
        const auto t0 = clock::now();
        metadataOk = hdf5.writeExperimentInfo(
            run.startWallClockNs, endNs, proc.getTotalValidFlushed(), proc.getTotalInvalidFlushed(),
            cfg, roi, bg.empty() ? nullptr : &bg, &core);
        if (!metadataOk) {
            SPDLOG_ERROR("ExperimentCoordinator: metadata/provenance write failed");
            ok = false;
        } else {
            if (!hdf5.writeRunAccounting(accounting)) {
                SPDLOG_ERROR("ExperimentCoordinator: run accounting could not be persisted");
                ok = false;
            }
            if (!hdf5.writeAcquisitionProvenance(backend_.capture().timestampDescriptor(),
                                                 backend_.capture().telemetrySnapshot())) {
                SPDLOG_ERROR("ExperimentCoordinator: acquisition provenance could not be persisted");
            }
            if (!hdf5.writeWallClockProvenance(run.wallClockSource, run.wallClockOffsetNs)) {
                SPDLOG_ERROR("ExperimentCoordinator: wall-clock provenance could not be persisted");
            }
            // Sorter settings as read back at readiness time (the generator is
            // not re-queried here: the run used what was verified at start).
            if (run.rfGeneratorConnected) {
                const auto rfState = backend_.rfGenerator().lastState();
                if (!rfState.identity.empty() && !hdf5.writeRfGeneratorProvenance(rfState)) {
                    SPDLOG_ERROR("ExperimentCoordinator: RF generator provenance could not be persisted");
                }
            }
            const std::string cfgJson = backend_.getLastConfigJson();
            if (!cfgJson.empty()) hdf5.writeConfigJson(cfgJson);
        }
        SPDLOG_INFO("ExperimentCoordinator: metadata+accounting+provenance took {:.3f} ms", sinceMs(t0));
        // Provisional KDE core contour (copy of the live view); best effort,
        // never affects the run outcome.
        if (!liveKdeCoreJson.empty()) {
            if (hdf5.writeKdeLiveJson(liveKdeCoreJson)) {
                SPDLOG_INFO("ExperimentCoordinator: stored provisional KDE core record ({} bytes)", liveKdeCoreJson.size());
            } else {
                SPDLOG_WARN("ExperimentCoordinator: provisional KDE core record could not be stored");
            }
        }
        const auto tClose = clock::now();
        hdf5.closeFile();
        SPDLOG_INFO("ExperimentCoordinator: closeFile took {:.3f} ms", sinceMs(tClose));
    }
    // An undeclared loss, a failure, an unknown outcome, or declared malformed frames above the
    // warning fraction is something the operator must see (#549): WARN, not INFO. Complete and
    // declared-partial runs stay at INFO.
    const bool warnRun = recording::needsOperatorAttention(accounting.completion) ||
                         recording::malformedAboveWarnFraction(accounting.storeMalformed, accounting.admitted);
    const auto accountingLevel = warnRun ? spdlog::level::warn : spdlog::level::info;
    SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), accountingLevel,
                       "ExperimentCoordinator: run {} accounting: completion={} ({}); persisted={}/{} failed={}",
                       run.startGeneration, recording::toString(accounting.completion), accounting.completionReason,
                       accounting.persistenceCommitted, accounting.persistenceAdmitted, accounting.persistenceFailed);
    // 7. Restore the realtime mode a multi-image run switched.
    if (restoreMode) {
        proc.setRealtimeProcessingMode(services::ProcessingService::RealtimeProcessingMode::AsyncBatch);
        SPDLOG_INFO("ExperimentCoordinator: restored realtime mode to async_batch");
    }

    // 8. Terminal status.
    lk.lock();
    lastAccounting_ = accounting;
    lastAccountingGeneration_ = run.startGeneration;
    haveLastAccounting_ = true;
    activeRun_.reset();
    wallHold_.reset(); // the end time and the provenance are on disk (or the run failed): the clock may move again
    status_.endWallClockNs = endNs;
    status_.terminal = true;
    status_.finalizationOk = ok && !failed;
    status_.flushing = false;
    if (failed) {
        status_.completion = recording::RunCompletionState::Failed;
        status_.completionReason = failMessage;
        faultActive_ = true;
        ++faultRevision_;
        faultCode_ = "experiment.saveFailed";
        faultMessage_ = failMessage;
    } else {
        status_.completion = accounting.completion;
        status_.completionReason = accounting.completionReason;
        if (!flushOk) {
            faultActive_ = true;
            ++faultRevision_;
            faultCode_ = "experiment.flushFailed";
            faultMessage_ = "a save error occurred while flushing experiment data to disk";
        } else if (!metadataOk) {
            faultActive_ = true;
            ++faultRevision_;
            faultCode_ = "experiment.provenanceFailed";
            faultMessage_ = "mandatory metadata/processing-core provenance could not be saved for the last run";
        } else if (!ssdStopOk) {
            faultActive_ = true;
            ++faultRevision_;
            faultCode_ = "experiment.ssdStopFailed";
            faultMessage_ = std::string("SSD run ") + std::to_string(run.ssdRunId) +
                            (ssdStopPending ? " is still stopping; the SSD strip shows when it is closed" : " was not confirmed closed") +
                            (ssdStopWhy.empty() ? std::string() : ": " + ssdStopWhy);
        }
    }
    state_ = (failed || !ok) ? ExperimentRunState::Failed : ExperimentRunState::Idle;
    // Keep the snapshot identity in the terminal status (activeRun_ is gone).
    status_.startGeneration = run.startGeneration;
    status_.readinessGeneration = run.readinessGeneration;
    status_.captureGeneration = run.captureGeneration;
    status_.outputPath = run.outputPath;
    status_.startWallClockNs = run.startWallClockNs;
    SPDLOG_INFO("ExperimentCoordinator: run {} finalized in {:.3f} ms (state={}, ok={})",
                run.startGeneration, sinceMs(tBegin), toString(state_), status_.finalizationOk);
    publishLocked(lk, status_.finalizationOk ? "finalized" : "finalized with errors");
    backend_.resumeLiveResults(); // live monitoring continues in Run once the run's accounting has settled (G5)
}

bool ExperimentCoordinator::lastRunAccounting(recording::RecordingAccountingSnapshot& out, uint64_t& startGeneration) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (!haveLastAccounting_) return false;
    out = lastAccounting_;
    startGeneration = lastAccountingGeneration_;
    return true;
}

void ExperimentCoordinator::shutdown()
{
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (state_ == ExperimentRunState::Active && activeRun_) stopRequested_ = true;
        workerExit_ = true;
        workerCv_.notify_all();
        toJoin = std::move(worker_);
    }
    if (toJoin.joinable()) toJoin.join();
}

} // namespace backend::app

namespace backend::app {
void ExperimentCoordinator::setStageBusyProbe(std::function<bool()> probe) {
    std::lock_guard<std::mutex> lock(mutex_);
    stageBusyProbe_ = std::move(probe);
}

bool ExperimentCoordinator::withIdleConfiguration(const std::function<void()>& transaction) {
    return withIdleConfiguration(transaction, true);
}

bool ExperimentCoordinator::withIdleConfiguration(const std::function<void()>& transaction, bool wait) {
    // Service setters can be called from a facade's idle transaction. Reuse
    // that authorization on this thread without locking the mutex twice.
    static thread_local ExperimentCoordinator* idleOwner = nullptr;
    if (idleOwner == this) {
        transaction();
        return true;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return false;
    if (state_ != ExperimentRunState::Idle) return false;
    struct IdleScope {
        ExperimentCoordinator*& owner;
        ExperimentCoordinator* previous;
        ~IdleScope() { owner = previous; }
    } scope{idleOwner, idleOwner};
    idleOwner = this;
    transaction();
    return true;
}
}
