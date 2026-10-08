#pragma once

// Which scatter point is under the pointer (issue #467). Qt-free on purpose:
// the React + Tauri Charts view implements the same rule and both are
// checked against tests/fixtures/review_scatter_hits.json, so the shells
// cannot drift on what a click selects.
//
// Rule: map every point whose data value lies inside the visible axis
// ranges to pixels (linear, y grows downwards), take the nearest within
// `tolerancePx` of the pointer; equal distances go to the lowest frame
// index. Points outside the visible ranges are never hit, even when their
// marker would still be within tolerance of the plot edge.

#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace frontend::scatterhit {

struct Viewport {
    // Visible data ranges.
    double x0{0.0}, x1{1.0}, y0{0.0}, y1{1.0};
    // Plot rectangle in pixels (same coordinate system as the pointer).
    double left{0.0}, top{0.0}, width{1.0}, height{1.0};
};

struct Point {
    double x{0.0};
    double y{0.0};
    int frame{-1}; // frame index the point stands for
};

inline bool insidePlot(const Viewport& v, double px, double py)
{
    return px >= v.left && px <= v.left + v.width && py >= v.top && py <= v.top + v.height;
}

// Index into `points` of the hit, or nullopt.
inline std::optional<std::size_t> nearest(const std::vector<Point>& points, const Viewport& v, double px, double py,
                                          double tolerancePx)
{
    if (!(v.x1 > v.x0) || !(v.y1 > v.y0) || !(v.width > 0.0) || !(v.height > 0.0)) return std::nullopt;
    if (!insidePlot(v, px, py)) return std::nullopt;
    const double sx = v.width / (v.x1 - v.x0);
    const double sy = v.height / (v.y1 - v.y0);
    const double tol2 = tolerancePx * tolerancePx;
    std::optional<std::size_t> best;
    double bestD2 = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point& p = points[i];
        if (p.x < v.x0 || p.x > v.x1 || p.y < v.y0 || p.y > v.y1) continue;
        const double dx = v.left + (p.x - v.x0) * sx - px;
        const double dy = v.top + (v.y1 - p.y) * sy - py;
        const double d2 = dx * dx + dy * dy;
        if (d2 > tol2) continue;
        if (d2 < bestD2 || (d2 == bestD2 && best && p.frame < points[*best].frame)) {
            bestD2 = d2;
            best = i;
        }
    }
    return best;
}

} // namespace frontend::scatterhit
