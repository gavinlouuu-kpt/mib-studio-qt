// Population density for the Monitoring scatter plot (Qt-free, header-only so
// it is unit testable and can run on a worker thread without touching any
// widget). A Gaussian kernel density estimate is evaluated at every sample of
// the (area, deformability) cloud and normalised to [0, 1] so the tab can
// colour each marker by how crowded its neighbourhood is.
//
// The two axes live in very different units (area in µm², hundreds; deformability,
// 0..1), so the bandwidth is per axis: Silverman's rule of thumb for a
// two-dimensional Gaussian kernel, h = sigma * n^(-1/6), scaled by a user
// factor. One isotropic bandwidth in raw units cannot serve both axes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frontend::monitoring {

struct DensityPoint {
    double x{0.0};
    double y{0.0};
};

struct DensityBandwidth {
    double x{1.0};
    double y{1.0};
};

inline bool isFinitePoint(const DensityPoint& p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

// Per-axis Silverman bandwidth (d = 2): factor * sigma_axis * n^(-1/6).
// Fewer than two finite points, a non-positive factor or a degenerate axis
// (zero spread) fall back to a bandwidth of 1 on that axis: every kernel term
// on a degenerate axis is exp(0) whatever the width, so any positive value is
// equivalent and the result stays finite.
inline DensityBandwidth silvermanBandwidth(const std::vector<DensityPoint>& points, double factor) {
    DensityBandwidth bw;
    if (!(factor > 0.0) || !std::isfinite(factor)) return bw;
    std::size_t n = 0;
    double meanX = 0.0, meanY = 0.0;
    for (const auto& p : points) {
        if (!isFinitePoint(p)) continue;
        ++n;
        meanX += p.x;
        meanY += p.y;
    }
    if (n < 2) return bw;
    meanX /= static_cast<double>(n);
    meanY /= static_cast<double>(n);
    double varX = 0.0, varY = 0.0;
    for (const auto& p : points) {
        if (!isFinitePoint(p)) continue;
        varX += (p.x - meanX) * (p.x - meanX);
        varY += (p.y - meanY) * (p.y - meanY);
    }
    varX /= static_cast<double>(n);
    varY /= static_cast<double>(n);
    const double scale = factor * std::pow(static_cast<double>(n), -1.0 / 6.0);
    const double hx = std::sqrt(varX) * scale;
    const double hy = std::sqrt(varY) * scale;
    if (hx > 0.0 && std::isfinite(hx)) bw.x = hx;
    if (hy > 0.0 && std::isfinite(hy)) bw.y = hy;
    return bw;
}

// Gaussian KDE evaluated at each input point with per-axis bandwidth, scaled
// so the densest point is 1.0. Non-finite points get 0 and never contribute.
// O(n²/2): every pair is visited once (the kernel is symmetric). 1000 points
// is ~0.5 M exp() calls — run it off the GUI thread.
inline std::vector<double> gaussianKdeAtPoints(const std::vector<DensityPoint>& points,
                                               DensityBandwidth bw) {
    std::vector<double> density(points.size(), 0.0);
    if (points.empty()) return density;
    if (!(bw.x > 0.0) || !std::isfinite(bw.x)) bw.x = 1.0;
    if (!(bw.y > 0.0) || !std::isfinite(bw.y)) bw.y = 1.0;
    const double invX = 1.0 / bw.x;
    const double invY = 1.0 / bw.y;
    const std::size_t n = points.size();
    std::vector<unsigned char> finite(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (isFinitePoint(points[i])) {
            finite[i] = 1;
            density[i] = 1.0; // self term, exp(0)
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite[i]) continue;
        const double xi = points[i].x * invX;
        const double yi = points[i].y * invY;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!finite[j]) continue;
            const double dx = xi - points[j].x * invX;
            const double dy = yi - points[j].y * invY;
            const double d2 = dx * dx + dy * dy;
            if (d2 > 50.0) continue; // exp(-25) is below double noise relative to the self term
            const double k = std::exp(-0.5 * d2);
            density[i] += k;
            density[j] += k;
        }
    }
    double maxDensity = 0.0;
    for (double d : density)
        maxDensity = std::max(maxDensity, d);
    if (maxDensity > 0.0) {
        for (double& d : density)
            d /= maxDensity;
    }
    return density;
}

