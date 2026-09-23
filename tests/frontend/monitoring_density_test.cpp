// monitoring_density_test
//
// Invariant + budget guard for the Monitoring scatter density (KDE) kernel:
//  - densities are finite, in [0, 1], and the densest sample is exactly 1;
//  - a crowded cluster outranks every sparse outlier, and the centre of a
//    symmetric cloud is its densest point;
//  - the bandwidth is per axis: two populations separated only in
//    deformability (0..1) while area spans hundreds of µm² are still told
//    apart (an isotropic bandwidth in raw units collapses them);
//  - degenerate input (identical points, a single point, non-finite samples,
//    empty input) never yields NaN or aborts;
//  - the result is a pure function of the sample set (order-invariant);
//  - the colour ramp is monotone (darker for denser) and clamps its input;
//  - the cost grows no worse than quadratically (ratio-gated, no absolute ms).

#include "frontend/tabs/MonitoringDensity.h"

#include "support/assert.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using frontend::monitoring::DensityBandwidth;
using frontend::monitoring::DensityPoint;
using frontend::monitoring::densityRampColor;
using frontend::monitoring::DensityRgb;
using frontend::monitoring::gaussianKdeAtPoints;
using frontend::monitoring::normalizedDensity;
using frontend::monitoring::silvermanBandwidth;

namespace {

bool allInUnitRange(const std::vector<double>& d) {
    return std::all_of(d.begin(), d.end(),
                       [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; });
}

double maxOf(const std::vector<double>& d) {
    return d.empty() ? -1.0 : *std::max_element(d.begin(), d.end());
}

double luminance(const DensityRgb& c) {
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

std::vector<DensityPoint> gaussianCloud(std::mt19937& rng, std::size_t n, double cx, double cy,
                                        double sx, double sy) {
    std::normal_distribution<double> dx(cx, sx), dy(cy, sy);
    std::vector<DensityPoint> pts;
    pts.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        pts.push_back({dx(rng), dy(rng)});
    return pts;
}

double timeMs(const std::vector<DensityPoint>& pts, double& sink) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto d = normalizedDensity(pts, 1.0);
    const auto t1 = std::chrono::steady_clock::now();
    sink += std::accumulate(d.begin(), d.end(), 0.0);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

int main() {
    std::mt19937 rng(20260923);

    // ---- empty / single / identical ------------------------------------------
    MIB_EXPECT(normalizedDensity({}, 1.0).empty(), "empty in -> empty out");
    {
        const auto d = normalizedDensity({{300.0, 0.05}}, 1.0);
        MIB_EXPECT(d.size() == 1 && d[0] == 1.0, "a single point is its own maximum");
    }
    {
        std::vector<DensityPoint> same(50, DensityPoint{420.0, 0.2});
        const auto d = normalizedDensity(same, 1.0);
        MIB_EXPECT(d.size() == 50 && allInUnitRange(d), "identical points: finite, in range");
        MIB_EXPECT(std::all_of(d.begin(), d.end(), [](double v) { return v == 1.0; }),
                   "identical points all share the maximum");
        const DensityBandwidth bw = silvermanBandwidth(same, 1.0);
        MIB_EXPECT(bw.x > 0.0 && bw.y > 0.0 && std::isfinite(bw.x) && std::isfinite(bw.y),
                   "degenerate axes fall back to a positive bandwidth");
    }

    // ---- non-finite samples ----------------------------------------------------
    {
        std::vector<DensityPoint> pts = gaussianCloud(rng, 40, 300.0, 0.1, 20.0, 0.01);
        pts.push_back({std::nan(""), 0.1});
        pts.push_back({300.0, std::numeric_limits<double>::infinity()});
        const auto d = normalizedDensity(pts, 1.0);
        MIB_EXPECT(d.size() == pts.size() && allInUnitRange(d),
                   "non-finite samples never poison the result");
        MIB_EXPECT(d[40] == 0.0 && d[41] == 0.0, "non-finite samples read as zero density");
        MIB_EXPECT(maxOf(d) == 1.0, "finite samples still normalise to 1");
    }

    // ---- cluster vs outliers -------------------------------------------------
    {
        auto pts = gaussianCloud(rng, 200, 300.0, 0.05, 15.0, 0.01);
        const std::size_t clusterEnd = pts.size();
        std::uniform_real_distribution<double> ux(600.0, 1000.0), uy(0.5, 1.0);
        for (int i = 0; i < 20; ++i)
            pts.push_back({ux(rng), uy(rng)});
        const auto d = normalizedDensity(pts, 1.0);
        MIB_EXPECT(allInUnitRange(d) && maxOf(d) == 1.0, "densities in [0,1] with max exactly 1");
        const double minCluster = *std::min_element(d.begin(), d.begin() + clusterEnd);
        const double maxOutlier = *std::max_element(d.begin() + clusterEnd, d.end());
        MIB_EXPECT(minCluster > maxOutlier, "every cluster point is denser than every outlier");
    }

    // ---- symmetric cloud: centre is the densest sample ----------------------
    {
        std::vector<DensityPoint> pts;
        pts.push_back({500.0, 0.5});
        for (int ring = 1; ring <= 4; ++ring) {
            for (int k = 0; k < 24; ++k) {
                const double a = 2.0 * 3.14159265358979323846 * k / 24.0;
                pts.push_back({500.0 + 40.0 * ring * std::cos(a), 0.5 + 0.04 * ring * std::sin(a)});
            }
        }
        const auto d = normalizedDensity(pts, 1.0);
        MIB_EXPECT(d[0] == 1.0, "centre of a symmetric cloud is the maximum");
        MIB_EXPECT(std::count(d.begin(), d.end(), 1.0) == 1, "only the centre reaches 1");
        // Outer ring is sparser than the inner ring.
        const double inner = d[1];
        const double outer = d[1 + 24 * 3];
        MIB_EXPECT(inner > outer, "density falls off with radius");
    }

    // ---- per-axis bandwidth: separation in deformability alone -------------
    {
        // Same area distribution (hundreds of µm²), populations differ only in
        // deformability by 0.8. Group A has ten times the members of group B,
        // so with a bandwidth that respects the Y scale every A point must be
        // denser than every B point. An isotropic bandwidth in raw units
        // (~tens of µm²) sees a Y gap of 0.8 as nothing and merges them.
        auto a = gaussianCloud(rng, 100, 400.0, 0.10, 60.0, 0.01);
        const auto b = gaussianCloud(rng, 10, 400.0, 0.90, 60.0, 0.01);
        std::vector<DensityPoint> pts = a;
        pts.insert(pts.end(), b.begin(), b.end());
        const auto d = normalizedDensity(pts, 1.0);
        const double minA = *std::min_element(d.begin(), d.begin() + 100);
        const double maxB = *std::max_element(d.begin() + 100, d.end());
        MIB_EXPECT(minA > maxB, "populations separated only in deformability are told apart");
        // Prove the point: an isotropic raw-unit bandwidth merges them.
        const auto iso = gaussianKdeAtPoints(pts, DensityBandwidth{50.0, 50.0});
        const double isoMinA = *std::min_element(iso.begin(), iso.begin() + 100);
        const double isoMaxB = *std::max_element(iso.begin() + 100, iso.end());
        MIB_EXPECT(!(isoMinA > isoMaxB),
                   "control: isotropic raw-unit bandwidth cannot separate them");
        const DensityBandwidth bw = silvermanBandwidth(pts, 1.0);
        MIB_EXPECT(bw.x > 10.0 && bw.y < 1.0, "bandwidth follows each axis' own scale");
        const DensityBandwidth wide = silvermanBandwidth(pts, 2.0);
        MIB_EXPECT(std::fabs(wide.x - 2.0 * bw.x) < 1e-9 && std::fabs(wide.y - 2.0 * bw.y) < 1e-9,
                   "factor scales both axes linearly");
    }

    // ---- order invariance ------------------------------------------------------
    {
        auto pts = gaussianCloud(rng, 120, 350.0, 0.2, 30.0, 0.05);
        const auto d = normalizedDensity(pts, 1.0);
        std::vector<std::size_t> perm(pts.size());
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), rng);
        std::vector<DensityPoint> shuffled;
        for (std::size_t i : perm)
            shuffled.push_back(pts[i]);
        const auto ds = normalizedDensity(shuffled, 1.0);
        bool same = true;
        for (std::size_t k = 0; k < perm.size(); ++k)
            same = same && std::fabs(ds[k] - d[perm[k]]) < 1e-9;
        MIB_EXPECT(same, "density is a property of the sample, not of its position");
    }

