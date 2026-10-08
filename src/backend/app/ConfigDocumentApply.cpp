#include "backend/app/ConfigDocumentApply.h"

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/MethodApply.h"
#include "backend/app/ProcessingConfigTransaction.h"
#include "backend/camera/common/ICamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/services/StageService.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace backend::app {
namespace {
using Json = nlohmann::json;

double number(const Json& root, const char* key, double fallback, double low, double high) {
    if (!root.contains(key)) return fallback;
    if (!root.at(key).is_number()) throw std::runtime_error(std::string("Expected number: ") + key);
    const double value = root.at(key).get<double>();
    if (!std::isfinite(value) || value < low || value > high) {
        std::ostringstream message;
        message << key << " must be between " << low << " and " << high << " (got " << value << ")";
        throw std::runtime_error(message.str());
    }
    return value;
}

int integer(const Json& root, const char* key, int fallback, int low, int high) {
    if (root.contains(key) && !root.at(key).is_number_integer())
        throw std::runtime_error(std::string("Expected integer: ") + key);
    return static_cast<int>(number(root, key, fallback, low, high));
}

bool boolean(const Json& root, const char* key, bool fallback) {
    if (!root.contains(key)) return fallback;
    if (!root.at(key).is_boolean()) throw std::runtime_error(std::string("Expected boolean: ") + key);
    return root.at(key).get<bool>();
}

// The Qt AppConfigWatcher's compatibility rules, applied to a copy of the
// image_processing section before the shared parser sees it.
Json watcherCompatible(Json section) {
    if (!section.is_object()) throw std::runtime_error("image_processing must be an object");
    if (section.contains("difference_threshold")) {
        if (!section.at("difference_threshold").is_number_integer())
            throw std::runtime_error("Expected integer: image_processing.difference_threshold");
        section["bg_subtract_threshold"] = section.at("difference_threshold");
    }
    if (const auto multi = section.find("multi_image"); multi != section.end() && multi->is_object()) {
        if (const auto count = multi->find("count");
            count != multi->end() && count->is_number_integer() && count->get<long long>() < 1)
            (*multi)["count"] = 1;
    }
    return section;
}

} // namespace

RoiCheck checkRoi(const services::ProcessingService::Roi& r, FrameSize frame, FrameSize window) {
    const FrameSize bound = frame.known() ? frame : window;
    if (!bound.known()) return RoiCheck::Pending;
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
                   static_cast<uint64_t>(r.x) + static_cast<uint64_t>(r.w) <= bound.width &&
                   static_cast<uint64_t>(r.y) + static_cast<uint64_t>(r.h) <= bound.height
               ? RoiCheck::Fits
               : RoiCheck::OutOfBounds;
}

std::string configApplyBlocker(AppBackend& backend) {
    if (backend.isFrameRecording()) return "Stop raw recording before applying a configuration";
    if (backend.capture().isRunning() || backend.autofocus().isEnabled() ||
        backend.processing().isRealtimeRunning())
        return "Stop capture/realtime processing and disable autofocus before applying a configuration";
    return {};
}

