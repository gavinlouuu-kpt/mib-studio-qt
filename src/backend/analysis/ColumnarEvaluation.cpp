#include "backend/analysis/ColumnarEvaluation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace backend::analysis {
namespace {
std::uint64_t popcount64(std::uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<std::uint64_t>(__builtin_popcountll(v));
#else
    std::uint64_t c = 0;
    for (; v; v &= v - 1)
        ++c;
    return c;
#endif
}
std::size_t chunkOf(const ResourceBounds& bounds) {
    if (bounds.chunkEvents == 0 || bounds.chunkEvents > (1u << 24))
        throw std::invalid_argument("Chunk size must be 1..16777216 events");
    return bounds.chunkEvents;
}
void requireFeature(const EventColumns& columns, const std::string& feature) {
    if (!columns.parameters().count(feature))
        throw std::invalid_argument("Feature unavailable in source: " + feature);
}
std::optional<double> finite(double v) {
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}
} // namespace

std::uint64_t EventColumns::eventCount() const {
    std::uint64_t total = 0;
    for (const auto& s : segments()) {
        if (s.count > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::invalid_argument("Event count overflow");
        total += s.count;
    }
    return total;
}

VectorEventColumns::VectorEventColumns(std::vector<EventSegment> segments,
                                       std::map<std::string, std::string> parameters)
    : segments_(std::move(segments)), parameters_(std::move(parameters)) {}

void VectorEventColumns::setColumn(const std::string& id, std::vector<double> values) {
    if (!parameters_.count(id)) throw std::invalid_argument("Unknown column " + id);
    if (values.size() != eventCount()) throw std::invalid_argument("Column length mismatch");
    values_[id] = std::move(values);
}

bool VectorEventColumns::scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
                              const ChunkSink& sink) const {
    if (chunkEvents == 0) throw std::invalid_argument("Chunk size must be positive");
    const auto total = eventCount();
    std::vector<const std::vector<double>*> sources;
    for (const auto& id : columns) {
        const auto found = values_.find(id);
        if (found == values_.end()) throw std::invalid_argument("Column unavailable: " + id);
        sources.push_back(&found->second);
    }
    std::vector<const double*> pointers(columns.size());
    for (std::uint64_t begin = 0; begin < total;) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunkEvents, total - begin));
        for (std::size_t c = 0; c < sources.size(); ++c)
            pointers[c] = sources[c]->data() + begin;
        if (!sink(begin, count, pointers)) return false;
        begin += count;
    }
    return true;
}

TriStateBits::TriStateBits(std::uint64_t events)
    : events_(events), selected_((events + 63) / 64, 0), unknown_((events + 63) / 64, 0) {}

Truth TriStateBits::get(std::uint64_t index) const {
    if (index >= events_) throw std::out_of_range("Event index outside membership");
    const auto word = index / 64;
    const auto bit = std::uint64_t{1} << (index % 64);
    if (selected_[word] & bit) return Truth::True;
    if (unknown_[word] & bit) return Truth::Unknown;
    return Truth::False;
}

void TriStateBits::set(std::uint64_t index, Truth value) {
    if (index >= events_) throw std::out_of_range("Event index outside membership");
    const auto word = index / 64;
    const auto bit = std::uint64_t{1} << (index % 64);
    selected_[word] &= ~bit;
    unknown_[word] &= ~bit;
    if (value == Truth::True)
        selected_[word] |= bit;
    else if (value == Truth::Unknown)
        unknown_[word] |= bit;
    else if (value != Truth::False)
        throw std::invalid_argument("Invalid membership truth value");
}

std::uint64_t TriStateBits::count(Truth value) const {
    std::uint64_t selected = 0, unknown = 0;
    for (const auto w : selected_)
        selected += popcount64(w);
    for (const auto w : unknown_)
        unknown += popcount64(w);
    if (value == Truth::True) return selected;
    if (value == Truth::Unknown) return unknown;
    return events_ - selected - unknown;
}

