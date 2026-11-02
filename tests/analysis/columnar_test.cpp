// F-1 (F04/F05/F08/F16): the columnar evaluator is the keyed evaluator at
// scale. Invariant: for random sources, graphs (interval, polygon, ellipse,
// quadrant, AND/OR/NOT, parents), QC eligibility and every chunk size,
// evaluateColumns gives the same tri-state membership at every node as
// PopulationGraph::evaluatePlan, and columnStatistics the same numbers as
// populationStatistics. Plus: binned counts conserve n, decimated scatter is
// bounded and deterministic, and every resource bound refuses explicitly.
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/PopulationGraph.h"
#include "backend/analysis/PopulationStatistics.h"
#include "support/assert.h"

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <string>

using namespace backend::analysis;
using Json = nlohmann::json;

namespace {
template <class F> bool refused(F action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Json graphJson() {
    return Json{
        {"id", "parity"},
        {"revision", "r1"},
        {"output", "final"},
        {"nodes",
         Json::array(
             {Json{{"id", "size"}, {"parent", ""}, {"kind", "interval"}, {"feature", "a"},
                   {"minimum", -1.0}, {"maximum", 2.0}},
              Json{{"id", "poly"}, {"parent", "size"}, {"kind", "polygon"}, {"x", "a"}, {"y", "b"},
                   {"vertices", Json::array({Json::array({-2, -2}), Json::array({2, -1}),
                                             Json::array({1, 2}), Json::array({-1.5, 1})})}},
              Json{{"id", "ell"}, {"parent", ""}, {"kind", "ellipse"}, {"x", "b"}, {"y", "c"},
                   {"centerX", 0.0}, {"centerY", 0.5}, {"radiusX", 1.5}, {"radiusY", 1.0}},
              Json{{"id", "quad"}, {"parent", ""}, {"kind", "quadrant"}, {"x", "a"}, {"y", "c"},
                   {"thresholdX", 0.0}, {"thresholdY", 0.0}, {"quadrant", "upper-right"}},
              Json{{"id", "both"}, {"parent", ""}, {"kind", "and"}, {"operands", {"poly", "ell"}}},
              Json{{"id", "either"}, {"parent", "size"}, {"kind", "or"}, {"operands", {"quad", "ell"}}},
              Json{{"id", "final"}, {"parent", ""}, {"kind", "not"}, {"operands", {"both"}}},
              Json{{"id", "open"}, {"parent", "either"}, {"kind", "interval"}, {"feature", "c"},
                   {"minimum", nullptr}, {"maximum", 0.75}}})}};
}
} // namespace

int main() {
    std::mt19937_64 rng(808);
    std::normal_distribution<double> normal(0.0, 1.2);
    const auto graph = compileGraph(graphJson());
    for (const std::uint64_t events : {0ULL, 1ULL, 63ULL, 64ULL, 65ULL, 2003ULL}) {
        std::vector<double> a(events), b(events), c(events);
        for (std::uint64_t i = 0; i < events; ++i) {
            a[i] = i % 17 == 3 ? kNaN : normal(rng);
            b[i] = i % 23 == 5 ? std::numeric_limits<double>::infinity() : normal(rng);
            c[i] = i % 29 == 7 ? kNaN : normal(rng);
            if (i % 31 == 0) a[i] = 2.0; // inclusive interval edge
        }
        const std::vector<EventSegment> segments{{"valid", events / 3}, {"invalid", events - events / 3}};
        VectorEventColumns columns(segments, {{"a", "u"}, {"b", "u"}, {"c", "kPa"}});
        columns.setColumn("a", a);
        columns.setColumn("b", b);
        columns.setColumn("c", c);
        // Keyed reference: the existing evaluatePlan over string keys.
        RevisionKey rev{"snap", "fp", graph.plan.revision, "app", "none", "p", "qc", "m", "e"};
        Membership universe{rev, {}}, qc{rev, {}};
        std::map<std::string, Membership> predicates;
        TriStateBits eligibility(events);
        for (const auto& [id, gate] : graph.gates)
            predicates.emplace(id, Membership{rev, {}});
        for (std::uint64_t i = 0; i < events; ++i) {
            const auto key = "e" + std::to_string(i);
            universe.rows.push_back({key, Truth::True});
            const Truth q = i % 11 == 0 ? Truth::Unknown : i % 13 == 0 ? Truth::False : Truth::True;
            qc.rows.push_back({key, q});
            eligibility.set(i, q);
            const std::map<std::string, double> v{{"a", a[i]}, {"b", b[i]}, {"c", c[i]}};
            for (const auto& [id, gate] : graph.gates) {
                const auto opt = [](double x) {
                    return std::isfinite(x) ? std::optional<double>(x) : std::nullopt;
                };
                predicates.at(id).rows.push_back(
                    {key, gateTruth(gate, opt(v.at(gate.x)),
                                    gate.kind == GateSpec::Kind::Interval ? std::nullopt
                                                                          : opt(v.at(gate.y)))});
            }
        }
        const auto keyed = evaluatePlan(graph.plan, universe, qc, predicates);
        for (const std::size_t chunk : {1u, 7u, 64u, 65536u}) {
            ResourceBounds bounds;
            bounds.chunkEvents = chunk;
            const auto set = evaluateColumns(graph, columns, rev, &eligibility, bounds);
            MIB_EXPECT(set->events == events, "membership covers every event");
            MIB_EXPECT(set->largestChunk <= chunk, "no scan step exceeds the chunk bound");
            MIB_EXPECT(set->chunksRead == (events + chunk - 1) / chunk, "one pass of bounded chunks");
            bool same = true;
            for (const auto& [id, membership] : keyed)
                for (std::uint64_t i = 0; i < events; ++i)
                    same = same && membership.rows[i].value == set->population(id).get(i);
            MIB_EXPECT(same, "columnar tri-state equals keyed evaluatePlan at every node, n=" +
                                 std::to_string(events) + " chunk=" + std::to_string(chunk));
            for (const auto& count : populationCounts(*set)) {
                const auto reference = countMembership(
                    count.parent.empty() ? universe : keyed.at(count.parent), keyed.at(count.id));
                MIB_EXPECT(count.selected == reference.selected &&
                               count.unknown == reference.unknown &&
                               count.excluded == reference.excluded &&
                               count.parentSelected == reference.parentSelected,
                           "population counts match countMembership for " + count.id);
            }
            // Statistics: same function, same numbers.
            for (const std::string feature : {"a", "c"}) {
                MeasurementColumn column{rev, feature, feature == "c" ? "kPa" : "u", {}};
                for (std::uint64_t i = 0; i < events; ++i)
                    column.rows.push_back(
                        {"e" + std::to_string(i),
                         std::isfinite((feature == "a" ? a : c)[i])
                             ? std::optional<double>((feature == "a" ? a : c)[i])
                             : std::nullopt});
                const auto expected = populationStatistics(keyed.at("final"), column);
                const auto got =
                    columnStatistics(set->population("final"), columns, feature, rev, bounds);
                MIB_EXPECT(got.selected == expected.selected && got.excluded == expected.excluded &&
                               got.unknownMembership == expected.unknownMembership &&
                               got.measured == expected.measured && got.missing == expected.missing &&
                               got.mean == expected.mean && got.median == expected.median &&
                               got.p05 == expected.p05 && got.p95 == expected.p95 &&
                               got.sampleStandardDeviation == expected.sampleStandardDeviation &&
                               got.cvPercent == expected.cvPercent && got.unit == column.unit,
                           "columnStatistics is populationStatistics bit for bit");
            }
            const auto h =
                columnHistogram(set->population("open"), columns, "a", -1.0, 1.0, 16, bounds);
            std::uint64_t binned = h.underflow + h.overflow;
            for (const auto n : h.counts)
                binned += n;
            MIB_EXPECT(binned == h.measured, "binned counts plus under/overflow sum to n");
            const auto s = columnScatter(set->population("final"), columns, "a", "b", 50, bounds);
            MIB_EXPECT(s.points.size() <= 50 && s.points.size() == (s.eligible + s.stride - 1) / s.stride,
                       "decimated scatter is bounded and keeps every stride-th point");
            const auto again = columnScatter(set->population("final"), columns, "a", "b", 50, bounds);
            MIB_EXPECT(again.points.size() == s.points.size() &&
                           (s.points.empty() || again.points.back().event == s.points.back().event),
                       "decimation is deterministic");
        }
    }

    // TriStateBits: find/count over word boundaries.
    TriStateBits bits(130);
    for (std::uint64_t i = 0; i < 130; i += 3)
        bits.set(i, Truth::True);
    bits.set(129, Truth::Unknown);
    bits.set(3, Truth::False);
    // 44 multiples of 3 in 0..129, minus 3 (excluded) and 129 (unknown).
    MIB_EXPECT(bits.count(Truth::True) == 42 && bits.count(Truth::Unknown) == 1 &&
                   bits.count(Truth::False) == 87,
               "two-bit counts conserve events");
    MIB_EXPECT(bits.find(Truth::True, 0) == 0ULL && bits.find(Truth::True, 1) == 6ULL &&
                   bits.find(Truth::True, 41) == 126ULL && !bits.find(Truth::True, 42),
               "rank lookup across words");
    MIB_EXPECT(bits.find(Truth::Unknown, 0) == 129ULL && bits.find(Truth::False, 86) == 128ULL &&
                   !bits.find(Truth::False, 87),
               "excluded rank ignores padding bits");
    MIB_EXPECT(eventReference({{"valid", 2}, {"invalid", 3}}, 2) ==
                   std::make_pair(std::string("invalid"), std::uint64_t{0}),
               "global index -> (set, ordinal)");

    // Explicit bounds refuse; nothing is sampled.
    VectorEventColumns small({{"valid", 1000}}, {{"a", "u"}, {"b", "u"}, {"c", "u"}});
    small.setColumn("a", std::vector<double>(1000, 0.5));
    small.setColumn("b", std::vector<double>(1000, 0.5));
    small.setColumn("c", std::vector<double>(1000, 0.5));
    RevisionKey rev{"s", "f", "p", "a", "o", "p", "q", "m", "e"};
    ResourceBounds tight;
    tight.maxEvents = 999;
    MIB_EXPECT(refused([&] { evaluateColumns(graph, small, rev, nullptr, tight); }),
               "event bound refuses");
    tight = {};
    tight.maxMembershipBytes = 1000;
    MIB_EXPECT(refused([&] { evaluateColumns(graph, small, rev, nullptr, tight); }),
               "membership-byte bound refuses");
    tight = {};
    tight.chunkEvents = 0;
    MIB_EXPECT(refused([&] { evaluateColumns(graph, small, rev, nullptr, tight); }),
               "zero chunk refuses");
    const auto everything = evaluateColumns(graph, small, rev);
    tight = {};
    tight.maxExactValues = 10;
    MIB_EXPECT(refused([&] { columnStatistics(everything->population("size"), small, "a", rev, tight); }),
               "exact-statistics bound refuses instead of approximating");
    VectorEventColumns missing({{"valid", 1}}, {{"a", "u"}});
    missing.setColumn("a", {1.0});
    MIB_EXPECT(refused([&] { evaluateColumns(graph, missing, rev); }),
               "gate feature absent from source refuses");
    return mib::test::exitCode();
}
