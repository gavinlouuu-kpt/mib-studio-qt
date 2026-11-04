#include "backend/analysis/AnalysisComparison.h"
#include "backend/analysis/AnalysisInference.h"
#include "backend/analysis/PopulationStatistics.h"
#include "backend/analysis/SourceAnalysis.h"
#include <cmath>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <vector>
namespace backend::analysis {
using Json = nlohmann::json;
namespace {
void fields(const Json& object, const std::set<std::string>& names) {
    if (!object.is_object() || object.size() != names.size())
        throw std::invalid_argument("Incomplete comparison fields");
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!names.count(i.key())) throw std::invalid_argument("Unknown comparison field");
}
std::string label(const Json& value) {
    const auto text = value.get<std::string>();
    if (text.empty() || text.size() > 128)
        throw std::invalid_argument("Invalid condition/experimental-unit label");
    return text;
}
Json summarize(const std::map<std::string, std::optional<double>>& values,
               const std::string& unit) {
    const RevisionKey revision{"comparison", "comparison", "comparison", "comparison", "none",
                               "endpoint",   "unknown",    "unknown",    "native/1"};
    Membership membership{revision, {}};
    MeasurementColumn column{revision, "endpoint", unit, {}};
    for (const auto& [key, value] : values) {
        membership.rows.push_back({key, Truth::True});
        column.rows.push_back({key, value});
    }
    const auto stats = populationStatistics(membership, column);
    const auto number = [&](std::optional<double> v) -> Json {
        return !stats.missing && v ? Json(*v) : Json(nullptr);
    };
    return {{"experimentalUnits", std::to_string(values.size())},
            {"missingEndpoints", std::to_string(stats.missing)},
            {"mean", number(stats.mean)},
            {"sampleStandardDeviation", number(stats.sampleStandardDeviation)}};
}
std::optional<double> difference(const Json& right, const Json& left) {
    if (right.is_null() || left.is_null()) return {};
    const auto value =
        static_cast<double>(static_cast<long double>(right.get<double>()) - left.get<double>());
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}
} // namespace
Json compareAnalysis(const Json& spec, const SampleEvaluator& evaluate) {
    fields(spec, {"conditions", "design", "weighting", "endpoint", "samples", "descriptiveOnly"});
    if (spec.at("descriptiveOnly") != true || spec.at("weighting") != "equal-experimental-unit" ||
        !spec.at("conditions").is_array() || spec.at("conditions").size() != 2 ||
        !spec.at("samples").is_array() || spec.at("samples").size() < 2 ||
        spec.at("samples").size() > 32)
        throw std::invalid_argument("Comparison requires two conditions, 2..32 explicit "
                                    "experimental units and descriptive-only equal-unit weighting");
    const auto left = label(spec.at("conditions")[0]), right = label(spec.at("conditions")[1]);
    const auto design = spec.at("design").get<std::string>();
    if (left == right || (design != "paired" && design != "independent"))
        throw std::invalid_argument("Invalid comparison design");
    const auto& endpoint = spec.at("endpoint");
    fields(endpoint, {"feature", "statistic", "unit"});
    const auto statistic = endpoint.at("statistic").get<std::string>();
    if (statistic != "mean" && statistic != "median" && statistic != "p05" && statistic != "p95")
        throw std::invalid_argument("Unsupported comparison endpoint");
    const auto unit = endpoint.at("unit").get<std::string>();
    std::map<std::string, std::map<std::string, std::optional<double>>> groups;
    std::set<std::string> fingerprints, sourceKeys, independentUnits;
    Json samples = Json::array();
    Json parameters = nullptr;
    for (const auto& sample : spec.at("samples")) {
        fields(sample, {"condition", "experimentalUnit", "planId", "revision", "source"});
        const auto condition = label(sample.at("condition")),
                   experimentalUnit = label(sample.at("experimentalUnit"));
        if ((condition != left && condition != right) || groups[condition].count(experimentalUnit))
            throw std::invalid_argument("Unknown condition or duplicate experimental unit");
        if (design == "independent" && !independentUnits.insert(experimentalUnit).second)
            throw std::invalid_argument("Independent design reuses an experimental unit");
        if (sample.at("source").at("kind") != "retained")
            throw std::invalid_argument(
                "Comparisons require retained immutable source descriptors");
        const auto response = evaluate(Json{{"action", "evaluate"},
                                            {"planId", sample.at("planId")},
                                            {"revision", sample.at("revision")},
                                            {"source", sample.at("source")}});
        if (response.value("status", std::string{}) != "ok")
            throw std::invalid_argument("Pinned sample evaluation refused: " + response.dump());
        const auto& result = response.at("evaluation");
        if (result.at("feature") != endpoint.at("feature") || result.at("unit") != unit)
            throw std::invalid_argument("Endpoint feature/unit mismatch");
        if (!fingerprints.insert(result.at("sourceFingerprint").get<std::string>()).second ||
            !sourceKeys.insert(result.at("sourceKey").get<std::string>()).second)
            throw std::invalid_argument(
                "Repeated source cannot be counted as independent experimental evidence");
        if (!parameters.is_null() && parameters != result.at("parameterFingerprint"))
            throw std::invalid_argument(
                "Comparison parameter preprocessing differs across samples");
        parameters = result.at("parameterFingerprint");
        const auto& value = result.at(statistic);
        const auto finite = value.is_number() && std::isfinite(value.get<double>());
        groups[condition][experimentalUnit] =
            finite ? std::optional<double>(value.get<double>()) : std::nullopt;
        samples.push_back({{"reference", sample},
                           {"sourceFingerprint", result.at("sourceFingerprint")},
                           {"sourceKey", result.at("sourceKey")},
                           {"value", finite ? value : Json(nullptr)},
                           {"selected", result.at("selected")},
                           {"unknown", result.at("unknown")},
                           {"populationComplete", result.at("populationComplete")},
                           {"qcFingerprint", result.at("qcFingerprint")},
                           {"parameterFingerprint", parameters}});
    }
    if (groups[left].empty() || groups[right].empty())
        throw std::invalid_argument("Both conditions require an experimental unit");
    const auto leftSummary = summarize(groups[left], unit),
               rightSummary = summarize(groups[right], unit);
    Json paired = nullptr;
    std::optional<double> delta;
    if (design == "paired") {
        if (groups[left].size() != groups[right].size())
            throw std::invalid_argument("Paired design requires complete unit matching");
        std::map<std::string, std::optional<double>> differences;
        for (const auto& [key, value] : groups[left]) {
            if (!groups[right].count(key))
                throw std::invalid_argument("Missing paired experimental unit");
            const auto r = groups[right].at(key);
            differences[key] =
                difference(r ? Json(*r) : Json(nullptr), value ? Json(*value) : Json(nullptr));
        }
        paired = summarize(differences, unit);
        if (!paired.at("mean").is_null()) delta = paired.at("mean").get<double>();
    } else
        delta = difference(rightSummary.at("mean"), leftSummary.at("mean"));
    return {{"schema", "F-analysis-comparison/1"},
            {"spec", spec},
            {"samples", samples},
            {"groups", {{left, leftSummary}, {right, rightSummary}}},
            {"pairedDifferences", paired},
            {"differenceRightMinusLeft", delta ? Json(*delta) : Json(nullptr)},
            {"unit", unit},
            {"compatibility", "unknown"},
            {"interpretation", "descriptive only; no inferred scientific compatibility, p-value, "
                               "confidence interval or causal effect"},
            {"weighting",
             "one endpoint per explicit experimental unit, equal weights; cell rows are "
             "not replicates; any missing endpoint makes its aggregate unavailable"}};
}

