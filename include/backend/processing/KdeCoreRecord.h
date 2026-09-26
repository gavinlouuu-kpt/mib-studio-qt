// KDE core contour record (Qt-free, header-only): the JSON document stored
// in an experiment file by Hdf5Service::writeKdeLiveJson (provisional, copy
// of the live Monitoring view) or writeKdeAnalysisJson (computed from the
// full recorded run). Schema `kde_core_schema_version` = 1; see
// docs/exec-plans/completed/2026-09-24-kde-core-region-split.md and
// knowledge_map/data-model/HDF5-Storage.md.
//
// Contours are closed polylines in native chart units (area µm²,
// deformability) so any reader can overlay them without re-running the
// kernel. A record with no contour (too few cells) is valid and carries an
// empty `contours` array.
#pragma once

#include "backend/processing/MonitoringDensity.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace backend::monitoring {

inline constexpr int kKdeCoreSchemaVersion = 1;

struct KdeCoreRecord {
    int schemaVersion{kKdeCoreSchemaVersion};
    bool provisional{true};
    std::string source{"live-buffer"}; // "live-buffer" | "full-run"
    double coreFraction{0.9};
    double level{0.0};
    std::uint64_t cellCount{0};
    std::uint64_t populationCount{0};
    std::uint64_t excludedPoints{0};
    std::string bandwidthRule{"silverman"};
    double bandwidthFactor{1.0};
    double bandwidthX{0.0}; // µm²
    double bandwidthY{0.0}; // deformability
    double pixelToMicron{0.0};
    double x0{0.0}, x1{0.0}, y0{0.0}, y1{0.0}; // axis range the contour was traced on
    int gridNx{0};
    int gridNy{0};
    std::vector<Contour> contours;
    std::uint64_t computedAtNs{0};
};

namespace detail {
inline double finiteOr(double v, double fallback) {
    return std::isfinite(v) ? v : fallback;
}
} // namespace detail

inline std::string toJson(const KdeCoreRecord& r) {
    nlohmann::json loops = nlohmann::json::array();
    for (const auto& loop : r.contours) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : loop)
            pts.push_back({p.x, p.y});
        loops.push_back(std::move(pts));
    }
    nlohmann::json j = {
        {"schema_version", r.schemaVersion},
        {"provisional", r.provisional},
        {"source", r.source},
        {"core_fraction", r.coreFraction},
        // NaN is not JSON; "no level" is stored as null.
        {"level", std::isfinite(r.level) ? nlohmann::json(r.level) : nlohmann::json(nullptr)},
        {"cell_count", r.cellCount},
        {"population_count", r.populationCount},
        {"excluded_points", r.excludedPoints},
        {"bandwidth_rule", r.bandwidthRule},
        {"bandwidth_factor", r.bandwidthFactor},
        {"bandwidth_x_um2", detail::finiteOr(r.bandwidthX, 0.0)},
        {"bandwidth_y", detail::finiteOr(r.bandwidthY, 0.0)},
        {"pixel_to_micron_factor", r.pixelToMicron},
        {"axis_range", {{"x0", r.x0}, {"x1", r.x1}, {"y0", r.y0}, {"y1", r.y1}}},
        {"grid", {{"nx", r.gridNx}, {"ny", r.gridNy}}},
        {"contours", std::move(loops)},
        {"computed_at_ns", r.computedAtNs},
    };
    return j.dump();
}

