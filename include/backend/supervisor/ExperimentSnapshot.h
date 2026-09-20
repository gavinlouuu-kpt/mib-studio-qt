// AI Experiment Supervisor — frozen, versioned, serializable experiment
// state snapshot (issue #422, ADR 0006).
//
// A snapshot is exactly what was known at one decision point. It is a plain
// value: immutable once handed to a provider, canonically serializable (so an
// identical snapshot always yields the identical JSON and hash) and replayable
// offline. Every metric the backend may not be able to observe is a
// `Metric` with `known == false` (serialized as null) — an unknown is never
// stored or fed to a model as a measured zero (same rule as
// services::MetricSample, issue #368).
//
// The builder (ExperimentSnapshotBuilder) only copies metrics the
// authoritative backend already produces; nothing here adds scientific
// processing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::supervisor {

inline constexpr uint32_t kExperimentSnapshotSchemaVersion = 1;

struct Metric {
    double value{0.0};
    bool known{false};

    static Metric of(double v) { return Metric{v, true}; }
    static Metric unknown() { return Metric{}; }
    std::optional<double> get() const
    {
        return known ? std::optional<double>(value) : std::nullopt;
    }
    bool operator==(const Metric& o) const
    {
        return known == o.known && (!known || value == o.value);
    }
};

// A recommendation the supervisor produced earlier in the same run. Only the
// selected labels are carried (no distributions) so the model sees its own
// history without the input growing unboundedly.
struct PriorRecommendation {
    uint64_t sequence{0};
    uint64_t elapsedSeconds{0};
    std::string action;    // NextAction token
    std::string target;    // AdjustmentTarget token
    std::string direction; // AdjustmentDirection token
    bool executed{false};  // always false in shadow mode
};

struct ExperimentSnapshot {
    // ---- identity / state ------------------------------------------------
    uint32_t schemaVersion{kExperimentSnapshotSchemaVersion};
    uint64_t sequence{0};             // per-run decision-point counter
    std::string runId;                // experiment/run identity (output path or generation)
    uint64_t startGeneration{0};
    std::string experimentState;      // app::ExperimentRunState token
    uint64_t hostTimeUs{0};           // Tools::getTimestamp when frozen
    uint64_t wallClockNs{0};
    double elapsedSeconds{0.0};
    std::string objective;            // free text set by the operator ("collect 500 valid cells")
    std::optional<uint64_t> targetValidObjects; // when the objective is a count

    // ---- current configuration (bounded subset; never hardware values the
    // model could echo back as commands) --------------------------------
    struct Configuration {
        std::string cameraSource;      // "mock" | "mindvision" | "egrabber" | "unknown"
        bool simulated{false};
        std::string deliveryMode;
        Metric requestedFps;
        Metric exposureUs;
        int roiWidth{0}, roiHeight{0};
        int detectionThreshold{0};     // ProcessingConfig::bg_subtract_threshold
        int minAreaUm2{0}, maxAreaUm2{0};
        bool areaRangeCheck{false};
        bool triggerEnabled{false};    // target-group trigger configured
        std::string processingCoreVersion;
        std::string configSha256;
    } configuration;

    // ---- camera / acquisition --------------------------------------------
    struct Acquisition {
        std::string captureState;      // CaptureLifecycleState token
        bool cameraReady{false};
        std::string lastFailure;       // CaptureFailureKind token ("none" when healthy)
        Metric measuredFps;
        Metric framesDelivered;
        Metric transportLostFrames;
        Metric intentionallyDiscardedFrames;
        Metric bufferUnderruns;
        Metric sdkQueueDepth;
        Metric sdkInputBuffers;
        Metric frameAgeUs;
        Metric publishLatencyUs;
    } acquisition;

    // ---- detection ------------------------------------------------------------
    struct Detection {
        Metric framesProcessed;        // frames available to the realtime loop (== frames delivered by capture)
        Metric validObjects;
        Metric invalidObjects;         // scientifically rejected
        Metric droppedValid;
        Metric droppedInvalid;
        Metric processingFailures;
        Metric algoFps;
        Metric validPerSecond;
        Metric invalidPerSecond;
        Metric processingTimeUs;       // mean per-frame algorithm time
        // Statistics over a recent window of accepted objects (unknown when
        // no window is available, e.g. monitoring off).
        uint64_t windowObjects{0};
        Metric areaMeanUm2, areaMinUm2, areaMaxUm2;
        Metric contrastMean;           // Q3 - Q1 brightness within the mask
        Metric brightnessMedianMean;   // Q2 mean
        Metric brightnessMaxMean;      // Q4 mean (saturation indicator)
        Metric laplacianVarianceMean;  // unknown unless the core produces it
        std::vector<std::string> rejectionReasons; // "border", "area", "ring_ratio", ...
        std::vector<uint64_t> rejectionCounts;     // parallel to rejectionReasons
    } detection;

    // ---- trigger --------------------------------------------------------------
    struct Trigger {
        bool cameraBound{false};
        Metric eligibleObjects;        // target-group hits
        Metric triggersIssued;
        Metric suppressedRequests;     // evicted from the pending queue
        Metric droppedPulses;          // no camera / set failed
        Metric staleRequests;
        Metric lastOnsetUs;            // detection -> pulse latency
    } trigger;

    // ---- recording / system -----------------------------------------------------
    struct Recording {
        std::string state;             // "idle" | "active" | "stopping" | "failed" | ...
        bool storageReady{false};
        std::string outputPath;
        Metric persistenceAdmitted;
        Metric persistenceCommitted;
        Metric persistenceFailed;
        Metric validBuffered;
        Metric invalidBuffered;
        std::string faultCode;         // unresolved fault, empty when none
        std::string faultMessage;
    } recording;

    std::vector<PriorRecommendation> priorRecommendations;
};

// Canonical JSON (sorted keys, fixed number formatting). Same snapshot ->
// same bytes.
std::string snapshotToJson(const ExperimentSnapshot& snapshot);
// Strict parse: schema version must match; unknown/malformed input returns
// false with `error` set. Missing metrics stay unknown.
bool snapshotFromJson(const std::string& json, ExperimentSnapshot& out, std::string& error);
// SHA-256 of snapshotToJson(snapshot), lowercase hex.
std::string snapshotHash(const ExperimentSnapshot& snapshot);

// Derived quantities shared by the policy, the rule provider and the report.
// Each is nullopt when an input is unknown.
std::optional<double> frameLossFraction(const ExperimentSnapshot& s);
std::optional<double> rejectionFraction(const ExperimentSnapshot& s);
std::optional<double> triggerSuccessFraction(const ExperimentSnapshot& s);

} // namespace backend::supervisor