std::optional<std::uint64_t> TriStateBits::find(Truth value, std::uint64_t rank) const {
    for (std::size_t w = 0; w < selected_.size(); ++w) {
        std::uint64_t word = value == Truth::True      ? selected_[w]
                             : value == Truth::Unknown ? unknown_[w]
                                                       : ~(selected_[w] | unknown_[w]);
        if (w + 1 == selected_.size() && events_ % 64)
            word &= (std::uint64_t{1} << (events_ % 64)) - 1;
        const auto bits = popcount64(word);
        if (rank >= bits) {
            rank -= bits;
            continue;
        }
        for (unsigned b = 0; b < 64; ++b)
            if (word & (std::uint64_t{1} << b)) {
                if (rank == 0) return static_cast<std::uint64_t>(w) * 64 + b;
                --rank;
            }
    }
    return std::nullopt;
}

std::size_t MembershipSet::bytes() const {
    std::size_t total = 0;
    for (const auto& [id, bits] : populations)
        total += bits.bytes();
    return total;
}

const TriStateBits& MembershipSet::population(const std::string& id) const {
    const auto found = populations.find(id);
    if (found == populations.end()) throw std::invalid_argument("Unknown population " + id);
    return found->second;
}

std::shared_ptr<const MembershipSet> evaluateColumns(const CompiledGraph& graph,
                                                     const EventColumns& columns,
                                                     const RevisionKey& revision,
                                                     const TriStateBits* eligibility,
                                                     const ResourceBounds& bounds) {
    const auto chunk = chunkOf(bounds);
    const auto events = columns.eventCount();
    if (events > bounds.maxEvents)
        throw std::invalid_argument("Source has " + std::to_string(events) +
                                    " events; the columnar evaluator bound is " +
                                    std::to_string(bounds.maxEvents) + "; nothing was sampled");
    const auto nodes = graph.order.size();
    const auto words = (events + 63) / 64;
    if (nodes && words > bounds.maxMembershipBytes / 16 / nodes)
        throw std::invalid_argument(
            "Membership of " + std::to_string(nodes) + " populations over " +
            std::to_string(events) + " events exceeds the " +
            std::to_string(bounds.maxMembershipBytes) + "-byte membership bound");
    if (eligibility && eligibility->size() != events)
        throw std::invalid_argument("Eligibility does not cover the source");
    const auto features = gateFeatures(graph);
    for (const auto& f : features)
        requireFeature(columns, f);

    auto result = std::make_shared<MembershipSet>();
    result->revision = revision;
    result->segments = columns.segments();
    result->events = events;
    result->order = graph.order;

    // Compiled per-node program: indices into `order`, -1 for the universe.
    struct Step {
        int parent{-1};
        NodeKind kind{NodeKind::Predicate};
        const GateSpec* gate{nullptr};
        int x{-1}, y{-1};
        int left{-1}, right{-1};
        TriStateBits* bits{nullptr};
    };
    std::map<std::string, int> index;
    for (std::size_t i = 0; i < nodes; ++i)
        index.emplace(graph.order[i], static_cast<int>(i));
    std::map<std::string, const PopulationNode*> byId;
    for (const auto& node : graph.plan.nodes)
        byId.emplace(node.id, &node);
    const auto featureIndex = [&](const std::string& f) {
        return static_cast<int>(std::find(features.begin(), features.end(), f) - features.begin());
    };
    std::vector<Step> program(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        const auto& node = *byId.at(graph.order[i]);
        auto& step = program[i];
        step.parent = node.parent.empty() ? -1 : index.at(node.parent);
        step.kind = node.kind;
        result->parents.emplace(node.id, node.parent);
        if (node.kind == NodeKind::Predicate) {
            step.gate = &graph.gates.at(node.predicate);
            step.x = featureIndex(step.gate->x);
            if (step.gate->kind != GateSpec::Kind::Interval) step.y = featureIndex(step.gate->y);
        } else {
            step.left = index.at(node.operands.at(0));
            if (node.operands.size() == 2) step.right = index.at(node.operands[1]);
        }
        step.bits = &result->populations.emplace(node.id, TriStateBits(events)).first->second;
    }

    // Per-chunk scratch: one Truth per node per event in the chunk.
    // At most nodes x chunkEvents bytes (256 x 65536 = 16 MiB at the defaults).
    const std::size_t lane =
        static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min<std::uint64_t>(chunk, events)));
    std::vector<Truth> scratch(nodes * lane);
    std::uint64_t seen = 0;
    const bool complete = columns.scan(
        features, chunk,
        [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& values) {
            if (begin != seen || count == 0 || count > lane || begin + count > events)
                throw std::invalid_argument("Columnar source delivered an out-of-order chunk");
            ++result->chunksRead;
            result->largestChunk = std::max<std::uint64_t>(result->largestChunk, count);
            for (std::size_t n = 0; n < nodes; ++n) {
                const auto& step = program[n];
                Truth* out = scratch.data() + n * lane;
                for (std::size_t j = 0; j < count; ++j) {
                    const Truth parent =
                        step.parent < 0 ? Truth::True : scratch[step.parent * lane + j];
                    const Truth quality = eligibility ? eligibility->get(begin + j) : Truth::True;
                    Truth predicate;
                    if (step.kind == NodeKind::Predicate) {
                        predicate = gateTruth(*step.gate, finite(values[step.x][j]),
                                              step.y < 0 ? std::nullopt
                                                         : finite(values[step.y][j]));
                    } else {
                        const Truth left = scratch[step.left * lane + j];
                        predicate = step.kind == NodeKind::Not ? logicalNot(left)
                                    : step.kind == NodeKind::And
                                        ? logicalAnd(left, scratch[step.right * lane + j])
                                        : logicalOr(left, scratch[step.right * lane + j]);
                    }
                    const Truth value = logicalAnd(logicalAnd(parent, quality), predicate);
                    out[j] = value;
                    if (value != Truth::False) step.bits->set(begin + j, value);
                }
            }
            seen += count;
            return true;
        });
    if (!complete || seen != events)
        throw std::invalid_argument("Columnar source ended before every event was read");
    return result;
}