// ---- F-4: ComparisonSpec and fit authority (F09, I09, I10) -------------------
namespace {
namespace inf = inference;
constexpr const char* kAuthority =
    "native C++ analysis authority (ADR 0016); scipy only as an offline test comparator";
constexpr const char* kAlgorithm = "F-inference/1";

void object(const Json& value, const std::set<std::string>& required,
            const std::set<std::string>& optional, const char* what) {
    if (!value.is_object()) throw std::invalid_argument(std::string(what) + " must be an object");
    for (const auto& name : required)
        if (!value.contains(name))
            throw std::invalid_argument(std::string(what) + " lacks field: " + name);
    for (auto i = value.begin(); i != value.end(); ++i)
        if (!required.count(i.key()) && !optional.count(i.key()))
            throw std::invalid_argument(std::string(what) + " has unknown field: " + i.key());
}
std::string text(const Json& value, const char* what, std::size_t max = 128) {
    if (!value.is_string()) throw std::invalid_argument(std::string(what) + " must be a string");
    auto out = value.get<std::string>();
    if (out.empty() || out.size() > max)
        throw std::invalid_argument(std::string(what) + " must be 1.." + std::to_string(max) +
                                    " characters");
    return out;
}
Json number(double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }
Json number(const std::optional<double>& value) { return value ? number(*value) : Json(nullptr); }

const std::set<std::string> kStatistics{"mean",          "median",
                                        "p05",           "p25",
                                        "p75",           "p95",
                                        "geometricMean", "sampleStandardDeviation",
                                        "cvPercent"};
struct Metric {
    std::string population, parameter, statistic, unit;
    Json json;
};
Metric parseMetric(const Json& value) {
    object(value, {"population", "parameter", "statistic", "unit"}, {}, "metric");
    Metric m{text(value.at("population"), "metric population", 200),
             text(value.at("parameter"), "metric parameter"),
             text(value.at("statistic"), "metric statistic"), text(value.at("unit"), "metric unit"),
             value};
    if (!kStatistics.count(m.statistic))
        throw std::invalid_argument("Unsupported metric statistic: " + m.statistic);
    return m;
}
double confidence(const Json& spec) {
    if (!spec.contains("confidenceLevel")) return 0.95;
    if (!spec.at("confidenceLevel").is_number())
        throw std::invalid_argument("confidenceLevel must be a number");
    // One rule for comparisons and fits (inference::checkConfidenceLevel).
    return inf::checkConfidenceLevel(spec.at("confidenceLevel").get<double>());
}

// One pinned source evaluated for the declared metric, then checked against
// the compatibility reference. Excluded sources keep their reasons.
struct Evaluated {
    Json sample;
    std::string id;
    bool included{true};
    Json reasons = Json::array();
    std::optional<double> value;
    Json evaluation;
    Json provenance;
    Json parameters;
};
void exclude(Evaluated& e, const std::string& reason) {
    e.included = false;
    e.reasons.push_back(reason);
}
std::string describe(const Json& value) {
    return value.is_null() ? std::string("unknown") : value.dump();
}
Json field(const Json& object, const std::string& key) {
    return object.is_object() && object.contains(key) ? object.at(key) : Json(nullptr);
}

Json compatibility(std::vector<Evaluated>& samples, const Metric& metric,
                   const SampleEvaluator& evaluate) {
    for (auto& e : samples) {
        if (!e.included) continue;
        const auto reply = evaluate(Json{{"action", "evaluate"},
                                         {"planId", e.sample.at("planId")},
                                         {"revision", e.sample.at("revision")},
                                         {"source", e.sample.at("source")},
                                         {"metric",
                                          {{"population", metric.population},
                                           {"parameter", metric.parameter}}}});
        const auto status = field(reply, "status");
        if (status != "ok") {
            const auto message = field(reply, "message");
            exclude(e, "missing metric: pinned evaluation refused (" +
                           (status.is_string() ? status.get<std::string>() : std::string("invalid")) +
                           "): " +
                           (message.is_string() ? message.get<std::string>() : reply.dump()));
            continue;
        }
        e.evaluation = reply.at("evaluation");
        e.evaluation.erase("graph");
        e.evaluation.erase("rows");
        e.provenance = field(e.evaluation, "sourceProvenance");
        e.parameters = field(e.evaluation, "sourceParameters");
        if (e.evaluation.at("unit") != metric.unit)
            exclude(e, "different unit: parameter " + metric.parameter + " is " +
                           e.evaluation.at("unit").get<std::string>() + ", metric declares " +
                           metric.unit);
        const auto& value = e.evaluation.at(metric.statistic);
        if (value.is_number() && std::isfinite(value.get<double>()))
            e.value = value.get<double>();
        else
            exclude(e, "missing metric: " + metric.statistic + " of " + metric.parameter +
                           " in population " + metric.population + " is unavailable (measured " +
                           e.evaluation.at("measured").get<std::string>() + " events)");
    }
    // The first sample (declared order) that evaluated with a finite metric is
    // the compatibility reference; every other source must match it.
    const Evaluated* reference = nullptr;
    for (const auto& e : samples)
        if (e.included) {
            reference = &e;
            break;
        }
    std::map<std::string, std::string> fingerprints;
    for (auto& e : samples) {
        if (e.evaluation.is_null()) continue;
        const auto fingerprint = e.evaluation.at("sourceFingerprint").get<std::string>();
        const auto inserted = fingerprints.emplace(fingerprint, e.id);
        if (!inserted.second) {
            exclude(e, "same source as sample " + inserted.first->second +
                           "; one recording cannot be two experimental units");
            continue;
        }
        if (!reference || &e == reference || !e.included) continue;
        if (e.parameters != reference->parameters)
            exclude(e, "different parameters or units: " + describe(e.parameters) + " vs " +
                           describe(reference->parameters));
        if (e.evaluation.at("parameterFingerprint") !=
            reference->evaluation.at("parameterFingerprint"))
            exclude(e, "different parameter preprocessing (transforms/compensation)");
        std::set<std::string> keys;
        for (const Json* p : {static_cast<const Json*>(&e.provenance), &reference->provenance})
            if (p->is_object())
                for (auto i = p->begin(); i != p->end(); ++i)
                    if (i.key() != "semantics") keys.insert(i.key());
        for (const auto& key : keys) {
            const auto mine = field(e.provenance, key);
            const auto theirs = field(reference->provenance, key);
            if (mine == theirs) continue;
            const std::string label = key == "processing"    ? "processing core/contract"
                                      : key == "calibration" ? "calibration identity"
                                      : key == "mechanics"   ? "E-LUT (mechanics) identity"
                                                             : key + " identity";
            exclude(e, "different " + label + ": " + describe(mine) + " vs reference " +
                           describe(theirs));
        }
    }
    return Json{{"reference", reference ? Json(reference->id) : Json(nullptr)},
                {"rule",
                 "each source is compared with the reference (first sample with a finite "
                 "metric): source parameters and units, parameter preprocessing fingerprint, "
                 "and every source provenance identity (processing core/contract, "
                 "calibration, E-LUT); unknown (null) provenance matches only unknown; a "
                 "repeated source fingerprint is excluded"}};
}

Json sampleRecord(const Evaluated& e, Json record) {
    record["sampleId"] = e.id;
    record["experimentalUnit"] = e.sample.at("experimentalUnit");
    record["planId"] = e.sample.at("planId");
    record["revision"] = e.sample.at("revision");
    record["status"] = e.included ? "included" : "excluded";
    record["reasons"] = e.reasons;
    record["value"] = number(e.value);
    for (const char* key : {"sourceFingerprint", "sourceKey", "graphFingerprint",
                            "parameterFingerprint", "unit", "selected", "measured", "missing",
                            "unknown", "qcFingerprint"})
        record[key] = field(e.evaluation, key);
    return record;
}

std::vector<Evaluated> parseSamples(const Json& spec, const std::set<std::string>& extraRequired) {
    const auto& list = spec.at("samples");
    if (!list.is_array() || list.empty() || list.size() > 256)
        throw std::invalid_argument("samples must hold 1..256 pinned sources");
    std::set<std::string> required{"sampleId", "experimentalUnit", "planId", "revision", "source"};
    required.insert(extraRequired.begin(), extraRequired.end());
    std::vector<Evaluated> out;
    std::set<std::string> ids;
    for (const auto& sample : list) {
        object(sample, required, {}, "sample");
        Evaluated e;
        e.sample = sample;
        e.id = text(sample.at("sampleId"), "sampleId");
        text(sample.at("experimentalUnit"), "experimentalUnit");
        if (!ids.insert(e.id).second) throw std::invalid_argument("Duplicate sampleId: " + e.id);
        if (sample.at("revision").is_null())
            throw std::invalid_argument("Sample " + e.id + " must pin a plan revision");
        out.push_back(std::move(e));
    }
    return out;
}

Json summary(std::vector<double> values) {
    PopulationStatistics stats;
    reduceMeasuredValues(values, stats);
    return {{"units", values.size()},
            {"mean", number(stats.mean)},
            {"median", number(stats.median)},
            {"sampleStandardDeviation", number(stats.sampleStandardDeviation)}};
}

Json testJson(const inf::TestResult& r, double level) {
    Json ci = nullptr;
    if (r.confidenceInterval)
        ci = {{"level", level},
              {"lower", number(r.confidenceInterval->lower)},
              {"upper", number(r.confidenceInterval->upper)}};
    return {{"test", r.test},
            {"status", r.status},
            {"undefinedReason", r.status == "undefined" ? Json(r.undefinedReason) : Json(nullptr)},
            {"statisticName", r.statisticName},
            {"statistic", number(r.statistic)},
            {"df1", number(r.df1)},
            {"df2", number(r.df2)},
            {"pValue", number(r.pValue)},
            {"pMethod", r.pMethod},
            {"estimate",
             {{"name", r.estimateName},
              {"value", number(r.estimate)},
              {"confidenceInterval", ci},
              {"ciUnavailableReason", ci.is_null() ? Json(r.ciUnavailableReason) : Json(nullptr)}}},
            {"effectSize", {{"name", r.effectSizeName}, {"value", number(r.effectSize)}}},
            {"notes", r.notes}};
}
double unitMean(const std::vector<double>& values) {
    long double sum = 0;
    for (double v : values) sum += v;
    return static_cast<double>(sum / values.size());
}
} // namespace

