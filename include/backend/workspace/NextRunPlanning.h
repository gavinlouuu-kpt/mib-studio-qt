#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace backend::services {
class SqliteService;
}
namespace backend::workspace {
using PlanningRead = std::function<nlohmann::json(const nlohmann::json&)>;
// Resolve immutable native finding/source/plan/method evidence; never apply or start.
std::string nextRunPlanning(services::SqliteService&, const std::string&, bool,
                            PlanningRead analysis, PlanningRead method);
} // namespace backend::workspace
