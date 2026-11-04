#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace backend::analysis {
// Evaluates one pinned sample: {action: evaluate, planId, revision, source[, metric]}
// through the host (AnalysisHost), returning its reply.
using SampleEvaluator = std::function<nlohmann::json(const nlohmann::json&)>;

// Descriptive experimental-unit comparison (F-analysis-comparison/1). No
// inferential/causal qualification. Kept for existing findings.
nlohmann::json compareAnalysis(const nlohmann::json& spec, const SampleEvaluator& evaluate);

// F-4 (#808 F09, I09): ComparisonSpec (F-comparison-spec/1). Declared metric
// (population x parameter x statistic, exact statistics), experimental units,
// groups, design (unpaired | paired by unit id) and inference. Per-source
// compatibility with exclusion reasons; per-group unit-level summaries; tests
// only when declared and every group has >= minimumUnitsPerGroup (>= 3)
// independent units, otherwise `insufficient` with reasons and no p-value.
nlohmann::json compareSpec(const nlohmann::json& spec, const SampleEvaluator& evaluate);

// F-4 (#808 F09, I10): concentration-response fit (F-fit-spec/1): 4PL or 3PL
// (hill 1) on unit-level values by Levenberg-Marquardt, declared concentration
// unit and scale, weighting none | inverse-variance (of replicate units).
// Insufficient input yields no fit; suspect fits carry reasons.
nlohmann::json fitSpec(const nlohmann::json& spec, const SampleEvaluator& evaluate);

inline constexpr int kMinimumUnitsPerGroup = 3;
} // namespace backend::analysis
