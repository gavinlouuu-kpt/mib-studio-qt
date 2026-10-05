#include "backend/processing/EModulusLut.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <spdlog/spdlog.h>

namespace backend {

std::string EModulusLut::resolvePath(const std::string& basePath) {
    namespace fs = std::filesystem;

    // Try the path as-is
    if (fs::exists(basePath)) return basePath;

    // Try paths relative to current working directory
    std::vector<std::string> candidates = {
        basePath,
        "../" + basePath,
        "../../" + basePath,
        "resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt",
        "../resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt",
        "../../resources/isoelastic_curve/scaled_isoelastic_data_LUT_6.16-4.24.txt",
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return fs::absolute(candidate).string();
    }

    return basePath; // Return original if nothing found
}

bool EModulusLut::parseFile(const std::string& path, std::vector<LutPoint>& points) const {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        double area_um, deform, emodulus;
        char tab1, tab2;

        // Parse tab-separated values
        if (!(iss >> area_um)) continue;
        if (!iss.get(tab1) || tab1 != '\t') continue;
        if (!(iss >> deform)) continue;
        if (!iss.get(tab2) || tab2 != '\t') continue;
        if (!(iss >> emodulus)) continue;

        points.push_back({area_um, deform, emodulus});
    }

    return !points.empty();
}

void EModulusLut::buildGrid(const std::vector<LutPoint>& points) {
    // Determine data bounds
    areaMin_ = std::numeric_limits<double>::max();
    areaMax_ = std::numeric_limits<double>::lowest();
    deformMin_ = std::numeric_limits<double>::max();
    deformMax_ = std::numeric_limits<double>::lowest();

    for (const auto& p : points) {
        areaMin_ = std::min(areaMin_, p.area_um);
        areaMax_ = std::max(areaMax_, p.area_um);
        deformMin_ = std::min(deformMin_, p.deform);
        deformMax_ = std::max(deformMax_, p.deform);
    }

    numAreaBins_ = GRID_RESOLUTION;
    numDeformBins_ = GRID_RESOLUTION;
    grid_.resize(numAreaBins_ * numDeformBins_, std::numeric_limits<double>::quiet_NaN());

    double areaStep = (areaMax_ - areaMin_) / static_cast<double>(numAreaBins_ - 1);
    double deformStep = (deformMax_ - deformMin_) / static_cast<double>(numDeformBins_ - 1);

    // For each grid cell, use inverse-distance-weighted (IDW) interpolation
    // with the k nearest LUT points. Normalize coordinates so both axes
    // contribute equally to distance.
    constexpr size_t K = 4;
    double areaRange = areaMax_ - areaMin_;
    double deformRange = deformMax_ - deformMin_;
    if (areaRange < 1e-12) areaRange = 1.0;
    if (deformRange < 1e-12) deformRange = 1.0;

    for (size_t di = 0; di < numDeformBins_; ++di) {
        double queryDeform = deformMin_ + di * deformStep;
        for (size_t ai = 0; ai < numAreaBins_; ++ai) {
            double queryArea = areaMin_ + ai * areaStep;

            // Find k nearest neighbors (simple linear scan - only done once at init)
            struct Neighbor {
                double dist2;
                double emodulus;
            };
            std::vector<Neighbor> nearest(K, {std::numeric_limits<double>::max(), 0.0});

            for (const auto& p : points) {
                double da = (p.area_um - queryArea) / areaRange;
                double dd = (p.deform - queryDeform) / deformRange;
                double dist2 = da * da + dd * dd;

                // Insert into sorted nearest list if closer
                if (dist2 < nearest[K - 1].dist2) {
                    nearest[K - 1] = {dist2, p.emodulus};
                    // Bubble sort the last element into place
                    for (size_t i = K - 1; i > 0 && nearest[i].dist2 < nearest[i - 1].dist2; --i) {
                        std::swap(nearest[i], nearest[i - 1]);
                    }
                }
            }

            // IDW interpolation
            double weightSum = 0.0;
            double valueSum = 0.0;
            bool exactMatch = false;

            for (size_t i = 0; i < K; ++i) {
                if (nearest[i].dist2 >= std::numeric_limits<double>::max()) break;
                if (nearest[i].dist2 < 1e-20) {
                    // Exact match
                    grid_[di * numAreaBins_ + ai] = nearest[i].emodulus;
                    exactMatch = true;
                    break;
                }
                double w = 1.0 / nearest[i].dist2;
                weightSum += w;
                valueSum += w * nearest[i].emodulus;
            }

            if (!exactMatch && weightSum > 0.0) {
                grid_[di * numAreaBins_ + ai] = valueSum / weightSum;
            }
        }
    }
}

