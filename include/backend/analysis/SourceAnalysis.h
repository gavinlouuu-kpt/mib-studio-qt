#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include "backend/evidence/LiveEvidence.h"
namespace backend::analysis {
// Explicit immutable F-source/1 projection. No file/source/hardware ownership.
//
// F-source/1 carries every event as a JSON row object (~1-2 KiB in memory
// each). That transport is bounded explicitly at kMaxJsonSourceEvents rows;
// above it a host refuses with jsonSourceBoundMessage() and the recording is
// analysed by the columnar evaluator (ColumnarEvaluation.h), which reads the
// file in chunks and keeps membership as bitsets. Internally evaluateSource
// also runs that columnar evaluator over in-memory columns.
constexpr std::size_t kMaxJsonSourceEvents = 100000;
inline std::string jsonSourceBoundMessage() {
    return "F-source/1 JSON rows are bounded at " + std::to_string(kMaxJsonSourceEvents) +
           " events; analyse larger recordings with the columnar evaluator "
           "(review_analyze_population); nothing was sampled";
}
std::string contentHash(const nlohmann::json& value);
using EvidenceLeaseRead =
    std::function<void(const evidence::Identity&, uint64_t, const nlohmann::json&)>;
nlohmann::json freezeEvidence(evidence::LiveEvidence& owner, const std::string& scope,
                              EvidenceLeaseRead read = {});
nlohmann::json adaptEvidenceArchive(const nlohmann::json& archive);
void validateAnalysisGraph(const nlohmann::json& graph);
nlohmann::json prepareAnalysisSource(const nlohmann::json& source, const nlohmann::json& strategy);
nlohmann::json evaluateSource(const nlohmann::json& source, const nlohmann::json& request);
nlohmann::json scatterSource(const nlohmann::json& source, const nlohmann::json& strategy);
nlohmann::json plotSource(const nlohmann::json& source, const nlohmann::json& strategy);
} // namespace backend::analysis
