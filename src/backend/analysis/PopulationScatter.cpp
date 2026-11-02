#include "backend/analysis/SourceAnalysis.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <cstdint>
namespace backend::analysis {
using Json = nlohmann::json;
Json scatterSource(const Json& rawSource, const Json& strategy) {
    const auto source = prepareAnalysisSource(rawSource, strategy);
    const auto& spec = strategy.at("plot");
    const std::set<std::string> fields{"kind", "x", "y", "layers", "maxPoints", "seed"};
    if (!spec.is_object() || spec.size() != fields.size())
        throw std::invalid_argument("Expected bounded scatter specification");
    for (auto i = spec.begin(); i != spec.end(); ++i)
        if (!fields.count(i.key())) throw std::invalid_argument("Unknown scatter field");
    const auto x = spec.at("x").get<std::string>(), y = spec.at("y").get<std::string>();
    if (!source.at("parameters").contains(x) || !source.at("parameters").contains(y))
        throw std::invalid_argument("Scatter parameter unavailable");
    if (!spec.at("maxPoints").is_number_unsigned() || spec.at("maxPoints").get<std::size_t>() < 1 ||
        spec.at("maxPoints").get<std::size_t>() > 1000 || !spec.at("seed").is_number_unsigned() ||
        spec.at("seed").get<uint64_t>() > UINT32_MAX || !spec.at("layers").is_array() ||
        spec.at("layers").empty() || spec.at("layers").size() > 8)
        throw std::invalid_argument("Scatter seed/layer/point budget invalid");
    std::map<std::string, std::pair<Json, Json>> coordinates;
    for (const auto& row : source.at("rows")) {
        const auto value = [&](const std::string& feature) -> Json {
            const auto& v = row.at("measurements");
            if (v.contains(feature) && v.at(feature).is_number() &&
                std::isfinite(v.at(feature).get<double>()))
                return v.at(feature);
            return nullptr;
        };
        coordinates.emplace(row.at("key").get<std::string>(), std::make_pair(value(x), value(y)));
    }
    struct Point {
        std::string rank, population, key;
        Json x, y;
    };
    std::vector<Point> points;
    Json layers = Json::array();
    std::set<std::string> populations;
    for (const auto& layer : spec.at("layers")) {
        if (!layer.is_object() || layer.size() != 1)
            throw std::invalid_argument("Scatter layer requires population only");
        const auto population = layer.at("population").get<std::string>();
        if (!populations.insert(population).second)
            throw std::invalid_argument("Duplicate scatter population");
        auto query = strategy;
        query["graph"]["output"] = population;
        query["feature"] = x;
        const auto evaluated = evaluateSource(rawSource, query);
        std::size_t missing = 0, eligible = 0;
        for (const auto& row : evaluated.at("rows")) {
            if (row.at("membership") != "selected") continue;
            const auto key = row.at("key").get<std::string>();
            const auto& xy = coordinates.at(key);
            if (xy.first.is_null() || xy.second.is_null()) {
                ++missing;
                continue;
            }
            ++eligible;
            points.push_back({contentHash(Json::array({spec.at("seed"), population, key})),
                              population, key, xy.first, xy.second});
        }
        layers.push_back(Json{{"population", population},
                              {"selected", evaluated.at("selected")},
                              {"unknown", evaluated.at("unknown")},
                              {"missingCoordinates", std::to_string(missing)},
                              {"eligible", std::to_string(eligible)}});
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return std::tie(a.rank, a.population, a.key) < std::tie(b.rank, b.population, b.key);
    });
    Json bounds = nullptr;
    if (!points.empty()) {
        double xmin = points[0].x.get<double>(), xmax = xmin, ymin = points[0].y.get<double>(),
               ymax = ymin;
        for (const auto& point : points) {
            xmin = std::min(xmin, point.x.get<double>());
            xmax = std::max(xmax, point.x.get<double>());
            ymin = std::min(ymin, point.y.get<double>());
            ymax = std::max(ymax, point.y.get<double>());
        }
        bounds = Json{{"x", Json::array({xmin, xmax})}, {"y", Json::array({ymin, ymax})}};
    }
    const auto eligible = points.size();
    points.resize(std::min(points.size(), spec.at("maxPoints").get<std::size_t>()));
    Json rendered = Json::array();
    for (const auto& point : points)
        rendered.push_back(Json{
            {"population", point.population}, {"key", point.key}, {"x", point.x}, {"y", point.y}});
    return Json{{"schema", "F-scatter/1"},
                {"sourceFingerprint", source.at("fingerprint")},
                {"parameterFingerprint", source.at("parameterFingerprint")},
                {"transforms", source.at("transforms")},
                {"spec", spec},
                {"xUnit", source.at("parameters").at(x)},
                {"yUnit", source.at("parameters").at(y)},
                {"layers", layers},
                {"points", rendered},
                {"bounds", bounds},
                {"eligible", std::to_string(eligible)},
                {"populationComplete", source.at("populationComplete")},
                {"semantics",
                 "Finite selected coordinates; explicit bounded display sample sorted by "
                 "SHA256(JSON[seed,population,eventKey]); statistics use all selected rows, "
                 "never display points. An event may occur in multiple population layers."}};
}
} // namespace backend::analysis
