#include "backend/analysis/Population.h"
#include "support/assert.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace backend::analysis;
namespace {
template <typename F> bool refused(F action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
RevisionKey revision() {
    return {"snapshot", "sha", "plan", "app", "none", "params", "qc", "lut", "cytoflow-1.3.2"};
}
Membership membership(std::initializer_list<Truth> values) {
    Membership result{revision(), {}};
    std::size_t i = 0;
    for (const auto value : values) {
        result.rows.push_back({"source-" + std::to_string(i % 2) + "/valid/" +
                                   std::to_string(9007199254740993ULL + i / 2),
                               value});
        ++i;
    }
    return result;
}
} // namespace
int main() {
    const Truth f = Truth::False, t = Truth::True, u = Truth::Unknown;
    const Truth values[] = {f, t, u};
    const Truth andTable[3][3] = {{f, f, f}, {f, t, u}, {f, u, u}};
    const Truth orTable[3][3] = {{f, t, u}, {t, t, t}, {u, t, u}};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
            MIB_EXPECT(logicalAnd(values[a], values[b]) == andTable[a][b], "three-valued AND");
            MIB_EXPECT(logicalOr(values[a], values[b]) == orTable[a][b], "three-valued OR");
        }
    MIB_EXPECT(logicalNot(u) == u, "NOT missing remains unknown");
    MIB_EXPECT(logicalNot(t) == f && logicalNot(f) == t, "NOT known");
    MIB_EXPECT(evaluateInterval(2., {2., 2.}) == t, "inclusive equal scalar interval");
    MIB_EXPECT(evaluateInterval(1., {2., 2.}) == f, "outside equal scalar interval");
    MIB_EXPECT(evaluateInterval(2., {2., 3.}) == t && evaluateInterval(3., {2., 3.}) == t,
               "both boundaries inclusive");
    MIB_EXPECT(evaluateInterval(-100., {{}, {}}) == t, "absent endpoints unbounded");
    MIB_EXPECT(evaluateInterval({}, {{}, {}}) == u, "unbounded does not admit missing");
    MIB_EXPECT(evaluateInterval(std::numeric_limits<double>::infinity(), {{}, {}}) == u,
               "nonfinite unknown");
    MIB_EXPECT(refused([] { validateInterval({2., 1.}); }), "reversed bounds rejected");
    MIB_EXPECT(refused([] { validateInterval({{}, std::numeric_limits<double>::quiet_NaN()}); }),
               "NaN bounds rejected");

    auto parent = membership({t, t, t, f});
    auto qc = membership({t, t, u, t});
    auto gate = membership({t, u, f, f});
    std::reverse(gate.rows.begin(), gate.rows.end());
    const auto negated = compose(parent, qc, gate, {}, BooleanOperation::Not);
    MIB_EXPECT(negated.rows[0].value == f && negated.rows[1].value == u &&
                   negated.rows[2].value == u && negated.rows[3].value == f,
               "NOT respects parent/QC/unknown");
    auto counts = countMembership(parent, negated);
    MIB_EXPECT(counts.selected == 0 && counts.unknown == 2 && counts.excluded == 2 &&
                   counts.parentSelected == 3 && counts.percentParent == 0.,
               "explicit universe accounting");
    auto empty = membership({});
    MIB_EXPECT(!countMembership(empty, empty).percentParent, "zero denominator is null");
    auto unionResult = compose(parent, membership({t, t, t, t}), membership({t, t, f, f}),
                               membership({t, f, t, f}), BooleanOperation::Or);
    MIB_EXPECT(countMembership(parent, unionResult).selected == 3,
               "union deduplicates by identity");
    MIB_EXPECT(refused([&] { compose(parent, qc, gate, {}, BooleanOperation::And); }),
               "missing operand rejected");
    auto foreign = gate;
    foreign.rows[0].key = "foreign";
    MIB_EXPECT(refused([&] { compose(parent, qc, foreign, {}, BooleanOperation::Not); }),
               "foreign key rejected");
    auto duplicate = gate;
    duplicate.rows[0].key = duplicate.rows[1].key;
    MIB_EXPECT(refused([&] { compose(parent, qc, duplicate, {}, BooleanOperation::Not); }),
               "duplicate key rejected");
    auto missing = gate;
    missing.rows.pop_back();
    MIB_EXPECT(refused([&] { compose(parent, qc, missing, {}, BooleanOperation::Not); }),
               "unexplained key loss rejected");
    auto stale = gate;
    stale.revision.mechanics = "different-lut";
    MIB_EXPECT(refused([&] { compose(parent, qc, stale, {}, BooleanOperation::Not); }),
               "mechanics revision mismatch rejected");
    stale = gate;
    stale.revision.sourceFingerprint = "different-bytes";
    MIB_EXPECT(refused([&] { compose(parent, qc, stale, {}, BooleanOperation::Not); }),
               "source mismatch rejected");
    auto invalid = gate;
    invalid.rows[0].value = static_cast<Truth>(9);
    MIB_EXPECT(refused([&] { compose(parent, qc, invalid, {}, BooleanOperation::Not); }),
               "invalid truth rejected");
    MIB_EXPECT(refused([&] { countMembership(parent, membership({t, t, t, t})); }),
               "cannot count outside parent");
    return mib::test::exitCode();
}