// Silverman bandwidth × factor, then the normalised per-point density.
inline std::vector<double> normalizedDensity(const std::vector<DensityPoint>& points,
                                             double bandwidthFactor) {
    return gaussianKdeAtPoints(points, silvermanBandwidth(points, bandwidthFactor));
}

// ---------------------------------------------------------------------------
// Core region (highest-density region holding a share of the population)
// ---------------------------------------------------------------------------

// Density level `t` such that at least ceil(fraction·n) finite samples have
// density >= t: the ceil(fraction·n)-th largest value. NaN when fewer than
// three finite densities exist or the fraction is not in (0, 1].
inline double coreLevel(const std::vector<double>& density, double fraction) {
    if (!(fraction > 0.0) || fraction > 1.0 || !std::isfinite(fraction)) return std::nan("");
    std::vector<double> finite;
    finite.reserve(density.size());
    for (double d : density)
        if (std::isfinite(d)) finite.push_back(d);
    if (finite.size() < 3) return std::nan("");
    const std::size_t k = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(finite.size()) - 1e-9));
    const std::size_t rank = std::clamp<std::size_t>(k, 1, finite.size());
    std::nth_element(finite.begin(), finite.begin() + static_cast<std::ptrdiff_t>(rank - 1),
                     finite.end(), std::greater<double>());
    return finite[rank - 1];
}

// Maximum raw (un-normalised) Gaussian kernel sum over the samples: the
// constant gaussianKdeAtPoints divides by, so a grid can share its [0, 1].
inline double rawKdeMaximum(const std::vector<DensityPoint>& points, DensityBandwidth bw) {
    if (!(bw.x > 0.0) || !std::isfinite(bw.x)) bw.x = 1.0;
    if (!(bw.y > 0.0) || !std::isfinite(bw.y)) bw.y = 1.0;
    const double invX = 1.0 / bw.x, invY = 1.0 / bw.y;
    double best = 0.0;
    for (const auto& p : points) {
        if (!isFinitePoint(p)) continue;
        double sum = 0.0;
        for (const auto& q : points) {
            if (!isFinitePoint(q)) continue;
            const double dx = (p.x - q.x) * invX, dy = (p.y - q.y) * invY;
            const double d2 = dx * dx + dy * dy;
            if (d2 > 50.0) continue;
            sum += std::exp(-0.5 * d2);
        }
        best = std::max(best, sum);
    }
    return best;
}

struct DensityGrid {
    int nx{0};
    int ny{0};
    double x0{0.0}, x1{0.0}, y0{0.0}, y1{0.0};
    std::vector<double> value; // row-major, value[j * nx + i] at (x(i), y(j))
    double x(int i) const { return nx > 1 ? x0 + (x1 - x0) * i / (nx - 1) : x0; }
    double y(int j) const { return ny > 1 ? y0 + (y1 - y0) * j / (ny - 1) : y0; }
    double at(int i, int j) const { return value[static_cast<std::size_t>(j) * nx + i]; }
};

// The same Gaussian KDE on a regular grid over [x0,x1]×[y0,y1], divided by
// `normaliser` (use rawKdeMaximum) so it is on the point-density scale.
// nx·ny·n exp() calls minus the 5-bandwidth cut; 128×64 over 1000 points is
// ~50 ms — worker thread only.
inline DensityGrid gaussianKdeGrid(const std::vector<DensityPoint>& points, DensityBandwidth bw,
                                   double normaliser, double x0, double x1, double y0, double y1,
                                   int nx, int ny) {
    DensityGrid g;
    if (nx < 2 || ny < 2 || !(x1 > x0) || !(y1 > y0) || !(normaliser > 0.0) ||
        !std::isfinite(normaliser))
        return g;
    if (!(bw.x > 0.0) || !std::isfinite(bw.x)) bw.x = 1.0;
    if (!(bw.y > 0.0) || !std::isfinite(bw.y)) bw.y = 1.0;
    g.nx = nx;
    g.ny = ny;
    g.x0 = x0;
    g.x1 = x1;
    g.y0 = y0;
    g.y1 = y1;
    g.value.assign(static_cast<std::size_t>(nx) * ny, 0.0);
    const double invX = 1.0 / bw.x, invY = 1.0 / bw.y, inv = 1.0 / normaliser;
    std::vector<double> sx, sy;
    sx.reserve(points.size());
    sy.reserve(points.size());
    for (const auto& p : points) {
        if (!isFinitePoint(p)) continue;
        sx.push_back(p.x * invX);
        sy.push_back(p.y * invY);
    }
    for (int j = 0; j < ny; ++j) {
        const double gy = g.y(j) * invY;
        for (int i = 0; i < nx; ++i) {
            const double gx = g.x(i) * invX;
            double sum = 0.0;
            for (std::size_t k = 0; k < sx.size(); ++k) {
                const double dx = gx - sx[k], dy = gy - sy[k];
                const double d2 = dx * dx + dy * dy;
                if (d2 > 50.0) continue;
                sum += std::exp(-0.5 * d2);
            }
            g.value[static_cast<std::size_t>(j) * nx + i] = sum * inv;
        }
    }
    return g;
}

