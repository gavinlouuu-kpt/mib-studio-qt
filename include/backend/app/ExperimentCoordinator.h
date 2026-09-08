// Backend-owned experiment readiness + Start/Stop transaction (issue #369;
// host-SDK portion of #274).
//
// One serialized transaction owner: evaluateReadiness() produces a
// generation-tagged ExperimentReadinessSnapshot from the actual backend state;
// start() re-evaluates, refuses a stale generation (any invalidation input
// changed since the presented evaluation), opens persistence, freezes the
// immutable RunConfigurationSnapshot, persists it, and only then enters
// Running. Failure at any step rolls back without publishing a run.
#pragma once

#include "backend/app/ExperimentReadiness.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace backend {
class AppBackend;
}

namespace backend::app {

struct ExperimentStartRequest {
    std::string outputPath;         // HDF5 destination (extension normalized)
    uint64_t readinessGeneration{0}; // generation the caller preflighted with
    std::string profileId;          // frontend profile identity (provenance only)
    bool acknowledgeLatestFrameDrops{false}; // operator accepted LatestFrame policy
};

class ExperimentCoordinator {
public:
    explicit ExperimentCoordinator(AppBackend& backend);

    // Application identity recorded in every run snapshot.
    void setApplicationIdentity(std::string version, std::string buildId, std::string os);

    // Evaluate every gate against the current backend state. Cheap; safe to
    // call from a UI timer. Each call increments the readiness generation
    // only when an invalidation input changed (so a stable state keeps its
    // generation and a preflight stays usable).
    ExperimentReadinessSnapshot evaluateReadiness(const std::string& outputPath = {},
                                                  const std::string& profileId = {});

    // The Start transaction. Serialized; TOCTOU-free with respect to the
    // presented readiness generation.
    ExperimentStartResult start(const ExperimentStartRequest& request);

    // Mark the run finished (the caller drives finalization through the
    // existing stop path; this releases the frozen snapshot and returns it).
    std::optional<RunConfigurationSnapshot> finish();

    // Lifecycle status (issue #372 G2/G3). The callback fires on every
    // transition, outside the coordinator mutex; consumers must not block
    // (same rule as the facade event sink).
    using StatusCallback = std::function<void(const ExperimentStatus&)>;
    void setStatusCallback(StatusCallback cb);
    ExperimentStatus status() const;

    ExperimentRunState state() const;
    // Frozen snapshot of the active run (empty when Idle).
    std::optional<RunConfigurationSnapshot> activeRun() const;
    uint64_t readinessGeneration() const { return readinessGeneration_.load(); }

    // Fault sink: an unresolved lifecycle/save/data-integrity fault blocks
    // readiness until cleared.
    void reportUnresolvedFault(const std::string& code, const std::string& message);
    void clearUnresolvedFault();
    bool hasUnresolvedFault() const;

private:
    // Everything readiness depends on, in one comparable value.
    struct InvalidationKey {
        uint64_t captureGeneration{0};
        bool cameraReady{false};
        std::string cameraSource;
        bool cameraFallback{false};
        std::string deliveryMode;
        uint64_t processingConfigVersion{0};
        std::string configJsonSha256;
        std::string coreVersion;
        std::string coreSha256;
        bool corePinSatisfied{false};
        uint64_t backgroundGeneration{0};
        int roiX{0}, roiY{0}, roiW{0}, roiH{0};
        double pixelToMicron{0.0};
        std::string outputPath;
        std::string profileId;
        bool faulted{false};
        bool operator==(const InvalidationKey& o) const;
        bool operator!=(const InvalidationKey& o) const { return !(*this == o); }
    };
    InvalidationKey currentKeyLocked(const std::string& outputPath, const std::string& profileId) const;
    ExperimentReadinessSnapshot evaluateLocked(const std::string& outputPath,
                                               const std::string& profileId);
    RunConfigurationSnapshot candidateLocked(const std::string& outputPath,
                                             const std::string& profileId) const;
    ExperimentStatus snapshotLocked() const;
    // Publish the current snapshot: copies it, releases `lk`, invokes the
    // callback, re-acquires `lk`.
    void publishLocked(std::unique_lock<std::mutex>& lk, const char* message);

    AppBackend& backend_;
    mutable std::mutex mutex_;
    std::atomic<uint64_t> readinessGeneration_{0};
    InvalidationKey lastKey_;
    bool haveLastKey_{false};
    ExperimentRunState state_{ExperimentRunState::Idle};
    uint64_t startCounter_{0};
    std::optional<RunConfigurationSnapshot> activeRun_;
    std::string appVersion_{"unknown"};
    std::string buildId_;
    std::string os_;
    bool faultActive_{false};
    std::string faultCode_;
    std::string faultMessage_;
    // Terminal/lifecycle fields that outlive activeRun_ (reset on start).
    ExperimentStatus status_;
    mutable std::mutex callbackMutex_;
    StatusCallback statusCallback_;
};

} // namespace backend::app
