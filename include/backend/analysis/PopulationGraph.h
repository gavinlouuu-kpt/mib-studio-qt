#pragma once

#include "backend/analysis/Population.h"
#include <map>

namespace backend::analysis {
// Internal scientific graph contract; not a bridge ABI or a workspace store.
// Empty parent means the input universe. Every population is clipped to its
// parent and QC. Predicate nodes name independently evaluated immutable masks.
enum class NodeKind { Predicate, And, Or, Not };
struct PopulationNode {
    std::string id;
    std::string parent;
    NodeKind kind{NodeKind::Predicate};
    std::string predicate;
    std::vector<std::string> operands;
};
struct PopulationPlan {
    std::string id;
    std::string revision;
    std::vector<PopulationNode> nodes;
};
// Returns topological IDs; rejects cycles (including parent edges), dangling
// references, invalid arity, duplicate IDs and unused predicate fields.
std::vector<std::string> validatePlan(const PopulationPlan& plan);
// Explicit bounds of the keyed (string-key, row-object) path, which carries
// worker acceptance and JSON sources. Each keyed node-event costs a key string
// (~64 B), so these cap it near 1 GiB. Full recordings never take this path:
// ColumnarEvaluation.h evaluates them in chunks into two-bit membership.
constexpr std::size_t kMaxKeyedEvaluationEvents = 1000000;
constexpr std::size_t kMaxKeyedNodeEvents = 16777216;
// All-or-nothing bounded evaluation using exact keys and full revision match.
// At most 256 nodes and the keyed bounds above; no hidden subsampling.
std::map<std::string, Membership> evaluatePlan(const PopulationPlan& plan,
                                               const Membership& universe, const Membership& qc,
                                               const std::map<std::string, Membership>& predicates);
} // namespace backend::analysis
