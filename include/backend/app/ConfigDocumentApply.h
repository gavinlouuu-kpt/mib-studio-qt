// Apply a whole config.json document in the backend: the one validated
// applier (#398 M2c). Qt-free.
//
// Two entry points share it:
//  - local profiles (app/ProfileStore.cpp, the Tauri Profiles panel), which
//    record a provenance document as the applied config;
//  - central methods (applyCentralMethod, the Tauri Central Methods panel),
//    which record the revision's exact text, because that is what the
//    method.revision gate and run provenance match.
//
// stageConfigDocument parses and validates every section against the current
// runtime values, bounding every number, and changes nothing. It throws
// std::exception with the reason. commitStagedConfig then applies everything.
// So a malformed document changes nothing. Rules:
//  - config_schema_version, if present, is 1;
//  - root processing_contract_version, if present, must be supported;
//  - image_processing goes through the shared processing parser and range
//    checks (app/ProcessingConfigTransaction). The v2 difference_threshold key
//    wins over bg_subtract_threshold, and a multi-image count below 1 is
//    clamped to 1, as the Qt AppConfigWatcher does;
//  - buffer_threshold, experiment_buffer_max_mb, pixel_to_micron_factor and
//    the autofocus keys must be in range;
//  - realtime_processing (enabled, drop_frames, batch settings, mode) must be
//    in range, and the queue must hold at least one batch;
//  - camera.frame_delivery_mode must be everyFrame or latestFrame (missing
//    means everyFrame);
//  - a stage block applies only while the Z stage is disconnected;
//  - a non-empty roi must fit the latest captured frame (capture a matching
//    preview, then stop capture);
//  - display_fps is validated (1..240) but is a display setting.
//
// Nothing that reads these settings may be running (configApplyBlocker): raw
// recording runs with the experiment idle, and the capture worker reads its
// Config unsynchronised.
#pragma once

#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/StageConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend {
class AppBackend;
}

namespace backend::app {

// Why a configuration cannot be applied now: raw recording, live capture,
// realtime processing or autofocus running. Empty = free to apply.
std::string configApplyBlocker(AppBackend& backend);

// A validated document, not yet applied (see the rules above).
struct StagedConfig {
    services::ProcessingConfig processing;
    int flushInterval{1};
    double experimentBufferMb{0};
    double pixelToMicron{1};
    bool realtimeEnabled{false};
    bool dropFrames{false};
    services::ProcessingService::RealtimeBatchSettings batch;
    services::ProcessingService::RealtimeProcessingMode mode{};
    services::CaptureService::Config capture;
    services::AutofocusService::Config autofocus;
    std::optional<services::StageConfig> stage;
    services::ProcessingService::Roi roi;
    int displayFps{60};
};

// Throws (std::exception) with the reason; never changes runtime state.
StagedConfig stageConfigDocument(AppBackend& backend, const std::string& configJson);

// Applies every staged value. The caller has checked configApplyBlocker and
// records what was applied (AppBackend::setLastConfigJson).
void commitStagedConfig(AppBackend& backend, const StagedConfig& staged);

struct ConfigApplyReport {
    bool ok{false};
    std::string error;                    // why nothing was applied
    std::vector<std::string> applied;     // section names applied
    std::vector<std::string> notApplied;  // present but not applicable in this shell
};

// Blocker check, stage, commit, then the exact text becomes the applied
// config.json. dot_grid (needs the Qt registry loader) and display_fps (a
// display setting) are reported as not applied, never silently dropped.
ConfigApplyReport applyConfigDocument(AppBackend& backend, const std::string& configJson);

// Apply a central revision exactly (#398 M2c), inside the coordinator's idle
// transaction (as local profiles are), so Start cannot interleave. Refused
// unless the experiment is Idle (a running experiment keeps its frozen
// method) and under configApplyBlocker; otherwise planMethodApply (cached,
// published/superseded, materialized, untampered) then applyConfigDocument.
ConfigApplyReport applyCentralMethod(AppBackend& backend, const std::string& revisionId);

} // namespace backend::app
