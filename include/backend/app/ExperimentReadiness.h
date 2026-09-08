// Experiment readiness + immutable run configuration snapshot (issue #369,
// host-SDK portion of #274).
//
// Start is authorized only by a backend readiness transaction: the
// coordinator evaluates every gate against the *actual* current state
// (camera session, hardware-vs-mock, delivery mode, ROI/pixel format,
// processing core, config revision, calibration, background, trigger, output
// storage, unresolved faults), tags the result with a generation, and a
// later Start must present that generation and still match every
// invalidation input. Unknown is never Pass. A successful Start freezes one
// RunConfigurationSnapshot that the running experiment uses; later edits do
// not mutate it.
#pragma once

#include "backend/processing/IProcessingKernel.h"
#include "backend/recording/RecordingAccounting.h"

#include <cstdint>
#include <string>
#include <vector>

namespace backend::app {

// Contract-pinned (bridge-contract.json readiness_gate_statuses); append only.
enum class GateStatus { Pass = 0, Warn = 1, Fail = 2, Unavailable = 3, NotRequired = 4 };

inline const char* toString(GateStatus s)
{
    switch (s) {
    case GateStatus::Pass: return "pass";
    case GateStatus::Warn: return "warn";
    case GateStatus::Fail: return "fail";
    case GateStatus::Unavailable: return "unavailable";
    case GateStatus::NotRequired: return "notRequired";
    }
    return "unknown";
}

struct ReadinessGate {
    std::string id;          // stable token, e.g. "camera.session"
    GateStatus status{GateStatus::Unavailable};
    std::string reason;      // why it is not Pass (empty on Pass)
    std::string remediation; // what the operator can do
    std::string detail;      // identity/value evaluated
    bool blocksStart() const { return status == GateStatus::Fail || status == GateStatus::Unavailable; }
};

// Where frames come from. Mock is an explicit choice, never a silent
// fallback for requested hardware.
struct CameraSourceInfo {
    std::string requested{"unknown"}; // "mock" | "egrabber" | "mindvision" | "unknown"
    std::string effective{"unknown"};
    std::string label;
    bool simulated{false};        // effective source is the mock camera
    bool fallback{false};         // effective != requested (hardware unavailable)
    std::string fallbackReason;
};

// Frozen identities/settings of one run. Owned by the coordinator; the UI
// only reads it.
struct RunConfigurationSnapshot {
    static constexpr uint32_t kSchemaVersion = 1;
    uint64_t readinessGeneration{0};
    uint64_t startGeneration{0};      // coordinator start counter
    uint64_t captureGeneration{0};    // CaptureService session generation
    uint64_t startHostTimeUs{0};
    uint64_t startWallClockNs{0};

    CameraSourceInfo camera;
    bool cameraReady{false};
    std::string deliveryModeRequested;
    std::string deliveryModeActive;
    bool deliveryModeConfirmed{false};
    std::string timestampDescriptor;  // camera::common::describe()

    int roiX{0}, roiY{0}, roiW{0}, roiH{0};
    uint64_t frameWidth{0}, frameHeight{0}, pixelFormat{0};
    bool frameGeometryKnown{false};

    backend::processing::ProcessingCoreIdentity processingCore;
    bool processingCorePinSatisfied{false};
    uint64_t processingConfigVersion{0};
    std::string processingConfigSha256;  // canonical serialization of ProcessingConfig
    std::string configJsonSha256;        // raw config.json as last applied
    std::string profileId;               // frontend-supplied profile identity (may be empty)
    double pixelToMicron{0.0};

    bool backgroundPresent{false};
    uint64_t backgroundGeneration{0};
    std::string backgroundSha256;

    bool triggerRequired{false};
    bool triggerBound{false};
    uint64_t triggerGeneration{0};

    std::string outputPath;
    std::string realtimeMode;

    std::string applicationVersion;
    std::string buildId;
    std::string operatingSystem;
};

struct ExperimentReadinessSnapshot {
    uint64_t generation{0};        // readiness evaluation generation
    uint64_t evaluatedHostTimeUs{0};
    bool ready{false};             // no gate blocks start
    std::vector<ReadinessGate> gates;
    RunConfigurationSnapshot candidate; // identities the evaluation was based on

