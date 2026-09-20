#include "backend/supervisor/ExperimentSnapshot.h"

#include "backend/processing/ProcessingCoreLoader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace backend::supervisor {

using json = nlohmann::json;

namespace {

json metricToJson(const Metric& m)
{
    if (!m.known || !std::isfinite(m.value)) return nullptr;
    return m.value;
}

Metric metricFromJson(const json& j, const char* key)
{
    if (!j.contains(key)) return Metric::unknown();
    const auto& v = j.at(key);
    if (v.is_null()) return Metric::unknown();
    if (!v.is_number()) throw std::runtime_error(std::string("metric '") + key + "' is not a number");
    return Metric::of(v.get<double>());
}

template <typename T>
T getOr(const json& j, const char* key, const T& fallback)
{
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    return j.at(key).get<T>();
}

json optU64(const std::optional<uint64_t>& v)
{
    if (!v) return nullptr;
    return *v;
}

std::optional<uint64_t> optU64From(const json& j, const char* key)
{
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    return j.at(key).get<uint64_t>();
}

} // namespace

std::string snapshotToJson(const ExperimentSnapshot& s)
{
    json j;
    j["schema_version"] = s.schemaVersion;
    j["sequence"] = s.sequence;
    j["run_id"] = s.runId;
    j["start_generation"] = s.startGeneration;
    j["experiment_state"] = s.experimentState;
    j["host_time_us"] = s.hostTimeUs;
    j["wall_clock_ns"] = s.wallClockNs;
    j["elapsed_seconds"] = s.elapsedSeconds;
    j["objective"] = s.objective;
    j["target_valid_objects"] = optU64(s.targetValidObjects);

    json c;
    c["camera_source"] = s.configuration.cameraSource;
    c["simulated"] = s.configuration.simulated;
    c["delivery_mode"] = s.configuration.deliveryMode;
    c["requested_fps"] = metricToJson(s.configuration.requestedFps);
    c["exposure_us"] = metricToJson(s.configuration.exposureUs);
    c["roi_width"] = s.configuration.roiWidth;
    c["roi_height"] = s.configuration.roiHeight;
    c["detection_threshold"] = s.configuration.detectionThreshold;
    c["min_area_um2"] = s.configuration.minAreaUm2;
    c["max_area_um2"] = s.configuration.maxAreaUm2;
    c["area_range_check"] = s.configuration.areaRangeCheck;
    c["trigger_enabled"] = s.configuration.triggerEnabled;
    c["processing_core_version"] = s.configuration.processingCoreVersion;
    c["config_sha256"] = s.configuration.configSha256;
    j["configuration"] = c;

    json a;
    a["capture_state"] = s.acquisition.captureState;
    a["camera_ready"] = s.acquisition.cameraReady;
    a["last_failure"] = s.acquisition.lastFailure;
    a["measured_fps"] = metricToJson(s.acquisition.measuredFps);
    a["frames_delivered"] = metricToJson(s.acquisition.framesDelivered);
    a["transport_lost_frames"] = metricToJson(s.acquisition.transportLostFrames);
    a["intentionally_discarded_frames"] = metricToJson(s.acquisition.intentionallyDiscardedFrames);
    a["buffer_underruns"] = metricToJson(s.acquisition.bufferUnderruns);
    a["sdk_queue_depth"] = metricToJson(s.acquisition.sdkQueueDepth);
    a["sdk_input_buffers"] = metricToJson(s.acquisition.sdkInputBuffers);
    a["frame_age_us"] = metricToJson(s.acquisition.frameAgeUs);
    a["publish_latency_us"] = metricToJson(s.acquisition.publishLatencyUs);
    j["acquisition"] = a;

    json d;
    d["frames_processed"] = metricToJson(s.detection.framesProcessed);
    d["valid_objects"] = metricToJson(s.detection.validObjects);
    d["invalid_objects"] = metricToJson(s.detection.invalidObjects);
    d["dropped_valid"] = metricToJson(s.detection.droppedValid);
    d["dropped_invalid"] = metricToJson(s.detection.droppedInvalid);
    d["processing_failures"] = metricToJson(s.detection.processingFailures);
    d["algo_fps"] = metricToJson(s.detection.algoFps);
    d["valid_per_second"] = metricToJson(s.detection.validPerSecond);
    d["invalid_per_second"] = metricToJson(s.detection.invalidPerSecond);
    d["processing_time_us"] = metricToJson(s.detection.processingTimeUs);
    d["window_objects"] = s.detection.windowObjects;
    d["area_mean_um2"] = metricToJson(s.detection.areaMeanUm2);
    d["area_min_um2"] = metricToJson(s.detection.areaMinUm2);
    d["area_max_um2"] = metricToJson(s.detection.areaMaxUm2);
    d["contrast_mean"] = metricToJson(s.detection.contrastMean);
    d["brightness_median_mean"] = metricToJson(s.detection.brightnessMedianMean);
    d["brightness_max_mean"] = metricToJson(s.detection.brightnessMaxMean);
    d["laplacian_variance_mean"] = metricToJson(s.detection.laplacianVarianceMean);
    json rej = json::object();
    for (size_t i = 0; i < s.detection.rejectionReasons.size() && i < s.detection.rejectionCounts.size(); ++i) {
        rej[s.detection.rejectionReasons[i]] = s.detection.rejectionCounts[i];
    }
    d["rejections"] = rej;
    j["detection"] = d;

    json t;
    t["camera_bound"] = s.trigger.cameraBound;
    t["eligible_objects"] = metricToJson(s.trigger.eligibleObjects);
    t["triggers_issued"] = metricToJson(s.trigger.triggersIssued);
    t["suppressed_requests"] = metricToJson(s.trigger.suppressedRequests);
    t["dropped_pulses"] = metricToJson(s.trigger.droppedPulses);
    t["stale_requests"] = metricToJson(s.trigger.staleRequests);
    t["last_onset_us"] = metricToJson(s.trigger.lastOnsetUs);
    j["trigger"] = t;

    json r;
    r["state"] = s.recording.state;
    r["storage_ready"] = s.recording.storageReady;
    r["output_path"] = s.recording.outputPath;
    r["persistence_admitted"] = metricToJson(s.recording.persistenceAdmitted);
    r["persistence_committed"] = metricToJson(s.recording.persistenceCommitted);
    r["persistence_failed"] = metricToJson(s.recording.persistenceFailed);
    r["valid_buffered"] = metricToJson(s.recording.validBuffered);
    r["invalid_buffered"] = metricToJson(s.recording.invalidBuffered);
    r["fault_code"] = s.recording.faultCode;
    r["fault_message"] = s.recording.faultMessage;
    j["recording"] = r;

    json priors = json::array();
    for (const auto& p : s.priorRecommendations) {
        json pj;
        pj["sequence"] = p.sequence;
        pj["elapsed_seconds"] = p.elapsedSeconds;
        pj["action"] = p.action;
        pj["target"] = p.target;
        pj["direction"] = p.direction;
        pj["executed"] = p.executed;
        priors.push_back(pj);
    }
    j["prior_recommendations"] = priors;
    return j.dump();
}

