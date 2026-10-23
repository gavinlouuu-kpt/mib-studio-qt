#include "backend/analysis/AnalysisHost.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
namespace {
void fields(const Json& q, const std::set<std::string>& allowed) {
    if (!q.is_object()) throw std::invalid_argument("Expected analysis request object");
    for (auto i = q.begin(); i != q.end(); ++i)
        if (!allowed.count(i.key()))
            throw std::invalid_argument("Unknown analysis request field: " + i.key());
}
std::string identity(const Json& q) {
    auto id = q.at("planId").get<std::string>();
    if (id.empty() || id.size() > 200) throw std::invalid_argument("Invalid plan ID");
    return "analysis/" + id;
}
Json unwrap(const Json& value) {
    if (value.value("status", std::string{}) != "ok") return value;
    if (!value.contains("document")) return value;
    if (value.at("document").value("type", std::string{}) != "F-analysis-strategy/1")
        throw std::invalid_argument("Stored revision is not an analysis strategy");
    validateAnalysisGraph(value.at("document").at("graph"));
    auto result = value;
    result["planId"] = value.at("workspace_id").get<std::string>().substr(9);
    result.erase("workspace_id");
    return result;
}
} // namespace
std::string analysisRequest(services::SqliteService& database, const std::string& text, bool write,
                            SourceResolver source, MethodResolver method) {
    try {
        if (text.size() > 131072) throw std::invalid_argument("Analysis request exceeds 128 KiB");
        const auto q = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 40) throw std::invalid_argument("Analysis request nesting exceeds limit");
            return true;
        });
        const auto action = q.at("action").get<std::string>();
        if (action == "snapshot") {
            fields(q, {"action", "source"});
            return Json{{"status", "ok"}, {"source", source(q.at("source"))}}.dump();
        }
        if (action == "method") {
            fields(q, {"action", "method"});
            if (!method)
                return Json{{"status", "unavailable"},
                            {"message", "Method registry is unavailable in this host"}}
                    .dump();
            return method(q.at("method")).dump();
        }
        if (action == "read" || action == "history") {
            fields(q, action == "read"
                          ? std::set<std::string>{"action", "planId", "revision"}
                          : std::set<std::string>{"action", "planId", "after", "limit"});
            Json request{{"action", action}, {"workspace_id", identity(q)}};
            for (const char* key : {"revision", "after", "limit"})
                if (q.contains(key)) request[key] = q.at(key);
            // Exact lookup never silently falls back to a moving head.
            if (action == "read" && (!q.contains("revision") || q.at("revision").is_null()))
                throw std::invalid_argument("Exact plan lookup requires revision");
            return unwrap(Json::parse(database.analysisWorkspaceRequest(request.dump(), false))).dump();
        }
        if (action == "save") {
            if (!write) throw std::invalid_argument("Save requires the analysis write endpoint");
            fields(q, {"action", "planId", "expectedRevision", "commandId", "graph", "feature",
                       "source", "sourceBinding", "method"});
            validateAnalysisGraph(q.at("graph"));
            if (q.at("graph").at("id") != q.at("planId"))
                throw std::invalid_argument("Graph ID must match plan ID");
            // Binding is mandatory immutable caller evidence. Resolve current source to
            // verify it; retries still replay the same saved document after a head change.
            const auto& binding = q.at("sourceBinding");
            fields(binding, {"fingerprint", "sourceKey", "parameters", "scope"});
            if (binding.size() != 4) throw std::invalid_argument("Incomplete source binding");
            Json doc{{"schema_version", 1},      {"type", "F-analysis-strategy/1"},
                     {"graph", q.at("graph")},   {"feature", q.at("feature")},
                     {"sourceBinding", binding}, {"method", q.value("method", Json(nullptr))}};
            const auto receipt = Json::parse(
                database.analysisWorkspaceRequest(Json{{"action", "receipt"},
                                               {"workspace_id", identity(q)},
                                               {"expected_revision", q.at("expectedRevision")},
                                               {"command_id", q.at("commandId")},
                                               {"document", doc}}
                                              .dump(),
                                          false));
            if (receipt.value("status", std::string{}) != "not_found")
                return unwrap(receipt).dump();
            const auto current = source(q.at("source"));
            for (const char* key : {"fingerprint", "sourceKey", "parameters", "scope"})
                if (current.at(key) != binding.at(key))
                    throw std::invalid_argument("Source binding changed; refresh explicitly");
            // Also validates source parameter availability and graph budgets.
            evaluateSource(current, doc);
            if (!doc.at("method").is_null()) {
                if (!method) throw std::invalid_argument("Method resolver unavailable");
                const auto resolved = method(doc.at("method"));
                if (resolved.value("status", std::string{}) != "ok") return resolved.dump();
            }
            return unwrap(Json::parse(database.analysisWorkspaceRequest(
                              Json{{"action", "commit"},
                                   {"workspace_id", identity(q)},
                                   {"expected_revision", q.at("expectedRevision")},
                                   {"command_id", q.at("commandId")},
                                   {"document", doc}}
                                  .dump(),
                              true)))
                .dump();
        }
        if (action == "restore") {
            if (!write) throw std::invalid_argument("Restore requires the analysis write endpoint");
            fields(q, {"action", "planId", "expectedRevision", "commandId", "targetRevision"});
            const auto old = unwrap(
                Json::parse(database.analysisWorkspaceRequest(Json{{"action", "read"},
                                                           {"workspace_id", identity(q)},
                                                           {"revision", q.at("targetRevision")}}
                                                          .dump(),
                                                      false)));
            if (old.value("status", std::string{}) != "ok") return old.dump();
            return unwrap(Json::parse(database.analysisWorkspaceRequest(
                              Json{{"action", "restore"},
                                   {"workspace_id", identity(q)},
                                   {"expected_revision", q.at("expectedRevision")},
                                   {"command_id", q.at("commandId")},
                                   {"target_revision", q.at("targetRevision")}}
                                  .dump(),
                              true)))
                .dump();
        }
        if (action == "evaluate" || action == "jobInput" || action == "gallery") {
            fields(q,
                   action == "gallery"
                       ? std::set<std::string>{"action", "planId", "revision", "source", "offset"}
                       : std::set<std::string>{"action", "planId", "revision", "source"});
            if (q.at("revision").is_null())
                throw std::invalid_argument("Evaluation requires pinned revision");
            const auto saved =
                unwrap(Json::parse(database.analysisWorkspaceRequest(Json{{"action", "read"},
                                                                  {"workspace_id", identity(q)},
                                                                  {"revision", q.at("revision")}}
                                                                 .dump(),
                                                             false)));
            if (saved.value("status", std::string{}) != "ok") return saved.dump();
            const auto& doc = saved.at("document");
            const auto current = source(q.at("source"));
            // Live run and archived run share scientific content keys; their access scope
            // differs intentionally. Never use pathname equality as scientific identity.
            for (const char* key : {"fingerprint", "sourceKey", "parameters"})
                if (current.at(key) != doc.at("sourceBinding").at(key))
                    return Json{
                        {"status", "source_mismatch"},
                        {"message",
                         "Pinned source does not match; create an explicit new application"}}
                        .dump();
            if (!doc.at("method").is_null()) {
                if (!method)
                    return Json{{"status", "unavailable"},
                                {"message", "Pinned method resolver unavailable"}}
                        .dump();
                const auto resolved = method(doc.at("method"));
                if (resolved.value("status", std::string{}) != "ok") return resolved.dump();
            }
            auto result = evaluateSource(current, doc);
            result["planId"] = q.at("planId");
            result["revision"] = saved.at("revision");
            if (action == "gallery") {
                if (!q.at("offset").is_number_unsigned() || q.at("offset").get<uint64_t>() > 10000)
                    throw std::invalid_argument("Invalid gallery offset");
                const auto offset = q.at("offset").get<std::size_t>();
                Json keys = Json::array(), images = Json::array();
                std::size_t selected = 0;
                for (const auto& row : result.at("rows")) {
                    if (row.at("membership") != "selected") continue;
                    if (selected++ < offset) continue;
                    if (keys.size() == 12) break;
                    keys.push_back(row.at("key"));
                    images.push_back(
                        {{"key", row.at("key")},
                         {"pixels", nullptr},
                         {"reason", "Provider does not supply exact retained-image locators"}});
                }
                if (current.at("scope") == "stored") {
                    const auto gallery = source(Json{{"kind", "stored-gallery"},
                                                     {"path", q.at("source").at("path")},
                                                     {"fingerprint", current.at("fingerprint")},
                                                     {"keys", keys}});
                    images = gallery.at("images");
                }
                return Json{{"status", "ok"},
                            {"schema", "F-gallery/1"},
                            {"sourceFingerprint", current.at("fingerprint")},
                            {"planId", q.at("planId")},
                            {"revision", saved.at("revision")},
                            {"offset", offset},
                            {"selected", result.at("selected")},
                            {"images", images}}
                    .dump();
            }
            Json reply{{"status", "ok"}, {"evaluation", result}};
            if (action == "jobInput") reply["source"] = current;
            return reply.dump();
        }
        throw std::invalid_argument("Unsupported analysis action");
    } catch (const std::exception& e) {
        return Json{{"status", "invalid"}, {"message", e.what()}}.dump();
    }
}
} // namespace backend::analysis
