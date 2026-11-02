#pragma once

#include "backend/analysis/PopulationGates.h"
#include "backend/analysis/PopulationStatistics.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Full-recording population evaluation (F04/F05/F08/F16, slice F-1).
//
// A source is read as columns in bounded chunks (EventColumns::scan); the
// population DAG is evaluated chunk by chunk with the unchanged tri-state
// semantics of PopulationGraph::evaluatePlan (each node = parent AND
// eligibility AND predicate, Boolean nodes through logicalAnd/Or/Not); every
// population's exact membership is kept as a native two-bit-per-event bitset
// (TriStateBits) keyed to the source identity and graph revision. Only bounded
// results (counts, statistics, bins, a decimated scatter, a page of event
// references) leave this layer. No HDF5, Qt, JSON-row or Python dependency.
namespace backend::analysis {

// One contiguous run of events of one stored set ("valid", "invalid", ...).
// A global event index i maps to (set, ordinal) by walking the segments.
struct EventSegment {
    std::string set;
    std::uint64_t count{0};
};

// Explicit resource bounds. Every refusal names the bound it hit; nothing is
// sampled or truncated to fit.
struct ResourceBounds {
    // Events read per scan step and per column (the working set of a scan).
    std::size_t chunkEvents{65536};
    // Events per source (2^32 - 1: ordinals stay exact in uint64 and doubles).
    std::uint64_t maxEvents{4294967295ULL};
    // Memory of all population bitsets of one evaluation (2 bits/event/node).
    std::uint64_t maxMembershipBytes{256ULL * 1024 * 1024};
    // Values held for exact order statistics (8 bytes each): 2^26 = 512 MiB.
    std::uint64_t maxExactValues{1ULL << 26};
    // Bins of one exact histogram (columnHistogram). 256 for display hosts;
    // acceptance tools comparing finer exports (FlowJo's 4,096) raise it.
    std::size_t maxHistogramBins{256};
};

// A read-only, chunked, columnar event source. Missing measurements are NaN
// (and every nonfinite value is treated as missing).
class EventColumns {
public:
    using ChunkSink = std::function<bool(std::uint64_t begin, std::size_t count,
                                         const std::vector<const double*>& columns)>;
    virtual ~EventColumns() = default;
    virtual const std::vector<EventSegment>& segments() const = 0;
    // parameter id -> unit
    virtual const std::map<std::string, std::string>& parameters() const = 0;
    // Sequential scan in event order; at most `chunkEvents` events per call of
    // `sink`; one pointer per requested column, in request order. Throws when
    // the source cannot be read completely; returns false when `sink` stopped.
    virtual bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
                      const ChunkSink& sink) const = 0;
    std::uint64_t eventCount() const;
};

// In-memory columns (tests, JSON F-source adaptor). Owns its values.
class VectorEventColumns final : public EventColumns {
public:
    VectorEventColumns(std::vector<EventSegment> segments,
                       std::map<std::string, std::string> parameters);
    // Values for one parameter, one per event (NaN = missing).
    void setColumn(const std::string& id, std::vector<double> values);
    const std::vector<EventSegment>& segments() const override { return segments_; }
    const std::map<std::string, std::string>& parameters() const override { return parameters_; }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
              const ChunkSink& sink) const override;

private:
    std::vector<EventSegment> segments_;
    std::map<std::string, std::string> parameters_;
    std::map<std::string, std::vector<double>> values_;
};

// Exact tri-state membership, two bits per event.
class TriStateBits {
public:
    TriStateBits() = default;
    explicit TriStateBits(std::uint64_t events);
    std::uint64_t size() const { return events_; }
    Truth get(std::uint64_t index) const;
    void set(std::uint64_t index, Truth value);
    std::uint64_t count(Truth value) const;
    // Index of the `rank`-th (0-based) event with `value`, or nullopt.
    std::optional<std::uint64_t> find(Truth value, std::uint64_t rank) const;
    std::size_t bytes() const { return (selected_.size() + unknown_.size()) * 8; }

private:
    std::uint64_t events_{0};
    std::vector<std::uint64_t> selected_; // bit set = True
    std::vector<std::uint64_t> unknown_;  // bit set = Unknown (never with selected)
};

// The membership handle: every population of one graph over one source.
struct MembershipSet {
    RevisionKey revision; // sourceFingerprint + plan (graph revision) pin it
    std::vector<EventSegment> segments;
    std::uint64_t events{0};
    std::vector<std::string> order; // topological population ids
    std::map<std::string, std::string> parents;
    std::map<std::string, TriStateBits> populations;
    // Evidence of the bounded read: chunks scanned and the largest chunk seen.
    std::uint64_t chunksRead{0};
    std::uint64_t largestChunk{0};
    std::size_t bytes() const;
    const TriStateBits& population(const std::string& id) const;
};

// Evaluate every node of `graph` over `columns`. `eligibility` (optional, QC)
// has the same event count; absent means every event is eligible.
std::shared_ptr<const MembershipSet> evaluateColumns(const CompiledGraph& graph,
                                                     const EventColumns& columns,
                                                     const RevisionKey& revision,
                                                     const TriStateBits* eligibility = nullptr,
                                                     const ResourceBounds& bounds = {});

// Exact reductions of `feature` over the selected events of `population`.
// The estimators are those of populationStatistics (same function, same
// numbers): order statistics by exact selection over the bitset-selected
// values held in memory (bounded by maxExactValues).
PopulationStatistics columnStatistics(const TriStateBits& population, const EventColumns& columns,
                                      const std::string& feature, const RevisionKey& revision,
                                      const ResourceBounds& bounds = {});

struct PopulationCount {
    std::string id;
    std::string parent;
    std::uint64_t selected{0}, excluded{0}, unknown{0}, parentSelected{0};
    std::optional<double> percentParent; // null for an empty parent
};
std::vector<PopulationCount> populationCounts(const MembershipSet& membership);

struct ColumnHistogram {
    std::vector<double> edges; // bins + 1, [left,right), last bin closed
    std::vector<std::uint64_t> counts;
    std::uint64_t underflow{0}, overflow{0}, measured{0}, missing{0};
};
// Exact binned counts of selected finite values; sum(counts)+under+over == measured.
// 1..bounds.maxHistogramBins bins (at most 65536).
ColumnHistogram columnHistogram(const TriStateBits& population, const EventColumns& columns,
                                const std::string& feature, double minimum, double maximum,
                                std::size_t bins, const ResourceBounds& bounds = {});

struct ScatterPoint {
    std::uint64_t event{0};
    double x{0}, y{0};
};
struct ColumnScatter {
    std::uint64_t eligible{0}; // selected events with both coordinates finite
    std::uint64_t stride{1};   // every stride-th eligible event, deterministic
    std::vector<ScatterPoint> points;
};
// Deterministic stride decimation: never more than maxPoints points.
ColumnScatter columnScatter(const TriStateBits& population, const EventColumns& columns,
                            const std::string& x, const std::string& y, std::size_t maxPoints,
                            const ResourceBounds& bounds = {});

// (set, ordinal) of a global event index.
std::pair<std::string, std::uint64_t> eventReference(const std::vector<EventSegment>& segments,
                                                     std::uint64_t index);
} // namespace backend::analysis