bool snapshotFromJson(const std::string& text, ExperimentSnapshot& out, std::string& error)
{
    try {
        const json j = json::parse(text);
        if (!j.is_object()) { error = "snapshot is not a JSON object"; return false; }
        const auto version = getOr<uint32_t>(j, "schema_version", 0);
        if (version != kExperimentSnapshotSchemaVersion) {
            error = "unsupported snapshot schema_version " + std::to_string(version);
            return false;
        }
        ExperimentSnapshot s;
        s.schemaVersion = version;
        s.sequence = getOr<uint64_t>(j, "sequence", 0);
        s.runId = getOr<std::string>(j, "run_id", "");
        s.startGeneration = getOr<uint64_t>(j, "start_generation", 0);
        s.experimentState = getOr<std::string>(j, "experiment_state", "");
        s.hostTimeUs = getOr<uint64_t>(j, "host_time_us", 0);
        s.wallClockNs = getOr<uint64_t>(j, "wall_clock_ns", 0);
        s.elapsedSeconds = getOr<double>(j, "elapsed_seconds", 0.0);
        s.objective = getOr<std::string>(j, "objective", "");
        s.targetValidObjects = optU64From(j, "target_valid_objects");

        if (j.contains("configuration")) {
            const auto& c = j.at("configuration");
            s.configuration.cameraSource = getOr<std::string>(c, "camera_source", "");
            s.configuration.simulated = getOr<bool>(c, "simulated", false);
            s.configuration.deliveryMode = getOr<std::string>(c, "delivery_mode", "");
            s.configuration.requestedFps = metricFromJson(c, "requested_fps");
            s.configuration.exposureUs = metricFromJson(c, "exposure_us");
            s.configuration.roiWidth = getOr<int>(c, "roi_width", 0);
            s.configuration.roiHeight = getOr<int>(c, "roi_height", 0);
            s.configuration.detectionThreshold = getOr<int>(c, "detection_threshold", 0);
            s.configuration.minAreaUm2 = getOr<int>(c, "min_area_um2", 0);
            s.configuration.maxAreaUm2 = getOr<int>(c, "max_area_um2", 0);
            s.configuration.areaRangeCheck = getOr<bool>(c, "area_range_check", false);
            s.configuration.triggerEnabled = getOr<bool>(c, "trigger_enabled", false);
            s.configuration.processingCoreVersion = getOr<std::string>(c, "processing_core_version", "");
            s.configuration.configSha256 = getOr<std::string>(c, "config_sha256", "");
        }
        if (j.contains("acquisition")) {
            const auto& a = j.at("acquisition");
            s.acquisition.captureState = getOr<std::string>(a, "capture_state", "");
            s.acquisition.cameraReady = getOr<bool>(a, "camera_ready", false);
            s.acquisition.lastFailure = getOr<std::string>(a, "last_failure", "");
            s.acquisition.measuredFps = metricFromJson(a, "measured_fps");
            s.acquisition.framesDelivered = metricFromJson(a, "frames_delivered");
            s.acquisition.transportLostFrames = metricFromJson(a, "transport_lost_frames");
            s.acquisition.intentionallyDiscardedFrames = metricFromJson(a, "intentionally_discarded_frames");
            s.acquisition.bufferUnderruns = metricFromJson(a, "buffer_underruns");
            s.acquisition.sdkQueueDepth = metricFromJson(a, "sdk_queue_depth");
            s.acquisition.sdkInputBuffers = metricFromJson(a, "sdk_input_buffers");
            s.acquisition.frameAgeUs = metricFromJson(a, "frame_age_us");
            s.acquisition.publishLatencyUs = metricFromJson(a, "publish_latency_us");
        }
        if (j.contains("detection")) {
            const auto& d = j.at("detection");
            s.detection.framesProcessed = metricFromJson(d, "frames_processed");
            s.detection.validObjects = metricFromJson(d, "valid_objects");
            s.detection.invalidObjects = metricFromJson(d, "invalid_objects");
            s.detection.droppedValid = metricFromJson(d, "dropped_valid");
            s.detection.droppedInvalid = metricFromJson(d, "dropped_invalid");
            s.detection.processingFailures = metricFromJson(d, "processing_failures");
            s.detection.algoFps = metricFromJson(d, "algo_fps");
            s.detection.validPerSecond = metricFromJson(d, "valid_per_second");
            s.detection.invalidPerSecond = metricFromJson(d, "invalid_per_second");
            s.detection.processingTimeUs = metricFromJson(d, "processing_time_us");
            s.detection.windowObjects = getOr<uint64_t>(d, "window_objects", 0);
            s.detection.areaMeanUm2 = metricFromJson(d, "area_mean_um2");
            s.detection.areaMinUm2 = metricFromJson(d, "area_min_um2");
            s.detection.areaMaxUm2 = metricFromJson(d, "area_max_um2");
            s.detection.contrastMean = metricFromJson(d, "contrast_mean");
            s.detection.brightnessMedianMean = metricFromJson(d, "brightness_median_mean");
            s.detection.brightnessMaxMean = metricFromJson(d, "brightness_max_mean");
            s.detection.laplacianVarianceMean = metricFromJson(d, "laplacian_variance_mean");
            if (d.contains("rejections") && d.at("rejections").is_object()) {
                for (auto it = d.at("rejections").begin(); it != d.at("rejections").end(); ++it) {
                    s.detection.rejectionReasons.push_back(it.key());
                    s.detection.rejectionCounts.push_back(it.value().get<uint64_t>());
                }
            }
        }
        if (j.contains("trigger")) {
            const auto& t = j.at("trigger");
            s.trigger.cameraBound = getOr<bool>(t, "camera_bound", false);
            s.trigger.eligibleObjects = metricFromJson(t, "eligible_objects");
            s.trigger.triggersIssued = metricFromJson(t, "triggers_issued");
            s.trigger.suppressedRequests = metricFromJson(t, "suppressed_requests");
            s.trigger.droppedPulses = metricFromJson(t, "dropped_pulses");
            s.trigger.staleRequests = metricFromJson(t, "stale_requests");
            s.trigger.lastOnsetUs = metricFromJson(t, "last_onset_us");
        }
        if (j.contains("recording")) {
            const auto& r = j.at("recording");
            s.recording.state = getOr<std::string>(r, "state", "");
            s.recording.storageReady = getOr<bool>(r, "storage_ready", false);
            s.recording.outputPath = getOr<std::string>(r, "output_path", "");
            s.recording.persistenceAdmitted = metricFromJson(r, "persistence_admitted");
            s.recording.persistenceCommitted = metricFromJson(r, "persistence_committed");
            s.recording.persistenceFailed = metricFromJson(r, "persistence_failed");
            s.recording.validBuffered = metricFromJson(r, "valid_buffered");
            s.recording.invalidBuffered = metricFromJson(r, "invalid_buffered");
            s.recording.faultCode = getOr<std::string>(r, "fault_code", "");
            s.recording.faultMessage = getOr<std::string>(r, "fault_message", "");
        }
        if (j.contains("prior_recommendations") && j.at("prior_recommendations").is_array()) {
            for (const auto& pj : j.at("prior_recommendations")) {
                PriorRecommendation p;
                p.sequence = getOr<uint64_t>(pj, "sequence", 0);
                p.elapsedSeconds = getOr<uint64_t>(pj, "elapsed_seconds", 0);
                p.action = getOr<std::string>(pj, "action", "");
                p.target = getOr<std::string>(pj, "target", "");
                p.direction = getOr<std::string>(pj, "direction", "");
                p.executed = getOr<bool>(pj, "executed", false);
                s.priorRecommendations.push_back(p);
            }
        }
        out = std::move(s);
        return true;
    } catch (const std::exception& e) {
        error = std::string("snapshot parse error: ") + e.what();
        return false;
    }
}

