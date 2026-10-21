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

#include "backend/app/CalibrationMethodSpec.h"
#include "backend/processing/IProcessingKernel.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/recording/RunProvenanceRecord.h"

#include <cstdint>
#include <optional>
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

// The experiment method a run used (#398 M2); resolved and gated in
// MethodProvenance.h. Plain values so the snapshot stays copyable.
struct MethodProvenance {
    std::string source{"none"};          // "central" | "local" | "none" (no config applied)
    std::string match;                   // "config_json" when central
    std::string configCanonicalSha256;   // canonical hash of the applied config.json
    std::string registryOrigin;
    std::string registrySession;         // worker session at evaluation ("signed_in", ...)
    // Central revision (source == "central").
    std::string revisionId;
    std::string methodId;
    std::string projectId;
    std::string displayName;
    std::string authorId;
    std::string contentHash;
    uint64_t revisionNumber{0};
    uint64_t metadataVersion{0};
    std::string centralState;            // "published", "revoked", ...
    uint32_t matchingRevisions{0};       // cached revisions sharing this config
    // Local validation on this instrument + context.
    std::string instrumentId;
    std::string instrumentName;
    std::string contextHash;
    std::string validation{"notApplicable"}; // "passed" | "failed" | "none" | "notApplicable"
    std::string validatorId;
    std::string validatedAtUtc;
    std::string validationEvidenceSha256; // test-run file hash from the evidence
    // The local profile the applied config came from (#783; source == "local" only): the
    // `profile_selection` ProfileStore::apply records in the applied config text (its name,
    // config.json path, revision = sha256 of the profile's files, and profile_id = its metadata
    // profile_id or its name). All empty when the local config names no profile (e.g. a config
    // applied from Settings or the retired Qt watcher). attachLocalProfile fills them.
    std::string localProfileName;
    std::string localProfilePath;
    std::string localProfileRevision;
    std::string localProfileId;
    // The applied text no longer is what the profile's apply left (a later settings apply changed
    // it), or carries no fingerprint: the profile is the base, not the exact method; its revision
    // is not claimed for the run's config (config hash: configCanonicalSha256).
    bool localProfileModified{false};
    bool hasLocalProfile() const { return !localProfileName.empty() || !localProfileId.empty(); }
};

// Frozen identities/settings of one run. Owned by the coordinator; the UI
// only reads it.
struct RunConfigurationSnapshot {
    // v2 (#398 M2): adds the "method" block. v3 (#712): adds the `v3` members below.
    static constexpr uint32_t kSchemaVersion = recording::kRunSnapshotSchemaVersion;
    static_assert(kSchemaVersion == 3, "run snapshot schema: update runSnapshotToJson and HDF5-Storage.md together");
    uint64_t readinessGeneration{0};
    uint64_t startGeneration{0};      // coordinator start counter
    uint64_t captureGeneration{0};    // CaptureService session generation
    uint64_t startHostTimeUs{0};
    uint64_t startWallClockNs{0};
    std::string wallClockSource;      // client_sync | board_clock_unsynced | system_clock (G14)
    int64_t wallClockOffsetNs{0};
    uint32_t ssdRunId{0};             // the SATA SSD run-table id this run records to (#667 S2; 0: no SSD run). Also the RUN_ID of its store records.
    // PZ7035 Run timing the camera runs at (bridge ABI 38; zeros outside an instrument Run): the configured frame rate, the exposure ("auto": ~50 µs after XVS, SHS 64 / 32),
    // the line period and frame length the producer was given. Recorded in the run snapshot (HDF5) beside the run table's tick_hz.
    double runFps{0.0}, runExposureUs{0.0};
    int runHmax{0}, runVmax{0}, runShs{0};
    bool runExposureAuto{true}, runPending{false}; // exposure "auto" or a number; the camera runs at another setting than the configured one

    CameraSourceInfo camera;
    bool cameraReady{false};
    std::string deliveryModeRequested;
    std::string deliveryModeActive;
    bool deliveryModeConfirmed{false};
    std::string timestampDescriptor;  // camera::common::describe()

    int roiX{0}, roiY{0}, roiW{0}, roiH{0};
    bool roiPending{false}; // an applied ROI awaits the first frame (#398 M2c)
    std::string roiNotice;  // why a pending ROI was dropped, if it was
    uint64_t frameWidth{0}, frameHeight{0}, pixelFormat{0};
    bool frameGeometryKnown{false};

    backend::processing::ProcessingCoreIdentity processingCore;
    bool processingCorePinSatisfied{false};
    uint64_t processingConfigVersion{0};
    std::string processingConfigSha256;  // canonical serialization of ProcessingConfig
    std::string configJsonSha256;        // raw config.json as last applied
    // The profile identity: the frontend-supplied one; when it supplies none, the applied local
    // profile's profile_id (#783, method.localProfileId); empty when neither names one.
    std::string profileId;
    MethodProvenance method;             // central revision / local method (#398 M2)
    double pixelToMicron{0.0};

    bool backgroundPresent{false};
    uint64_t backgroundGeneration{0};
    std::string backgroundSha256;

