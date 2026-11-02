#include "backend/analysis/PopulationGates.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace backend::analysis {
using Json = nlohmann::json;
namespace {
using Point = std::array<double, 2>;
double cross(const Point& a, const Point& b, const Point& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
bool onSegment(const Point& a, const Point& b, const Point& p) {
    return cross(a, b, p) == 0 && p[0] >= std::min(a[0], b[0]) && p[0] <= std::max(a[0], b[0]) &&
           p[1] >= std::min(a[1], b[1]) && p[1] <= std::max(a[1], b[1]);
}
std::vector<Point> polygonPoints(const Json& vertices) {
    if (!vertices.is_array() || vertices.size() < 3 || vertices.size() > 129)
        throw std::invalid_argument("Polygon requires 3..128 vertices, optionally closed");
    std::vector<Point> points;
    for (const auto& v : vertices) {
        if (!v.is_array() || v.size() != 2) throw std::invalid_argument("Invalid polygon vertex");
        Point p;
        for (size_t i = 0; i < 2; ++i) {
            if (!v[i].is_number())
                throw std::invalid_argument("Polygon coordinates must be numeric");
            p[i] = v[i].get<double>();
            if (!std::isfinite(p[i]) || std::abs(p[i]) > 1e100)
                throw std::invalid_argument("Polygon coordinate outside bounded domain");
        }
        points.push_back(p);
    }
    if (points.front() == points.back()) points.pop_back();
    const auto n = points.size();
    if (n < 3 || n > 128 || std::set<Point>(points.begin(), points.end()).size() != n)
        throw std::invalid_argument("Duplicate polygon vertex");
    long double area = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& a = points[(i + n - 1) % n];
        const auto& b = points[i];
        const auto& c = points[(i + 1) % n];
        if (cross(a, b, c) == 0 &&
            (a[0] - b[0]) * (c[0] - b[0]) + (a[1] - b[1]) * (c[1] - b[1]) > 0)
            throw std::invalid_argument("Overlapping polygon edges");
        area += static_cast<long double>(b[0]) * c[1] - static_cast<long double>(c[0]) * b[1];
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;
            const auto& d = points[j];
            const auto& e = points[(j + 1) % n];
            const auto x = cross(b, c, d), y = cross(b, c, e), z = cross(d, e, b),
                       w = cross(d, e, c);
            if (((x > 0 && y < 0) || (x < 0 && y > 0)) && ((z > 0 && w < 0) || (z < 0 && w > 0)) ||
                onSegment(b, c, d) || onSegment(b, c, e) || onSegment(d, e, b) ||
                onSegment(d, e, c))
                throw std::invalid_argument("Self-intersecting polygon");
        }
    }
    if (area == 0) throw std::invalid_argument("Zero-area polygon");
    return points;
}
Truth polygonMembership(const std::vector<Point>& points, double x, double y) {
    // Match the worker's bounded numeric domain; out-of-domain measurements are unknown.
    if (std::abs(x) > 1e100 || std::abs(y) > 1e100) return Truth::Unknown;
    const Point p{x, y};
    bool inside = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto& a = points[j];
        const auto& b = points[i];
        if (onSegment(a, b, p)) return Truth::True; // all edges and vertices are inclusive
        if ((a[1] > y) != (b[1] > y)) {
            const auto orientation = cross(a, b, p);
            if ((orientation > 0) == (b[1] > a[1])) inside = !inside;
        }
    }
    return inside ? Truth::True : Truth::False;
}
std::string featureName(const Json& value, const char* message) {
    const auto feature = value.get<std::string>();
    if (feature.empty() || feature.size() > 64 ||
        feature.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
        throw std::invalid_argument(message);
    return feature;
}
} // namespace

