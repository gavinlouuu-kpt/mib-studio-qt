#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include "backend/analysis/SourceAnalysis.h"
namespace backend::services {
class SqliteService;
}
namespace backend::analysis {
using SourceResolver = std::function<nlohmann::json(const nlohmann::json&)>;
using MethodResolver = std::function<nlohmann::json(const nlohmann::json&)>;
// No database/source ownership. Uses the existing app metadata owner and source fences.
// `source` resolves a descriptor to an F-source/1 (inline rows) or an F-source/2
// columnar handle; `columns` lends a columnar source's chunked columns (F-1b).
std::string analysisRequest(services::SqliteService& database, const std::string& request,
                            bool write, SourceResolver source, MethodResolver method = {},
                            ColumnResolver columns = {});
} // namespace backend::analysis
