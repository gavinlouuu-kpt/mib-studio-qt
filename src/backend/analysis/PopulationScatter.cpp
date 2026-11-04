#include "backend/analysis/SourceAnalysis.h"
#include "SourceCore.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <set>
#include <tuple>
namespace backend::analysis {
using Json = nlohmann::json;
// Bounded display sample (F05): every selected event with finite (x, y) is
// ranked by SHA256(JSON[seed, population, eventKey]); the maxPoints lowest
// ranks are kept with a bounded heap, so the sample is identical for row and
// columnar sources and memory never holds every eligible point.
Json scatterSource(const Json& rawSource, const Json& strategy, const ColumnResolver& columns) {
    Json result;
    detail::withSource(rawSource, columns, [&](const detail::SourceView& view) {
        const auto prepared = detail::prepare(view, strategy);
        const auto& spec = strategy.at("plot");
        const std::set<std::string> fields{"kind", "x", "y", "layers", "maxPoints", "seed"};
        if (!spec.is_object() || spec.size() != fields.size())
            throw std::invalid_argument("Expected bounded scatter specification");
        for (auto i = spec.begin(); i != spec.end(); ++i)
            if (!fields.count(i.key())) throw std::invalid_argument("Unknown scatter field");
        const auto x = spec.at("x").get<std::string>(), y = spec.at("y").get<std::string>();
        if (!prepared.units.contains(x) || !prepared.units.contains(y))
            throw std::invalid_argument("Scatter parameter unavailable");
        if (!spec.at("maxPoints").is_number_unsigned() || spec.at("maxPoints").get<std::size_t>() < 1 ||
            spec.at("maxPoints").get<std::size_t>() > 1000 || !spec.at("seed").is_number_unsigned() ||
            spec.at("seed").get<std::uint64_t>() > UINT32_MAX || !spec.at("layers").is_array() ||
            spec.at("layers").empty() || spec.at("layers").size() > 8)
            throw std::invalid_argument("Scatter seed/layer/point budget invalid");
        const auto maxPoints = spec.at("maxPoints").get<std::size_t>();
        struct Point {
            std::string rank, population, key;
            std::uint64_t event{0};
            double x{0}, y{0};
            bool operator<(const Point& o) const {
                return std::tie(rank, population, key) < std::tie(o.rank, o.population, o.key);
            }
        };
        std::priority_queue<Point> kept; // largest rank on top
        std::uint64_t eligibleTotal = 0;
        bool anyPoint = false;
        double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
        auto query = strategy;
        query["feature"] = x;
        const auto run = detail::runGraph(view, prepared, query);
        Json layers = Json::array();
        std::set<std::string> populations;
        for (const auto& layer : spec.at("layers")) {
            if (!layer.is_object() || layer.size() != 1)
                throw std::invalid_argument("Scatter layer requires population only");
            const auto population = layer.at("population").get<std::string>();
            if (!populations.insert(population).second)
                throw std::invalid_argument("Duplicate scatter population");
            (void)detail::graphFor(query, population);
            const auto& bits = run.membership->population(population);
            std::uint64_t missing = 0, eligible = 0;
            prepared.columns->scan(
                {x, y}, 65536,
                [&](std::uint64_t begin, std::size_t count, const std::vector<const double*>& v) {
                    for (std::size_t j = 0; j < count; ++j) {
                        const auto i = begin + j;
                        if (bits.get(i) != Truth::True) continue;
                        if (!std::isfinite(v[0][j]) || !std::isfinite(v[1][j])) {
                            ++missing;
                            continue;
                        }
                        ++eligible;
                        const double px = v[0][j], py = v[1][j];
                        if (!anyPoint) {
                            xmin = xmax = px;
                            ymin = ymax = py;
                            anyPoint = true;
                        }
                        xmin = std::min(xmin, px);
                        xmax = std::max(xmax, px);
                        ymin = std::min(ymin, py);
                        ymax = std::max(ymax, py);
                        const auto key = view.key(i);
                        Point point{contentHash(Json::array({spec.at("seed"), population, key})),
                                    population, key, i, px, py};
                        if (kept.size() < maxPoints) {
                            kept.push(std::move(point));
                        } else if (point < kept.top()) {
                            kept.pop();
                            kept.push(std::move(point));
                        }
                    }
                    return true;
                });
            eligibleTotal += eligible;
            layers.push_back(Json{{"population", population},
                                  {"selected", std::to_string(bits.count(Truth::True))},
                                  {"unknown", std::to_string(bits.count(Truth::Unknown))},
                                  {"missingCoordinates", std::to_string(missing)},
                                  {"eligible", std::to_string(eligible)}});
        }
        std::vector<Point> points;
        while (!kept.empty()) {
            points.push_back(kept.top());
            kept.pop();
        }
        std::reverse(points.begin(), points.end());
        Json bounds = nullptr;
        if (anyPoint)
            bounds = Json{{"x", Json::array({xmin, xmax})}, {"y", Json::array({ymin, ymax})}};
        Json rendered = Json::array();
        for (const auto& point : points)
            rendered.push_back(Json{{"population", point.population},
                                    {"key", point.key},
                                    {"x", view.value(point.event, x, point.x)},
                                    {"y", view.value(point.event, y, point.y)}});
        result = Json{{"schema", "F-scatter/1"},
                      {"sourceFingerprint", rawSource.at("fingerprint")},
                      {"parameterFingerprint", prepared.parameterFingerprint},
                      {"transforms", prepared.transforms},
                      {"spec", spec},
                      {"xUnit", prepared.units.at(x)},
                      {"yUnit", prepared.units.at(y)},
                      {"layers", layers},
                      {"points", rendered},
                      {"bounds", bounds},
                      {"eligible", std::to_string(eligibleTotal)},
                      {"populationComplete", rawSource.at("populationComplete")},
                      {"semantics",
                       "Finite selected coordinates; explicit bounded display sample sorted by "
                       "SHA256(JSON[seed,population,eventKey]); statistics use all selected rows, "
                       "never display points. An event may occur in multiple population layers."}};
    });
    return result;
}
} // namespace backend::analysis