// Parses a record. Returns nullopt (with `error` set) for malformed JSON, a
// missing/incompatible schema version, or malformed contours. Unknown extra
// members are ignored so later minor additions stay readable.
inline std::optional<KdeCoreRecord> fromJson(const std::string& text,
                                             std::string* error = nullptr) {
    auto fail = [&](std::string why) -> std::optional<KdeCoreRecord> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
    const auto version = j.find("schema_version");
    if (version == j.end() || !version->is_number_integer()) return fail("missing schema_version");
    const int v = version->get<int>();
    if (v != kKdeCoreSchemaVersion) return fail("unsupported schema_version " + std::to_string(v));
    try {
        KdeCoreRecord r;
        r.schemaVersion = v;
        r.provisional = j.value("provisional", true);
        r.source = j.value("source", std::string("live-buffer"));
        r.coreFraction = j.value("core_fraction", 0.9);
        const auto level = j.find("level");
        r.level = (level != j.end() && level->is_number()) ? level->get<double>() : std::nan("");
        r.cellCount = j.value("cell_count", std::uint64_t{0});
        r.populationCount = j.value("population_count", std::uint64_t{0});
        r.excludedPoints = j.value("excluded_points", std::uint64_t{0});
        r.bandwidthRule = j.value("bandwidth_rule", std::string("silverman"));
        r.bandwidthFactor = j.value("bandwidth_factor", 1.0);
        r.bandwidthX = j.value("bandwidth_x_um2", 0.0);
        r.bandwidthY = j.value("bandwidth_y", 0.0);
        r.pixelToMicron = j.value("pixel_to_micron_factor", 0.0);
        if (const auto ax = j.find("axis_range"); ax != j.end() && ax->is_object()) {
            r.x0 = ax->value("x0", 0.0);
            r.x1 = ax->value("x1", 0.0);
            r.y0 = ax->value("y0", 0.0);
            r.y1 = ax->value("y1", 0.0);
        }
        if (const auto g = j.find("grid"); g != j.end() && g->is_object()) {
            r.gridNx = g->value("nx", 0);
            r.gridNy = g->value("ny", 0);
        }
        r.computedAtNs = j.value("computed_at_ns", std::uint64_t{0});
        const auto loops = j.find("contours");
        if (loops == j.end() || !loops->is_array()) return fail("missing contours array");
        for (const auto& loop : *loops) {
            if (!loop.is_array()) return fail("contour is not an array");
            Contour c;
            c.reserve(loop.size());
            for (const auto& p : loop) {
                if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
                    return fail("contour vertex is not [x, y]");
                c.push_back({p[0].get<double>(), p[1].get<double>()});
            }
            if (c.size() >= 3) r.contours.push_back(std::move(c));
        }
        return r;
    } catch (const nlohmann::json::exception& e) {
        return fail(std::string("malformed member: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Full-run (authoritative) core record
// ---------------------------------------------------------------------------

inline constexpr std::size_t kFullRunMaxPoints = 5000;
inline constexpr std::uint32_t kFullRunSampleSeed = 20260924u;

// Core contour of a recorded population (points in µm² / deformability,
// valid cells only). Non-finite points are excluded and counted. Above
// `maxPoints` a uniform random subsample with a fixed seed is used (the
// O(n²) kernel), so the same file always yields the same record;
// `populationCount` is the size actually estimated. Bandwidth: Silverman,
// factor 1. The grid spans the finite points' range padded by 10% (the
// Review scatter's own axis rule). `computedAtNs` is left 0 for the caller.
inline KdeCoreRecord computeFullRunCoreRecord(const std::vector<DensityPoint>& points,
                                              double fraction, double pixelToMicron,
                                              std::size_t maxPoints = kFullRunMaxPoints,
                                              std::uint32_t seed = kFullRunSampleSeed,
                                              int gridNx = 128, int gridNy = 64) {
    KdeCoreRecord r;
    r.provisional = false;
    r.source = "full-run";
    r.coreFraction = fraction;
    r.bandwidthRule = "silverman";
    r.bandwidthFactor = 1.0;
    r.pixelToMicron = pixelToMicron;
    r.gridNx = gridNx;
    r.gridNy = gridNy;
    std::vector<DensityPoint> finite;
    finite.reserve(points.size());
    for (const auto& p : points) {
        if (isFinitePoint(p))
            finite.push_back(p);
        else
            ++r.excludedPoints;
    }
    if (maxPoints > 0 && finite.size() > maxPoints) {
        std::mt19937 rng(seed);
        for (std::size_t i = 0; i < maxPoints; ++i) { // partial Fisher-Yates
            std::uniform_int_distribution<std::size_t> pick(i, finite.size() - 1);
            std::swap(finite[i], finite[pick(rng)]);
        }
        finite.resize(maxPoints);
    }
    r.populationCount = finite.size();
    r.level = std::nan("");
    if (finite.size() < 3) return r;
    double x0 = finite.front().x, x1 = x0, y0 = finite.front().y, y1 = y0;
    for (const auto& p : finite) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    const double padX = x1 > x0 ? 0.1 * (x1 - x0) : 1.0;
    const double padY = y1 > y0 ? 0.1 * (y1 - y0) : 0.01;
    r.x0 = x0 - padX;
    r.x1 = x1 + padX;
    r.y0 = y0 - padY;
    r.y1 = y1 + padY;
    const DensityBandwidth bw = silvermanBandwidth(finite, 1.0);
    r.bandwidthX = bw.x;
    r.bandwidthY = bw.y;
    std::vector<double> density = rawKdeAtPoints(finite, bw);
    const double rawMax = normaliseByMaximum(density);
    r.level = coreLevel(density, fraction);
    if (!std::isfinite(r.level)) return r;
    for (double d : density)
        if (d >= r.level) ++r.cellCount;
    const DensityGrid grid = gaussianKdeGrid(finite, bw, rawMax, r.x0, r.x1,
                                             r.y0, r.y1, gridNx, gridNy);
    r.contours = isoContours(grid, r.level);
    return r;
}

} // namespace backend::monitoring
