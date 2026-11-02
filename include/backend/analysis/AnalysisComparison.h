#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace backend::analysis {
// Descriptive experimental-unit comparison. No inferential/causal qualification.
nlohmann::json
compareAnalysis(const nlohmann::json& spec,
                const std::function<nlohmann::json(const nlohmann::json&)>& evaluate);
} // namespace backend::analysis
