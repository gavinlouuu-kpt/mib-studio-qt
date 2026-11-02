#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace backend::services {
class SqliteService;
}
namespace backend::analysis {
nlohmann::json
analysisReportRequest(services::SqliteService& database, const nlohmann::json& request, bool write,
                      const std::function<nlohmann::json(const nlohmann::json&)>& evaluate);
}
