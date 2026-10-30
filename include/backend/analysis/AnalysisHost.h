#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
namespace backend::services {
class SqliteService;
}
namespace backend::analysis {
using SourceResolver = std::function<nlohmann::json(const nlohmann::json&)>;
using MethodResolver = std::function<nlohmann::json(const nlohmann::json&)>;
// No database/source ownership. Uses the existing app metadata owner and source fences.
std::string analysisRequest(services::SqliteService& database, const std::string& request,
                            bool write, SourceResolver source, MethodResolver method = {});
} // namespace backend::analysis
