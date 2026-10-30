#include "backend/analysis/PopulationGraph.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace backend::analysis {
std::vector<std::string> validatePlan(const PopulationPlan& plan) {
    if (plan.id.empty() || plan.revision.empty() || plan.nodes.size() > 256)
        throw std::invalid_argument("Invalid or oversized population plan");
    std::map<std::string, const PopulationNode*> nodes;
    for (const auto& node : plan.nodes) {
        if (node.id.empty() || !nodes.emplace(node.id, &node).second)
            throw std::invalid_argument("Empty or duplicate population ID");
        if (node.kind == NodeKind::Predicate) {
            if (node.predicate.empty() || !node.operands.empty())
                throw std::invalid_argument("Invalid predicate population");
        } else {
            const auto arity = node.kind == NodeKind::Not ? 1u : 2u;
            if ((node.kind != NodeKind::Not && node.kind != NodeKind::And &&
                 node.kind != NodeKind::Or) ||
                !node.predicate.empty() || node.operands.size() != arity)
                throw std::invalid_argument("Invalid Boolean population");
        }
    }
    std::map<std::string, std::unordered_set<std::string>> dependencies;
    for (const auto& node : plan.nodes) {
        auto& deps = dependencies[node.id];
        if (!node.parent.empty()) deps.insert(node.parent);
        for (const auto& operand : node.operands)
            deps.insert(operand);
        for (const auto& dep : deps)
            if (!nodes.count(dep)) throw std::invalid_argument("Dangling population dependency");
    }
    std::vector<std::string> order;
    std::unordered_set<std::string> done;
    while (order.size() < nodes.size()) {
        const auto before = order.size();
        for (const auto& node : plan.nodes) {
            if (done.count(node.id)) continue;
            const auto& deps = dependencies.at(node.id);
            if (std::all_of(deps.begin(), deps.end(),
                            [&](const auto& dep) { return done.count(dep); })) {
                done.insert(node.id);
                order.push_back(node.id);
            }
        }
        if (order.size() == before) throw std::invalid_argument("Population dependency cycle");
    }
    return order;
}
std::map<std::string, Membership>
evaluatePlan(const PopulationPlan& plan, const Membership& universe, const Membership& qc,
             const std::map<std::string, Membership>& predicates) {
    const auto order = validatePlan(plan);
    if (universe.revision.plan != plan.revision)
        throw std::invalid_argument("Plan revision does not match snapshot inputs");
    if (universe.rows.size() > 10000 ||
        (!plan.nodes.empty() && universe.rows.size() > 1000000 / plan.nodes.size()))
        throw std::invalid_argument("Population evaluation budget exceeded");
    std::vector<std::string> keys;
    for (const auto& row : universe.rows)
        keys.push_back(row.key);
    const auto root = acceptWorkerMembership(universe.revision, keys, universe);
    const auto quality = acceptWorkerMembership(universe.revision, keys, qc);
    std::map<std::string, const PopulationNode*> nodes;
    std::unordered_set<std::string> required;
    for (const auto& node : plan.nodes) {
        nodes.emplace(node.id, &node);
        if (node.kind == NodeKind::Predicate) required.insert(node.predicate);
    }
    if (required.size() != predicates.size())
        throw std::invalid_argument("Unexpected or missing predicate masks");
    // Preflight all masks before evaluating, even on an empty universe.
    std::map<std::string, Membership> masks;
    for (const auto& id : required) {
        const auto found = predicates.find(id);
        if (found == predicates.end()) throw std::invalid_argument("Missing predicate mask");
        masks.emplace(id, acceptWorkerMembership(universe.revision, keys, found->second));
    }
    std::map<std::string, Membership> results;
    for (const auto& id : order) {
        const auto& node = *nodes.at(id);
        const auto& parent = node.parent.empty() ? root : results.at(node.parent);
        if (node.kind == NodeKind::Predicate) {
            results.emplace(id, compose(parent, quality, masks.at(node.predicate), root,
                                        BooleanOperation::And));
        } else {
            const auto op = node.kind == NodeKind::And  ? BooleanOperation::And
                            : node.kind == NodeKind::Or ? BooleanOperation::Or
                                                        : BooleanOperation::Not;
            std::optional<Membership> right;
            if (node.operands.size() == 2) right = results.at(node.operands[1]);
            results.emplace(id, compose(parent, quality, results.at(node.operands[0]), right, op));
        }
    }
    return results;
}
} // namespace backend::analysis
