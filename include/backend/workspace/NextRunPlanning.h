#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace backend::services {
class SqliteService;
}
namespace backend::workspace {
class RecipeProvider;
using PlanningRead = std::function<nlohmann::json(const nlohmann::json&)>;
// Resolve immutable native finding/source/plan/method evidence; never apply or start.
// draft_from_finding derives the next-run recipe through the native recipe provider (#807 I02):
// without `recipes` it refuses to create a draft.
std::string nextRunPlanning(services::SqliteService&, const std::string&, bool,
                            PlanningRead analysis, PlanningRead method,
                            const RecipeProvider* recipes = nullptr);
} // namespace backend::workspace
