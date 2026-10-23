#include "backend/analysis/AnalysisHost.h"
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