StagedConfig stageConfigDocument(AppBackend& backend, const std::string& bytes) {
    auto root = Json::parse(bytes);
    if (!root.is_object()) throw std::runtime_error("config.json root must be an object");
    integer(root, "config_schema_version", 1, 1, 1);
    auto& processing = backend.processing();
    StagedConfig s;

    if (root.contains("image_processing")) root["image_processing"] = watcherCompatible(root.at("image_processing"));
    try {
        s.processing = validatedProcessingConfig(root.dump(), processing.getProcessingConfig());
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("image_processing: ") + e.what());
    }
    if (root.contains("processing_contract_version") && !root.at("processing_contract_version").is_null()) {
        const int contract = integer(root, "processing_contract_version", 1, 1, 1000000);
        if (!processing::contract::isSupportedProcessingContract(contract))
            throw std::runtime_error("processing_contract_version " + std::to_string(contract) +
                                     " is not supported");
        s.processing.processing_contract_version = contract;
    }

    s.flushInterval =
        integer(root, "buffer_threshold", static_cast<int>(processing.getFlushInterval()), 1, 10000000);
    s.experimentBufferMb = number(root, "experiment_buffer_max_mb",
                                  processing.getMaxBufferedBytes() / (1024.0 * 1024.0), 0, 1048576);
    s.pixelToMicron = number(root, "pixel_to_micron_factor", processing.getPixelToMicronFactor(), 1e-12, 1e12);

    s.realtimeEnabled = processing.isRealtimeEnabled();
    s.dropFrames = processing.getRealtimeDropFrames();
    s.batch = processing.getRealtimeBatchSettings();
    s.mode = processing.getRealtimeProcessingMode();
    if (root.contains("realtime_processing")) {
        const auto& rp = root.at("realtime_processing");
        if (!rp.is_object()) throw std::runtime_error("realtime_processing must be an object");
        s.realtimeEnabled = boolean(rp, "enabled", s.realtimeEnabled);
        s.dropFrames = boolean(rp, "drop_frames", s.dropFrames);
        s.batch.batchSize = integer(rp, "batch_size", static_cast<int>(s.batch.batchSize), 1, 1000000);
        s.batch.maxQueuedFrames =
            integer(rp, "max_queued_frames", static_cast<int>(s.batch.maxQueuedFrames), 1, 10000000);
        s.batch.workerCount = integer(rp, "worker_count", static_cast<int>(s.batch.workerCount), 1, 256);
        s.batch.maxBatchDelayMs = integer(rp, "max_batch_delay_ms", s.batch.maxBatchDelayMs, 1, 60000);
        if (rp.contains("mode")) {
            if (!rp.at("mode").is_string()) throw std::runtime_error("Expected string: realtime_processing.mode");
            const auto text = rp.at("mode").get<std::string>();
            if (text == "inline")
                s.mode = services::ProcessingService::RealtimeProcessingMode::Inline;
            else if (text == "async_batch" || text == "batch" || text == "kin6")
                s.mode = services::ProcessingService::RealtimeProcessingMode::AsyncBatch;
            else
                throw std::runtime_error("Unknown realtime processing mode");
        }
    }
    if (s.batch.maxQueuedFrames < s.batch.batchSize)
        throw std::runtime_error("Realtime queue must hold at least one batch");

    if (root.contains("camera")) {
        const auto& camera = root.at("camera");
        if (!camera.is_object()) throw std::runtime_error("camera must be an object");
        const auto it = camera.find("frame_delivery_mode");
        const std::string text = it == camera.end() || it->is_null()
                                     ? std::string("everyFrame")
                                     : (it->is_string() ? it->get<std::string>() : std::string("?"));
        if (text != "everyFrame" && text != "latestFrame")
            throw std::runtime_error("Unknown frame delivery mode");
        s.capture.deliveryMode = ::camera::common::frameDeliveryModeFromString(text);
    }

    auto& af = s.autofocus;
    af = backend.autofocus().getConfig();
    af.focusSetpoint = number(root, "autofocus_focus_setpoint", af.focusSetpoint, 0, 1e9);
    af.focusRange = number(root, "autofocus_focus_range", af.focusRange, 0, 1e9);
    af.voltageStep = number(root, "autofocus_voltage_step", af.voltageStep, 0, 1e6);
    af.fineVoltageStep = number(root, "autofocus_fine_voltage_step", af.fineVoltageStep, 0, 1e6);
    af.minVoltage = number(root, "autofocus_min_voltage", af.minVoltage, -1e6, 1e6);
    af.maxVoltage = number(root, "autofocus_max_voltage", af.maxVoltage, -1e6, 1e6);
    af.initialVoltage = number(root, "autofocus_initial_voltage", af.initialVoltage, af.minVoltage, af.maxVoltage);
    af.manualVoltageStep = number(root, "autofocus_manual_voltage_step", af.manualVoltageStep, 0, 1e6);
    af.safeShutdownVoltage =
        number(root, "safe_shutdown_voltage", af.safeShutdownVoltage, af.minVoltage, af.maxVoltage);
    af.ringRatioStaleMs = integer(root, "ring_ratio_stale_ms", af.ringRatioStaleMs, 1, 3600000);
    af.minSamplesPerStep = integer(root, "autofocus_min_samples_per_step", af.minSamplesPerStep, 1, 10000000);
    af.requireNewSamplePerStep = boolean(root, "require_new_sample_per_step", af.requireNewSamplePerStep);
    af.focusDirection = boolean(root, "focus_direction", af.focusDirection);
    if (af.minVoltage > af.maxVoltage || af.initialVoltage < af.minVoltage || af.initialVoltage > af.maxVoltage ||
        af.safeShutdownVoltage < af.minVoltage || af.safeShutdownVoltage > af.maxVoltage)
        throw std::runtime_error("Invalid autofocus voltage bounds");

    // Z stage block (#464): configuration only; nothing connects or moves.
    if (root.contains("stage")) {
        s.stage = services::parseStageConfig(root.at("stage"));
        if (backend.stage().snapshot().connected)
            throw std::runtime_error("Disconnect the Z stage before applying a configuration with a stage block");
    }

    s.roi = processing.getRealtimeRoi();
    if (root.contains("roi")) {
        s.roiSpecified = true;
        const auto& r = root.at("roi");
        if (!r.is_object()) throw std::runtime_error("roi must be an object");
        s.roi.x = integer(r, "x", 0, 0, 1000000);
        s.roi.y = integer(r, "y", 0, 0, 1000000);
        s.roi.w = integer(r, "w", 0, 0, 1000000);
        s.roi.h = integer(r, "h", 0, 0, 1000000);
        if (s.roi.w || s.roi.h) {
            if (!s.roi.w || !s.roi.h) throw std::runtime_error("roi needs both w and h (or both 0 for none)");
            FrameSize frame, window;
            playback::Frame latest;
            if (backend.getFrameStore() && backend.getFrameStore()->getLatest(latest))
                frame = {latest.width, latest.height};
            const auto g = backend.cameraGeometry();
            if (g.roiWidth > 0 && g.roiHeight > 0)
                window = {static_cast<uint64_t>(g.roiWidth), static_cast<uint64_t>(g.roiHeight)};
            else if (g.sensorWidth > 0 && g.sensorHeight > 0)
                window = {static_cast<uint64_t>(g.sensorWidth), static_cast<uint64_t>(g.sensorHeight)};
            switch (checkRoi(s.roi, frame, window)) {
            case RoiCheck::Fits:
                break;
            case RoiCheck::Pending:
                s.roiPending = true;
                break;
            case RoiCheck::OutOfBounds: {
                const auto bound = frame.known() ? frame : window;
                throw std::runtime_error("roi " + std::to_string(s.roi.w) + "x" + std::to_string(s.roi.h) + "@" +
                                         std::to_string(s.roi.x) + "," + std::to_string(s.roi.y) +
                                         " does not fit the " + std::to_string(bound.width) + "x" +
                                         std::to_string(bound.height) +
                                         (frame.known() ? " captured frame" : " camera window"));
            }
            }
        }
    }
    s.displayFps = integer(root, "display_fps", 60, 1, 240);
    return s;
}

