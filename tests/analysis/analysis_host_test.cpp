#include "backend/analysis/AnalysisHost.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/workspace/NextRunPlanning.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <thread>
using nlohmann::json;
int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir temp("analysis_host");
    const auto path = (temp.path() / "app.sqlite3").string();
    json source{{"schema", "F-source/1"},
                {"fingerprint", "sha256:original"},
                {"sourceKey", "source"},
                {"parameters", {{"area", "pixel2"}}},
                {"scope", "stored"},
                {"populationComplete", true},
                {"rows",
                 {{{"key", "row1"}, {"measurements", {{"area", 5}}}},
                  {{"key", "row2"}, {"measurements", {{"area", nullptr}}}}}}};
    json graph{{"id", "plan"},
               {"revision", "draft1"},
               {"output", "gate"},
               {"nodes",
                {{{"id", "gate"},
                  {"parent", ""},
                  {"kind", "interval"},
                  {"feature", "area"},
                  {"minimum", 1},
                  {"maximum", 10}}}}};
    json binding;
    for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"})
        binding[k] = source[k];
    json save{{"action", "save"},
              {"planId", "plan"},
              {"expectedRevision", nullptr},
              {"commandId", "first"},
              {"graph", graph},
              {"feature", "area"},
              {"source", {{"kind", "stored"}, {"path", "test"}}},
              {"sourceBinding", binding}};
    {
        auto plottedSource = source;
        plottedSource["rows"] = json::array();
        for (const auto& value : json::array({0, 1, 2, nullptr, -1, 3})) {
            const auto key = std::to_string(plottedSource["rows"].size());
            plottedSource["rows"].push_back({{"key", key}, {"measurements", {{"area", value}}}});
        }
        auto g = graph;
        g["nodes"][0]["minimum"] = nullptr;
        g["nodes"][0]["maximum"] = nullptr;
        json spec{{"kind", "histogram"},
                  {"feature", "area"},
                  {"minimum", 0},
                  {"maximum", 2},
                  {"bins", 2u},
                  {"normalization", "fraction"},
                  {"layers", {{{"population", "gate"}, {"displayOffset", 0}}}}};
        json query{{"graph", g}, {"feature", "area"}, {"plot", spec}};
        const auto plot = backend::analysis::plotSource(plottedSource, query);
        MIB_EXPECT(plot["layers"][0]["counts"] == json::array({"1", "2"}),
                   "left-closed bins and inclusive final boundary");
        MIB_EXPECT(plot["layers"][0]["values"] == json::array({.2, .4}),
                   "normalization includes finite under/overflow denominator");
        MIB_EXPECT(plot["layers"][0]["underflow"] == "1" && plot["layers"][0]["overflow"] == "1" &&
                       plot["layers"][0]["unknown"] == "1",
                   "plot names outside range and unknown membership");
        query["plot"]["layers"][0]["displayOffset"] = 99;
        MIB_EXPECT(backend::analysis::plotSource(plottedSource, query)["layers"][0]["counts"] ==
                       plot["layers"][0]["counts"],
                   "display offset cannot change counts");
    }
    json retainedDescriptor;
    auto resolver = [&](const json&) { return source; };
    {
        backend::services::SqliteService db;
        MIB_REQUIRE(db.initialize(path), "open metadata authority");
        auto call = [&](const json& q, bool write = true) {
            return json::parse(backend::analysis::analysisRequest(db, q.dump(), write, resolver));
        };
        MIB_EXPECT(call(save, false)["status"] == "invalid", "read endpoint cannot save");
        MIB_EXPECT(call(save)["revision"] == "1", "create strategy");
        auto next = save;
        next["commandId"] = "second";
        next["expectedRevision"] = "1";
        MIB_EXPECT(call(next)["revision"] == "2", "CAS append");
        source["fingerprint"] = "sha256:changed";
        MIB_EXPECT(call(save)["revision"] == "1", "lost-reply replay survives source/head change");
        auto collision = save;
        collision["feature"] = "deformability";
        MIB_EXPECT(call(collision)["status"] == "invalid",
                   "command collision fails before source resolution");
        next["commandId"] = "wrong-source";
        next["expectedRevision"] = "2";
        MIB_EXPECT(call(next)["status"] == "invalid", "new write rejects changed source");
        json evaluate{{"action", "evaluate"},
                      {"planId", "plan"},
                      {"revision", "1"},
                      {"source", save["source"]}};
        MIB_EXPECT(call(evaluate, false)["status"] == "source_mismatch",
                   "evaluation fences source");
        source["fingerprint"] = binding["fingerprint"];
        const auto result = call(evaluate, false);
        MIB_EXPECT(result["evaluation"]["selected"] == "1" &&
                       result["evaluation"]["unknown"] == "1",
                   "native evaluation preserves unknown");
        MIB_EXPECT(result["evaluation"]["mean"] == 5, "selected reduction");
        const auto retained = call(json{{"action", "retainSource"},
                                        {"source", save.at("source")},
                                        {"commandId", "freeze-one"}});
        MIB_EXPECT(retained["status"] == "ok", "retain exact native source in metadata authority");
        retainedDescriptor = retained.at("descriptor");
        auto retainedEvaluation = evaluate;
        retainedEvaluation["source"] = retained.at("descriptor");
        source["fingerprint"] = "sha256:advanced";
        MIB_EXPECT(call(json{{"action", "retainSource"},
                             {"source", save.at("source")},
                             {"commandId", "freeze-one"}}) == retained,
                   "retention lost-reply replay preserves original snapshot after source advances");
        MIB_EXPECT(call(retainedEvaluation, false)["evaluation"]["mean"] == 5,
                   "retained snapshot does not follow advancing live source");
        source["fingerprint"] = binding["fingerprint"];
        auto reportQuery = evaluate;
        reportQuery["action"] = "report";
        const auto report = call(reportQuery, false);
        MIB_EXPECT(report["status"] == "ok" && report["report"]["table"][0]["median"] == 5,
                   "revision-bound table uses native reductions");
        MIB_EXPECT(report["report"]["table"][0]["sampleStandardDeviation"].is_null(),
                   "singleton sample SD stays null in export");
        MIB_EXPECT(report["contentHash"] == call(reportQuery, false)["contentHash"],
                   "deterministic report content identity");
        const auto manifest =
            call(json{{"action", "manifest"}, {"planId", "plan"}, {"revision", "1"}}, false);
        backend::services::SqliteService imported;
        MIB_REQUIRE(imported.initialize(":memory:"),
                    "separate metadata authority for portable import");
        json import{{"action", "import"},
                    {"manifest", manifest.at("manifest")},
                    {"contentHash", manifest.at("contentHash")},
                    {"source", save.at("source")},
                    {"expectedRevision", nullptr},
                    {"commandId", "import-one"}};
        const auto importedReply = json::parse(
            backend::analysis::analysisRequest(imported, import.dump(), true, resolver));
        MIB_EXPECT(importedReply["revision"] == "1" &&
                       importedReply["document"] == manifest["manifest"]["document"],
                   "portable manifest restores typed source-bound strategy in a new authority");
        import["manifest"]["document"]["feature"] = "deformability";
        MIB_EXPECT(json::parse(backend::analysis::analysisRequest(imported, import.dump(), true,
                                                                  resolver))["status"] == "invalid",
                   "altered manifest checksum rejected");
        auto qcPlan = save;
        qcPlan["planId"] = "qc-plan";
        qcPlan["graph"]["id"] = "qc-plan";
        qcPlan["commandId"] = "qc-one";
        qcPlan["qc"] = {{"policy", "good-only"}, {"decisions", json::array()}};
        MIB_EXPECT(call(qcPlan)["status"] == "ok", "save explicit unknown QC policy");
        json qcQuery{{"action", "evaluate"},
                     {"planId", "qc-plan"},
                     {"revision", "1"},
                     {"source", save.at("source")}};
        const auto unknownQc = call(qcQuery, false);
        MIB_EXPECT(unknownQc["evaluation"]["selected"] == "0" &&
                       unknownQc["evaluation"]["unknown"] == "2",
                   "unlabelled QC never silently admitted as good");
        qcPlan["expectedRevision"] = "1";
        qcPlan["commandId"] = "qc-two";
        qcPlan["qc"]["decisions"].push_back({{"key", "row1"},
                                             {"status", "good"},
                                             {"actor", "test operator"},
                                             {"reason", "manual morphology review"}});
        MIB_EXPECT(call(qcPlan)["revision"] == "2", "append attributed QC assertion");
        qcQuery["revision"] = "2";
        const auto qualified = call(qcQuery, false);
        MIB_EXPECT(qualified["evaluation"]["selected"] == "1" &&
                       qualified["evaluation"]["qcCounts"]["unknown"] == "1",
                   "manual good and unknown remain distinct");
        const auto qcManifest =
            call(json{{"action", "manifest"}, {"planId", "qc-plan"}, {"revision", "2"}}, false);
        json qcImport{{"action", "import"},
                      {"manifest", qcManifest.at("manifest")},
                      {"contentHash", qcManifest.at("contentHash")},
                      {"source", save.at("source")},
                      {"expectedRevision", nullptr},
                      {"commandId", "qc-import"}};
        const auto importedQc = json::parse(
            backend::analysis::analysisRequest(imported, qcImport.dump(), true, resolver));
        MIB_EXPECT(importedQc["document"]["qc"] == qcManifest["manifest"]["document"]["qc"],
                   "portable QC attribution round trip");
        qcPlan["expectedRevision"] = "2";
        qcPlan["commandId"] = "qc-foreign";
        qcPlan["qc"]["decisions"][0]["key"] = "other-source-row";
        MIB_EXPECT(call(qcPlan)["status"] == "invalid", "QC refuses foreign exact event key");
        auto plottedPlan = save;
        plottedPlan["planId"] = "plot-plan";
        plottedPlan["graph"]["id"] = "plot-plan";
        plottedPlan["commandId"] = "plot-save";
        plottedPlan["plot"] = {{"kind", "histogram"},
                               {"feature", "area"},
                               {"minimum", 0},
                               {"maximum", 10},
                               {"bins", 2},
                               {"normalization", "count"},
                               {"layers", {{{"population", "gate"}, {"displayOffset", 0}}}}};
        MIB_EXPECT(call(plottedPlan)["status"] == "ok", "persist validated PlotSpec with strategy");
        const auto plotReply = call(json{{"action", "plot"},
                                         {"planId", "plot-plan"},
                                         {"revision", "1"},
                                         {"source", save.at("source")}},
                                    false);
        MIB_EXPECT(plotReply["plot"]["layers"][0]["counts"] == json::array({"0", "1"}),
                   "saved PlotSpec is evaluated through production host");
        auto csvPlan = save;
        csvPlan["planId"] = "csv-plan";
        csvPlan["graph"]["id"] = "csv-plan";
        csvPlan["graph"]["output"] = "=1+1";
        csvPlan["graph"]["nodes"][0]["id"] = "=1+1";
        csvPlan["commandId"] = "csv-label";
        MIB_EXPECT(call(csvPlan)["status"] == "ok", "save user-authored population label");
        const auto csvReport = call(json{{"action", "report"},
                                         {"planId", "csv-plan"},
                                         {"revision", "1"},
                                         {"source", save.at("source")}},
                                    false);
        MIB_EXPECT(csvReport.at("csv").get<std::string>().find("\"'=1+1\"") != std::string::npos,
                   "CSV user labels cannot become spreadsheet formulas");
        const auto originalSource = source;
        source["fingerprint"] = "sha256:second-source";
        source["sourceKey"] = "second-source";
        source["rows"][0]["measurements"]["area"] = 8;
        json secondBinding;
        for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"})
            secondBinding[k] = source[k];
        json apply{{"action", "apply"},
                   {"planId", "applied"},
                   {"expectedRevision", nullptr},
                   {"commandId", "apply-one"},
                   {"application",
                    {{"planId", "plan"},
                     {"revision", "1"},
                     {"overrides", {{{"node", "gate"}, {"minimum", 6}, {"maximum", 9}}}}}},
                   {"source", save.at("source")},
                   {"sourceBinding", secondBinding}};
        const auto applied = call(apply);
        MIB_EXPECT(applied["status"] == "ok", "apply pinned template with explicit override");
        const auto secondRetained = call(json{{"action", "retainSource"},
                                              {"source", save.at("source")},
                                              {"commandId", "freeze-two"}})
                                        .at("descriptor");
        json spec{{"conditions", {"before", "after"}},
                  {"design", "independent"},
                  {"weighting", "equal-experimental-unit"},
                  {"descriptiveOnly", true},
                  {"endpoint", {{"feature", "area"}, {"statistic", "mean"}, {"unit", "pixel2"}}},
                  {"samples",
                   {{{"condition", "before"},
                     {"experimentalUnit", "unit-a"},
                     {"planId", "plan"},
                     {"revision", "1"},
                     {"source", retainedDescriptor}},
                    {{"condition", "after"},
                     {"experimentalUnit", "unit-b"},
                     {"planId", "applied"},
                     {"revision", "1"},
                     {"source", secondRetained}}}}};
        const auto comparison = call(json{{"action", "compare"}, {"spec", spec}}, false);
        MIB_EXPECT(comparison["status"] == "ok" &&
                       comparison["comparison"]["differenceRightMinusLeft"] == 3,
                   "comparison uses one native endpoint per retained experimental unit");
        auto duplicate = spec;
        duplicate["samples"][1]["planId"] = "plan";
        duplicate["samples"][1]["source"] = retainedDescriptor;
        MIB_EXPECT(call(json{{"action", "compare"}, {"spec", duplicate}}, false)["status"] ==
                       "invalid",
                   "same recording cannot be a second independent replicate");
        auto paired = spec;
        paired["design"] = "paired";
        MIB_EXPECT(call(json{{"action", "compare"}, {"spec", paired}}, false)["status"] ==
                       "invalid",
                   "paired design requires explicit matching unit identities");
        paired["samples"][1]["experimentalUnit"] = "unit-a";
        MIB_EXPECT(call(json{{"action", "compare"}, {"spec", paired}},
                        false)["comparison"]["pairedDifferences"]["mean"] == 3,
                   "paired reduction uses matched unit differences");
        json finding{{"action", "saveFinding"},
                     {"findingId", "finding-one"},
                     {"commandId", "finding-command"},
                     {"spec", spec},
                     {"note", "descriptive evidence"}};
        const auto savedFinding = call(finding);
        MIB_EXPECT(savedFinding["status"] == "ok", "persist immutable finding");

        // I consumer: actual persisted F strategy, retained sources and finding authority.
        auto workspace = [&](const json& q, bool write = true) {
            return json::parse(db.workspaceRequest(q.dump(), write));
        };
        json doc{
            {"schema_version", 1},
            {"title", "Original"},
            {"plan",
             {{"experimentId", "11111111-1111-1111-1111-111111111111"},
              {"format", "single"},
              {"operator", {{"id", "qa"}, {"name", "QA"}}},
              {"rows", json::array({{{"ref", "S1"},
                                     {"sample", "QA"},
                                     {"condition", "before"},
                                     {"experimentalUnit", "unit-a"},
                                     {"concentrationValue", ""},
                                     {"concentrationUnit", ""},
                                     {"channel", ""},
                                     {"medium", ""},
                                     {"temperatureC", ""},
                                     {"notes", ""},
                                     {"free", json::array()}}})}}},
            {"recipe",
             {{"id", "22222222-2222-2222-2222-222222222222"},
              {"revision", "1"},
              {"analysis_plan", {{"id", "plan"}, {"revision", "1"}}},
              {"method", {{"id", "method"}, {"revision", "method-sha"}, {"source", "local"}}}}},
            {"run_observation", {{"running", true}}},
            {"findings", json::array({"historical"})}};
        MIB_REQUIRE(workspace({{"action", "commit"},
                               {"workspace_id", "origin"},
                               {"command_id", "origin"},
                               {"expected_revision", nullptr},
                               {"document", doc}})["status"] == "ok",
                    "save source workspace");
        json methodReply{{"status", "ok"},
                         {"method",
                          {{"status", "resolved"},
                           {"content_sha256", "method-sha"},
                           {"id", "method"},
                           {"revision", "method-sha"}}}};
        auto planning = [&](const json& q, bool write = true) {
            return json::parse(backend::workspace::nextRunPlanning(
                db, q.dump(), write, [&](const json& req) { return call(req, false); },
                [&](const json&) { return methodReply; }));
        };
        json draft{{"action", "draft_from_finding"},
                   {"workspace_id", "next-run"},
                   {"command_id", "next-command"},
                   {"source_workspace_id", "origin"},
                   {"source_revision", "1"},
                   {"finding",
                    {{"findingId", "finding-one"},
                     {"revision", "1"},
                     {"contentHash", savedFinding.at("document").at("contentHash")}}},
                   {"sample_index", 0},
                   {"profile_base", ""},
                   {"experiment_id", "33333333-3333-3333-3333-333333333333"},
                   {"recipe_id", "44444444-4444-4444-4444-444444444444"}};
        MIB_EXPECT(planning(draft, false)["status"] == "invalid", "viewer cannot derive");
        auto bad = draft;
        bad["finding"]["contentHash"] = "sha256:wrong";
        MIB_EXPECT(planning(bad)["status"] == "invalid", "finding hash mismatch refuses");
        bad = draft;
        bad["finding"]["findingId"] = "missing";
        MIB_EXPECT(planning(bad)["status"] == "invalid", "missing native finding refuses");
        bad = draft;
        bad["sample_index"] = 1;
        MIB_EXPECT(planning(bad)["status"] == "invalid", "different strategy sample refuses");
        MIB_REQUIRE(
            db.execute("CREATE TRIGGER fail_derived BEFORE INSERT ON workspace_commands WHEN "
                       "NEW.workspace_id='next-run' BEGIN SELECT RAISE(ABORT,'fault'); END"),
            "inject derived receipt fault");
        MIB_EXPECT(planning(draft)["status"] == "storage", "derived receipt fault is explicit");
        MIB_EXPECT(workspace({{"action", "read"}, {"workspace_id", "next-run"}}, false)["status"] ==
                       "not_found",
                   "failed receipt rolls back entire new draft");
        MIB_REQUIRE(db.execute("DROP TRIGGER fail_derived"), "remove receipt fault");
        const auto created = planning(draft);
        MIB_REQUIRE(created["status"] == "ok", created.dump());
        MIB_EXPECT(!created["document"].contains("run_observation") &&
                       !created["document"].contains("findings"),
                   "new draft does not copy execution/results");
        MIB_EXPECT(created["document"]["next_run_evidence"]["applied"] == false,
                   "planning never applies");
        json verify{{"action", "verify_next_run"},
                    {"workspace_id", "next-run"},
                    {"revision", "1"},
                    {"profile_base", ""}};
        MIB_EXPECT(planning(verify, false)["verified"] == true,
                   "reopened exact next-run evidence resolves");
        const auto exported = workspace({{"action", "export"},
                                         {"workspace_id", "next-run"},
                                         {"revision", "1"},
                                         {"job_id", "next-export"}});
        MIB_REQUIRE(exported["status"] == "ok", exported.dump());
        MIB_EXPECT(exported["manifest"]["payload"]["dependencies"].size() == 5,
                   "export includes finding, both strategies, both retained sources");
        json nextImport{{"action", "import"},
                        {"workspace_id", "imported-next"},
                        {"command_id", "import-next"},
                        {"manifest", exported.at("manifest")}};
        MIB_EXPECT(workspace(nextImport)["status"] == "ok",
                   "native dependencies permit exact import");
        auto tampered = nextImport;
        tampered["workspace_id"] = "tampered";
        tampered["manifest"]["payload"]["document"]["next_run_evidence"]["sample"]["value"] = 999;
        tampered["manifest"]["sha256"] =
            backend::analysis::contentHash(tampered["manifest"]["payload"]).substr(7);
        MIB_EXPECT(workspace(tampered)["status"] == "invalid",
                   "recomputed manifest checksum cannot forge native finding evidence");
        {
            backend::services::SqliteService fresh;
            MIB_REQUIRE(fresh.initialize((temp.path() / "fresh.sqlite3").string()),
                        "fresh database");
            MIB_EXPECT(json::parse(fresh.workspaceRequest(nextImport.dump(), true))["status"] ==
                           "invalid",
                       "fresh import cannot mint native finding/source/strategy");
        }
        MIB_EXPECT(workspace({{"action", "commit"},
                              {"workspace_id", "origin"},
                              {"command_id", "advance"},
                              {"expected_revision", "1"},
                              {"document", doc}})["status"] == "ok",
                   "advance source head");
        methodReply["method"]["status"] = "unavailable";
        MIB_EXPECT(planning(draft) == created,
                   "lost reply returns immutable receipt after source and method change");
        bad = draft;
        bad["workspace_id"] = "another";
        bad["command_id"] = "another";
        MIB_EXPECT(planning(bad)["status"] == "conflict", "new draft refuses stale source");
        bad = draft;
        bad["profile_base"] = "changed";
        MIB_EXPECT(planning(bad)["status"] == "invalid",
                   "command collision cannot alias changed input");
        MIB_EXPECT(planning(verify, false)["status"] == "invalid",
                   "reverification refuses unavailable method");
        source = originalSource;
        MIB_EXPECT(call(apply) == applied, "application receipt survives current source change");
        MIB_EXPECT(call(finding) == savedFinding, "finding receipt replay is stable");
        apply["commandId"] = "apply-invalid";
        apply["application"]["overrides"][0]["node"] = "removed";
        MIB_EXPECT(call(apply)["status"] == "invalid",
                   "removed override requires explicit migration");
        evaluate["revision"] = nullptr;
        MIB_EXPECT(call(evaluate, false)["status"] == "invalid", "no moving-head evaluation");
        MIB_REQUIRE(db.execute("CREATE TRIGGER fail_receipt BEFORE INSERT ON workspace_commands "
                               "BEGIN SELECT RAISE(ABORT,'fault'); END"),
                    "inject receipt fault");
        next["commandId"] = "retry-fault";
        MIB_EXPECT(call(next)["status"] == "storage", "fault reported");
        MIB_REQUIRE(db.execute("DROP TRIGGER fail_receipt"), "clear fault");
        MIB_EXPECT(call(next)["revision"] == "3", "transaction rollback did not consume revision");
        auto left = next, right = next;
        left["expectedRevision"] = "3";
        right["expectedRevision"] = "3";
        left["commandId"] = "left";
        right["commandId"] = "right";
        json l, r;
        std::thread a([&] { l = call(left); });
        std::thread b([&] { r = call(right); });
        a.join();
        b.join();
        MIB_EXPECT((l["status"] == "ok" && r["status"] == "conflict") ||
                       (r["status"] == "ok" && l["status"] == "conflict"),
                   "concurrent CAS has one winner");
    }
    {
        backend::services::SqliteService db;
        MIB_REQUIRE(db.initialize(path), "reopen metadata authority");
        const auto finding = json::parse(backend::analysis::analysisRequest(
            db,
            json{{"action", "readFinding"}, {"findingId", "finding-one"}, {"revision", "1"}}.dump(),
            false, resolver));
        MIB_EXPECT(finding["document"]["comparison"]["differenceRightMinusLeft"] == 3,
                   "immutable finding survives metadata reopen");
        source["fingerprint"] = "sha256:changed";
        const auto replay =
            json::parse(backend::analysis::analysisRequest(db, save.dump(), true, resolver));
        MIB_EXPECT(replay["revision"] == "1", "receipt survives restart and source change");
        const auto retained =
            json::parse(backend::analysis::analysisRequest(db,
                                                           json{{"action", "evaluate"},
                                                                {"planId", "plan"},
                                                                {"revision", "1"},
                                                                {"source", retainedDescriptor}}
                                                               .dump(),
                                                           false, resolver));
        MIB_EXPECT(retained["evaluation"]["mean"] == 5, "retained source survives metadata reopen");
    }
    return mib::test::exitCode();
}
