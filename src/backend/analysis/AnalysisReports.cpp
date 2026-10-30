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
        } else if (kind == "overlay") {
            fields(ref, {"overlayId", "revision"});
            identifier(ref.at("overlayId"));
            if (!ref.at("revision").is_string() || ref.at("revision").get<std::string>().empty())
                throw std::invalid_argument("Exact overlay revision required");
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
        return Json::parse(db.analysisWorkspaceRequest(request.dump(), mutate));
    };
    if (action.rfind("reportBatch", 0) == 0) {
        const auto id = identifier(q.at("batchId"));
        const auto workspace = "analysis-report-batch/" + id;
        const auto current =
            store(Json{{"action", "read"}, {"workspace_id", workspace}, {"revision", nullptr}});
        if (action == "reportBatchRead") {
            fields(q, {"action", "batchId"});
            return current;
        }
        if (!write) throw std::invalid_argument("Report batch mutation requires write endpoint");
        if (action == "reportBatchCreate") {
            fields(q, {"action", "batchId", "commandId", "reports"});
            if (current.value("status", std::string{}) == "ok") {
                if (current.at("document").at("request") != q)
                    throw std::invalid_argument("Batch ID collision");
                return current;
            }
            if (current.value("status", std::string{}) != "not_found") return current;
            const auto& reports = q.at("reports");
            if (!reports.is_array() || reports.empty() || reports.size() > 16)
                throw std::invalid_argument("Batch requires 1..16 pinned reports");
            Json runs = Json::array();
            std::set<std::string> seen;
            for (const auto& pin : reports) {
                fields(pin, {"reportId", "revision"});
                if (!seen.insert(pin.dump()).second)
                    throw std::invalid_argument("Duplicate report pin");
                auto query = pin;
                query["action"] = "reportRead";
                auto resolved = analysisReportRequest(db, query, false, evaluate);
                if (resolved.value("status", std::string{}) != "ok") return resolved;
                auto run = pin;
                // Stable ownership from complete creation request; retry never invents another run.
                run["runId"] =
                    "batch-" + contentHash(Json{{"request", q}, {"index", runs.size()}}).substr(7);
                runs.push_back(run);
            }
            Json doc{{"schema_version", 1},
                     {"type", "F-report-batch/1"},
                     {"request", q},
                     {"state", "ready"},
                     {"runs", runs},
                     {"next", 0u},
                     {"results", Json::array()},
                     {"failure", nullptr}};
            return store(Json{{"action", "commit"},
                              {"workspace_id", workspace},
                              {"expected_revision", nullptr},
                              {"command_id", q.at("commandId")},
                              {"document", doc}},
                         true);
        }
        fields(q, {"action", "batchId", "revision"});
        if (action != "reportBatchAdvance" && action != "reportBatchCancel")
            throw std::invalid_argument("Unknown batch action");
        if (current.value("status", std::string{}) != "ok") return current;
        auto doc = current.at("document");
        if (doc.at("type") != "F-report-batch/1")
            throw std::invalid_argument("Report batch type mismatch");
        if (doc.at("state") == "cancelled" || doc.at("state") == "completed") return current;
        if (current.at("revision") != q.at("revision"))
            return Json{{"status", "conflict"},
                        {"message", "Refresh batch revision before advancing or cancelling"}};
        const auto command = "batch-step-" + contentHash(q).substr(7);
        if (action == "reportBatchCancel")
            doc["state"] = "cancelled";
        else {
            const auto next = doc.at("next").get<size_t>();
            auto build = doc.at("runs").at(next);
            build["action"] = "reportBuild";
            build["commandId"] =
                "batch-run-" +
                contentHash(Json{{"batch", doc.at("request")}, {"index", next}}).substr(7);
            Json result;
            try {
                result = analysisReportRequest(db, build, true, evaluate);
            } catch (const std::exception& error) {
                result = {{"status", "invalid"}, {"message", error.what()}};
            }
            if (result.value("status", std::string{}) == "ok") {
                doc["results"].push_back(
                    {{"runId", build.at("runId")},
                     {"revision", "1"},
                     {"contentHash", result.at("document").at("contentHash")}});
                doc["next"] = next + 1;
                doc["state"] = next + 1 == doc.at("runs").size() ? "completed" : "ready";
                doc["failure"] = nullptr;
            } else {
                doc["state"] = "failed";
                doc["failure"] = result;
            }
        }
        // A concurrent cancellation wins its CAS. An already-built immutable run may remain,
        // but no stale advance can overwrite the cancellation or schedule another report.
        return store(Json{{"action", "commit"},
                          {"workspace_id", workspace},
                          {"expected_revision", q.at("revision")},
                          {"command_id", command},
                          {"document", doc}},
                     true);
    }
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
        query["action"] = kind == "table"     ? "report"
                          : kind == "plot"    ? "plot"
                          : kind == "overlay" ? "overlayEvaluate"
                                              : "readFinding";
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