PopulationStatistics columnStatistics(const TriStateBits& population, const EventColumns& columns,
                                      const std::string& feature, const RevisionKey& revision,
                                      const ResourceBounds& bounds) {
    requireFeature(columns, feature);
    if (population.size() != columns.eventCount())
        throw std::invalid_argument("Membership does not cover the source");
    PopulationStatistics result;
    result.revision = revision;
    result.feature = feature;
    result.unit = columns.parameters().at(feature);
    result.selected = population.count(Truth::True);
    result.unknownMembership = population.count(Truth::Unknown);
    result.excluded = population.size() - result.selected - result.unknownMembership;
    if (result.selected > bounds.maxExactValues)
        throw std::invalid_argument(
            "Exact order statistics over " + std::to_string(result.selected) +
            " selected events exceed the " + std::to_string(bounds.maxExactValues) +
            "-value bound; no approximation was substituted");
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(result.selected));
    columns.scan({feature}, chunkOf(bounds),
                 [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
                     for (std::size_t j = 0; j < count; ++j) {
                         if (population.get(begin + j) != Truth::True) continue;
                         const double x = v[0][j];
                         if (std::isfinite(x))
                             values.push_back(x);
                         else
                             ++result.missing;
                     }
                     return true;
                 });
    reduceMeasuredValues(values, result);
    return result;
}

std::vector<PopulationCount> populationCounts(const MembershipSet& membership) {
    std::vector<PopulationCount> out;
    for (const auto& id : membership.order) {
        PopulationCount c;
        c.id = id;
        c.parent = membership.parents.at(id);
        const auto& bits = membership.population(id);
        c.selected = bits.count(Truth::True);
        c.unknown = bits.count(Truth::Unknown);
        c.excluded = bits.size() - c.selected - c.unknown;
        c.parentSelected =
            c.parent.empty() ? membership.events : membership.population(c.parent).count(Truth::True);
        if (c.parentSelected)
            c.percentParent = 100.0 * static_cast<double>(c.selected) /
                              static_cast<double>(c.parentSelected);
        out.push_back(std::move(c));
    }
    return out;
}

