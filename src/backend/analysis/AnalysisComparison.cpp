#include "backend/analysis/AnalysisComparison.h"
#include "backend/analysis/PopulationStatistics.h"
#include "backend/analysis/SourceAnalysis.h"
#include <cmath>
#include <map>
#include <set>
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
Json compareAnalysis(const Json& spec, const std::function<Json(const Json&)>& evaluate) {
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
} // namespace backend::analysis