    const ReadinessGate* gate(const std::string& id) const
    {
        for (const auto& g : gates) if (g.id == id) return &g;
        return nullptr;
    }
    std::vector<std::string> blockingGateIds() const
    {
        std::vector<std::string> ids;
        for (const auto& g : gates) if (g.blocksStart()) ids.push_back(g.id);
        return ids;
    }
};

// Typed outcome of the Start transaction. Contract-pinned
// (bridge-contract.json experiment_start_outcomes); append only.
enum class ExperimentStartOutcome {
    Started = 0,
    NotReady = 1,           // a gate blocks start (see readiness)
    StaleReadiness = 2,     // presented generation no longer matches current state
    AlreadyActive = 3,
    StorageFailed = 4,      // HDF5 open/init failed (rolled back)
    ProvenanceFailed = 5,   // run snapshot could not be persisted (rolled back)
    Busy = 6,               // another start/stop transaction is in progress
};

inline const char* toString(ExperimentStartOutcome o)
{
    switch (o) {
    case ExperimentStartOutcome::Started: return "started";
    case ExperimentStartOutcome::NotReady: return "notReady";
    case ExperimentStartOutcome::StaleReadiness: return "staleReadiness";
    case ExperimentStartOutcome::AlreadyActive: return "alreadyActive";
    case ExperimentStartOutcome::StorageFailed: return "storageFailed";
    case ExperimentStartOutcome::ProvenanceFailed: return "provenanceFailed";
    case ExperimentStartOutcome::Busy: return "busy";
    }
    return "unknown";
}

struct ExperimentStartResult {
    ExperimentStartOutcome outcome{ExperimentStartOutcome::Busy};
    std::string message;
    ExperimentReadinessSnapshot readiness; // the re-evaluation performed at start
    RunConfigurationSnapshot run;          // valid when outcome == Started
    bool started() const { return outcome == ExperimentStartOutcome::Started; }
};

// Lifecycle state of the coordinator. Values are the bridge contract's
// experiment_states (bridge-contract.json); append only, never renumber.
enum class ExperimentRunState { Idle = 0, Starting = 1, Active = 2, Stopping = 3, Failed = 4 };

inline const char* toString(ExperimentRunState s)
{
    switch (s) {
    case ExperimentRunState::Idle: return "idle";
    case ExperimentRunState::Starting: return "starting";
    case ExperimentRunState::Active: return "active";
    case ExperimentRunState::Stopping: return "stopping";
    case ExperimentRunState::Failed: return "failed";
    }
    return "unknown";
}

// Typed outcome of requestStop().
enum class ExperimentStopOutcome { Accepted = 0, NotActive = 1, Busy = 2 };

inline const char* toString(ExperimentStopOutcome o)
{
    switch (o) {
    case ExperimentStopOutcome::Accepted: return "accepted";
    case ExperimentStopOutcome::NotActive: return "notActive";
    case ExperimentStopOutcome::Busy: return "busy";
    }
    return "unknown";
}

// Pullable lifecycle snapshot; also delivered through the coordinator's
// status callback on every transition. Qt-free by design so the bridge can
// carry it verbatim.
struct ExperimentStatus {
    ExperimentRunState state{ExperimentRunState::Idle};
    uint64_t startGeneration{0};
    uint64_t readinessGeneration{0};
    uint64_t captureGeneration{0};
    std::string outputPath;
    uint64_t startWallClockNs{0};     // 0 until known
    uint64_t endWallClockNs{0};
    uint64_t validBuffered{0};        // live, from ProcessingService
    uint64_t invalidBuffered{0};
    uint64_t persistenceAdmitted{0};
    uint64_t persistenceCommitted{0};
    uint64_t persistenceFailed{0};
    bool flushing{false};
    bool cancelled{false};
    bool terminal{false};             // finalization finished (Idle or Failed)
    bool finalizationOk{false};       // every finalize step succeeded
    backend::recording::RunCompletionState completion{backend::recording::RunCompletionState::Unknown};
    std::string completionReason;
    std::string faultCode;            // unresolved fault, if any
    std::string faultMessage;
    std::string message;              // last human-readable transition note
};

// Serialize the frozen run snapshot for HDF5 provenance (stable key order).
std::string runSnapshotToJson(const RunConfigurationSnapshot& snapshot);
std::string readinessToJson(const ExperimentReadinessSnapshot& readiness);

} // namespace backend::app
