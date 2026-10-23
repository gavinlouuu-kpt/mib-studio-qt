#include "backend/analysis/AnalysisReports.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
namespace {
void fields(const Json& q, const std::set<std::string>& names) {
    if (!q.is_object() || q.size() != names.size())
        throw std::invalid_argument("Incomplete report fields");
    for (auto i = q.begin(); i != q.end(); ++i)
        if (!names.count(i.key())) throw std::invalid_argument("Unknown report field");
}
std::string identifier(const Json& value) {
    const auto s = value.get<std::string>();
    if (s.empty() || s.size() > 128 ||
        s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
            std::string::npos)
        throw std::invalid_argument("Invalid report identity");
    return s;
}
void layout(const Json& value) {
    fields(value, {"title", "blocks"});
    if (!value.at("title").is_string() || value.at("title").get<std::string>().size() > 256 ||
        !value.at("blocks").is_array() || value.at("blocks").empty() ||
        value.at("blocks").size() > 8)
        throw std::invalid_argument("Report requires a bounded title and 1..8 blocks");
    std::set<std::string> ids;
    for (const auto& block : value.at("blocks")) {
        fields(block, {"id", "title", "kind", "reference"});
        if (!ids.insert(identifier(block.at("id"))).second || !block.at("title").is_string() ||
            block.at("title").get<std::string>().size() > 256)
            throw std::invalid_argument("Invalid or duplicate report block");
        const auto kind = block.at("kind").get<std::string>();
        const auto& ref = block.at("reference");
        if (kind == "finding") {
            fields(ref, {"findingId", "revision"});
            identifier(ref.at("findingId"));
            if (ref.at("revision") != "1")
                throw std::invalid_argument("Finding revision must be exact");
        } else if (kind == "table" || kind == "plot") {
            fields(ref, {"planId", "revision", "source"});
            if (!ref.at("planId").is_string() || !ref.at("revision").is_string() ||
                ref.at("revision").get<std::string>().empty())
                throw std::invalid_argument("Report requires exact plan revision");
            fields(ref.at("source"), {"kind", "id"});
            if (ref.at("source").at("kind") != "retained")
                throw std::invalid_argument("Report requires retained immutable source");
            identifier(ref.at("source").at("id"));
        } else
            throw std::invalid_argument("Unsupported report block kind");
    }
}
} // namespace
Json analysisReportRequest(services::SqliteService& db, const Json& q, bool write,
                           const std::function<Json(const Json&)>& evaluate) {
    const auto action = q.at("action").get<std::string>();
    const auto store = [&](const Json& request, bool mutate = false) {
        return Json::parse(db.workspaceRequest(request.dump(), mutate));
    };
    if (action == "reportSave") {
        if (!write) throw std::invalid_argument("Saving report layout requires write endpoint");
        fields(q, {"action", "reportId", "expectedRevision", "commandId", "layout"});
        const auto id = identifier(q.at("reportId"));
        layout(q.at("layout"));
        const Json doc{
            {"schema_version", 1}, {"type", "F-report-layout/1"}, {"layout", q.at("layout")}};
        return store(Json{{"action", "commit"},
                          {"workspace_id", "analysis-report/" + id},
                          {"expected_revision", q.at("expectedRevision")},
                          {"command_id", q.at("commandId")},
                          {"document", doc}},
                     true);
    }
    if (action == "reportRead" || action == "reportRunRead") {
        fields(q, {"action", "reportId", "revision"});
        const auto id = identifier(q.at("reportId"));
        if (!q.at("revision").is_string() || q.at("revision").get<std::string>().empty())
            throw std::invalid_argument("Exact report revision required");
        const bool run = action == "reportRunRead";
        if (run && q.at("revision") != "1")
            throw std::invalid_argument("Report runs are immutable revision 1");
        const auto saved =
            store(Json{{"action", "read"},
                       {"workspace_id", (run ? "analysis-report-run/" : "analysis-report/") + id},
                       {"revision", q.at("revision")}});
        if (saved.value("status", std::string{}) != "ok") return saved;
        const auto& doc = saved.at("document");
        if (doc.at("type") != (run ? "F-report-run/1" : "F-report-layout/1"))
            throw std::invalid_argument("Report type mismatch");
        if (run) {
            if (contentHash(doc.at("artifact")) != doc.at("contentHash"))
                throw std::invalid_argument("Report run integrity mismatch");
        } else
            layout(doc.at("layout"));
        return saved;
    }
    if (action != "reportBuild") throw std::invalid_argument("Unknown report action");
    if (!write)
        throw std::invalid_argument("Building immutable report run requires write endpoint");
    fields(q, {"action", "reportId", "revision", "runId", "commandId"});
    const auto id = identifier(q.at("reportId")), run = identifier(q.at("runId"));
    const auto existing = store(Json{
        {"action", "read"}, {"workspace_id", "analysis-report-run/" + run}, {"revision", "1"}});
    if (existing.value("status", std::string{}) == "ok") {
        const auto& doc = existing.at("document");
        if (doc.at("type") != "F-report-run/1" ||
            contentHash(doc.at("artifact")) != doc.at("contentHash") || doc.at("request") != q)
            throw std::invalid_argument("Report run collision or integrity mismatch");
        return existing;
    }
    if (existing.value("status", std::string{}) != "not_found") return existing;
    const auto saved = analysisReportRequest(
        db, Json{{"action", "reportRead"}, {"reportId", id}, {"revision", q.at("revision")}}, false,
        evaluate);
    if (saved.value("status", std::string{}) != "ok") return saved;
    const auto& spec = saved.at("document").at("layout");
    Json blocks = Json::array();
    for (const auto& block : spec.at("blocks")) {
        auto query = block.at("reference");
        const auto kind = block.at("kind").get<std::string>();
        query["action"] = kind == "table" ? "report" : kind == "plot" ? "plot" : "readFinding";
        auto result = evaluate(query);
        if (result.value("status", std::string{}) != "ok") return result;
        blocks.push_back(Json{{"spec", block}, {"result", result}});
    }
    const Json artifact{{"schema", "F-report-artifact/1"}, {"reportId", id},
                        {"revision", q.at("revision")},    {"layoutHash", contentHash(spec)},
                        {"title", spec.at("title")},       {"blocks", blocks}};
    const Json doc{{"schema_version", 1},
                   {"type", "F-report-run/1"},
                   {"request", q},
                   {"artifact", artifact},
                   {"contentHash", contentHash(artifact)}};
    if (doc.dump().size() > 900000)
        throw std::invalid_argument("Report run exceeds 900000-byte budget; reduce blocks");
    return store(Json{{"action", "commit"},
                      {"workspace_id", "analysis-report-run/" + run},
                      {"expected_revision", nullptr},
                      {"command_id", q.at("commandId")},
                      {"document", doc}},
                 true);
}
} // namespace backend::analysis
