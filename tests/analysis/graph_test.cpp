#include "backend/analysis/PopulationGraph.h"
#include "support/assert.h"
#include <algorithm>
#include <stdexcept>
using namespace backend::analysis;
namespace {
template <class F> bool refused(F f) {
    try {
        f();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}
} // namespace
int main() {
    const RevisionKey rev{"snapshot", "source", "plan-1", "application", "none",
                          "params",   "qc",     "lut",    "engine"};
    const auto t = Truth::True, f = Truth::False, u = Truth::Unknown;
    const Membership root{
        rev,
        {{"A/valid/9007199254740993", t}, {"B/invalid/9007199254740993", t}, {"A/valid/7", t}}};
    auto gate = root;
    gate.rows[1].value = u;
    gate.rows[2].value = f;
    auto qc = root;
    qc.rows[2].value = u;
    // Intentionally reverse dependency order. NOT of unknown stays unknown;
    // child NOT of itself is false only where its parent is known true.
    PopulationPlan plan{"plan-id",
                        "plan-1",
                        {{"child-not", "gate", NodeKind::Not, "", {"gate"}},
                         {"not", "", NodeKind::Not, "", {"gate"}},
                         {"gate", "", NodeKind::Predicate, "polygon", {}},
                         {"union", "", NodeKind::Or, "", {"gate", "gate"}}}};
    auto result = evaluatePlan(plan, root, qc, {{"polygon", gate}});
    MIB_EXPECT(result.at("not").rows[0].value == f && result.at("not").rows[1].value == u &&
                   result.at("not").rows[2].value == u,
               "unknown-safe graph NOT and QC");
    MIB_EXPECT(result.at("child-not").rows[0].value == f &&
                   result.at("child-not").rows[2].value == f,
               "child constrained by parent");
    MIB_EXPECT(countMembership(root, result.at("union")).selected == 1,
               "union does not duplicate events");
    auto bad = plan;
    bad.nodes[2].parent = "child-not";
    MIB_EXPECT(refused([&] { validatePlan(bad); }), "parent/operand mixed cycle refused");
    bad = plan;
    bad.nodes[0].parent = "absent";
    MIB_EXPECT(refused([&] { validatePlan(bad); }), "dangling parent refused");
    bad = plan;
    bad.nodes.push_back(bad.nodes[0]);
    MIB_EXPECT(refused([&] { validatePlan(bad); }), "duplicate population refused");
    bad = plan;
    bad.nodes[1].operands.push_back("gate");
    MIB_EXPECT(refused([&] { validatePlan(bad); }), "wrong arity refused");
    bad = plan;
    bad.revision = "stale";
    MIB_EXPECT(refused([&] { evaluatePlan(bad, root, qc, {{"polygon", gate}}); }),
               "stale plan refused");
    MIB_EXPECT(refused([&] { evaluatePlan(plan, root, qc, {}); }), "missing predicate refused");
    MIB_EXPECT(refused([&] { evaluatePlan(plan, root, qc, {{"polygon", gate}, {"extra", gate}}); }),
               "unused mask refused");
    auto stale = gate;
    stale.revision.qc = "stale";
    MIB_EXPECT(refused([&] { evaluatePlan(plan, root, qc, {{"polygon", stale}}); }),
               "stale input refused");
    bad = plan;
    bad.nodes.resize(257);
    MIB_EXPECT(refused([&] { validatePlan(bad); }), "graph budget refused");
    auto reordered = gate;
    std::reverse(reordered.rows.begin(), reordered.rows.end());
    MIB_EXPECT(evaluatePlan(plan, root, qc, {{"polygon", reordered}}).at("gate").rows[0].value == t,
               "exact-key joins");
    const Membership empty{rev, {}};
    MIB_EXPECT(evaluatePlan(plan, empty, empty, {{"polygon", empty}}).size() == 4,
               "empty graph memberships valid");
    return mib::test::exitCode();
}
