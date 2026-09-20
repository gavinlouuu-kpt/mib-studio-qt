// AI Experiment Supervisor — snapshot builder over the authoritative backend
// (issue #422, phase A).
//
// Copies metrics the backend already produces (CaptureService telemetry and
// lifecycle, ProcessingService counters and monitoring window, TriggerService
// counters, ExperimentCoordinator status, RecordingAccounting) into one
// frozen ExperimentSnapshot. Read-only: it never starts, stops or configures
// anything and adds no scientific processing. Cheap enough for a
// multi-second cadence; it must still never run on acquisition threads.
#pragma once

#include "backend/supervisor/ExperimentSnapshot.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend { class AppBackend; }

namespace backend::supervisor {

struct SnapshotBuildContext {
    uint64_t sequence{0};
    std::string objective;
    std::optional<uint64_t> targetValidObjects;
    std::vector<PriorRecommendation> priorRecommendations;
    // Max monitoring objects scanned for area/contrast statistics.
    std::size_t statisticsWindow{256};
};

ExperimentSnapshot buildExperimentSnapshot(AppBackend& backend, const SnapshotBuildContext& context);

} // namespace backend::supervisor