Json compareSpec(const Json& spec, const SampleEvaluator& evaluate) {
    object(spec, {"schema", "metric", "design", "groups", "inference", "samples"},
           {"minimumUnitsPerGroup", "confidenceLevel"}, "comparison spec");
    if (spec.at("schema") != "F-comparison-spec/1")
        throw std::invalid_argument("Comparison spec schema must be F-comparison-spec/1");
    const auto metric = parseMetric(spec.at("metric"));
    const auto design = text(spec.at("design"), "design");
    if (design != "unpaired" && design != "paired")
        throw std::invalid_argument("design must be unpaired or paired (by experimental unit id)");
    const auto inference = text(spec.at("inference"), "inference");
    if (inference != "none" && inference != "parametric" && inference != "rank" &&
        inference != "both")
        throw std::invalid_argument("inference must be none, parametric, rank or both");
    int minimum = kMinimumUnitsPerGroup;
    if (spec.contains("minimumUnitsPerGroup")) {
        if (!spec.at("minimumUnitsPerGroup").is_number_integer())
            throw std::invalid_argument("minimumUnitsPerGroup must be an integer");
        minimum = spec.at("minimumUnitsPerGroup").get<int>();
    }
    if (minimum < kMinimumUnitsPerGroup || minimum > 1000)
        throw std::invalid_argument("minimumUnitsPerGroup must be >= " +
                                    std::to_string(kMinimumUnitsPerGroup));
    const double level = confidence(spec);
    const auto& groupList = spec.at("groups");
    if (!groupList.is_array() || groupList.size() < 2 || groupList.size() > 16)
        throw std::invalid_argument("groups must list 2..16 conditions (first is the reference)");
    std::vector<std::string> groups;
    for (const auto& g : groupList) {
        const auto name = text(g, "group");
        if (std::find(groups.begin(), groups.end(), name) != groups.end())
            throw std::invalid_argument("Duplicate group: " + name);
        groups.push_back(name);
    }
    auto samples = parseSamples(spec, {"group"});
    std::map<std::string, std::string> unitGroup; // unpaired: unit -> its one group
    for (const auto& e : samples) {
        const auto group = text(e.sample.at("group"), "group");
        if (std::find(groups.begin(), groups.end(), group) == groups.end())
            throw std::invalid_argument("Sample " + e.id + " names an undeclared group");
        const auto unit = e.sample.at("experimentalUnit").get<std::string>();
        if (design == "unpaired") {
            const auto inserted = unitGroup.emplace(unit, group);
            if (!inserted.second && inserted.first->second != group)
                throw std::invalid_argument(
                    "Unpaired design: experimental unit " + unit + " appears in groups " +
                    inserted.first->second + " and " + group +
                    "; declare design paired or distinct independent units");
        }
    }
    const auto compat = compatibility(samples, metric, evaluate);

    // Unit-level values: sources sharing (group, unit) are technical replicates
    // of one unit and are averaged with equal weight; n counts units.
    std::map<std::string, std::map<std::string, std::vector<double>>> replicate;
    Json sampleRecords = Json::array();
    for (const auto& e : samples) {
        sampleRecords.push_back(sampleRecord(e, Json{{"group", e.sample.at("group")}}));
        if (e.included)
            replicate[e.sample.at("group").get<std::string>()]
                     [e.sample.at("experimentalUnit").get<std::string>()]
                         .push_back(*e.value);
    }
    std::map<std::string, std::map<std::string, double>> unitValues;
    Json groupSummaries = Json::array();
    for (const auto& group : groups) {
        Json units = Json::array();
        std::vector<double> values;
        for (const auto& entry : replicate[group]) {
            const double value = unitMean(entry.second);
            unitValues[group][entry.first] = value;
            values.push_back(value);
            units.push_back({{"experimentalUnit", entry.first},
                             {"value", number(value)},
                             {"technicalReplicates", entry.second.size()}});
        }
        auto s = summary(values);
        s["group"] = group;
        s["unitValues"] = units;
        groupSummaries.push_back(s);
    }

    Json inferenceOut{{"status", "not-requested"},
                      {"reasons", Json::array()},
                      {"tests", Json::array()},
                      {"minimumUnitsPerGroup", minimum}};
    if (inference != "none") {
        std::vector<std::vector<double>> data(groups.size());
        Json reasons = Json::array();
        Json dropped = Json::array();
        const auto complete = [&](const std::string& unit) {
            for (const auto& g : groups)
                if (!unitValues[g].count(unit)) return false;
            return true;
        };
        if (design == "paired") {
            for (const auto& entry : unitValues[groups[0]])
                if (complete(entry.first))
                    for (std::size_t i = 0; i < groups.size(); ++i)
                        data[i].push_back(unitValues[groups[i]].at(entry.first));
            for (const auto& g : groups)
                for (const auto& entry : unitValues[g])
                    if (!complete(entry.first))
                        dropped.push_back({{"group", g}, {"experimentalUnit", entry.first}});
            if (static_cast<int>(data[0].size()) < minimum)
                reasons.push_back("paired design has " + std::to_string(data[0].size()) +
                                  " complete unit pair(s); needs >= " + std::to_string(minimum));
        } else {
            for (std::size_t i = 0; i < groups.size(); ++i) {
                for (const auto& entry : unitValues[groups[i]]) data[i].push_back(entry.second);
                if (static_cast<int>(data[i].size()) < minimum)
                    reasons.push_back("group " + groups[i] + " has " +
                                      std::to_string(data[i].size()) +
                                      " independent experimental unit(s); needs >= " +
                                      std::to_string(minimum));
            }
        }
        inferenceOut["unpairedUnitsDropped"] = dropped;
        if (design == "paired" && groups.size() > 2) {
            inferenceOut["status"] = "unsupported";
            reasons.push_back("repeated-measures inference across more than two groups is not "
                              "implemented; no test is reported");
        } else if (!reasons.empty()) {
            inferenceOut["status"] = "insufficient";
        } else {
            const bool parametric = inference == "parametric" || inference == "both";
            const bool rank = inference == "rank" || inference == "both";
            Json tests = Json::array();
            if (groups.size() == 2 && design == "unpaired") {
                if (parametric)
                    tests.push_back(testJson(inf::welchT(data[0], data[1], level), level));
                if (rank) tests.push_back(testJson(inf::mannWhitneyU(data[0], data[1]), level));
            } else if (groups.size() == 2) {
                if (parametric)
                    tests.push_back(testJson(inf::pairedT(data[0], data[1], level), level));
                if (rank)
                    tests.push_back(testJson(inf::wilcoxonSignedRank(data[0], data[1]), level));
            } else {
                if (parametric) tests.push_back(testJson(inf::oneWayAnova(data), level));
                if (rank) tests.push_back(testJson(inf::kruskalWallis(data), level));
            }
            // Each test carries its own status; the comparison is "undefined"
            // only when no requested test is defined for these values.
            bool anyComputed = false;
            for (const auto& t : tests) {
                if (t.at("status") == "computed")
                    anyComputed = true;
                else
                    reasons.push_back(t.at("test").get<std::string>() + ": " +
                                      t.at("undefinedReason").get<std::string>());
            }
            inferenceOut["status"] = anyComputed ? "computed" : "undefined";
            inferenceOut["tests"] = tests;
            inferenceOut["referenceGroup"] = groups[0];
            inferenceOut["comparatorGroup"] = groups.size() == 2 ? Json(groups[1]) : Json(nullptr);
            inferenceOut["multiplicity"] =
                "no correction across the requested tests; each p-value is two-sided";
        }
        inferenceOut["reasons"] = reasons;
    }
    std::size_t excluded = 0;
    for (const auto& e : samples) excluded += e.included ? 0 : 1;
    return {{"schema", "F-comparison/2"},
            {"authority", kAuthority},
            {"algorithm", kAlgorithm},
            {"spec", spec},
            {"metric", metric.json},
            {"design", design},
            {"compatibility", compat},
            {"samples", sampleRecords},
            {"excludedSamples", excluded},
            {"groups", groupSummaries},
            {"inference", inferenceOut},
            {"unitRule",
             "one value per experimental unit: the declared statistic of the declared "
             "population and parameter over that source's events (exact statistics); sources "
             "sharing a unit within a group are technical replicates averaged with equal "
             "weight; events are never units; n counts units"}};
}