// A closed loop in axis units (last vertex repeats the first).
using Contour = std::vector<DensityPoint>;

// Marching squares: the iso-lines of `grid` at `level`, chained into closed
// loops. Loops cut by the grid border are closed along the border edge
// (the region is the visible chart). Saddle cells are split by the cell's
// centre value. Empty for an invalid grid or a non-finite level.
inline std::vector<Contour> isoContours(const DensityGrid& grid, double level) {
    std::vector<Contour> loops;
    if (grid.nx < 2 || grid.ny < 2 || !std::isfinite(level)) return loops;
    // Segment endpoints are keyed by (edge id) so the two cells sharing an
    // edge produce bit-identical vertices and chain exactly.
    struct Vertex {
        DensityPoint p;
        long long key;
    };
    // Edge id: horizontal edge (i, j) between (i,j)-(i+1,j): 2*(j*nx+i);
    // vertical edge between (i,j)-(i,j+1): 2*(j*nx+i)+1.
    auto hEdge = [&](int i, int j) { return 2LL * (static_cast<long long>(j) * grid.nx + i); };
    auto vEdge = [&](int i, int j) { return 2LL * (static_cast<long long>(j) * grid.nx + i) + 1; };
    auto lerp = [&](double xa, double ya, double va, double xb, double yb, double vb) {
        const double t = (vb != va) ? std::clamp((level - va) / (vb - va), 0.0, 1.0) : 0.5;
        return DensityPoint{xa + t * (xb - xa), ya + t * (yb - ya)};
    };
    std::vector<std::pair<Vertex, Vertex>> segments;
    for (int j = 0; j + 1 < grid.ny; ++j) {
        for (int i = 0; i + 1 < grid.nx; ++i) {
            const double v00 = grid.at(i, j), v10 = grid.at(i + 1, j);
            const double v11 = grid.at(i + 1, j + 1), v01 = grid.at(i, j + 1);
            if (!std::isfinite(v00) || !std::isfinite(v10) || !std::isfinite(v11) ||
                !std::isfinite(v01))
                continue;
            const int code = (v00 >= level ? 1 : 0) | (v10 >= level ? 2 : 0) |
                             (v11 >= level ? 4 : 0) | (v01 >= level ? 8 : 0);
            if (code == 0 || code == 15) continue;
            const double xa = grid.x(i), xb = grid.x(i + 1), ya = grid.y(j), yb = grid.y(j + 1);
            const Vertex bottom{lerp(xa, ya, v00, xb, ya, v10), hEdge(i, j)};
            const Vertex right{lerp(xb, ya, v10, xb, yb, v11), vEdge(i + 1, j)};
            const Vertex top{lerp(xa, yb, v01, xb, yb, v11), hEdge(i, j + 1)};
            const Vertex left{lerp(xa, ya, v00, xa, yb, v01), vEdge(i, j)};
            switch (code) {
            case 1: case 14: segments.push_back({left, bottom}); break;
            case 2: case 13: segments.push_back({bottom, right}); break;
            case 3: case 12: segments.push_back({left, right}); break;
            case 4: case 11: segments.push_back({right, top}); break;
            case 6: case 9: segments.push_back({bottom, top}); break;
            case 7: case 8: segments.push_back({left, top}); break;
            case 5: case 10: {
                const double centre = 0.25 * (v00 + v10 + v11 + v01);
                const bool centreInside = centre >= level;
                if ((code == 5) == centreInside) {
                    segments.push_back({left, top});
                    segments.push_back({bottom, right});
                } else {
                    segments.push_back({left, bottom});
                    segments.push_back({right, top});
                }
                break;
            }
            default: break;
            }
        }
    }
    if (segments.empty()) return loops;
    // Chain by edge key.
    std::unordered_map<long long, std::vector<std::size_t>> byKey;
    for (std::size_t s = 0; s < segments.size(); ++s) {
        byKey[segments[s].first.key].push_back(s);
        byKey[segments[s].second.key].push_back(s);
    }
    std::vector<unsigned char> used(segments.size(), 0);
    auto walk = [&](std::size_t start, bool forward, Contour& out) {
        std::size_t s = start;
        long long key = forward ? segments[s].second.key : segments[s].first.key;
        for (;;) {
            const auto it = byKey.find(key);
            if (it == byKey.end()) return;
            std::size_t next = segments.size();
            for (std::size_t cand : it->second)
                if (!used[cand]) { next = cand; break; }
            if (next == segments.size()) return;
            used[next] = 1;
            const bool fromFirst = segments[next].first.key == key;
            const Vertex& far = fromFirst ? segments[next].second : segments[next].first;
            out.push_back(far.p);
            key = far.key;
            if (key == (forward ? segments[start].first.key : segments[start].second.key)) return;
        }
    };
    for (std::size_t s = 0; s < segments.size(); ++s) {
        if (used[s]) continue;
        used[s] = 1;
        Contour forwardPart{segments[s].first.p, segments[s].second.p};
        walk(s, true, forwardPart);
        if (!(forwardPart.front().x == forwardPart.back().x &&
              forwardPart.front().y == forwardPart.back().y)) {
            // Open at the border: extend backwards, then close along the border.
            Contour backwardPart;
            walk(s, false, backwardPart);
            Contour loop(backwardPart.rbegin(), backwardPart.rend());
            loop.insert(loop.end(), forwardPart.begin(), forwardPart.end());
            loop.push_back(loop.front());
            loops.push_back(std::move(loop));
        } else {
            loops.push_back(std::move(forwardPart));
        }
    }
    return loops;
}