ColumnHistogram columnHistogram(const TriStateBits& population, const EventColumns& columns,
                                const std::string& feature, double minimum, double maximum,
                                std::size_t bins, const ResourceBounds& bounds) {
    requireFeature(columns, feature);
    if (population.size() != columns.eventCount())
        throw std::invalid_argument("Membership does not cover the source");
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum || bins < 1 ||
        bins > std::min<std::size_t>(bounds.maxHistogramBins, 65536))
        throw std::invalid_argument("Histogram needs finite minimum < maximum and 1.." +
                                    std::to_string(std::min<std::size_t>(bounds.maxHistogramBins, 65536)) +
                                    " bins");
    ColumnHistogram h;
    for (std::size_t i = 0; i <= bins; ++i) {
        const long double weight = static_cast<long double>(i) / bins;
        const double edge = static_cast<double>((1 - weight) * minimum + weight * maximum);
        if (!std::isfinite(edge) || (!h.edges.empty() && edge <= h.edges.back()))
            throw std::invalid_argument("Histogram edges are not distinctly representable");
        h.edges.push_back(edge);
    }
    h.counts.assign(bins, 0);
    columns.scan({feature}, chunkOf(bounds),
                 [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
                     for (std::size_t j = 0; j < count; ++j) {
                         if (population.get(begin + j) != Truth::True) continue;
                         const double x = v[0][j];
                         if (!std::isfinite(x)) {
                             ++h.missing;
                             continue;
                         }
                         ++h.measured;
                         if (x < minimum) {
                             ++h.underflow;
                             continue;
                         }
                         if (x > maximum) {
                             ++h.overflow;
                             continue;
                         }
                         const auto at = static_cast<std::size_t>(
                             std::upper_bound(h.edges.begin(), h.edges.end(), x) - h.edges.begin() -
                             1);
                         ++h.counts[std::min(at, bins - 1)];
                     }
                     return true;
                 });
    return h;
}

ColumnScatter columnScatter(const TriStateBits& population, const EventColumns& columns,
                            const std::string& x, const std::string& y, std::size_t maxPoints,
                            const ResourceBounds& bounds) {
    requireFeature(columns, x);
    requireFeature(columns, y);
    if (population.size() != columns.eventCount())
        throw std::invalid_argument("Membership does not cover the source");
    if (maxPoints < 1 || maxPoints > 65536)
        throw std::invalid_argument("Scatter needs 1..65536 points");
    ColumnScatter s;
    const auto chunk = chunkOf(bounds);
    columns.scan({x, y}, chunk,
                 [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
                     for (std::size_t j = 0; j < count; ++j)
                         s.eligible += population.get(begin + j) == Truth::True &&
                                       std::isfinite(v[0][j]) && std::isfinite(v[1][j]);
                     return true;
                 });
    s.stride = std::max<std::uint64_t>(1, (s.eligible + maxPoints - 1) / maxPoints);
    std::uint64_t ordinal = 0;
    columns.scan({x, y}, chunk,
                 [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
                     for (std::size_t j = 0; j < count; ++j) {
                         if (population.get(begin + j) != Truth::True ||
                             !std::isfinite(v[0][j]) || !std::isfinite(v[1][j]))
                             continue;
                         if (ordinal++ % s.stride == 0 && s.points.size() < maxPoints)
                             s.points.push_back({begin + j, v[0][j], v[1][j]});
                     }
                     return true;
                 });
    return s;
}

std::pair<std::string, std::uint64_t> eventReference(const std::vector<EventSegment>& segments,
                                                     std::uint64_t index) {
    for (const auto& s : segments) {
        if (index < s.count) return {s.set, index};
        index -= s.count;
    }
    throw std::out_of_range("Event index outside source");
}
} // namespace backend::analysis
