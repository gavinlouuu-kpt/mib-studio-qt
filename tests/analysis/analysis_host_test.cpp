#include "backend/analysis/AnalysisHost.h"
#include "backend/analysis/SourceAnalysis.h"
#include "backend/database/SqliteService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>
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
    {
        auto transformed = save;
        transformed["transforms"] = {{{"input", "area"},
                                      {"inputUnit", "pixel2"},
                                      {"output", "scaled"},
                                      {"kind", "linear"},
                                      {"scale", 5},
                                      {"offset", 0}},
                                     {{"input", "scaled"},
                                      {"inputUnit", "dimensionless"},
                                      {"output", "logged"},
                                      {"kind", "log10"},
                                      {"scale", 1},
                                      {"offset", 0}}};
        transformed["feature"] = "logged";
        transformed["graph"]["nodes"][0]["feature"] = "logged";
        transformed["graph"]["nodes"][0]["minimum"] = 0;
        transformed["graph"]["nodes"][0]["maximum"] = 0;
        const auto result = backend::analysis::evaluateSource(source, transformed);
        MIB_EXPECT(result["selected"] == "1" && result["unknown"] == "1" && result["mean"] == 0,
                   "ordered transforms feed native inclusive gates and nullable statistics");
        const auto prepared = backend::analysis::prepareAnalysisSource(source, transformed);
        MIB_EXPECT(prepared["rows"][0]["measurements"]["area"] == 5 &&
                       prepared["rows"][0]["measurements"]["logged"] == 0,
                   "derived parameters never overwrite raw measurements");
        auto invalidDomain = source;
        invalidDomain["rows"][0]["measurements"]["area"] = 0;
        MIB_EXPECT(backend::analysis::evaluateSource(invalidDomain, transformed)["unknown"] == "2",
                   "log domain failure stays unknown rather than clipped or zero");
        transformed["transforms"][1]["kind"] = "asinh";
        MIB_EXPECT(
            backend::analysis::prepareAnalysisSource(source, transformed)["parameterFingerprint"] !=
                prepared["parameterFingerprint"],
            "transform identity pins ordered numerical semantics");
    }
    {
        auto scatterInput = source;
        scatterInput["parameters"]["deformability"] = "dimensionless";
        scatterInput["rows"] = json::array();
        for (int i = 0; i < 20; ++i)
            scatterInput["rows"].push_back(
                {{"key", std::to_string(i)},
                 {"measurements",
                  {{"area", 5}, {"deformability", i == 19 ? json(nullptr) : json(i / 20.)}}}});
        auto query = save;
        query["plot"] = {{"kind", "scatter"},    {"x", "area"},
                         {"y", "deformability"}, {"seed", 42u},
                         {"maxPoints", 5u},      {"layers", {{{"population", "gate"}}}}};
        const auto plot = backend::analysis::plotSource(scatterInput, query);
        MIB_EXPECT(plot["points"].size() == 5 && plot["eligible"] == "19" &&
                       plot["layers"][0]["missingCoordinates"] == "1",
                   "scatter reports explicit display sampling and missing coordinate counts");
        std::reverse(scatterInput["rows"].begin(), scatterInput["rows"].end());
        MIB_EXPECT(backend::analysis::plotSource(scatterInput, query)["points"] == plot["points"],
                   "scatter sample independent of row traversal order");
        MIB_EXPECT(backend::analysis::evaluateSource(scatterInput, query)["selected"] == "20",
                   "display sample never replaces scientific population");
    }
    json retainedDescriptor;
    auto resolver = [&](const json&) { return source; };
    {
        backend::services::SqliteService db;
        MIB_REQUIRE(db.initialize(path), "open metadata authority");
        for (const char* name : {"analysis/forged", "analysis-source/forged",
                                 "analysis-finding/forged", "analysis-report-run/forged"}) {
            const auto forged =
                json::parse(db.workspaceRequest(json{{"action", "commit"},
                                                     {"workspace_id", name},
                                                     {"expected_revision", nullptr},
                                                     {"command_id", "forged"},
                                                     {"document", {{"schema_version", 1}}}}
                                                    .dump(),
                                                true));
            MIB_EXPECT(forged["status"] == "invalid",
                       "generic workspace RPC cannot forge analysis authority records");
        }
        auto call = [&](const json& q, bool write = true) {
            return json::parse(backend::analysis::analysisRequest(db, q.dump(), write, resolver));
        };
        MIB_EXPECT(call(save, false)["status"] == "invalid", "read endpoint cannot save");
        MIB_EXPECT(call(save)["revision"] == "1", "create strategy");
        auto transformedPlan = save;
        transformedPlan["planId"] = "transformed";
        transformedPlan["graph"]["id"] = "transformed";
        transformedPlan["commandId"] = "transform-save";
        transformedPlan["feature"] = "scaled";
        transformedPlan["transforms"] = {{{"input", "area"},
                                          {"inputUnit", "pixel2"},
                                          {"output", "scaled"},
                                          {"kind", "linear"},
                                          {"scale", 5},
                                          {"offset", 0}}};
        transformedPlan["plot"] = {{"kind", "histogram"},
                                   {"feature", "scaled"},
                                   {"minimum", 0},
                                   {"maximum", 2},
                                   {"bins", 2},
                                   {"normalization", "count"},
                                   {"layers", {{{"population", "gate"}, {"displayOffset", 0}}}}};
        MIB_EXPECT(call(transformedPlan)["status"] == "ok", "persist ordered preprocessing");
        json transformedQuery{{"action", "jobInput"},
                              {"planId", "transformed"},
                              {"revision", "1"},
                              {"source", save.at("source")}};
        const auto jobInput = call(transformedQuery, false);
        MIB_EXPECT(jobInput["evaluation"]["mean"] == 1 &&
                       jobInput["source"]["rows"][0]["measurements"]["scaled"] == 1,
                   "worker receives same transformed values as native reductions");
        MIB_EXPECT(jobInput["evaluation"]["parameterFingerprint"] ==
                       jobInput["source"]["parameterFingerprint"],
                   "worker and native preprocessing identity match");
        transformedQuery["action"] = "plot";
        MIB_EXPECT(call(transformedQuery, false)["plot"]["layers"][0]["counts"] ==
                       json::array({"0", "1"}),
                   "native plot bins transformed values exactly once");
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
        json library{{"title", "Two source study"},
                     {"groups", {"control", "treated"}},
                     {"preferences", {{"decimalDigits", 3}}},
                     {"samples",
                      {{{"source", retainedDescriptor},
                        {"label", "Before"},
                        {"groups", {"control"}},
                        {"tags", {"synthetic"}},
                        {"notes", "operator annotation"},
                        {"preferences", {{"decimalDigits", 6}}}},
                       {{"source", secondRetained},
                        {"label", "After"},
                        {"groups", {"treated"}},
                        {"tags", json::array()},
                        {"notes", ""},
                        {"preferences", json::object()}}}}};
        json librarySave{{"action", "librarySave"},
                         {"libraryId", "study"},
                         {"expectedRevision", nullptr},
                         {"commandId", "library-one"},
                         {"library", library}};
        const auto librarySaved = call(librarySave);
        MIB_EXPECT(librarySaved["status"] == "ok",
                   "persist verified retained-source annotation library");
        MIB_EXPECT(call(librarySave) == librarySaved, "library command receipt replays exactly");
        const auto libraryRead =
            call(json{{"action", "libraryRead"}, {"libraryId", "study"}, {"revision", "1"}}, false);
        MIB_EXPECT(libraryRead["display"][0]["preferences"]["values"]["decimalDigits"] == 6 &&
                       libraryRead["display"][1]["preferences"]["values"]["decimalDigits"] == 3 &&
                       libraryRead["display"][0]["preferences"]["origin"]["showUnknown"] ==
                           "application-default",
                   "display preferences resolve sample over library over explicit defaults");
        auto duplicateLibrary = librarySave;
        duplicateLibrary["commandId"] = "duplicate-library";
        duplicateLibrary["library"]["samples"][1]["source"] = retainedDescriptor;
        MIB_EXPECT(call(duplicateLibrary)["status"] == "invalid",
                   "library deduplicates exact source identity not labels");
        auto invalidGroup = librarySave;
        invalidGroup["commandId"] = "invalid-group";
        invalidGroup["library"]["samples"][0]["groups"] = {"not-declared"};
        MIB_EXPECT(call(invalidGroup)["status"] == "invalid", "library refuses undeclared group");
        json reportLayout{
            {"title", "Pinned research evidence"},
            {"blocks",
             {{{"id", "table"},
               {"title", "Native table"},
               {"kind", "table"},
               {"reference",
                {{"planId", "plan"}, {"revision", "1"}, {"source", retainedDescriptor}}}},
              {{"id", "finding"},
               {"title", "Descriptive comparison"},
               {"kind", "finding"},
               {"reference", {{"findingId", "finding-one"}, {"revision", "1"}}}}}}};
        json reportSave{{"action", "reportSave"},
                        {"reportId", "report-one"},
                        {"expectedRevision", nullptr},
                        {"commandId", "report-save"},
                        {"layout", reportLayout}};
        MIB_EXPECT(call(reportSave)["revision"] == "1", "persist reusable exact report layout");
        json buildReport{{"action", "reportBuild"},
                         {"reportId", "report-one"},
                         {"revision", "1"},
                         {"runId", "run-one"},
                         {"commandId", "report-build"}};
        const auto reportRun = call(buildReport);
        MIB_EXPECT(reportRun["status"] == "ok" &&
                       reportRun["document"]["artifact"]["blocks"].size() == 2,
                   "publish all pinned report blocks atomically");
        MIB_EXPECT(call(buildReport) == reportRun, "report run exact replay");
        auto collisionRun = buildReport;
        collisionRun["commandId"] = "other-build";
        MIB_EXPECT(call(collisionRun)["status"] == "invalid",
                   "report run ID cannot overwrite prior evidence");
        reportSave["expectedRevision"] = "1";
        reportSave["commandId"] = "report-next";
        reportSave["layout"]["title"] = "New title";
        MIB_EXPECT(call(reportSave)["revision"] == "2", "append layout revision");
        MIB_EXPECT(call(buildReport) == reportRun, "saved report run never follows layout head");
        auto invalidReport = reportSave;
        invalidReport["commandId"] = "report-invalid";
        invalidReport["layout"]["blocks"][0]["reference"]["source"] = save.at("source");
        MIB_EXPECT(call(invalidReport)["status"] == "invalid",
                   "reports require retained source references");
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
        const auto library = json::parse(backend::analysis::analysisRequest(
            db, json{{"action", "libraryRead"}, {"libraryId", "study"}, {"revision", "1"}}.dump(),
            false, resolver));
        MIB_EXPECT(library["document"]["library"]["samples"][0]["label"] == "Before",
                   "sample library metadata survives authority reopen");
        const auto reportRun = json::parse(backend::analysis::analysisRequest(
            db,
            json{{"action", "reportRunRead"}, {"reportId", "run-one"}, {"revision", "1"}}.dump(),
            false, resolver));
        MIB_EXPECT(reportRun["document"]["artifact"]["title"] == "Pinned research evidence",
                   "immutable report run reopens independent of layout changes");
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