void commitStagedConfig(AppBackend& backend, const StagedConfig& s) {
    // Everything was validated first and nothing that reads these settings is
    // running (configApplyBlocker), so the setters neither fail half-way nor
    // restart worker threads. No hardware actuation is issued.
    auto& processing = backend.processing();
    processing.setProcessingConfig(s.processing);
    processing.setFlushInterval(static_cast<size_t>(s.flushInterval));
    processing.setMaxBufferedBytes(static_cast<uint64_t>(s.experimentBufferMb * 1024.0 * 1024.0));
    processing.setRealtimeDropFrames(s.dropFrames);
    processing.setRealtimeBatchSettings(s.batch);
    processing.setRealtimeProcessingMode(s.mode);
    processing.setRealtimeEnabled(s.realtimeEnabled);
    processing.setPixelToMicronFactor(s.pixelToMicron);
    if (s.roiPending)
        processing.setPendingRealtimeRoi(s.roi);
    else if (s.roiSpecified)
        processing.setRealtimeRoi(s.roi);
    backend.capture().setConfig(s.capture);
    backend.autofocus().setConfig(s.autofocus);
    if (s.stage) backend.stage().setConfig(*s.stage); // disconnected: checked when staged
}

ConfigApplyReport applyConfigDocument(AppBackend& backend, const std::string& text) {
    ConfigApplyReport report;
    report.error = configApplyBlocker(backend);
    if (!report.error.empty()) return report;
    StagedConfig staged;
    Json root;
    try {
        staged = stageConfigDocument(backend, text);
        root = Json::parse(text);
    } catch (const std::exception& e) {
        report.error = e.what();
        return report;
    }
    commitStagedConfig(backend, staged);
    backend.setLastConfigJson(text);

    for (const auto* key : {"image_processing", "buffer_threshold", "experiment_buffer_max_mb", "realtime_processing",
                            "pixel_to_micron_factor", "stage"})
        if (root.contains(key)) report.applied.emplace_back(key);
    if (root.contains("roi")) {
        if (staged.roiPending)
            report.notApplied.emplace_back("roi (pending: applied on the first captured frame)");
        else
            report.applied.emplace_back("roi");
    }
    report.applied.emplace_back("camera.frame_delivery_mode"); // missing = everyFrame, always applied
    for (const auto& item : root.items()) {
        const auto& key = item.key();
        if (key.rfind("autofocus_", 0) == 0 || key == "ring_ratio_stale_ms" || key == "require_new_sample_per_step" ||
            key == "safe_shutdown_voltage" || key == "focus_direction") {
            report.applied.emplace_back("autofocus");
            break;
        }
    }
    if (root.contains("dot_grid")) report.notApplied.emplace_back("dot_grid (Qt shell only)");
    if (root.contains("display_fps")) report.notApplied.emplace_back("display_fps (Qt display setting)");
    report.ok = true;
    SPDLOG_INFO("ConfigDocumentApply: applied config.json ({} section(s); not applicable here: {})",
                report.applied.size(), report.notApplied.size());
    return report;
}

ConfigApplyReport applyCentralMethod(AppBackend& backend, const std::string& revisionId) {
    ConfigApplyReport report;
    // Under the coordinator's idle transaction, as local profiles are: Start
    // cannot slip in between these checks and the commit.
    const bool idle = backend.experiment().withIdleConfiguration([&] {
        report.error = configApplyBlocker(backend);
        if (!report.error.empty()) return;
        const auto plan =
            planMethodApply(backend.profileRegistry().snapshot(), revisionId, backend.getLastConfigJson());
        if (!plan.ok) {
            report.error = plan.error;
            return;
        }
        report = applyConfigDocument(backend, plan.configText);
    });
    if (!idle) {
        report = {};
        report.error = "An experiment is in progress (or failed and not yet cleared); apply the method after it ends";
    }
    return report;
}

} // namespace backend::app
