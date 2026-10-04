// Applying and validating a central method revision (#398 M2b). Qt-free.
//
// planMethodApply: the shell asks "what would applying revision X do?". The
// answer comes from the registry worker's value snapshot plus the materialized
// files: refused unless the revision is cached, published/superseded and
// materialized, and the materialized config.json still hashes to the
// revision's config (tampered files are never applied). It lists the
// config.json keys that would change so the operator confirms knowingly
// (instrument keys such as autofocus_com_port or save_directory travel with
// the method). Writing config.json stays with the shell's config applier
// (Qt: AppConfigWatcher), which also records the applied text the coordinator
// matches.
//
// checkValidationEvidence: "Mark validated" only accepts a test run whose
// frozen /run_provenance names this exact revision (id + content hash), was
// recorded on this instrument, and under the current method context.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace backend::profiles {
struct RegistryWorkerSnapshot;
struct CachedRevisionSummary;
} // namespace backend::profiles

namespace backend::app {

struct MethodApplyPlan {
    bool ok{false};
    std::string error; // why not (empty when ok)
    std::string revisionId;
    std::string displayName;
    uint64_t revisionNumber{0};
    std::string centralState;
    std::string configText;              // exact materialized config.json bytes
    std::string cameraScriptPath;        // materialized egrabberConfig.js (not applied here)
    std::vector<std::string> changedKeys; // dotted paths that differ from the current config
};

MethodApplyPlan planMethodApply(const profiles::RegistryWorkerSnapshot& registry,
                                const std::string& revisionId,
                                const std::string& currentConfigJson);

// Dotted JSON paths whose value differs between two config documents
// (objects recurse; arrays and scalars compare whole). Unparsable input on
// either side yields {"<entire document>"}.
std::vector<std::string> configDifferences(const std::string& currentJson,
                                           const std::string& nextJson);

// The newest local validation of `revision` on this instrument under this
// context (what the method.revision gate would see). Contract-pinned
// (bridge-contract.json registry_local_validation); append only.
enum class LocalValidationState { None = 0, Passed = 1, Failed = 2 };
struct LocalValidationView {
    LocalValidationState state{LocalValidationState::None};
    std::string validatorId;
    std::string validatedAtUtc;
};
LocalValidationView localValidationFor(const profiles::RegistryWorkerSnapshot& registry,
                                       const profiles::CachedRevisionSummary& revision,
                                       const std::string& instrumentId,
                                       const std::string& contextHash);

// Empty when `runSnapshotJson` (the evidence file's run_snapshot_json) is
// acceptable evidence for validating `revisionId`/`contentHash` on
// `instrumentId` under `contextHash`; otherwise the reason.
std::string checkValidationEvidence(const std::string& runSnapshotJson,
                                    const std::string& revisionId,
                                    const std::string& contentHash,
                                    const std::string& instrumentId,
                                    const std::string& contextHash);

} // namespace backend::app
