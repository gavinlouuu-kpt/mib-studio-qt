#include "backend/app/ConfigDocumentApply.h"

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/MethodApply.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/camera/common/ICamera.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CaptureService.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace backend::app {
namespace {
using Json = nlohmann::json;

struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T> std::optional<T> get(const Json& object, const char* key, const std::string& where) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::nullopt;
    try {
        if constexpr (std::is_same_v<T, bool>) {
            if (!it->is_boolean()) throw Invalid("");
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!it->is_string()) throw Invalid("");
        } else {
            if (!it->is_number()) throw Invalid("");
        }
        return it->get<T>();
    } catch (const std::exception&) {
        throw Invalid(where + key + " has the wrong type");
    }
}

const Json* object(const Json& parent, const char* key, const std::string& where) {
    const auto it = parent.find(key);
    if (it == parent.end() || it->is_null()) return nullptr;
    if (!it->is_object()) throw Invalid(where + key + " must be an object");
    return &*it;
}

// Same precondition as the local-profile apply (app/ProfileStore.cpp):
// nothing that reads these settings may be running. Raw recording runs with
// the experiment idle, and the capture worker reads its Config unsynchronised,
// so a live change would race it (#493 review). Empty = free to apply.
std::string busyReason(AppBackend& backend) {
    if (backend.isFrameRecording()) return "Stop raw recording before applying a method";
    if (backend.capture().isRunning() || backend.autofocus().isEnabled() ||
        backend.processing().isRealtimeRunning())
        return "Stop capture/realtime processing and disable autofocus before applying a method";
    return {};
}

} // namespace

