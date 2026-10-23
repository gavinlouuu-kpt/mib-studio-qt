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
        const auto nativeSource = source;
        source = [&](const Json& descriptor) -> Json {
            if (descriptor.value("kind", std::string{}) != "retained")
                return nativeSource(descriptor);
            fields(descriptor, {"kind", "id"});
            const auto id = descriptor.at("id").get<std::string>();
            if (id.empty() || id.size() > 128 ||
                id.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
                    std::string::npos)
                throw std::invalid_argument("Invalid retained snapshot ID");
            const auto saved = Json::parse(
                database.workspaceRequest(Json{{"action", "read"},
                                               {"workspace_id", "analysis-source/" + id},
                                               {"revision", "1"}}
                                              .dump(),
                                          false));
            if (saved.value("status", std::string{}) != "ok")
                throw std::invalid_argument("Retained snapshot unavailable");
            const auto& doc = saved.at("document");
            if (doc.at("type") != "F-retained-source/1" ||
                contentHash(doc.at("source")) != doc.at("contentHash"))
                throw std::invalid_argument("Retained snapshot integrity mismatch");
            return doc.at("source");
        };
        if (action == "retainSource") {
            if (!write)
                throw std::invalid_argument("Retaining a snapshot requires the write endpoint");
            fields(q, {"action", "source", "commandId"});
            const auto id = q.at("commandId").get<std::string>();
            if (id.empty() || id.size() > 128 ||
                id.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
                    std::string::npos)
                throw std::invalid_argument("Invalid retained snapshot command ID");
            const auto reply = [&](const Json& doc) {
                if (doc.at("request") != q || doc.at("type") != "F-retained-source/1" ||
                    contentHash(doc.at("source")) != doc.at("contentHash"))
                    throw std::invalid_argument(
                        "Retained snapshot command collision or integrity mismatch");
                return Json{{"status", "ok"},
                            {"source", doc.at("source")},
                            {"contentHash", doc.at("contentHash")},
                            {"descriptor", {{"kind", "retained"}, {"id", id}}}}
                    .dump();
            };
            const auto existing = Json::parse(
                database.workspaceRequest(Json{{"action", "read"},
                                               {"workspace_id", "analysis-source/" + id},
                                               {"revision", "1"}}
                                              .dump(),
                                          false));
            if (existing.value("status", std::string{}) == "ok")
                return reply(existing.at("document"));
            if (existing.value("status", std::string{}) != "not_found") return existing.dump();
            const auto frozen = source(q.at("source"));
            if (frozen.dump().size() > 900000)
                throw std::invalid_argument(
                    "Retained snapshot exceeds 900000-byte metadata budget; no sampling performed");
            const Json doc{{"schema_version", 1},
                           {"type", "F-retained-source/1"},
                           {"source", frozen},
                           {"contentHash", contentHash(frozen)},
                           {"request", q}};
            const auto saved = Json::parse(
                database.workspaceRequest(Json{{"action", "commit"},
                                               {"workspace_id", "analysis-source/" + id},
                                               {"expected_revision", nullptr},
                                               {"command_id", id},
                                               {"document", doc}}
                                              .dump(),
                                          true));
            if (saved.value("status", std::string{}) != "ok") return saved.dump();
            return reply(saved.at("document"));
        }

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
            return unwrap(Json::parse(database.workspaceRequest(request.dump(), false))).dump();
        }
        if (action == "manifest") {
            fields(q, {"action", "planId", "revision"});
            if (q.at("revision").is_null())
                throw std::invalid_argument("Manifest requires exact revision");
            const auto saved =
                unwrap(Json::parse(database.workspaceRequest(Json{{"action", "read"},
                                                                  {"workspace_id", identity(q)},
                                                                  {"revision", q.at("revision")}}
                                                                 .dump(),
                                                             false)));
            if (saved.value("status", std::string{}) != "ok") return saved.dump();
            Json manifest{{"schema", "F-analysis-manifest/1"},
                          {"planId", q.at("planId")},
                          {"originRevision", saved.at("revision")},
                          {"document", saved.at("document")}};
            return Json{
                {"status", "ok"}, {"manifest", manifest}, {"contentHash", contentHash(manifest)}}
                .dump();
        }
        if (action == "import") {
            if (!write) throw std::invalid_argument("Import requires write endpoint");
            fields(q, {"action", "manifest", "contentHash", "source", "expectedRevision",
                       "commandId"});
            const auto& manifest = q.at("manifest");
            fields(manifest, {"schema", "planId", "originRevision", "document"});
            if (manifest.at("schema") != "F-analysis-manifest/1" ||
                contentHash(manifest) != q.at("contentHash"))
                throw std::invalid_argument("Manifest schema/hash mismatch");
            const auto& doc = manifest.at("document");
            fields(doc, {"schema_version", "type", "graph", "feature", "sourceBinding", "method"});
            if (doc.at("schema_version") != 1 || doc.at("type") != "F-analysis-strategy/1")
                throw std::invalid_argument("Unsupported strategy manifest");
            Json save{{"action", "save"},
                      {"planId", manifest.at("planId")},
                      {"graph", doc.at("graph")},
                      {"feature", doc.at("feature")},
                      {"sourceBinding", doc.at("sourceBinding")},
                      {"method", doc.at("method")},
                      {"source", q.at("source")},
                      {"expectedRevision", q.at("expectedRevision")},
                      {"commandId", q.at("commandId")}};
            return analysisRequest(database, save.dump(), true, source, method);
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
                database.workspaceRequest(Json{{"action", "receipt"},
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
            return unwrap(Json::parse(database.workspaceRequest(
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
                Json::parse(database.workspaceRequest(Json{{"action", "read"},
                                                           {"workspace_id", identity(q)},
                                                           {"revision", q.at("targetRevision")}}
                                                          .dump(),
                                                      false)));
            if (old.value("status", std::string{}) != "ok") return old.dump();
            return unwrap(Json::parse(database.workspaceRequest(
                              Json{{"action", "restore"},
                                   {"workspace_id", identity(q)},
                                   {"expected_revision", q.at("expectedRevision")},
                                   {"command_id", q.at("commandId")},
                                   {"target_revision", q.at("targetRevision")}}
                                  .dump(),
                              true)))
                .dump();
        }
        if (action == "evaluate" || action == "jobInput" || action == "gallery" ||
            action == "report") {
            fields(q,
                   action == "gallery"
                       ? std::set<std::string>{"action", "planId", "revision", "source", "offset"}
                       : std::set<std::string>{"action", "planId", "revision", "source"});
            if (q.at("revision").is_null())
                throw std::invalid_argument("Evaluation requires pinned revision");
            const auto saved =
                unwrap(Json::parse(database.workspaceRequest(Json{{"action", "read"},
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
            if (action == "report") {
                const auto& nodes = doc.at("graph").at("nodes");
                if (nodes.size() > 32)
                    throw std::invalid_argument("Report supports at most 32 populations");
                Json table = Json::array();
                const std::vector<std::string> columns{"population",
                                                       "feature",
                                                       "unit",
                                                       "selected",
                                                       "excluded",
                                                       "unknown",
                                                       "measured",
                                                       "missing",
                                                       "minimum",
                                                       "maximum",
                                                       "mean",
                                                       "median",
                                                       "p05",
                                                       "p25",
                                                       "p75",
                                                       "p95",
                                                       "sampleStandardDeviation",
                                                       "cvPercent",
                                                       "geometricMean",
                                                       "denominator"};
                auto csvCell = [](const Json& cell) {
                    if (cell.is_null()) return std::string{};
                    if (!cell.is_string()) return cell.dump();
                    auto raw = cell.get<std::string>();
                    // Do not turn user-authored labels into spreadsheet formulas.
                    if (!raw.empty() && (raw[0] == '=' || raw[0] == '+' || raw[0] == '-' ||
                                         raw[0] == '@' || raw[0] == '\t' || raw[0] == '\r'))
                        raw.insert(raw.begin(), '\'');
                    std::string escaped = "\"";
                    for (const char c : raw) {
                        if (c == '"') escaped += '"';
                        escaped += c;
                    }
                    return escaped + "\"";
                };
                std::string csv;
                for (std::size_t i = 0; i < columns.size(); ++i) {
                    if (i) csv += ',';
                    csv += columns[i];
                }
                csv += "\r\n";
                for (const auto& node : nodes) {
                    auto request = doc;
                    request["graph"]["output"] = node.at("id");
                    auto row = evaluateSource(current, request);
                    row["population"] = node.at("id");
                    Json compact = Json::object();
                    for (std::size_t i = 0; i < columns.size(); ++i) {
                        const auto& key = columns[i];
                        compact[key] = row.at(key);
                        if (i) csv += ',';
                        csv += csvCell(row.at(key));
                    }
                    csv += "\r\n";
                    table.push_back(std::move(compact));
                }
                Json report{{"schema", "F-analysis-report/1"},
                            {"planId", q.at("planId")},
                            {"revision", saved.at("revision")},
                            {"sourceBinding", doc.at("sourceBinding")},
                            {"strategy", doc},
                            {"estimators", result.at("estimators")},
                            {"table", table},
                            {"scientificCompatibility", "unknown"},
                            {"qcPolicy", "unknown-not-filtered"},
                            {"populationComplete", current.at("populationComplete")}};
                return Json{{"status", "ok"},
                            {"report", report},
                            {"contentHash", contentHash(report)},
                            {"csv", csv},
                            {"csvNullSemantics", "empty cell means unavailable, never zero"}}
                    .dump();
            }
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
                                                     {"path", current.at("path")},
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
