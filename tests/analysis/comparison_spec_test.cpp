// F-4 (#808 F09, I09, I10): the ComparisonSpec and fit authority through the
// production analysis host (AnalysisHost -> pinned revisions -> native exact
// statistics -> AnalysisInference). Every unit value is a population statistic
// of its own source; expected numbers come from scipy (inference_reference.h,
// tools/analysis_inference/generate_reference.py), not from the code under test.
#include "analysis/inference_reference.h"
#include "backend/analysis/AnalysisHost.h"
#include "backend/database/SqliteService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <map>
#include <string>

using nlohmann::json;
namespace ref = inference_reference;

namespace {
bool near(const json& actual, double expected, double relative) {
    return actual.is_number() &&
           std::fabs(actual.get<double>() - expected) <= relative * std::fabs(expected) + 1e-300;
}
bool anyReason(const json& reasons, const std::string& needle) {
    for (const auto& r : reasons)
        if (r.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}
bool containsKey(const json& value, const std::string& key) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it)
            if (it.key() == key || containsKey(it.value(), key)) return true;
    } else if (value.is_array()) {
        for (const auto& item : value)
            if (containsKey(item, key)) return true;
    }
    return false;
}

struct Fixture {
    backend::services::SqliteService db;
    std::map<std::string, json> sources;
    int plans = 0;

    json call(const json& q, bool write) {
        return json::parse(backend::analysis::analysisRequest(
            db, q.dump(), write, [this](const json& descriptor) {
                return sources.at(descriptor.at("id").get<std::string>());
            }));
    }
    // A source whose every event has `value` (so each population statistic of
    // "cells" is exactly `value`), and a plan pinned to it. Returns a sample.
    json sample(const std::string& id, double value, json parameters = nullptr,
                json provenance = nullptr, const std::string& population = "cells") {
        if (parameters.is_null()) parameters = {{"area", "pixel2"}, {"deformability", "dimensionless"}};
        if (provenance.is_null())
            provenance = {{"processing", "core-a"}, {"calibration", "cal-1"}, {"mechanics", "lut-1"}};
        json rows = json::array();
        for (int i = 0; i < 3; ++i) {
            json measurements = {{"area", value}};
            if (parameters.contains("deformability")) measurements["deformability"] = 0.01 * i;
            rows.push_back({{"key", id + "/" + std::to_string(i)}, {"measurements", measurements}});
        }
        sources[id] = {{"schema", "F-source/1"},
                       {"fingerprint", "sha256:" + id},
                       {"sourceKey", "key-" + id},
                       {"parameters", parameters},
                       {"scope", "stored"},
                       {"populationComplete", true},
                       {"provenance", provenance},
                       {"rows", rows}};
        const auto planId = "plan-" + std::to_string(++plans);
        json binding;
        for (const char* k : {"fingerprint", "sourceKey", "parameters", "scope"})
            binding[k] = sources[id][k];
        const json graph{{"id", planId},
                         {"revision", "draft"},
                         {"output", population},
                         {"nodes",
                          {{{"id", population},
                            {"parent", ""},
                            {"kind", "interval"},
                            {"feature", "area"},
                            {"minimum", -1e9},
                            {"maximum", 1e9}}}}};
        const auto saved = call({{"action", "save"},
                                 {"planId", planId},
                                 {"expectedRevision", nullptr},
                                 {"commandId", "save-" + planId},
                                 {"graph", graph},
                                 {"feature", "area"},
                                 {"source", {{"kind", "test"}, {"id", id}}},
                                 {"sourceBinding", binding}},
                                true);
        MIB_REQUIRE(saved.value("status", "") == "ok", "save plan: " + saved.dump());
        return {{"sampleId", "s-" + id},
                {"experimentalUnit", "u-" + id},
                {"planId", planId},
                {"revision", saved.at("revision")},
                {"source", {{"kind", "test"}, {"id", id}}}};
    }
};

