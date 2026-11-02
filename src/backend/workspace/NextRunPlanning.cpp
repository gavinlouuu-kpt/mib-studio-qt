#include "backend/workspace/NextRunPlanning.h"
#include "backend/database/SqliteService.h"
#include "backend/analysis/SourceAnalysis.h"
#include <regex>
#include <set>
namespace backend::workspace {
using J = nlohmann::json;
namespace {
J requireOk(J r) {
    if (r.value("status", std::string{}) != "ok")
        throw std::invalid_argument(r.value("message", r.dump()));
    return r;
}
void fields(const J& q, const std::set<std::string>& expected) {
    if (!q.is_object() || q.size() != expected.size())
        throw std::invalid_argument("Unexpected planning fields");
    for (const auto& k : expected)
        if (!q.contains(k)) throw std::invalid_argument("Missing planning field: " + k);
}
void uuid(const J& v) {
    static const std::regex pattern("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$");
    if (!v.is_string() || !std::regex_match(v.get<std::string>(), pattern))
        throw std::invalid_argument("Fresh UUID required");
}
void validatePlan(const J& doc) {
    if (doc.at("schema_version") != 1)
        throw std::invalid_argument("Unsupported workspace document");
    const auto& plan = doc.at("plan");
    uuid(plan.at("experimentId"));
    if (plan.at("format") != "single" && plan.at("format") != "queue")
        throw std::invalid_argument("Invalid plan format");
    if (!plan.at("operator").at("id").is_string() || !plan.at("operator").at("name").is_string())
        throw std::invalid_argument("Invalid declared operator");
    const auto& rows = plan.at("rows");
    if (!rows.is_array() || rows.empty() || rows.size() > 1000 ||
        (plan.at("format") == "single" && rows.size() != 1))
        throw std::invalid_argument("Invalid sample sheet");
    std::set<std::string> refs;
    for (const auto& row : rows) {
        for (const auto* key :
             {"ref", "sample", "condition", "experimentalUnit", "concentrationValue",
              "concentrationUnit", "channel", "medium", "temperatureC", "notes"})
            if (!row.at(key).is_string()) throw std::invalid_argument("Invalid sample annotation");
        const auto ref = row.at("ref").get<std::string>();
        if (ref.empty() || !refs.insert(ref).second || !row.at("free").is_array())
            throw std::invalid_argument("Invalid sample identity");
        for (const auto& annotation : row.at("free"))
            if (!annotation.at("key").is_string() || !annotation.at("value").is_string())
                throw std::invalid_argument("Invalid free annotation");
    }
}
} // namespace
std::string nextRunPlanning(services::SqliteService& db, const std::string& text, bool write,
                            PlanningRead analysisRead, PlanningRead methodRead) {
    try {
        if (text.size() > 16384) throw std::invalid_argument("Planning request exceeds 16 KiB");
        const auto q = J::parse(text);
        const bool create = q.at("action") == "draft_from_finding";
        const bool verify = q.at("action") == "verify_next_run";
        if (!create && !verify && q.at("action") != "resolve_finding")
            throw std::invalid_argument("Unsupported planning action");
        if (create && !write)
            throw std::invalid_argument("Draft creation requires control-only write endpoint");
        fields(q, create ? std::set<std::string>{"action", "workspace_id", "command_id",
                                                 "source_workspace_id", "source_revision",
                                                 "finding", "sample_index", "profile_base",
                                                 "experiment_id", "recipe_id"}
                  : verify
                      ? std::set<std::string>{"action", "workspace_id", "revision", "profile_base"}
                      : std::set<std::string>{"action", "workspace_id", "revision", "finding",
                                              "sample_index", "profile_base"});
        const auto target = q.at("workspace_id").get<std::string>();
        if (target.rfind("analysis/", 0) == 0 || target.rfind("analysis-", 0) == 0)
            throw std::invalid_argument("Reserved analysis namespace");
        if (create) {
            auto receipt =
                J::parse(db.analysisWorkspaceRequest(J{{"action", "derived_receipt"},
                                                       {"workspace_id", target},
                                                       {"command_id", q.at("command_id")},
                                                       {"origin_request", q}}
                                                         .dump(),
                                                     false));
            if (receipt.at("status") != "not_found") return receipt.dump();
            uuid(q.at("experiment_id"));
            uuid(q.at("recipe_id"));
        }
        const auto sourceId = q.at(create ? "source_workspace_id" : "workspace_id");
        const auto sourceRev = q.at(create ? "source_revision" : "revision");
        if (sourceRev.is_null() || !sourceRev.is_string())
            throw std::invalid_argument("Exact source workspace revision required");
        auto saved = requireOk(J::parse(db.workspaceRequest(
            J{{"action", "read"}, {"workspace_id", sourceId}, {"revision", sourceRev}}.dump(),
            false)));
        auto head = requireOk(J::parse(
            db.workspaceRequest(J{{"action", "read"}, {"workspace_id", sourceId}}.dump(), false)));
        if (!verify && head.at("revision") != sourceRev)
            return J{{"status", "conflict"},
                     {"message", "Source workspace changed; reopen before planning"}}
                .dump();
        const auto& doc = saved.at("document");
        validatePlan(doc);
        const auto& recipe = doc.at("recipe");
        const auto& ref = verify ? doc.at("next_run_evidence").at("finding") : q.at("finding");
        fields(ref, {"findingId", "revision", "contentHash"});
        if (ref.at("revision") != "1")
            throw std::invalid_argument("Exact immutable finding revision 1 required");
        auto found = requireOk(analysisRead(J{{"action", "readFinding"},
                                              {"findingId", ref.at("findingId")},
                                              {"revision", ref.at("revision")}}));
        if (found.at("findingId") != ref.at("findingId") ||
            found.at("revision") != ref.at("revision"))
            throw std::invalid_argument("Finding identity mismatch");
        const auto& finding = found.at("document");
        if (finding.at("type") != "F-analysis-finding/1" ||
            finding.at("contentHash") != ref.at("contentHash") ||
            analysis::contentHash(finding.at("comparison")) != ref.at("contentHash"))
            throw std::invalid_argument("Finding comparison hash mismatch");
        // Resolve every immutable dependency and recompute through F, never browser rows.
        const auto comparison = requireOk(
            analysisRead(J{{"action", "compare"}, {"spec", finding.at("request").at("spec")}}));
        if (comparison.at("contentHash") != ref.at("contentHash") ||
            comparison.at("comparison") != finding.at("comparison"))
            throw std::invalid_argument("Finding dependency evidence changed");
        const auto& selected =
            verify ? doc.at("next_run_evidence").at("sampleIndex") : q.at("sample_index");
        if (!selected.is_number_unsigned())
            throw std::invalid_argument("Select an explicit finding sample");
        const auto index = selected.get<std::size_t>();
        const auto& sample = finding.at("comparison").at("samples").at(index);
        const auto& sampleRef = sample.at("reference");
        if (sampleRef.at("planId") != recipe.at("analysis_plan").at("id") ||
            sampleRef.at("revision") != recipe.at("analysis_plan").at("revision"))
            throw std::invalid_argument(
                "Finding sample and workspace pin different analysis revisions");
        auto strategy = requireOk(analysisRead(J{{"action", "read"},
                                                 {"planId", sampleRef.at("planId")},
                                                 {"revision", sampleRef.at("revision")}}));
        const J methodRequest = {{"action", "resolve_recipe"},
                                 {"workspace_id", sourceId},
                                 {"revision", sourceRev},
                                 {"profile_base", q.at("profile_base")}};
        auto resolved = requireOk(methodRead(methodRequest));
        if (resolved.at("method").at("status") != "resolved")
            throw std::invalid_argument("Pinned method unavailable");
        const auto& method = strategy.at("document").at("method");
        if (!method.is_null() && (method.at("method_id") != recipe.at("method").at("id") ||
                                  method.at("revision_id") != recipe.at("method").at("revision") ||
                                  method.at("content_hash") !=
                                      resolved.at("method").value("content_sha256", std::string{})))
            throw std::invalid_argument("Finding strategy and acquisition method mismatch");
        if (methodRead(methodRequest) != resolved)
            throw std::invalid_argument("Method changed during planning");
        const J evidence = {{"schema", "I-next-run-evidence/1"},
                            {"finding", ref},
                            {"findingDocumentHash", analysis::contentHash(finding)},
                            {"sourceWorkspace", {{"id", sourceId}, {"revision", sourceRev}}},
                            {"sampleIndex", index},
                            {"sample", sample},
                            {"analysisPlan",
                             {{"id", sampleRef.at("planId")},
                              {"revision", sampleRef.at("revision")},
                              {"documentHash", analysis::contentHash(strategy.at("document"))}}},
                            {"method", resolved.at("method")},
                            {"compatibility", "unknown"},
                            {"hardwareReadiness", "not_evaluated"},
                            {"applied", false}};
        if (verify) {
            const auto& recorded = doc.at("next_run_evidence");
            for (const auto* key :
                 {"schema", "finding", "findingDocumentHash", "sampleIndex", "sample",
                  "analysisPlan", "method", "compatibility", "hardwareReadiness", "applied"})
                if (recorded.at(key) != evidence.at(key))
                    throw std::invalid_argument("Recorded next-run evidence mismatch");
            return J{{"status", "ok"}, {"evidence", recorded}, {"verified", true}}.dump();
        }
        if (!create) return J{{"status", "ok"}, {"evidence", evidence}}.dump();
        if (q.at("experiment_id") == doc.at("plan").at("experimentId") ||
            q.at("recipe_id") == recipe.at("id"))
            throw std::invalid_argument("Next run requires fresh experiment and recipe identities");
        // Copy planning declarations only; never copy an active queue, outcomes, or reference run.
        J draft = {
            {"schema_version", 1},
            {"title", doc.value("title", std::string{"Workspace"}).substr(0, 480) + " — next run"},
            {"plan",
             {{"experimentId", doc.at("plan").at("experimentId")},
              {"format", doc.at("plan").at("format")},
              {"operator", doc.at("plan").at("operator")},
              {"rows", doc.at("plan").at("rows")}}},
            {"recipe", recipe},
            {"next_run_evidence", evidence}};
        draft["plan"]["experimentId"] = q.at("experiment_id");
        draft["recipe"]["id"] = q.at("recipe_id");
        draft["recipe"]["revision"] = "1";
        draft["recipe"]["lineage"] = {
            {"recipe", {{"id", recipe.at("id")}, {"revision", recipe.at("revision")}}},
            {"finding", {{"id", ref.at("findingId")}, {"revision", ref.at("revision")}}}};
        if (doc.contains("annotation_display"))
            draft["annotation_display"] = doc.at("annotation_display");
        return db.analysisWorkspaceRequest(J{{"action", "derive"},
                                             {"workspace_id", target},
                                             {"command_id", q.at("command_id")},
                                             {"origin_request", q},
                                             {"document", draft}}
                                               .dump(),
                                           true);
    } catch (const std::exception& e) {
        return J{{"status", "invalid"}, {"message", e.what()}}.dump();
    }
}
} // namespace backend::workspace
