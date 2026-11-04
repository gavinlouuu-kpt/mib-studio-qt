#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/evidence/LiveEvidence.h"
namespace backend::analysis {
// Explicit immutable F-source/1 projection. No file/source/hardware ownership.
//
// Two source kinds, one evaluator (F-1b):
//  - F-source/1 carries every event inline as a JSON row object (~1-2 KiB in
//    memory each), bounded explicitly at kMaxJsonSourceEvents rows. Hosts use
//    it only for small inline sources (live/archived run evidence, retained
//    live snapshots).
//  - F-source/2 is a columnar source *handle*: a descriptor (identity, units,
//    segments, coverage) with no rows. The host lends the source's raw
//    columns in bounded chunks through a ColumnResolver (a MIB recording via
//    review::RecordingColumns, an FCS file via analysis::FcsColumns), so
//    saved strategies, reports, plots, overlays and comparisons run on full
//    recordings. Event keys are JSON [sourceKey, set, ordinal].
// Both kinds run through ColumnarEvaluation (row sources as in-memory
// columns): same graph compiler, tri-state membership and estimators.
constexpr std::size_t kMaxJsonSourceEvents = 100000;
inline std::string jsonSourceBoundMessage() {
    return "F-source/1 JSON rows are bounded at " + std::to_string(kMaxJsonSourceEvents) +
           " events; analyse larger recordings with the columnar evaluator "
           "(review_analyze_population); nothing was sampled";
}
// Parameters of a columnar (F-source/2) source. Columns stay lazy: a scan
// reads only the parameters a graph, reduction or transform references, so
// a spectral FCS panel costs only what is gated on. Equal to the FCS
// reader's $PAR bound, so every importable file is also evaluable.
constexpr std::size_t kMaxColumnarParameters = 1024;
// Materialized F-source/1 rows (events x parameters) are bounded by cells,
// not only events: the former worst case (100,000 events x 64 parameters).
constexpr std::uint64_t kMaxJsonSourceCells = 6400000;
std::string contentHash(const nlohmann::json& value);
constexpr const char* kColumnarSourceSchema = "F-source/2";
// The host lends a columnar source's raw columns (descriptor units) to `use`
// for the duration of the call, after verifying the descriptor's identity
// (e.g. the full-file SHA-256); it throws when it cannot.
using ColumnVisitor = std::function<void(const EventColumns&)>;
using ColumnResolver = std::function<void(const nlohmann::json& source, const ColumnVisitor& use)>;
bool isColumnarSource(const nlohmann::json& source);
// Validated (set, count) segments of an F-source/2 descriptor.
std::vector<EventSegment> columnarSegments(const nlohmann::json& source);
std::string columnarEventKey(const std::string& sourceKey, const std::string& set,
                             std::uint64_t ordinal);
// The F-source/1 row form of a columnar source (raw values, no transforms;
// the stored-file row shape), refused above kMaxJsonSourceEvents or
// kMaxJsonSourceCells. For the
// few consumers that need every row (worker job input, observation export).
nlohmann::json materializeColumnarSource(const nlohmann::json& source, const EventColumns& columns);
using EvidenceLeaseRead =
    std::function<void(const evidence::Identity&, uint64_t, const nlohmann::json&)>;
nlohmann::json freezeEvidence(evidence::LiveEvidence& owner, const std::string& scope,
                              EvidenceLeaseRead read = {});
nlohmann::json adaptEvidenceArchive(const nlohmann::json& archive);
void validateAnalysisGraph(const nlohmann::json& graph);
nlohmann::json prepareAnalysisSource(const nlohmann::json& source, const nlohmann::json& strategy);
// F-evaluation/1 of request.graph's output. Row sources also return every
// row's membership ("rows"); columnar sources return bounded results only.
nlohmann::json evaluateSource(const nlohmann::json& source, const nlohmann::json& request,
                              const ColumnResolver& columns = {});
// One evaluation per population over a single membership pass (reports).
std::vector<nlohmann::json> evaluatePopulations(const nlohmann::json& source,
                                                const nlohmann::json& request,
                                                const std::vector<std::string>& populations,
                                                const ColumnResolver& columns = {});
// Keys of selected events of request.graph's output from selected rank
// `offset`, at most `limit`, in event order: {"selected": n, "keys": [...]}.
nlohmann::json selectedEventKeys(const nlohmann::json& source, const nlohmann::json& request,
                                 std::uint64_t offset, std::size_t limit,
                                 const ColumnResolver& columns = {});
nlohmann::json scatterSource(const nlohmann::json& source, const nlohmann::json& strategy,
                             const ColumnResolver& columns = {});
nlohmann::json plotSource(const nlohmann::json& source, const nlohmann::json& strategy,
                          const ColumnResolver& columns = {});
} // namespace backend::analysis