    // ---- colour ramp -----------------------------------------------------------
    {
        double last = luminance(densityRampColor(0.0));
        bool monotone = true;
        for (int i = 1; i <= 100; ++i) {
            const double lum = luminance(densityRampColor(i / 100.0));
            monotone = monotone && lum <= last + 1e-9;
            last = lum;
        }
        MIB_EXPECT(monotone, "denser -> darker, monotonically");
        const DensityRgb lo = densityRampColor(0.0), hi = densityRampColor(1.0);
        MIB_EXPECT(luminance(lo) - luminance(hi) > 80.0, "the ramp spans a clearly visible range");
        const auto eq = [](DensityRgb a, DensityRgb b) {
            return a.r == b.r && a.g == b.g && a.b == b.b;
        };
        MIB_EXPECT(eq(densityRampColor(-3.0), lo) && eq(densityRampColor(7.0), hi),
                   "input is clamped");
        MIB_EXPECT(eq(densityRampColor(std::nan("")), lo), "non-finite reads as sparse");
    }

    // ---- budget: no worse than quadratic ------------------------------------
    {
        double sink = 0.0;
        const auto small = gaussianCloud(rng, 500, 400.0, 0.3, 80.0, 0.1);
        const auto large = gaussianCloud(rng, 2000, 400.0, 0.3, 80.0, 0.1);
        timeMs(small, sink); // warm up
        double tSmall = 1e9, tLarge = 1e9;
        for (int rep = 0; rep < 3; ++rep) {
            tSmall = std::min(tSmall, timeMs(small, sink));
            tLarge = std::min(tLarge, timeMs(large, sink));
        }
        std::fprintf(stderr,
                     "kde 500 pts: %.3f ms, 2000 pts: %.3f ms (ratio %.1f, quadratic = 16)\n",
                     tSmall, tLarge, tLarge / std::max(tSmall, 1e-6));
        MIB_EXPECT(tLarge <= 64.0 * std::max(tSmall, 0.05),
                   "cost grows no worse than quadratically (4x points -> <= 64x time)");
        MIB_EXPECT(sink > 0.0, "work not optimised away");
    }

    return mib::test::exitCode();
}
