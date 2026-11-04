#pragma once
// Internal to the analysis library (F-1b): one evaluation core for inline row
// sources (F-source/1) and columnar source handles (F-source/2). Hosts and
// tests use SourceAnalysis.h; this header is not installed.
#include "backend/analysis/ColumnarEvaluation.h"
#include "backend/analysis/PopulationGates.h"
#include "backend/analysis/SourceAnalysis.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace backend::analysis::detail {
using Json = nlohmann::json;

// One source opened for analysis: its descriptor, raw columns (descriptor
// units) and exact event keys. Row sources build it from their rows;
// columnar sources borrow the columns from the host for the call.
struct SourceView {
    const Json* source{nullptr};
    const EventColumns* raw{nullptr};
    std::uint64_t events{0};
    bool inlineRows{false};
    std::function<std::string(std::uint64_t)> key;
    std::function<std::optional<std::uint64_t>(const std::string&)> index;
    // The value as the source states it (row sources keep their JSON number
    // spelling for raw parameters); transform outputs are Json(double).
    std::function<Json(std::uint64_t, const std::string&, double)> value;
};
void withSource(const Json& source, const ColumnResolver& columns,
                const std::function<void(const SourceView&)>& use);

// Raw columns plus ordered derived parameters (linear / asinh / log10 of a
// raw or earlier derived parameter), computed per chunk.
struct TransformStage {
    std::string input, output, kind;
    double scale{1}, offset{0};
};
class TransformedColumns final : public EventColumns {
public:
    TransformedColumns(const EventColumns& raw, std::map<std::string, std::string> units,
                       std::vector<TransformStage> stages);
    const std::vector<EventSegment>& segments() const override { return raw_.segments(); }
    const std::map<std::string, std::string>& parameters() const override { return units_; }
    bool scan(const std::vector<std::string>& columns, std::size_t chunkEvents,
              const ChunkSink& sink) const override;
    bool derived(const std::string& id) const;

private:
    const EventColumns& raw_;
    std::map<std::string, std::string> units_;
    std::vector<TransformStage> stages_;
};
double transformValue(const TransformStage& stage, double raw);

struct Prepared {
    Json units = Json::object(); // raw + transform outputs, as the source states them
    Json transforms = Json::array();
    std::string parameterFingerprint;
    std::unique_ptr<TransformedColumns> columns;
};
Prepared prepare(const SourceView& view, const Json& strategy);

struct GraphRun {
    CompiledGraph graph;
    std::string qcPolicy, qcFingerprint;
    std::uint64_t qcGood{0}, qcBad{0}, qcUnknown{0};
    RevisionKey revision;
    std::shared_ptr<const MembershipSet> membership;
};
GraphRun runGraph(const SourceView& view, const Prepared& prepared, const Json& request);
// The F-evaluation/1 of `population` (request.graph with that output).
Json evaluation(const SourceView& view, const Prepared& prepared, const GraphRun& run,
                const Json& request, const std::string& population);
// request.graph with `population` as output, validated by the one compiler.
Json graphFor(const Json& request, const std::string& population);
} // namespace backend::analysis::detail
