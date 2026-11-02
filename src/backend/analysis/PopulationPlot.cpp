#include "backend/analysis/SourceAnalysis.h"
#include "SourceCore.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace backend::analysis {
using Json = nlohmann::json;
// Histogram layers over exact membership (F05). One membership pass for all
// layers; bins counted in bounded chunks (columnHistogram) for row and
// columnar sources alike.
Json plotSource(const Json& rawSource, const Json& strategy, const ColumnResolver& columns) {
    if (strategy.at("plot").value("kind", std::string{}) == "scatter")
        return scatterSource(rawSource, strategy, columns);
    Json result;
    detail::withSource(rawSource, columns, [&](const detail::SourceView& view) {
        const auto prepared = detail::prepare(view, strategy);
        const auto& spec = strategy.at("plot");
        if (!spec.is_object() || spec.size() != 7 || spec.at("kind") != "histogram" ||
            !spec.at("bins").is_number_unsigned() || !spec.at("layers").is_array())
            throw std::invalid_argument(
                "Expected histogram feature, minimum, maximum, bins, normalization and layers");
        const auto feature = spec.at("feature").get<std::string>();
        if (!prepared.units.contains(feature)) throw std::invalid_argument("Plot feature unavailable");
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
        auto query = strategy;
        query["feature"] = feature;
        const auto run = detail::runGraph(view, prepared, query);
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
            (void)detail::graphFor(query, population);
            const auto& bits = run.membership->population(population);
            const auto h = columnHistogram(bits, *prepared.columns, feature, minimum, maximum, bins);
            const auto measured = h.measured;
            Json exact = Json::array(), normalized = Json::array();
            for (std::size_t i = 0; i < bins; ++i) {
                exact.push_back(std::to_string(h.counts[i]));
                if (normalization != "count" && measured == 0) {
                    normalized.push_back(nullptr);
                    continue;
                }
                long double value = h.counts[i];
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
                              {"selected", std::to_string(bits.count(Truth::True))},
                              {"unknown", std::to_string(bits.count(Truth::Unknown))},
                              {"measured", std::to_string(h.measured)},
                              {"missing", std::to_string(h.missing)},
                              {"underflow", std::to_string(h.underflow)},
                              {"overflow", std::to_string(h.overflow)}});
        }
        result = Json{{"schema", "F-plot/1"},
                      {"sourceFingerprint", rawSource.at("fingerprint")},
                      {"parameterFingerprint", prepared.parameterFingerprint},
                      {"transforms", prepared.transforms},
                      {"spec", spec},
                      {"feature", feature},
                      {"unit", prepared.units.at(feature)},
                      {"edges", edges},
                      {"layers", layers},
                      {"populationComplete", rawSource.at("populationComplete")},
                      {"semantics",
                       "Analysis-parameter bins [left,right), last bin closed; fraction/density "
                       "denominator is all finite selected measurements including under/overflow. "
                       "Display offsets never change counts, gates or statistics."}};
    });
    return result;
}
} // namespace backend::analysis