bool EModulusLut::loadFromFile(const std::string& basePath) {
    std::string resolvedPath = resolvePath(basePath);

    std::vector<LutPoint> points;
    if (!parseFile(resolvedPath, points)) {
        SPDLOG_WARN("EModulusLut: Failed to load LUT from {}", resolvedPath);
        return false;
    }

    // Reject degenerate files (all-identical area or deform values): the
    // grid step would be zero and lookup()'s index math would divide 0/0.
    double aMin = std::numeric_limits<double>::max(), aMax = std::numeric_limits<double>::lowest();
    double dMin = std::numeric_limits<double>::max(), dMax = std::numeric_limits<double>::lowest();
    for (const auto& p : points) {
        aMin = std::min(aMin, p.area_um);
        aMax = std::max(aMax, p.area_um);
        dMin = std::min(dMin, p.deform);
        dMax = std::max(dMax, p.deform);
    }
    if (!(aMax > aMin) || !(dMax > dMin)) {
        SPDLOG_WARN("EModulusLut: rejected degenerate LUT {} ({} points, "
                    "area=[{}, {}], deform=[{}, {}]): axis range is zero",
                    resolvedPath, points.size(), aMin, aMax, dMin, dMax);
        return false;
    }

    buildGrid(points);
    loaded_ = true;

    SPDLOG_INFO("EModulusLut: Loaded {} data points from {}, grid {}x{}, "
                "area=[{:.1f}, {:.1f}] um², deform=[{:.4f}, {:.4f}]",
                points.size(), resolvedPath, numAreaBins_, numDeformBins_,
                areaMin_, areaMax_, deformMin_, deformMax_);
    return true;
}

bool EModulusLut::loadGrid(double areaMin, double areaStep, size_t areaBins, double deformMin,
                           double deformStep, size_t deformBins, std::vector<double> grid) {
    loaded_ = false;
    if (areaBins < 2 || deformBins < 2 || grid.size() != areaBins * deformBins ||
        !(areaStep > 0.0) || !(deformStep > 0.0)) {
        return false;
    }
    grid_ = std::move(grid);
    numAreaBins_ = areaBins;
    numDeformBins_ = deformBins;
    areaMin_ = areaMin;
    areaMax_ = areaMin + areaStep * static_cast<double>(areaBins - 1);
    deformMin_ = deformMin;
    deformMax_ = deformMin + deformStep * static_cast<double>(deformBins - 1);
    loaded_ = true;
    return true;
}

double EModulusLut::lookup(double area_um, double deformability) const {
    if (!loaded_) return std::numeric_limits<double>::quiet_NaN();

    // Check bounds - return NaN if outside LUT range
    if (area_um < areaMin_ || area_um > areaMax_ ||
        deformability < deformMin_ || deformability > deformMax_) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double areaStep = (areaMax_ - areaMin_) / static_cast<double>(numAreaBins_ - 1);
    double deformStep = (deformMax_ - deformMin_) / static_cast<double>(numDeformBins_ - 1);

    // Continuous grid coordinates
    double aIdx = (area_um - areaMin_) / areaStep;
    double dIdx = (deformability - deformMin_) / deformStep;

    // Defense in depth: a zero step (degenerate LUT is rejected at load, but
    // keep the invariant local) yields NaN/inf here, and casting a NaN to
    // size_t is UB that indexes grid_ far out of bounds.
    if (!std::isfinite(aIdx) || !std::isfinite(dIdx)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Integer grid indices for bilinear interpolation
    size_t ai0 = std::min(static_cast<size_t>(std::floor(aIdx)), numAreaBins_ - 1);
    size_t di0 = std::min(static_cast<size_t>(std::floor(dIdx)), numDeformBins_ - 1);
    size_t ai1 = std::min(ai0 + 1, numAreaBins_ - 1);
    size_t di1 = std::min(di0 + 1, numDeformBins_ - 1);

    double fa = aIdx - ai0; // fractional part [0, 1]
    double fd = dIdx - di0;

    // Bilinear interpolation
    double v00 = grid_[di0 * numAreaBins_ + ai0];
    double v10 = grid_[di0 * numAreaBins_ + ai1];
    double v01 = grid_[di1 * numAreaBins_ + ai0];
    double v11 = grid_[di1 * numAreaBins_ + ai1];

    // If any corner is NaN, return NaN
    if (std::isnan(v00) || std::isnan(v10) || std::isnan(v01) || std::isnan(v11)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double v0 = v00 * (1.0 - fa) + v10 * fa;
    double v1 = v01 * (1.0 - fa) + v11 * fa;
    return v0 * (1.0 - fd) + v1 * fd;
}

} // namespace backend
