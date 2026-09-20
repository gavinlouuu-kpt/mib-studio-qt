// AI Experiment Supervisor — durable decision audit record (issue #422).
//
// Every evaluation (policy-only or provider-backed) produces one
// DecisionRecord. Records are appended to a versioned JSON-lines sidecar next
// to the experiment file (`<run>.supervisor.jsonl`) so the stable HDF5 schema
// is untouched; the eventual provenance integration is documented in
// docs/decisions/0006-ai-experiment-supervisor.md. Shadow-mode records carry
// `executed == false` and `mode == "shadow"` explicitly.
#pragma once

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/ExperimentSnapshot.h"
#include "backend/supervisor/SafetyPolicy.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backend::supervisor {

inline constexpr uint32_t kDecisionRecordSchemaVersion = 1;

enum class SupervisorMode { Off = 0, Shadow = 1 };
const char* toString(SupervisorMode m);
std::optional<SupervisorMode> parseSupervisorMode(std::string_view token);

struct DecisionRecord {
    uint32_t recordSchemaVersion{kDecisionRecordSchemaVersion};
    uint64_t sequence{0};
    uint64_t wallClockNs{0};
    uint64_t hostTimeUs{0};
    std::string mode{"shadow"};
    std::string runId;
    // Snapshot identity (the snapshot itself is stored inline so the exact
    // decision can be replayed offline).
    std::string snapshotHash;
    uint32_t snapshotSchemaVersion{kExperimentSnapshotSchemaVersion};
    ExperimentSnapshot snapshot;
    // Deterministic policy.
    PolicyOutcome policy;
    // Provider (absent when the policy decided and the provider was skipped).
    bool providerConsulted{false};
    DecisionResult provider;
    // Final recommendation after policy precedence.
    DecisionAnswers recommendation;
    std::string decidedBy;            // "policy" | "provider" | "fail_closed"
    EligibilityOutcome eligibility;
    bool executed{false};             // always false in shadow mode
    // Later associations (filled by the service when an operator/system
    // action or an outcome label becomes known).
    std::string operatorAction;       // free text or NextAction token
    uint64_t operatorActionWallClockNs{0};
    std::string outcomeLabel;         // later ground truth, when available
};

std::string recordToJson(const DecisionRecord& record);
bool recordFromJson(const std::string& json, DecisionRecord& out, std::string& error);

// Append-only JSON-lines sidecar. First line is a header object
// ({"supervisor_log": 1, ...}); every following line is one DecisionRecord.
// Thread-safe; a write failure is reported, never thrown.
class DecisionLog {
public:
    DecisionLog() = default;
    ~DecisionLog();

    bool open(const std::string& path, const std::string& runId, SupervisorMode mode,
              const std::string& providerName, const std::string& providerVersion,
              const std::string& modelVersion, std::string* error = nullptr);
    void close();
    bool isOpen() const;
    std::string path() const;

    bool append(const DecisionRecord& record, std::string* error = nullptr);
    uint64_t recordsWritten() const;

    // Offline reader (harness / tests). Skips the header line.
    static bool readAll(const std::string& path, std::vector<DecisionRecord>& out,
                        std::string& error);

private:
    mutable std::mutex mutex_;
    std::string path_;
    void* file_{nullptr}; // FILE*, kept opaque to avoid <cstdio> in the header
    uint64_t written_{0};
};

} // namespace backend::supervisor
