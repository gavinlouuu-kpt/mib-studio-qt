// AI Experiment Supervisor — shadow-mode service (issue #422, ADR 0006).
//
// Owns the decision provider, the deterministic policy, the audit log and a
// single worker thread that, while Shadow mode is enabled and a snapshot
// source is installed, freezes a snapshot at a bounded rate, evaluates the
// policy, consults the provider off the acquisition path, and appends a
// DecisionRecord. It holds no camera, trigger, processing, recording or
// hardware reference: the only inputs are a snapshot-producing function and a
// provider, so there is structurally no actuation path.
//
// Threading: configure/start/requestStop/status/recordOperatorAction are
// callable from any thread and never block on a provider call. shutdown()
// joins the worker; a provider that ignores cancel() and its own timeout can
// delay shutdown by at most its remaining timeout. Nothing here is ever
// invoked from acquisition-critical threads.
#pragma once

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/DecisionProvider.h"
#include "backend/supervisor/DecisionRecord.h"
#include "backend/supervisor/ExperimentSnapshot.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace backend::supervisor {

struct SupervisorConfig {
    SupervisorMode mode{SupervisorMode::Off}; // disabled by default
    int intervalMs{5000};                     // decision-point cadence (experiment-level, not frame-level)
    int minIntervalMs{500};                   // lower bound enforced by configure()
    bool onlyWhileExperimentActive{true};     // snapshots outside an active run are skipped
    std::string logPath;                      // "" = no sidecar (records kept in memory only)
    std::string runId;                        // recorded in the sidecar header
    std::string objective;
    std::optional<uint64_t> targetValidObjects;
    DecisionPolicy policy;
    std::size_t retainedRecords{64};
};

struct SupervisorStatus {
    SupervisorMode mode{SupervisorMode::Off};
    bool running{false};
    bool inFlight{false};                     // a provider call is in progress
    std::string providerName;
    std::string providerVersion;
    std::string modelVersion;
    uint64_t evaluations{0};
    uint64_t policyDecisions{0};
    uint64_t providerDecisions{0};
    uint64_t providerFailures{0};
    uint64_t skippedInactive{0};
    uint64_t logWriteFailures{0};
    std::string lastError;                    // last provider/log error text (never a credential)
    std::optional<DecisionRecord> lastRecord;
    uint64_t lastLatencyUs{0};
    // Always true: this service has no actuation authority.
    bool recommendationOnly{true};
};

// Decide once, synchronously, without a service: policy first, then provider
// (only when the policy does not require an action), then eligibility. Used
// by the worker, the harness and tests so every path decides identically.
DecisionRecord decideOnce(const ExperimentSnapshot& snapshot, const DecisionPolicy& policy,
                          DecisionProvider* provider, SupervisorMode mode);

class SupervisorService {
public:
    using SnapshotSource = std::function<std::optional<ExperimentSnapshot>(uint64_t sequence)>;
    using RecordObserver = std::function<void(const DecisionRecord&)>;

    SupervisorService();
    ~SupervisorService(); // shutdown()

    SupervisorService(const SupervisorService&) = delete;
    SupervisorService& operator=(const SupervisorService&) = delete;

    // Composition-time wiring. Replacing the provider while running is
    // refused (returns false).
    bool setProvider(std::unique_ptr<DecisionProvider> provider);
    DecisionProvider* provider() const;
    void setSnapshotSource(SnapshotSource source);
    uint64_t addObserver(RecordObserver observer);
    void removeObserver(uint64_t id);

    // Validates and applies the configuration. Mode changes take effect on
    // the next tick; a running worker is started/stopped as needed.
    bool configure(const SupervisorConfig& config, std::string* error = nullptr);
    SupervisorConfig config() const;

    // Start/stop the shadow worker. start() is a no-op unless mode == Shadow
    // and a provider + snapshot source are installed. requestStop() returns
    // immediately; shutdown() joins.
    bool start(std::string* error = nullptr);
    void requestStop();
    void shutdown();
    bool isRunning() const;

    // Synchronous single evaluation on the caller's thread (tests, harness,
    // an explicit operator "evaluate now"). Appends to the log like a tick.
    DecisionRecord evaluateNow(const ExperimentSnapshot& snapshot);

    // Associate an operator/system action with the most recent
    // recommendation (for later comparison). Never triggers anything.
    void recordOperatorAction(const std::string& action);

    SupervisorStatus status() const;
    std::vector<DecisionRecord> recentRecords() const;

private:
    void worker();
    void tick(uint64_t sequence);
    DecisionRecord finishRecord(DecisionRecord record);
    void notify(const DecisionRecord& record);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<DecisionProvider> provider_;
    SnapshotSource source_;
    SupervisorConfig config_;
    std::thread worker_;
    bool workerExit_{false};
    bool running_{false};
    std::atomic<bool> inFlight_{false};
    uint64_t nextSequence_{1};
    DecisionLog log_;
    std::vector<DecisionRecord> recent_;
    SupervisorStatus stats_;
    std::vector<PriorRecommendation> priors_;

    mutable std::mutex observersMutex_;
    std::vector<std::pair<uint64_t, RecordObserver>> observers_;
    uint64_t nextObserverId_{1};
};

} // namespace backend::supervisor