    bool triggerRequired{false};
    bool triggerBound{false};
    uint64_t triggerGeneration{0};

    // RF sort generator (SSG3021X) as read back over SCPI at evaluation time.
    // Configured = an rf_generator block enables the link; connected = the
    // instrument identified and answered the readback.
    bool rfGeneratorConfigured{false};
    bool rfGeneratorConnected{false};
    std::string rfGeneratorIdentity;
    std::string rfGeneratorTriggerMode;
    double rfGeneratorTriggerDelayS{0.0};
    double rfGeneratorPulseWidthS{0.0};
    std::string rfGeneratorError;               // last link error when not connected
    std::vector<std::string> rfGeneratorIssues; // blocking preflight findings ("gate: message")

    std::string outputPath;
    std::string realtimeMode;
    // Where the science ran ("host" | "pl") and, for the PL, the execution
    // provider that brought its results to the PS (YOFO S3 provenance).
    std::string sciencePlacement;
    std::string executionProvider;
    // The PL core (ADR 0011): build and weights ids from the device registers.
    bool plCoreValid{false};
    uint32_t plAbiVersion{0};
    uint16_t plScienceProfile{0};
    uint16_t plProfileVersion{0};
    std::string plBuildId;
    std::string plWeightsId;

    std::string applicationVersion;
    std::string buildId;
    std::string operatingSystem;

    // Snapshot v3 (#712, ADR 0015 item 5): the run's own id, the E-modulus LUT identity, the
    // device facts at Start and the run id armed into the PL. Filled by the Start transaction only
    // (a readiness candidate leaves them unknown). Unknown is nullopt, written as null.
    recording::RunProvenanceV3 v3;
};

struct ExperimentReadinessSnapshot {
    uint64_t generation{0};        // readiness evaluation generation
    uint64_t evaluatedHostTimeUs{0};
    bool ready{false};             // no gate blocks start
    std::vector<ReadinessGate> gates;
    RunConfigurationSnapshot candidate; // identities the evaluation was based on
    // The applied calibration record in effect at this evaluation (#716 A4a, gate
    // `calibration.record`): its identity matches and its factor is the current factor. Start
    // records it as the run snapshot's calibration_record_id and refuses a declared id that differs.
    std::optional<std::string> calibrationRecordId;
    // The applied method's calibration declarations as this evaluation's `calibration.record`
    // gate read them (#776, bridge ABI 43: the readiness report's `method_calibration`). No method
    // applied, or one that declares nothing: the defaults (measurement, none) with `declared`
    // false. A declaration that cannot be read: the defaults, with the reason in
    // methodCalibrationError (the gate then fails closed).
    CalibrationMethodSpec methodCalibration;
    std::string methodCalibrationError;

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
    InvalidRequest = 7,     // the start request's declared identity/annotation is refused (#712, ABI 39),
                            // or its output path is empty or leaves the storage root (#762)
};

// How the operator frees the HDF5 library when a file is open for review (F2 of #763): the
// storage.hdf5 gate, Start's refusal and raw recording's refusal all say it. HDF5 is built without
// thread safety (#722), so the refusal itself stays (D1 (a)). UTF-8 "→" spelled as bytes.
inline constexpr const char* kCloseReviewFile = "Close the file in Review (Review \xE2\x86\x92 Close File)";

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
    case ExperimentStartOutcome::InvalidRequest: return "invalidRequest";
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
enum class ExperimentStopOutcome {
    Accepted = 0,
    NotActive = 1,
    Busy = 2,
    // The stop named a run (expected start generation, #741, ABI 40) that is not the run the
    // coordinator holds: another run is active or finalizing, or none is. Nothing was stopped.
    RunMismatch = 3,
};

inline const char* toString(ExperimentStopOutcome o)
{
    switch (o) {
    case ExperimentStopOutcome::Accepted: return "accepted";
    case ExperimentStopOutcome::NotActive: return "notActive";
    case ExperimentStopOutcome::Busy: return "busy";
    case ExperimentStopOutcome::RunMismatch: return "runMismatch";
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
    uint64_t validSaved{0};
    uint64_t invalidSaved{0};
    uint64_t droppedValid{0};
    uint64_t droppedInvalid{0};
    uint64_t persistenceAdmitted{0};
    uint64_t persistenceCommitted{0};
    uint64_t persistenceFailed{0};
    bool flushing{false};
    bool cancelled{false};
    bool terminal{false};             // finalization finished (Idle or Failed)
    bool finalizationOk{false};       // every finalize step succeeded
    backend::recording::RunCompletionState completion{backend::recording::RunCompletionState::Unknown};
    std::string completionReason;
    uint64_t faultRevision{0}; // monotonic occurrence identity, including repeated identical faults
    std::string faultCode;            // unresolved fault, if any
    std::string faultMessage;
    std::string message;              // last human-readable transition note
};

// Serialize the frozen run snapshot for HDF5 provenance (stable key order).
std::string runSnapshotToJson(const RunConfigurationSnapshot& snapshot);
std::string readinessToJson(const ExperimentReadinessSnapshot& readiness);

} // namespace backend::app