json metric(const std::string& statistic = "median") {
    return {{"population", "cells"}, {"parameter", "area"}, {"statistic", statistic}, {"unit", "pixel2"}};
}
json comparisonSpec(const std::string& design, json groups, json samples,
                    const std::string& inference = "both") {
    return {{"schema", "F-comparison-spec/1"},
            {"metric", metric()},
            {"design", design},
            {"groups", groups},
            {"inference", inference},
            {"samples", samples}};
}
const json* findTest(const json& comparison, const std::string& name) {
    for (const auto& t : comparison.at("inference").at("tests"))
        if (t.at("test") == name) return &t;
    return nullptr;
}
} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir temp("comparison_spec");
    Fixture f;
    MIB_REQUIRE(f.db.initialize((temp.path() / "app.sqlite3").string()), "open metadata authority");

    // Unpaired, unequal variances: Welch t and Mann-Whitney through the host.
    json samples = json::array();
    for (std::size_t i = 0; i < ref::welchReference.size(); ++i) {
        auto s = f.sample("ctl" + std::to_string(i), ref::welchReference[i]);
        s["group"] = "control";
        samples.push_back(s);
    }
    for (std::size_t i = 0; i < ref::welchComparator.size(); ++i) {
        auto s = f.sample("trt" + std::to_string(i), ref::welchComparator[i]);
        s["group"] = "treated";
        samples.push_back(s);
    }
    const auto spec = comparisonSpec("unpaired", {"control", "treated"}, samples);
    const auto reply = f.call({{"action", "comparisonSpec"}, {"spec", spec}}, false);
    MIB_REQUIRE(reply.value("status", "") == "ok", "comparison: " + reply.dump().substr(0, 400));
    const auto& comparison = reply.at("comparison");
    MIB_EXPECT(comparison.at("inference").at("status") == "computed", "declared inference ran");
    MIB_EXPECT(comparison.at("groups")[0].at("units") == 10 &&
                   comparison.at("groups")[1].at("units") == 20,
               "n counts experimental units (sources), not events");
    MIB_EXPECT(near(comparison.at("groups")[0].at("mean"), ref::welchRefMean, 1e-12) &&
                   near(comparison.at("groups")[0].at("median"), ref::welchRefMedian, 1e-12),
               "group summary: mean and median of unit values");
    const auto* welch = findTest(comparison, "welch-t");
    MIB_REQUIRE(welch, "Welch test reported");
    MIB_EXPECT(near(welch->at("statistic"), ref::welchT, 1e-10) &&
                   near(welch->at("df1"), ref::welchDf, 1e-10) &&
                   near(welch->at("pValue"), ref::welchP, 1e-8),
               "Welch t through the host equals scipy");
    MIB_EXPECT(near(welch->at("estimate").at("confidenceInterval").at("lower"), ref::welchCiLower, 1e-8),
               "Welch CI through the host equals scipy");
    const auto* mwu = findTest(comparison, "mann-whitney-u");
    MIB_REQUIRE(mwu, "Mann-Whitney reported");
    MIB_EXPECT(near(mwu->at("statistic"), ref::welchDataMwuU, 1e-12) &&
                   near(mwu->at("pValue"), ref::welchDataMwuP, 1e-9),
               "Mann-Whitney with ties through the host equals scipy");
    MIB_EXPECT(comparison.at("samples")[0].at("revision") == "1" &&
                   comparison.at("samples")[0].at("graphFingerprint").is_string() &&
                   comparison.at("samples")[0].at("sourceFingerprint") == "sha256:ctl0",
               "unit values are bound to pinned revisions and source identity");
    const auto again = f.call({{"action", "comparisonSpec"}, {"spec", spec}}, false);
    MIB_EXPECT(again.at("contentHash") == reply.at("contentHash"),
               "same pinned inputs give the same content hash");

    // Paired: paired t and Wilcoxon (ties -> normal approximation).
    samples = json::array();
    for (std::size_t i = 0; i < ref::pairedReference.size(); ++i) {
        for (const auto& [group, value] :
             {std::pair<std::string, double>{"before", ref::pairedReference[i]},
              std::pair<std::string, double>{"after", ref::pairedComparator[i]}}) {
            auto s = f.sample("p" + group + std::to_string(i), value);
            s["group"] = group;
            s["experimentalUnit"] = "donor-" + std::to_string(i);
            samples.push_back(s);
        }
    }
    const auto paired = f.call({{"action", "comparisonSpec"},
                                {"spec", comparisonSpec("paired", {"before", "after"}, samples)}},
                               false);
    MIB_REQUIRE(paired.value("status", "") == "ok", "paired comparison: " + paired.dump().substr(0, 400));
    const auto* pt = findTest(paired.at("comparison"), "paired-t");
    const auto* wsr = findTest(paired.at("comparison"), "wilcoxon-signed-rank");
    MIB_REQUIRE(pt && wsr, "paired tests reported");
    MIB_EXPECT(near(pt->at("statistic"), ref::pairedT, 1e-10) && near(pt->at("pValue"), ref::pairedP, 1e-8),
               "paired t through the host equals scipy");
    MIB_EXPECT(wsr->at("statistic") == ref::pairedWilcoxonW &&
                   near(wsr->at("pValue"), ref::pairedWilcoxonP, 1e-9),
               "Wilcoxon through the host equals scipy");
    MIB_EXPECT(wsr->at("pMethod").get<std::string>().find("normal approximation") != std::string::npos,
               "tied Wilcoxon states its approximation");

    // Five groups: one-way ANOVA / Kruskal-Wallis.
    samples = json::array();
    json groupNames = json::array();
    for (std::size_t g = 0; g < ref::anovaGroups.size(); ++g) {
        groupNames.push_back("site" + std::to_string(g));
        for (std::size_t i = 0; i < ref::anovaGroups[g].size(); ++i) {
            auto s = f.sample("a" + std::to_string(g) + "-" + std::to_string(i), ref::anovaGroups[g][i]);
            s["group"] = "site" + std::to_string(g);
            samples.push_back(s);
        }
    }
    const auto anova = f.call(
        {{"action", "comparisonSpec"}, {"spec", comparisonSpec("unpaired", groupNames, samples)}}, false);
    MIB_REQUIRE(anova.value("status", "") == "ok", "anova comparison");
    MIB_EXPECT(near(findTest(anova.at("comparison"), "one-way-anova")->at("statistic"), ref::anovaF, 1e-10) &&
                   near(findTest(anova.at("comparison"), "one-way-anova")->at("pValue"), ref::anovaP, 1e-8),
               "ANOVA through the host equals scipy");
    MIB_EXPECT(near(findTest(anova.at("comparison"), "kruskal-wallis")->at("statistic"), ref::kruskalH, 1e-10),
               "Kruskal-Wallis through the host equals scipy");

    // Insufficient units: summaries, but no test and no p-value.
    samples = json::array();
    for (int i = 0; i < 5; ++i) {
        auto s = f.sample("few" + std::to_string(i), 10.0 + i);
        s["group"] = i < 2 ? "control" : "treated";
        samples.push_back(s);
    }
    const auto few = f.call({{"action", "comparisonSpec"},
                             {"spec", comparisonSpec("unpaired", {"control", "treated"}, samples)}},
                            false);
    MIB_REQUIRE(few.value("status", "") == "ok", "insufficient comparison still answers");
    const auto& fewInference = few.at("comparison").at("inference");
    MIB_EXPECT(fewInference.at("status") == "insufficient" && fewInference.at("tests").empty() &&
                   anyReason(fewInference.at("reasons"), "group control has 2 independent") &&
                   !containsKey(few.at("comparison"), "pValue"),
               "fewer than 3 units in a group: insufficient with reason, no p-value");
    MIB_EXPECT(few.at("comparison").at("groups")[0].at("units") == 2,
               "descriptive summary is still reported");
    // Technical replicates of one unit do not raise n.
    samples = json::array();
    for (int i = 0; i < 6; ++i) {
        auto s = f.sample("tech" + std::to_string(i), 5.0 + i);
        s["group"] = i < 3 ? "control" : "treated";
        s["experimentalUnit"] = i < 3 ? "dish-1" : "dish-" + std::to_string(i);
        samples.push_back(s);
    }
    const auto technical = f.call({{"action", "comparisonSpec"},
                                   {"spec", comparisonSpec("unpaired", {"control", "treated"}, samples)}},
                                  false);
    MIB_EXPECT(technical.at("comparison").at("groups")[0].at("units") == 1 &&
                   technical.at("comparison").at("groups")[0].at("unitValues")[0].at("technicalReplicates") == 3 &&
                   technical.at("comparison").at("groups")[0].at("mean") == 6.0 &&
                   technical.at("comparison").at("inference").at("status") == "insufficient",
               "three sources of one unit are one averaged unit, not three");

    // Incompatible sources are excluded with reasons; the rest is still compared.
    samples = json::array();
    for (int i = 0; i < 6; ++i) {
        auto s = f.sample("ok" + std::to_string(i), 20.0 + i);
        s["group"] = i < 3 ? "control" : "treated";
        samples.push_back(s);
    }
    auto calibration = f.sample("cal", 99, nullptr,
                                {{"processing", "core-a"}, {"calibration", "cal-2"}, {"mechanics", "lut-1"}});
    calibration["group"] = "treated";
    auto lut = f.sample("lut", 99, nullptr,
                        {{"processing", "core-a"}, {"calibration", "cal-1"}, {"mechanics", "lut-9"}});
    lut["group"] = "treated";
    auto core = f.sample("core", 99, nullptr,
                         {{"processing", "core-b"}, {"calibration", "cal-1"}, {"mechanics", "lut-1"}});
    core["group"] = "treated";
    auto params = f.sample("params", 99, {{"area", "pixel2"}});
    params["group"] = "treated";
    auto unit = f.sample("unit", 99, {{"area", "um2"}, {"deformability", "dimensionless"}});
    unit["group"] = "treated";
    auto missing = f.sample("missing", 99, nullptr, nullptr, "other-population");
    missing["group"] = "treated";
    auto duplicate = samples[4];
    duplicate["sampleId"] = "s-dup";
    duplicate["experimentalUnit"] = "u-dup";
    for (const auto& s : {calibration, lut, core, params, unit, missing, duplicate}) samples.push_back(s);
    const auto mixed = f.call({{"action", "comparisonSpec"},
                               {"spec", comparisonSpec("unpaired", {"control", "treated"}, samples)}},
                              false);
    MIB_REQUIRE(mixed.value("status", "") == "ok", "mixed comparison: " + mixed.dump().substr(0, 400));
    std::map<std::string, json> reasons;
    for (const auto& s : mixed.at("comparison").at("samples"))
        reasons[s.at("sampleId").get<std::string>()] = s.at("reasons");
    MIB_EXPECT(mixed.at("comparison").at("excludedSamples") == 7, "seven incompatible sources excluded");
    MIB_EXPECT(anyReason(reasons["s-cal"], "different calibration identity"), "calibration exclusion");
    MIB_EXPECT(anyReason(reasons["s-lut"], "E-LUT"), "E-LUT exclusion");
    MIB_EXPECT(anyReason(reasons["s-core"], "processing core/contract"), "processing core exclusion");
    MIB_EXPECT(anyReason(reasons["s-params"], "different parameters"), "parameter-set exclusion");
    MIB_EXPECT(anyReason(reasons["s-unit"], "different unit"), "unit exclusion");
    MIB_EXPECT(anyReason(reasons["s-missing"], "missing metric"), "missing population exclusion");
    MIB_EXPECT(anyReason(reasons["s-dup"], "same source"), "a repeated source is not a second unit");
    MIB_EXPECT(mixed.at("comparison").at("groups")[1].at("units") == 3 &&
                   mixed.at("comparison").at("inference").at("status") == "computed",
               "compatible sources are still compared");
    MIB_EXPECT(mixed.at("comparison").at("compatibility").at("reference") == "s-ok0",
               "the compatibility reference is named");

    // Invalid declarations are refused, not guessed.
    auto crossed = comparisonSpec("unpaired", {"control", "treated"}, samples);
    crossed["samples"][3]["experimentalUnit"] = crossed["samples"][0]["experimentalUnit"];
    MIB_EXPECT(f.call({{"action", "comparisonSpec"}, {"spec", crossed}}, false).at("status") == "invalid",
               "an unpaired design cannot reuse a unit across groups");
    auto lowered = comparisonSpec("unpaired", {"control", "treated"}, samples);
    lowered["minimumUnitsPerGroup"] = 2;
    MIB_EXPECT(f.call({{"action", "comparisonSpec"}, {"spec", lowered}}, false).at("status") == "invalid",
               "the minimum of 3 units per group cannot be lowered");
    auto cellStat = comparisonSpec("unpaired", {"control", "treated"}, samples);
    cellStat["metric"]["statistic"] = "selected";
    MIB_EXPECT(f.call({{"action", "comparisonSpec"}, {"spec", cellStat}}, false).at("status") == "invalid",
               "only declared unit-level statistics are metrics");

    // Concentration-response fit through the host (I10).
    json fitSamples = json::array();
    for (std::size_t i = 0; i < ref::fitConcentration.size(); ++i) {
        auto s = f.sample("dose" + std::to_string(i), ref::fitResponse[i]);
        s["concentration"] = {{"value", ref::fitConcentration[i]}, {"unit", "nM"}};
        fitSamples.push_back(s);
    }
    auto wrongUnit = f.sample("dose-uM", 50);
    wrongUnit["concentration"] = {{"value", 0.003}, {"unit", "uM"}};
    fitSamples.push_back(wrongUnit);
    json fitRequest{{"schema", "F-fit-spec/1"},
                    {"metric", metric()},
                    {"model", "4PL"},
                    {"concentrationUnit", "nM"},
                    {"concentrationScale", "log10"},
                    {"weighting", "none"},
                    {"design", "independent-units"},
                    {"samples", fitSamples}};
    const auto fit = f.call({{"action", "concentrationFit"}, {"spec", fitRequest}}, false);
    MIB_REQUIRE(fit.value("status", "") == "ok", "fit: " + fit.dump().substr(0, 400));
    const auto& fitted = fit.at("fit");
    MIB_EXPECT(fitted.at("status") == "fit" && fitted.at("suspect") == false, "4PL fit, not suspect");
    MIB_EXPECT(near(fitted.at("ec50").at("value"), ref::fitEc50, 1e-6) &&
                   near(fitted.at("ec50").at("confidenceInterval").at("lower"), ref::fitEc50Lower, 1e-5) &&
                   fitted.at("ec50").at("unit") == "nM",
               "EC50 and CI through the host equal scipy");
    MIB_EXPECT(near(fitted.at("rSquared").at("value"), ref::fitR2, 1e-8) &&
                   fitted.at("residuals").size() == ref::fitConcentration.size(),
               "R squared and one residual per unit point");
    bool excludedUnit = false;
    for (const auto& s : fitted.at("samples"))
        excludedUnit = excludedUnit || (s.at("sampleId") == "s-dose-uM" &&
                                        anyReason(s.at("reasons"), "no unit conversion"));
    MIB_EXPECT(excludedUnit, "a concentration in another unit is excluded, not converted");
    fitRequest["weighting"] = "inverse-variance";
    fitRequest["samples"].erase(fitRequest["samples"].size() - 1);
    const auto weighted = f.call({{"action", "concentrationFit"}, {"spec", fitRequest}}, false);
    MIB_EXPECT(near(weighted.at("fit").at("ec50").at("value"), ref::wfitEc50, 1e-6),
               "inverse-variance weighted EC50 equals scipy");

    json suspectSamples = json::array();
    for (std::size_t i = 0; i < ref::suspectConcentration.size(); ++i) {
        auto s = f.sample("sus" + std::to_string(i), ref::suspectResponse[i]);
        s["concentration"] = {{"value", ref::suspectConcentration[i]}, {"unit", "nM"}};
        suspectSamples.push_back(s);
    }
    fitRequest["weighting"] = "none";
    fitRequest["samples"] = suspectSamples;
    const auto suspect = f.call({{"action", "concentrationFit"}, {"spec", fitRequest}}, false);
    MIB_EXPECT(suspect.at("fit").at("status") == "fit" && suspect.at("fit").at("suspect") == true &&
                   anyReason(suspect.at("fit").at("suspectReasons"), "EC50 outside the tested"),
               "EC50 beyond the tested range is a suspect fit");

    fitRequest["samples"] = json::array({suspectSamples[0], suspectSamples[3], suspectSamples[6]});
    const auto insufficient = f.call({{"action", "concentrationFit"}, {"spec", fitRequest}}, false);
    MIB_EXPECT(insufficient.at("fit").at("status") == "insufficient" &&
                   insufficient.at("fit").at("parameters").is_null() &&
                   insufficient.at("fit").at("ec50").is_null() &&
                   !insufficient.at("fit").at("reasons").empty(),
               "three points: no fit, with reasons");

    // Review fixes (F09/I10). Degenerate units: zero within-group variance
    // is "undefined" with a reason and a null pValue, never p = 1 computed.
    samples = json::array();
    for (int i = 0; i < 6; ++i) {
        auto s = f.sample("flat" + std::to_string(i), i < 3 ? 5.0 : 7.0);
        s["group"] = i < 3 ? "control" : "treated";
        samples.push_back(s);
    }
    const auto flat = f.call({{"action", "comparisonSpec"},
                              {"spec", comparisonSpec("unpaired", {"control", "treated"}, samples,
                                                      "parametric")}},
                             false);
    MIB_REQUIRE(flat.value("status", "") == "ok", "degenerate comparison answers");
    const auto& flatInference = flat.at("comparison").at("inference");
    const auto* flatWelch = findTest(flat.at("comparison"), "welch-t");
    MIB_EXPECT(flatInference.at("status") == "undefined" && flatWelch &&
                   flatWelch->at("status") == "undefined" && flatWelch->at("pValue").is_null() &&
                   flatWelch->at("statistic").is_null() &&
                   flatWelch->at("estimate").at("confidenceInterval").is_null() &&
                   anyReason(flatInference.at("reasons"), "zero within-group variance"),
               "zero within-group variance: undefined, reason, null pValue: " + flatInference.dump());
    const auto flatBoth = f.call({{"action", "comparisonSpec"},
                                  {"spec", comparisonSpec("unpaired", {"control", "treated"}, samples)}},
                                 false);
    const auto& bothInference = flatBoth.at("comparison").at("inference");
    MIB_EXPECT(bothInference.at("status") == "computed" &&
                   findTest(flatBoth.at("comparison"), "welch-t")->at("status") == "undefined" &&
                   findTest(flatBoth.at("comparison"), "mann-whitney-u")->at("status") == "computed" &&
                   findTest(flatBoth.at("comparison"), "welch-t")->at("undefinedReason").is_string(),
               "each test carries its own status; one defined test keeps the comparison computed");
    // One confidence-level rule: 0.5 is refused for comparisons as for fits.
    auto halfSpec = comparisonSpec("unpaired", {"control", "treated"}, samples);
    halfSpec["confidenceLevel"] = 0.5;
    const auto half = f.call({{"action", "comparisonSpec"}, {"spec", halfSpec}}, false);
    MIB_EXPECT(half.value("status", "") != "ok", "confidenceLevel 0.5 refused: " + half.dump().substr(0, 200));
    fitRequest["samples"] = suspectSamples;
    fitRequest["confidenceLevel"] = 0.5;
    MIB_EXPECT(f.call({{"action", "concentrationFit"}, {"spec", fitRequest}}, false).value("status", "") != "ok",
               "the fit applies the same confidence-level rule");
    fitRequest.erase("confidenceLevel");
    MIB_EXPECT(suspect.at("fit").at("termination").is_string() &&
                   fitted.at("termination") == "converged",
               "the fit reports how Levenberg-Marquardt terminated");

    // The evaluate metric selector refuses an unknown population.
    const auto badMetric = f.call({{"action", "evaluate"},
                                   {"planId", "plan-1"},
                                   {"revision", "1"},
                                   {"source", {{"kind", "test"}, {"id", "ctl0"}}},
                                   {"metric", {{"population", "nope"}, {"parameter", "area"}}}},
                                  false);
    MIB_EXPECT(badMetric.at("status") == "invalid", "unknown metric population refused");
    return mib::test::exitCode();
}