std::string snapshotHash(const ExperimentSnapshot& snapshot)
{
    const std::string canonical = snapshotToJson(snapshot);
    return backend::processing::processingCoreBytesSha256(
        reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
}

std::optional<double> frameLossFraction(const ExperimentSnapshot& s)
{
    const auto delivered = s.acquisition.framesDelivered.get();
    const auto lost = s.acquisition.transportLostFrames.get();
    if (!delivered || !lost) return std::nullopt;
    const double total = *delivered + *lost;
    if (total <= 0.0) return 0.0;
    return *lost / total;
}

std::optional<double> rejectionFraction(const ExperimentSnapshot& s)
{
    const auto valid = s.detection.validObjects.get();
    const auto invalid = s.detection.invalidObjects.get();
    if (!valid || !invalid) return std::nullopt;
    const double total = *valid + *invalid;
    if (total <= 0.0) return std::nullopt;
    return *invalid / total;
}

std::optional<double> triggerSuccessFraction(const ExperimentSnapshot& s)
{
    const auto eligible = s.trigger.eligibleObjects.get();
    const auto issued = s.trigger.triggersIssued.get();
    if (!eligible || !issued) return std::nullopt;
    if (*eligible <= 0.0) return std::nullopt;
    return std::min(1.0, *issued / *eligible);
}

} // namespace backend::supervisor
