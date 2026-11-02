#pragma once

#include "backend/analysis/PopulationGraph.h"

#include <array>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// The one F population-graph compiler (F04): interval, polygon, ellipse and
// quadrant predicates plus Boolean nodes, validated once and evaluated per
// event by every host. Shared by the JSON source path (SourceAnalysis) and the
// columnar full-recording path (ColumnarEvaluation). No HDF5, Qt or Python.
namespace backend::analysis {
struct GateSpec {
    enum class Kind { Interval, Polygon, Ellipse, Quadrant };
    Kind kind{Kind::Interval};
    std::string x; // interval: the gated feature
    std::string y; // geometry only
    Interval bounds;
    std::vector<std::array<double, 2>> polygon;
    double centerX{0}, centerY{0}, radiusX{1}, radiusY{1};
    double thresholdX{0}, thresholdY{0};
    bool right{false}, upper{false};
};
struct CompiledGraph {
    PopulationPlan plan;
    std::string output;
    std::vector<std::string> order; // topological
    std::map<std::string, GateSpec> gates; // predicate node id -> gate
};
// Validates every node (fields, feature names, polygon simplicity, finite
// geometry), parents/operands/cycles (validatePlan) and the output.
CompiledGraph compileGraph(const nlohmann::json& graph);
// Tri-state membership of one event in one gate: a missing (nullopt or
// nonfinite) coordinate is Unknown, never False.
Truth gateTruth(const GateSpec& gate, std::optional<double> x, std::optional<double> y);
// Every feature a compiled graph reads, sorted and unique.
std::vector<std::string> gateFeatures(const CompiledGraph& graph);
} // namespace backend::analysis
