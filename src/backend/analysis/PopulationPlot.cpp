#include "backend/analysis/SourceAnalysis.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
Json plotSource(const Json& source, const Json& strategy) {
    const auto& spec = strategy.at("plot");
    if (!spec.is_object() || spec.size() != 7 || spec.at("kind") != "histogram" ||
        !spec.at("bins").is_number_unsigned() || !spec.at("layers").is_array())
        throw std::invalid_argument(
            "Expected histogram feature, minimum, maximum, bins, normalization and layers");
    const auto feature = spec.at("feature").get<std::string>();
    if (!source.at("parameters").contains(feature))
        throw std::invalid_argument("Plot feature unavailable");
    const double minimum = spec.at("minimum").get<double>(),
                 maximum = spec.at("maximum").get<double>();
    const auto bins = spec.at("bins").get<std::size_t>();
    const auto normalization = spec.at("normalization").get<std::string>();
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum || bins < 1 ||
        bins > 256 ||
        (normalization != "count" && normalization != "fraction" && normalization != "density") ||
        spec.at("layers").empty() || spec.at("layers").size() > 8)
        throw std::invalid_argument("Invalid histogram limits/normalization/layers");
    std::vector<double> edges;
    for (std::size_t i = 0; i <= bins; ++i) {
        const long double weight = static_cast<long double>(i) / bins;
        const double edge = static_cast<double>((1 - weight) * minimum + weight * maximum);
        if (!std::isfinite(edge) || (!edges.empty() && edge <= edges.back()))
            throw std::invalid_argument("Histogram edges are not distinctly representable");
        edges.push_back(edge);
    }
    std::map<std::string, std::optional<double>> values;
    for (const auto& row : source.at("rows")) {
        const auto& measurements = row.at("measurements");
        std::optional<double> value;
        if (measurements.contains(feature) && !measurements.at(feature).is_null()) {
            const auto raw = measurements.at(feature).get<double>();
            if (std::isfinite(raw)) value = raw;
        }
        values.emplace(row.at("key").get<std::string>(), value);
    }
    Json layers = Json::array();
    std::set<std::string> populations;
    for (const auto& layer : spec.at("layers")) {
        if (!layer.is_object() || layer.size() != 2)
            throw std::invalid_argument("Expected population and displayOffset");
        const auto population = layer.at("population").get<std::string>();
        const auto offset = layer.at("displayOffset").get<double>();
        if (!std::isfinite(offset) || std::abs(offset) > 1000000 ||
            !populations.insert(population).second)
            throw std::invalid_argument("Invalid display offset or duplicate population layer");
        auto query = strategy;
        query["graph"]["output"] = population;
        query["feature"] = feature;
        const auto evaluation = evaluateSource(source, query);
        std::vector<uint64_t> counts(bins, 0);
        uint64_t underflow = 0, overflow = 0;
        for (const auto& row : evaluation.at("rows")) {
            if (row.at("membership") != "selected") continue;
            const auto value = values.at(row.at("key").get<std::string>());
            if (!value) continue;
            if (*value < minimum) {
                ++underflow;
                continue;
            }
            if (*value > maximum) {
                ++overflow;
                continue;
            }
            auto index = static_cast<std::size_t>(
                std::upper_bound(edges.begin(), edges.end(), *value) - edges.begin() - 1);
            ++counts[std::min(index, bins - 1)];
        }
        const auto measured = std::stoull(evaluation.at("measured").get<std::string>());
        Json exact = Json::array(), normalized = Json::array();
        for (std::size_t i = 0; i < bins; ++i) {
            exact.push_back(std::to_string(counts[i]));
            if (normalization != "count" && measured == 0) {
                normalized.push_back(nullptr);
                continue;
            }
            long double value = counts[i];
            if (normalization != "count") value /= measured;
            if (normalization == "density")
                value /= static_cast<long double>(edges[i + 1]) - edges[i];
            const double display = static_cast<double>(value);
            normalized.push_back(std::isfinite(display) ? Json(display) : Json(nullptr));
        }
        layers.push_back({{"population", population},
                          {"displayOffset", offset},
                          {"counts", exact},
                          {"values", normalized},
                          {"selected", evaluation.at("selected")},
                          {"unknown", evaluation.at("unknown")},
                          {"measured", evaluation.at("measured")},
                          {"missing", evaluation.at("missing")},
                          {"underflow", std::to_string(underflow)},
                          {"overflow", std::to_string(overflow)}});
    }
    return {{"schema", "F-plot/1"},
            {"sourceFingerprint", source.at("fingerprint")},
            {"spec", spec},
            {"feature", feature},
            {"unit", source.at("parameters").at(feature)},
            {"edges", edges},
            {"layers", layers},
            {"populationComplete", source.at("populationComplete")},
            {"semantics",
             "Raw-value bins [left,right), last bin closed; fraction/density denominator "
             "is all finite selected measurements including under/overflow. Display "
             "offsets never change counts, gates or statistics."}};
}
} // namespace backend::analysis