Json fitSpec(const Json& spec, const SampleEvaluator& evaluate) {
    object(spec,
           {"schema", "metric", "model", "concentrationUnit", "concentrationScale", "weighting",
            "design", "samples"},
           {"confidenceLevel"}, "fit spec");
    if (spec.at("schema") != "F-fit-spec/1")
        throw std::invalid_argument("Fit spec schema must be F-fit-spec/1");
    const auto metric = parseMetric(spec.at("metric"));
    const auto model = text(spec.at("model"), "model");
    if (model != "4PL" && model != "3PL")
        throw std::invalid_argument("model must be 4PL or 3PL (hill fixed at 1)");
    const auto concentrationUnit = text(spec.at("concentrationUnit"), "concentrationUnit", 32);
    const auto scale = text(spec.at("concentrationScale"), "concentrationScale");
    if (scale != "log10" && scale != "linear")
        throw std::invalid_argument("concentrationScale must be log10 or linear");
    const auto weighting = text(spec.at("weighting"), "weighting");
    if (weighting != "none" && weighting != "inverse-variance")
        throw std::invalid_argument("weighting must be none or inverse-variance");
    if (spec.at("design") != "independent-units")
        throw std::invalid_argument(
            "design must be independent-units: each experimental unit contributes at one "
            "concentration (repeated measures across concentrations are not modelled)");
    const double level = confidence(spec);
    auto samples = parseSamples(spec, {"concentration"});
    std::map<std::string, double> unitConcentration;
    for (auto& e : samples) {
        const auto& c = e.sample.at("concentration");
        object(c, {"value", "unit"}, {}, "concentration");
        if (!c.at("value").is_number())
            throw std::invalid_argument("concentration value must be a number");
        const double value = c.at("value").get<double>();
        const auto unit = text(c.at("unit"), "concentration unit", 32);
        const auto id = e.sample.at("experimentalUnit").get<std::string>();
        const auto inserted = unitConcentration.emplace(id, value);
        if (!inserted.second && inserted.first->second != value)
            throw std::invalid_argument("Experimental unit " + id +
                                        " appears at two concentrations; declare distinct units");
        if (unit != concentrationUnit)
            exclude(e, "concentration unit " + unit + " differs from declared " +
                           concentrationUnit + " (no unit conversion is performed)");
        else if (!(value > 0) || !std::isfinite(value))
            exclude(e, "concentration must be finite and > 0 to place on the logistic "
                       "concentration axis (vehicle controls are not fitted)");
    }
    const auto compat = compatibility(samples, metric, evaluate);
    std::map<std::string, std::vector<double>> replicate;
    Json sampleRecords = Json::array();
    for (const auto& e : samples) {
        sampleRecords.push_back(
            sampleRecord(e, Json{{"concentration", e.sample.at("concentration")}}));
        if (e.included)
            replicate[e.sample.at("experimentalUnit").get<std::string>()].push_back(*e.value);
    }
    inf::LogisticFitRequest request;
    request.model = model == "4PL" ? inf::LogisticModel::FourParameter
                                   : inf::LogisticModel::ThreeParameterHillOne;
    request.scale =
        scale == "log10" ? inf::ConcentrationScale::Log10 : inf::ConcentrationScale::Linear;
    request.weighting = weighting == "none" ? inf::FitWeighting::None
                                            : inf::FitWeighting::InverseVarianceOfReplicateUnits;
    request.confidenceLevel = level;
    std::vector<std::string> pointUnits;
    for (const auto& entry : replicate) {
        pointUnits.push_back(entry.first);
        request.concentration.push_back(unitConcentration.at(entry.first));
        request.response.push_back(unitMean(entry.second));
    }
    const auto fit = inf::fitLogistic(request);
    Json out{{"schema", "F-fit/1"},
             {"authority", kAuthority},
             {"algorithm", kAlgorithm},
             {"spec", spec},
             {"metric", metric.json},
             {"model", model},
             {"concentrationUnit", concentrationUnit},
             {"concentrationScale", scale},
             {"weighting", weighting},
             {"compatibility", compat},
             {"samples", sampleRecords},
             {"unitPoints", pointUnits.size()},
             {"distinctConcentrations", fit.distinctConcentrations}};
    if (!fit.fitted) {
        out["status"] = "insufficient";
        out["reasons"] = fit.insufficientReasons;
        out["parameters"] = nullptr;
        out["ec50"] = nullptr;
        return out;
    }
    Json parameters = Json::array();
    for (const auto& p : fit.parameters) {
        const std::string unit = p.name == "logEC50" ? "log10(" + concentrationUnit + ")"
                                 : p.name == "ec50"  ? concentrationUnit
                                 : p.name == "hill"  ? std::string("dimensionless")
                                                     : metric.unit;
        Json ci = nullptr;
        if (p.confidenceInterval)
            ci = {{"level", level},
                  {"lower", number(p.confidenceInterval->lower)},
                  {"upper", number(p.confidenceInterval->upper)}};
        parameters.push_back({{"name", p.name},
                              {"estimate", number(p.estimate)},
                              {"unit", unit},
                              {"standardError", number(p.standardError)},
                              {"confidenceInterval", ci},
                              {"atBound", p.atBound}});
    }
    Json residuals = Json::array();
    for (std::size_t i = 0; i < pointUnits.size(); ++i)
        residuals.push_back({{"experimentalUnit", pointUnits[i]},
                             {"concentration", number(request.concentration[i])},
                             {"response", number(request.response[i])},
                             {"fitted", number(fit.fittedValues[i])},
                             {"residual", number(fit.residuals[i])},
                             {"weight", number(fit.weights[i])}});
    Json ec50Interval = nullptr;
    if (fit.ec50Interval)
        ec50Interval = {{"level", level},
                        {"lower", number(fit.ec50Interval->lower)},
                        {"upper", number(fit.ec50Interval->upper)},
                        {"method", scale == "log10"
                                       ? "10^(logEC50 +/- t * SE(logEC50)) (asymmetric)"
                                       : "EC50 +/- t * SE(EC50), SE by exact reparameterization"}};
    out["status"] = "fit";
    out["reasons"] = Json::array();
    out["parameters"] = parameters;
    out["ec50"] = {{"value", number(fit.ec50)},
                   {"unit", concentrationUnit},
                   {"confidenceInterval", ec50Interval}};
    out["residuals"] = residuals;
    out["rSquared"] = {{"value", number(fit.rSquared)},
                       {"estimator", weighting == "none"
                                         ? "1 - SSres/SStot"
                                         : "weighted: 1 - sum w r^2 / sum w (y - weighted mean)^2"}};
    out["residualSumSquares"] = number(fit.residualSumSquares);
    out["degreesOfFreedom"] = fit.degreesOfFreedom;
    out["converged"] = fit.converged;
    out["termination"] = fit.termination;
    out["iterations"] = fit.iterations;
    out["suspect"] = !fit.suspectReasons.empty();
    out["suspectReasons"] = fit.suspectReasons;
    out["method"] =
        "Levenberg-Marquardt least squares on y = bottom + (top - bottom) / (1 + "
        "10^((logEC50 - log10 c) * hill)) (3PL: hill = 1), three starts, covariance "
        "s^2 (J'WJ)^-1 with s^2 = WSSR/(n - p), Student t CIs; bounds |hill| <= 10, logEC50 "
        "within 3 decades of the tested range; weights 1/variance of replicate units per "
        "concentration when declared";
    return out;
}
} // namespace backend::analysis
