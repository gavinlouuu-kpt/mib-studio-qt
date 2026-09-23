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