CompiledGraph compileGraph(const Json& q) {
    if (!q.is_object() || q.size() != 4 || !q.at("nodes").is_array())
        throw std::invalid_argument("Expected graph id, revision, output and nodes");
    CompiledGraph g;
    g.plan.id = q.at("id").get<std::string>();
    g.plan.revision = q.at("revision").get<std::string>();
    g.output = q.at("output").get<std::string>();
    for (const auto& v : q.at("nodes")) {
        PopulationNode node;
        node.id = v.at("id").get<std::string>();
        node.parent = v.at("parent").get<std::string>();
        const auto kind = v.at("kind").get<std::string>();
        if (kind == "interval") {
            if (v.size() != 6) throw std::invalid_argument("Invalid interval node fields");
            node.predicate = node.id;
            GateSpec gate;
            if (!v.at("minimum").is_null()) gate.bounds.minimum = v.at("minimum").get<double>();
            if (!v.at("maximum").is_null()) gate.bounds.maximum = v.at("maximum").get<double>();
            validateInterval(gate.bounds);
            gate.x = featureName(v.at("feature"), "Unsupported graph feature");
            g.gates.emplace(node.id, std::move(gate));
        } else if (kind == "ellipse" || kind == "quadrant" || kind == "polygon") {
            if (v.size() != (kind == "ellipse" ? 9u : kind == "polygon" ? 6u : 8u))
                throw std::invalid_argument("Invalid geometry node fields");
            GateSpec gate;
            gate.x = featureName(v.at("x"), "Unsupported geometry parameter");
            gate.y = featureName(v.at("y"), "Unsupported geometry parameter");
            if (gate.x == gate.y)
                throw std::invalid_argument("Geometry requires distinct parameters");
            if (kind == "polygon") {
                gate.kind = GateSpec::Kind::Polygon;
                gate.polygon = polygonPoints(v.at("vertices"));
            } else if (kind == "ellipse") {
                gate.kind = GateSpec::Kind::Ellipse;
                for (const char* key : {"centerX", "centerY", "radiusX", "radiusY"})
                    if (!std::isfinite(v.at(key).get<double>()))
                        throw std::invalid_argument("Ellipse geometry must be finite");
                gate.centerX = v.at("centerX").get<double>();
                gate.centerY = v.at("centerY").get<double>();
                gate.radiusX = v.at("radiusX").get<double>();
                gate.radiusY = v.at("radiusY").get<double>();
                if (gate.radiusX <= 0 || gate.radiusY <= 0)
                    throw std::invalid_argument("Ellipse radii must be positive");
            } else {
                gate.kind = GateSpec::Kind::Quadrant;
                gate.thresholdX = v.at("thresholdX").get<double>();
                gate.thresholdY = v.at("thresholdY").get<double>();
                if (!std::isfinite(gate.thresholdX) || !std::isfinite(gate.thresholdY))
                    throw std::invalid_argument("Quadrant thresholds must be finite");
                const auto name = v.at("quadrant").get<std::string>();
                const std::set<std::string> choices{"lower-left", "lower-right", "upper-left",
                                                    "upper-right"};
                if (!choices.count(name)) throw std::invalid_argument("Unknown quadrant");
                gate.right = name == "lower-right" || name == "upper-right";
                gate.upper = name == "upper-left" || name == "upper-right";
            }
            node.predicate = node.id;
            g.gates.emplace(node.id, std::move(gate));
        } else {
            if (v.size() != 4) throw std::invalid_argument("Invalid Boolean node fields");
            if (kind == "and")
                node.kind = NodeKind::And;
            else if (kind == "or")
                node.kind = NodeKind::Or;
            else if (kind == "not")
                node.kind = NodeKind::Not;
            else
                throw std::invalid_argument("Unsupported graph node kind");
            node.operands = v.at("operands").get<std::vector<std::string>>();
        }
        g.plan.nodes.push_back(std::move(node));
    }
    g.order = validatePlan(g.plan);
    if (std::find(g.order.begin(), g.order.end(), g.output) == g.order.end())
        throw std::invalid_argument("Graph output is not a population");
    return g;
}

Truth gateTruth(const GateSpec& gate, std::optional<double> x, std::optional<double> y) {
    if (gate.kind == GateSpec::Kind::Interval) {
        if (!x || !std::isfinite(*x)) return Truth::Unknown;
        return ((!gate.bounds.minimum || *x >= *gate.bounds.minimum) &&
                (!gate.bounds.maximum || *x <= *gate.bounds.maximum))
                   ? Truth::True
                   : Truth::False;
    }
    if (!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) return Truth::Unknown;
    if (gate.kind == GateSpec::Kind::Polygon) return polygonMembership(gate.polygon, *x, *y);
    if (gate.kind == GateSpec::Kind::Quadrant) {
        const bool right = *x >= gate.thresholdX;
        const bool upper = *y >= gate.thresholdY;
        return (right == gate.right && upper == gate.upper) ? Truth::True : Truth::False;
    }
    const long double dx = (static_cast<long double>(*x) - gate.centerX) / gate.radiusX;
    const long double dy = (static_cast<long double>(*y) - gate.centerY) / gate.radiusY;
    return std::hypot(dx, dy) <= 1 ? Truth::True : Truth::False;
}

std::vector<std::string> gateFeatures(const CompiledGraph& graph) {
    std::set<std::string> features;
    for (const auto& [id, gate] : graph.gates) {
        features.insert(gate.x);
        if (gate.kind != GateSpec::Kind::Interval) features.insert(gate.y);
    }
    return {features.begin(), features.end()};
}
} // namespace backend::analysis
