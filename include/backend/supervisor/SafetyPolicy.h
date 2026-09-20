// AI Experiment Supervisor — deterministic policy layer (issue #422).
//
// Hard invariants are evaluated in ordinary code before any provider is
// asked, and the returned answers are checked against the same limits
// afterwards. The model never overrides a policy outcome: when the policy
// requires an action, that outcome *is* the decision and the provider is not
// consulted (or its answer is recorded for comparison only).
#pragma once

#include "backend/supervisor/DecisionContract.h"
#include "backend/supervisor/ExperimentSnapshot.h"

#include <string>
#include <vector>

namespace backend::supervisor {

struct PolicyOutcome {
    bool requiresAction{false};       // true: deterministic decision, model not authoritative
    std::string ruleId;               // "policy.frame_loss", "policy.camera_unavailable", ...
    std::string reason;
    DecisionAnswers answers;          // valid only when requiresAction
    std::vector<std::string> rulesChecked; // every rule evaluated, for the audit record
};

// Evaluate every hard invariant against the snapshot. Rules are checked in a
// fixed order; the first that fires wins (the order is part of the contract
// and documented in knowledge_map/services/SupervisorService.md).
PolicyOutcome evaluateSafetyPolicy(const ExperimentSnapshot& snapshot, const DecisionPolicy& policy);

// Whether a recommendation would be *eligible* for execution under the
// policy (bounded targets, coherent ADJUST fields, not below the empirical
// probability threshold). Shadow mode records this flag but never acts on it.
struct EligibilityOutcome {
    bool eligible{false};
    std::string reason; // why not (empty when eligible)
};
EligibilityOutcome evaluateEligibility(const DecisionResult& result, const DecisionPolicy& policy);

} // namespace backend::supervisor
