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

#include "backend/processing/MonitoringDensity.h"

#include "support/assert.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using backend::monitoring::DensityBandwidth;
using backend::monitoring::DensityPoint;
using backend::monitoring::densityRampColor;
using backend::monitoring::DensityRgb;
using backend::monitoring::gaussianKdeAtPoints;
using backend::monitoring::normalizedDensity;
using backend::monitoring::silvermanBandwidth;

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

template <class F>
double timeMs(F&& work) {
    const auto t0 = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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

    // ---- core region: level, grid, contour --------------------------------------
    {
        using backend::monitoring::Contour;
        using backend::monitoring::coreLevel;
        using backend::monitoring::DensityGrid;
        using backend::monitoring::fractionInside;
        using backend::monitoring::gaussianKdeGrid;
        using backend::monitoring::isoContours;
        using backend::monitoring::rawKdeMaximum;

        // coreLevel: rank semantics and degenerate input.
        {
            const std::vector<double> d{0.1, 0.9, 0.5, 0.3, 0.7};
            MIB_EXPECT(coreLevel(d, 1.0) == 0.1, "p = 1 -> the minimum density");
            MIB_EXPECT(coreLevel(d, 0.4) == 0.7, "ceil(0.4*5) = 2 -> the 2nd largest");
            MIB_EXPECT(coreLevel(d, 0.5) == 0.5, "ceil(0.5*5) = 3 -> the median");
            MIB_EXPECT(std::isnan(coreLevel({0.2, 0.4}, 0.9)), "fewer than 3 samples -> no level");
            MIB_EXPECT(std::isnan(coreLevel(d, 0.0)) && std::isnan(coreLevel(d, 1.5)), "fraction outside (0,1] -> no level");
            const std::vector<double> withNan{0.1, std::nan(""), 0.9, 0.5, 0.3};
            MIB_EXPECT(coreLevel(withNan, 0.5) == 0.5, "non-finite densities are ignored in the rank");
        }

        auto contoursFor = [&](const std::vector<DensityPoint>& pts, double frac, double x0, double x1, double y0,
                               double y1) {
            const auto bw = silvermanBandwidth(pts, 1.0);
            const auto d = gaussianKdeAtPoints(pts, bw);
            const double level = coreLevel(d, frac);
            const DensityGrid g = gaussianKdeGrid(pts, bw, rawKdeMaximum(pts, bw), x0, x1, y0, y1, 128, 64);
            return isoContours(g, level);
        };

        // One Gaussian cloud: one loop enclosing about p of the samples.
        {
            const auto pts = gaussianCloud(rng, 600, 300.0, 0.10, 30.0, 0.015);
            const auto loops = contoursFor(pts, 0.9, 0.0, 700.0, 0.0, 0.5);
            MIB_EXPECT(loops.size() == 1, "a single cloud gives exactly one loop");
            const double inside = fractionInside(pts, loops);
            std::fprintf(stderr, "single cloud: %zu loop(s), %.3f inside the 90%% contour\n", loops.size(), inside);
            MIB_EXPECT(inside >= 0.85 && inside <= 0.96, "the 90% contour encloses ~90% of the samples");
            MIB_EXPECT(!loops.empty() && loops[0].size() > 20 && loops[0].front().x == loops[0].back().x &&
                           loops[0].front().y == loops[0].back().y,
                       "loop is closed");
        }

        // Two separated clouds: two loops.
        {
            auto pts = gaussianCloud(rng, 400, 150.0, 0.05, 20.0, 0.01);
            const auto b = gaussianCloud(rng, 300, 450.0, 0.30, 20.0, 0.01);
            pts.insert(pts.end(), b.begin(), b.end());
            const auto loops = contoursFor(pts, 0.9, 0.0, 700.0, 0.0, 0.5);
            MIB_EXPECT(loops.size() == 2, "two separated clouds give two loops");
            MIB_EXPECT(fractionInside(pts, loops) >= 0.85, "both cores are enclosed");
        }

        // Translation invariance (to within one grid cell).
        {
            const auto pts = gaussianCloud(rng, 500, 250.0, 0.15, 25.0, 0.012);
            std::vector<DensityPoint> shifted;
            for (const auto& p : pts) shifted.push_back({p.x + 100.0, p.y + 0.1});
            const auto a = contoursFor(pts, 0.9, 0.0, 700.0, 0.0, 0.5);
            const auto b = contoursFor(shifted, 0.9, 0.0, 700.0, 0.0, 0.5);
            MIB_REQUIRE(a.size() == 1 && b.size() == 1, "one loop each");
            auto centroid = [](const Contour& c) {
                double sx = 0, sy = 0;
                for (const auto& p : c) { sx += p.x; sy += p.y; }
                return DensityPoint{sx / c.size(), sy / c.size()};
            };
            const auto ca = centroid(a[0]), cb = centroid(b[0]);
            const double cellX = 700.0 / 127, cellY = 0.5 / 63;
            MIB_EXPECT(std::fabs((cb.x - ca.x) - 100.0) < 2 * cellX && std::fabs((cb.y - ca.y) - 0.1) < 2 * cellY,
                       "shifting the population shifts the contour by the same offset");
        }

        // Population cut by the chart border: every loop is closed (along the
        // border where the iso-line leaves the chart) and the visible samples
        // are still mostly enclosed.
        {
            const auto pts = gaussianCloud(rng, 400, 40.0, 0.10, 25.0, 0.01);
            const auto loops = contoursFor(pts, 0.9, 0.0, 700.0, 0.0, 0.5);
            std::vector<DensityPoint> visible;
            for (const auto& p : pts)
                if (p.x >= 0.0) visible.push_back(p);
            std::fprintf(stderr, "border-cut: %zu loop(s), %.3f of visible samples inside\n", loops.size(),
                         fractionInside(visible, loops));
            MIB_EXPECT(!loops.empty(), "border-cut population still yields a contour");
            bool closed = true;
            for (const auto& l : loops) closed = closed && l.size() >= 3 && l.front().x == l.back().x && l.front().y == l.back().y;
            MIB_EXPECT(closed, "border-cut loops are closed along the border");
            MIB_EXPECT(fractionInside(visible, loops) >= 0.8, "visible samples are enclosed");
        }

        // Grid and normaliser: the fast (separable, symmetric) evaluation equals
        // the direct radial sum, and costs a fraction of it on a wide
        // population where the 5-bandwidth cut prunes almost nothing (the real
        // 512x96 stream: ~600 ms per 1000 cells with the direct sum on GCC).
        {
            // Spread like the real stream (Silverman bandwidth ~68 um^2 x 0.06 on a
            // 0..1000 x 0..1 chart), so the cut prunes almost nothing.
            const auto pts = gaussianCloud(rng, 1000, 450.0, 0.3, 215.0, 0.19);
            const auto bw = silvermanBandwidth(pts, 1.0);
            const auto directMax = [&] {
                const double ix = 1.0 / bw.x, iy = 1.0 / bw.y;
                double best = 0.0;
                for (const auto& p : pts) {
                    double sum = 0.0;
                    for (const auto& q : pts) {
                        const double dx = (p.x - q.x) * ix, dy = (p.y - q.y) * iy, d2 = dx * dx + dy * dy;
                        if (d2 <= 50.0) sum += std::exp(-0.5 * d2);
                    }
                    best = std::max(best, sum);
                }
                return best;
            };
            const auto directGrid = [&](double norm, double x0, double x1, double y0, double y1, int nx, int ny) {
                std::vector<double> v(static_cast<std::size_t>(nx) * ny, 0.0);
                const double ix = 1.0 / bw.x, iy = 1.0 / bw.y;
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        const double gx = x0 + (x1 - x0) * i / (nx - 1), gy = y0 + (y1 - y0) * j / (ny - 1);
                        double sum = 0.0;
                        for (const auto& p : pts) {
                            const double dx = (gx - p.x) * ix, dy = (gy - p.y) * iy, d2 = dx * dx + dy * dy;
                            if (d2 <= 50.0) sum += std::exp(-0.5 * d2);
                        }
                        v[static_cast<std::size_t>(j) * nx + i] = sum / norm;
                    }
                return v;
            };
            double refMax = 0.0, fastMax = 0.0;
            const double tDirectMax = timeMs([&] { refMax = directMax(); });
            const double tFastMax = timeMs([&] { fastMax = rawKdeMaximum(pts, bw); });
            MIB_EXPECT(std::abs(fastMax - refMax) <= 1e-9 * refMax, "normaliser equals the direct pairwise sum");
            {
                // The service and the full-run record take the normaliser from
                // the per-point pass instead of a second pairwise pass.
                auto raw = backend::monitoring::rawKdeAtPoints(pts, bw);
                const double fromPass = backend::monitoring::normaliseByMaximum(raw);
                const auto normalised = backend::monitoring::gaussianKdeAtPoints(pts, bw);
                bool same = std::abs(fromPass - refMax) <= 1e-9 * refMax && raw.size() == normalised.size();
                for (std::size_t k = 0; same && k < raw.size(); ++k) same = raw[k] == normalised[k];
                MIB_EXPECT(same, "one pass yields both the normalised densities and the normaliser");
            }
            std::vector<double> ref;
            DensityGrid fast;
            const double tDirectGrid = timeMs([&] { ref = directGrid(refMax, 0.0, 1000.0, 0.0, 1.0, 128, 64); });
            const double tFastGrid =
                timeMs([&] { fast = gaussianKdeGrid(pts, bw, refMax, 0.0, 1000.0, 0.0, 1.0, 128, 64); });
            double worst = 0.0;
            for (std::size_t k = 0; k < ref.size() && k < fast.value.size(); ++k)
                worst = std::max(worst, std::abs(ref[k] - fast.value[k]));
            std::fprintf(stderr,
                         "grid 128x64 over 1000 clustered points: direct %.1f ms, fast %.1f ms; normaliser direct "
                         "%.1f ms, fast %.1f ms; max |diff| %.2e\n",
                         tDirectGrid, tFastGrid, tDirectMax, tFastMax, worst);
            // Per-axis cut: the extra corner terms are each < exp(-25) of the self term.
            MIB_EXPECT(fast.value.size() == ref.size() && worst <= 1e-7, "grid equals the direct radial sum");
            MIB_EXPECT(tFastGrid * 4.0 <= tDirectGrid, "grid costs <= 1/4 of the direct sum");
            MIB_EXPECT(tFastMax * 1.5 <= tDirectMax, "normaliser visits each pair once");
        }

        // Degenerate input never yields NaN or loops.
        {
            const std::vector<DensityPoint> same(20, DensityPoint{100.0, 0.1});
            const auto bw = silvermanBandwidth(same, 1.0);
            const auto g = gaussianKdeGrid(same, bw, rawKdeMaximum(same, bw), 0.0, 700.0, 0.0, 0.5, 32, 16);
            MIB_EXPECT(std::all_of(g.value.begin(), g.value.end(), [](double v) { return std::isfinite(v); }),
                       "identical points: finite grid");
            MIB_EXPECT(isoContours(g, std::nan("")).empty(), "NaN level -> no loops");
            MIB_EXPECT(isoContours(DensityGrid{}, 0.5).empty(), "empty grid -> no loops");
            MIB_EXPECT(contoursFor({{1, 1}, {2, 2}}, 0.9, 0, 10, 0, 10).empty(), "two points -> no level -> no loops");
        }
    }

    return mib::test::exitCode();
}