ConfigApplyReport applyConfigDocument(AppBackend& backend, const std::string& text) {
    ConfigApplyReport report;
    report.error = busyReason(backend);
    if (!report.error.empty()) return report;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const Json::exception&) {
        report.error = "config.json is not valid JSON";
        return report;
    }
    if (!root.is_object()) {
        report.error = "config.json root must be an object";
        return report;
    }

    auto& processing = backend.processing();
    // ---- stage (nothing changes until every section validated) ----------
    auto pcfg = processing.getProcessingConfig();
    std::optional<size_t> flushEvery;
    std::optional<uint64_t> maxBufferedBytes;
    std::optional<services::ProcessingService::RealtimeBatchSettings> batch;
    std::optional<services::ProcessingService::RealtimeProcessingMode> mode;
    services::CaptureService::Config capture{};
    std::optional<double> pixelToMicron;
    auto autofocus = backend.autofocus().getConfig();
    bool autofocusTouched = false;
    std::optional<services::ProcessingService::Roi> roi;
    try {
        const int contract = get<int>(root, "processing_contract_version", "").value_or(1);
        if (!processing::contract::isSupportedProcessingContract(contract))
            throw Invalid("processing_contract_version " + std::to_string(contract) + " is not supported");
        pcfg.processing_contract_version = contract;

        if (const auto* ip = object(root, "image_processing", "")) {
            std::string error;
            if (!processing::config_json::fromJson(*ip, pcfg, &error)) throw Invalid("image_processing: " + error);
            // Keys the shared parser predates (same rules as AppConfigWatcher).
            if (const auto v = get<int>(*ip, "difference_threshold", "image_processing.")) pcfg.bg_subtract_threshold = *v;
            if (const auto v = get<double>(*ip, "laplacian_variance_min", "image_processing."))
                pcfg.laplacian_variance_min = *v;
            if (const auto v = get<double>(*ip, "laplacian_variance_max", "image_processing."))
                pcfg.laplacian_variance_max = *v;
            if (const auto* filters = object(*ip, "filters", "image_processing."))
                if (const auto v = get<bool>(*filters, "enable_laplacian_variance_check", "image_processing.filters."))
                    pcfg.enable_laplacian_variance_check = *v;
            pcfg.multi_image_count = std::max(1, pcfg.multi_image_count);
            report.applied.push_back("image_processing");
        }
        if (const auto v = get<int>(root, "buffer_threshold", "")) flushEvery = static_cast<size_t>(std::max(1, *v));
        if (const auto v = get<double>(root, "experiment_buffer_max_mb", ""))
            maxBufferedBytes = static_cast<uint64_t>(std::max(0.0, *v) * 1024.0 * 1024.0);
        if (const auto* rp = object(root, "realtime_processing", "")) {
            auto b = processing.getRealtimeBatchSettings();
            if (const auto v = get<int>(*rp, "batch_size", "realtime_processing.")) b.batchSize = static_cast<size_t>(std::max(1, *v));
            if (const auto v = get<int>(*rp, "max_queued_frames", "realtime_processing."))
                b.maxQueuedFrames = static_cast<size_t>(std::max(1, *v));
            if (const auto v = get<int>(*rp, "worker_count", "realtime_processing.")) b.workerCount = static_cast<size_t>(std::max(1, *v));
            if (const auto v = get<int>(*rp, "max_batch_delay_ms", "realtime_processing.")) b.maxBatchDelayMs = std::max(1, *v);
            batch = b;
            if (const auto v = get<std::string>(*rp, "mode", "realtime_processing.")) {
                std::string m = *v;
                std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                mode = (m == "async_batch" || m == "batch" || m == "kin6")
                           ? services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                           : services::ProcessingService::RealtimeProcessingMode::Inline;
            }
            report.applied.push_back("realtime_processing");
        }
        {
            // Missing / unknown maps to EveryFrame, applied unconditionally
            // (deterministic migration, as the watcher does).
            std::string deliveryMode;
            if (const auto* camera = object(root, "camera", ""))
                deliveryMode = get<std::string>(*camera, "frame_delivery_mode", "camera.").value_or("");
            capture.deliveryMode = ::camera::common::frameDeliveryModeFromString(deliveryMode);
            report.applied.push_back("camera.frame_delivery_mode");
        }
        if (const auto v = get<double>(root, "pixel_to_micron_factor", "")) {
            if (*v <= 0.0) throw Invalid("pixel_to_micron_factor must be positive");
            pixelToMicron = *v;
        }
        const auto af = [&](const char* key, auto& field) {
            using T = std::decay_t<decltype(field)>;
            if (const auto v = get<T>(root, key, "")) {
                field = *v;
                autofocusTouched = true;
            }
        };
        af("autofocus_focus_setpoint", autofocus.focusSetpoint);
        af("autofocus_focus_range", autofocus.focusRange);
        af("autofocus_voltage_step", autofocus.voltageStep);
        af("autofocus_fine_voltage_step", autofocus.fineVoltageStep);
        af("autofocus_max_voltage", autofocus.maxVoltage);
        af("autofocus_min_voltage", autofocus.minVoltage);
        af("autofocus_initial_voltage", autofocus.initialVoltage);
        af("autofocus_manual_voltage_step", autofocus.manualVoltageStep);
        af("ring_ratio_stale_ms", autofocus.ringRatioStaleMs);
        af("require_new_sample_per_step", autofocus.requireNewSamplePerStep);
        af("autofocus_min_samples_per_step", autofocus.minSamplesPerStep);
        af("safe_shutdown_voltage", autofocus.safeShutdownVoltage);
        af("focus_direction", autofocus.focusDirection);
        if (const auto* r = object(root, "roi", "")) {
            const auto x = get<int>(*r, "x", "roi."), y = get<int>(*r, "y", "roi."), w = get<int>(*r, "w", "roi."),
                       h = get<int>(*r, "h", "roi.");
            if (x && y && w && h && *x >= 0 && *y >= 0 && *w > 0 && *h > 0)
                roi = services::ProcessingService::Roi{*x, *y, *w, *h};
            else
                report.notApplied.push_back("roi (incomplete or invalid)");
        }
    } catch (const Invalid& e) {
        report.applied.clear();
        report.notApplied.clear();
        report.error = e.what();
        return report;
    }
    if (root.contains("dot_grid")) report.notApplied.push_back("dot_grid (Qt shell only)");
    if (root.contains("display_fps")) report.notApplied.push_back("display_fps (Qt display setting)");

    // ---- commit ---------------------------------------------------------
    processing.setProcessingConfig(pcfg);
    if (flushEvery) {
        processing.setFlushInterval(*flushEvery);
        report.applied.push_back("buffer_threshold");
    }
    if (maxBufferedBytes) {
        processing.setMaxBufferedBytes(*maxBufferedBytes);
        report.applied.push_back("experiment_buffer_max_mb");
    }
    if (batch) processing.setRealtimeBatchSettings(*batch);
    if (mode) processing.setRealtimeProcessingMode(*mode);
    backend.capture().setConfig(capture);
    if (pixelToMicron) {
        processing.setPixelToMicronFactor(*pixelToMicron);
        report.applied.push_back("pixel_to_micron_factor");
    }
    if (autofocusTouched) {
        backend.autofocus().setConfig(autofocus);
        report.applied.push_back("autofocus");
    }
    if (roi) {
        processing.setRealtimeRoi(*roi);
        report.applied.push_back("roi");
    }
    backend.setLastConfigJson(text);
    report.ok = true;
    SPDLOG_INFO("ConfigDocumentApply: applied config.json ({} section(s); not applicable here: {})",
                report.applied.size(), report.notApplied.size());
    return report;
}

ConfigApplyReport applyCentralMethod(AppBackend& backend, const std::string& revisionId) {
    ConfigApplyReport report;
    const auto runState = backend.experiment().state();
    if (runState == ExperimentRunState::Starting || runState == ExperimentRunState::Active ||
        runState == ExperimentRunState::Stopping) {
        report.error = "An experiment is in progress; apply the method after it ends";
        return report;
    }
    report.error = busyReason(backend);
    if (!report.error.empty()) return report;
    const auto plan = planMethodApply(backend.profileRegistry().snapshot(), revisionId, backend.getLastConfigJson());
    if (!plan.ok) {
        report.error = plan.error;
        return report;
    }
    return applyConfigDocument(backend, plan.configText);
}

} // namespace backend::app
