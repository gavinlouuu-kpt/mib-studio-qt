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
// All-or-nothing bounded evaluation using exact keys and full revision match.
// At most 256 nodes, 10k events and 1M node-events; no hidden subsampling.
std::map<std::string, Membership> evaluatePlan(const PopulationPlan& plan,
                                               const Membership& universe, const Membership& qc,
                                               const std::map<std::string, Membership>& predicates);
} // namespace backend::analysis
