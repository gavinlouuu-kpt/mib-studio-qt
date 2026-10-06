// config_document_apply_test (#398 M2c): the backend config.json applier the
// React/Tauri shell uses to Apply a central method exactly.
//  - every supported section reaches its service (processing incl. the v2
//    difference_threshold and Laplacian keys, contract version, flush
//    interval, buffer bytes, realtime batch + mode, pixel-to-micron,
//    autofocus, ROI) and the exact text becomes the applied config.json;
//  - dot_grid / display_fps are reported as not applicable, never dropped
//    silently;
//  - fail closed: a wrong type anywhere, an unsupported contract, a
//    non-positive pixel factor, invalid JSON or a non-object root change
//    nothing (services and applied text untouched);
//  - refused while live capture or a raw recording runs (the capture worker
//    reads its config unsynchronised; recording runs with the experiment
//    idle), with nothing changed.
#include "backend/app/AppBackend.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

using backend::services::ProcessingService;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool has(const std::vector<std::string>& v, const std::string& item) {
    return std::find(v.begin(), v.end(), item) != v.end();
}

const char* kMethod = R"({
  "config_schema_version": 1,
  "processing_contract_version": 1,
  "buffer_threshold": 250,
  "experiment_buffer_max_mb": 2,
  "pixel_to_micron_factor": 0.75,
  "camera": {"frame_delivery_mode": "everyFrame"},
  "realtime_processing": {"mode": "async_batch", "batch_size": 7, "max_queued_frames": 33, "worker_count": 2,
                          "max_batch_delay_ms": 9},
  "image_processing": {
    "gaussian_blur_size": 7,
    "difference_threshold": 21,
    "bg_subtract_threshold": 5,
    "area_threshold_min": 61,
    "area_threshold_max": 291,
    "laplacian_variance_min": 1.5,
    "laplacian_variance_max": 99.5,
    "filters": {"enable_border_check": false, "enable_laplacian_variance_check": true},
    "target_group": {"enabled": true, "area_min": 70},
    "multi_image": {"enabled": true, "count": 0}
  },
  "autofocus_focus_setpoint": 18.5,
  "focus_direction": false,
  "roi": {"x": 4, "y": 6, "w": 40, "h": 20},
  "dot_grid": {"enabled": false},
  "display_fps": 30
})";

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("mib_config_document_apply");
    const auto frames = dir / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 4, 32, 32), "mock frames");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((dir / "data").string()), "backend init");
    auto& proc = backend.processing();

    watchdog.mark("full document");
    {
        const auto report = backend::app::applyConfigDocument(backend, kMethod);
        MIB_REQUIRE(report.ok, report.error);
        const auto c = proc.getProcessingConfig();
        MIB_EXPECT(c.gaussian_blur_size == 7 && c.bg_subtract_threshold == 21,
                   "difference_threshold wins over bg_subtract_threshold");
        MIB_EXPECT(c.area_threshold_min == 61 && c.area_threshold_max == 291, "areas");
        MIB_EXPECT(c.laplacian_variance_min == 1.5 && c.laplacian_variance_max == 99.5 &&
                       c.enable_laplacian_variance_check && !c.enable_border_check,
                   "Laplacian keys and filters");
        MIB_EXPECT(c.enable_target_group && c.target_group_area_min == 70, "target group");
        MIB_EXPECT(c.multi_image_enabled && c.multi_image_count == 1, "multi-image count clamped to 1");
        MIB_EXPECT(c.processing_contract_version == 1, "contract version");
        MIB_EXPECT(proc.getFlushInterval() == 250, "flush interval");
        MIB_EXPECT(proc.getMaxBufferedBytes() == 2ull * 1024 * 1024, "buffer bytes");
        const auto batch = proc.getRealtimeBatchSettings();
        MIB_EXPECT(batch.batchSize == 7 && batch.maxQueuedFrames == 33 && batch.workerCount == 2 &&
                       batch.maxBatchDelayMs == 9,
                   "batch settings");
        MIB_EXPECT(proc.getRealtimeProcessingMode() == ProcessingService::RealtimeProcessingMode::AsyncBatch,
                   "realtime mode");
        MIB_EXPECT(proc.getPixelToMicronFactor() == 0.75, "pixel to micron");
        const auto af = backend.autofocus().getConfig();
        MIB_EXPECT(af.focusSetpoint == 18.5 && !af.focusDirection, "autofocus");
        const auto roi = proc.getRealtimeRoi();
        MIB_EXPECT(roi.x == 4 && roi.y == 6 && roi.w == 40 && roi.h == 20, "roi");
        MIB_EXPECT(backend.getLastConfigJson() == kMethod, "exact text recorded as applied");
        MIB_EXPECT(has(report.applied, "image_processing") && has(report.applied, "realtime_processing") &&
                       has(report.applied, "camera.frame_delivery_mode") && has(report.applied, "autofocus") &&
                       has(report.applied, "roi") && has(report.applied, "buffer_threshold"),
                   "applied sections reported");
        MIB_EXPECT(has(report.notApplied, "dot_grid (Qt shell only)") &&
                       has(report.notApplied, "display_fps (Qt display setting)"),
                   "inapplicable sections reported");
    }

    watchdog.mark("fail closed");
    {
        const auto before = proc.getProcessingConfig();
        const auto beforeText = backend.getLastConfigJson();
        const auto unchanged = [&](const char* what) {
            const auto c = proc.getProcessingConfig();
            MIB_EXPECT(c.area_threshold_max == before.area_threshold_max &&
                           c.gaussian_blur_size == before.gaussian_blur_size && proc.getFlushInterval() == 250 &&
                           proc.getPixelToMicronFactor() == 0.75 && backend.getLastConfigJson() == beforeText,
                       std::string("nothing changed: ") + what);
        };
        auto r = backend::app::applyConfigDocument(
            backend, R"({"buffer_threshold": 9, "image_processing": {"area_threshold_max": "big"}})");
        MIB_EXPECT(!r.ok && r.error.find("image_processing") != std::string::npos && r.applied.empty(),
                   "wrong type refused");
        unchanged("wrong type in image_processing");
        r = backend::app::applyConfigDocument(backend, R"({"buffer_threshold": 9, "autofocus_focus_setpoint": "x"})");
        MIB_EXPECT(!r.ok, "wrong autofocus type refused");
        unchanged("wrong type at the root (after an earlier valid section)");
        r = backend::app::applyConfigDocument(backend, R"({"processing_contract_version": 99, "buffer_threshold": 9})");
        MIB_EXPECT(!r.ok && r.error.find("not supported") != std::string::npos, "unsupported contract refused");
        unchanged("unsupported contract");
        r = backend::app::applyConfigDocument(backend, R"({"pixel_to_micron_factor": 0})");
        MIB_EXPECT(!r.ok, "non-positive pixel factor refused");
        unchanged("pixel factor");
        r = backend::app::applyConfigDocument(backend, R"({"image_processing": [1, 2]})");
        MIB_EXPECT(!r.ok, "non-object section refused");
        unchanged("non-object section");
        MIB_EXPECT(!backend::app::applyConfigDocument(backend, "{not json").ok, "invalid JSON refused");
        MIB_EXPECT(!backend::app::applyConfigDocument(backend, "[1]").ok, "non-object root refused");
        unchanged("invalid documents");
    }

    watchdog.mark("minimal document");
    {
        const std::string minimal = R"({"config_schema_version": 1})";
        const auto r = backend::app::applyConfigDocument(backend, minimal);
        MIB_EXPECT(r.ok && backend.getLastConfigJson() == minimal, "a minimal document applies");
        MIB_EXPECT(proc.getFlushInterval() == 250, "absent sections keep their values");
        MIB_EXPECT(r.notApplied.empty(), "nothing to report");
    }

    watchdog.mark("central method refusals");
    {
        const auto r = backend::app::applyCentralMethod(backend, "unknown-revision");
        MIB_EXPECT(!r.ok && r.error.find("cache") != std::string::npos, "uncached revision refused");
    }

    watchdog.mark("refused while capturing or recording");
    {
        using backend::services::CaptureLifecycleState;
        const auto applied = backend.getLastConfigJson();
        const auto before = proc.getProcessingConfig();
        const auto unchanged = [&](const char* what) {
            MIB_EXPECT(backend.getLastConfigJson() == applied, what);
            MIB_EXPECT(proc.getProcessingConfig().area_threshold_max == before.area_threshold_max, what);
        };
        MIB_REQUIRE(backend.capture().start(), "mock capture accepted");
        MIB_REQUIRE(backend.capture().waitForState({CaptureLifecycleState::Running, CaptureLifecycleState::Faulted},
                                                   std::chrono::seconds(10)) == CaptureLifecycleState::Running,
                    "mock capture running");
        // Apply while the capture worker is live: refused before any setter
        // runs (under TSan, a setConfig here would race the worker).
        auto r = backend::app::applyConfigDocument(backend, kMethod);
        MIB_EXPECT(!r.ok && r.error.find("Stop capture") != std::string::npos && r.applied.empty(),
                   "live capture: apply refused");
        unchanged("live capture: nothing changed");

        MIB_REQUIRE(backend.startFrameRecording((dir / "raw.h5").string()), "raw recording started");
        MIB_REQUIRE(backend.isFrameRecording(), "raw recording running");
        r = backend::app::applyConfigDocument(backend, kMethod);
        MIB_EXPECT(!r.ok && r.error.find("raw recording") != std::string::npos, "raw recording: apply refused");
        r = backend::app::applyCentralMethod(backend, "unknown-revision");
        MIB_EXPECT(!r.ok && r.error.find("raw recording") != std::string::npos,
                   "raw recording: central method refused before planning");
        unchanged("raw recording: nothing changed");
        backend.stopFrameRecording();
        backend.capture().stop();
        MIB_EXPECT(!backend.capture().isRunning(), "capture stopped");
        MIB_EXPECT(backend::app::applyConfigDocument(backend, kMethod).ok, "applies again once stopped");
    }

    backend.shutdown();
    return mib::test::exitCode();
}