// Fraction of finite samples that lie inside any of the loops (even-odd rule).
inline double fractionInside(const std::vector<DensityPoint>& points,
                             const std::vector<Contour>& loops) {
    std::size_t n = 0, inside = 0;
    for (const auto& p : points) {
        if (!isFinitePoint(p)) continue;
        ++n;
        bool in = false;
        for (const auto& loop : loops) {
            for (std::size_t a = 0, b = loop.size() - 1; a < loop.size(); b = a++) {
                const auto& pa = loop[a];
                const auto& pb = loop[b];
                if ((pa.y > p.y) != (pb.y > p.y)) {
                    const double x = pb.x + (p.y - pb.y) / (pa.y - pb.y) * (pa.x - pb.x);
                    if (p.x < x) in = !in;
                }
            }
        }
        if (in) ++inside;
    }
    return n ? static_cast<double>(inside) / static_cast<double>(n) : 0.0;
}

// Sequential single-hue ramp for the density colouring (blue, light → dark;
// the light end still clears 2:1 on a white chart so sparse markers remain
// visible). `t` is clamped to [0, 1]; a non-finite value reads as 0.
struct DensityRgb {
    int r{0};
    int g{0};
    int b{0};
};

inline DensityRgb densityRampColor(double t) {
    static constexpr DensityRgb kStops[] = {
        {0x86, 0xb6, 0xef}, // 250
        {0x6d, 0xa7, 0xec}, // 300
        {0x55, 0x98, 0xe7}, // 350
        {0x39, 0x87, 0xe5}, // 400
        {0x2a, 0x78, 0xd6}, // 450
        {0x25, 0x6a, 0xbf}, // 500
        {0x1c, 0x5c, 0xab}, // 550
        {0x18, 0x4f, 0x95}, // 600
        {0x10, 0x42, 0x81}, // 650
        {0x0d, 0x36, 0x6b}, // 700
    };
    constexpr int kLast = static_cast<int>(sizeof(kStops) / sizeof(kStops[0])) - 1;
    if (!std::isfinite(t)) t = 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double pos = t * kLast;
    const int lo = std::min(static_cast<int>(pos), kLast - 1);
    const double frac = pos - lo;
    const auto mix = [frac](int a, int b) {
        return static_cast<int>(std::lround(a + (b - a) * frac));
    };
    const DensityRgb& a = kStops[lo];
    const DensityRgb& b = kStops[lo + 1];
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

} // namespace frontend::monitoring
