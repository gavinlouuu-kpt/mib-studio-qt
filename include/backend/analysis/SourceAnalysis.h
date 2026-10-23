#pragma once
#include <nlohmann/json.hpp>
#include "backend/evidence/LiveEvidence.h"
namespace backend::analysis {
// Explicit immutable F-source/1 projection. No file/source/hardware ownership.
std::string contentHash(const nlohmann::json& value);
nlohmann::json freezeEvidence(evidence::LiveEvidence& owner, const std::string& scope);
nlohmann::json adaptEvidenceArchive(const nlohmann::json& archive);
void validateAnalysisGraph(const nlohmann::json& graph);
nlohmann::json prepareAnalysisSource(const nlohmann::json& source, const nlohmann::json& strategy);
nlohmann::json evaluateSource(const nlohmann::json& source, const nlohmann::json& request);
nlohmann::json scatterSource(const nlohmann::json& source, const nlohmann::json& strategy);
nlohmann::json plotSource(const nlohmann::json& source, const nlohmann::json& strategy);
} // namespace backend::analysis
