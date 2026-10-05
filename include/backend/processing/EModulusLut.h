#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace backend {

/// Look-up table for Young's modulus (elastic modulus) from (area_um, deformability).
/// Loads scattered LUT data and builds a regular grid for fast bilinear interpolation.
/// Thread-safe after loading (read-only queries).
class EModulusLut {
public:
    EModulusLut() = default;

    /// Load LUT from a tab-separated file with columns: area_um, deform, emodulus.
    /// Returns true on success. Tries multiple path candidates relative to the executable.
    bool loadFromFile(const std::string& basePath);

    /// Whether a LUT has been successfully loaded.
    bool isLoaded() const { return loaded_; }

    /// Look up Young's modulus (kPa) for given area (µm²) and deformability.
    /// Returns NaN if the query point is outside the LUT coverage area.
    double lookup(double area_um, double deformability) const;

    // Load a ready grid (row-major [deformability][area], NaN = no value), e.g.
    // the PZ7035 profile table. Axis i covers min + k * step, k = 0..bins-1.
    // Returns false (and stays unloaded) on a size mismatch, fewer than two bins
    // per axis, or a non-positive step.
    bool loadGrid(double areaMin, double areaStep, size_t areaBins, double deformMin,
                  double deformStep, size_t deformBins, std::vector<double> grid);

private:
    struct LutPoint {
        double area_um;
        double deform;
        double emodulus;
    };

    bool parseFile(const std::string& path, std::vector<LutPoint>& points) const;
    void buildGrid(const std::vector<LutPoint>& points);
    static std::string resolvePath(const std::string& basePath);

    // Regular grid for bilinear interpolation
    std::vector<double> grid_; // row-major: grid_[deformIdx * numAreaBins_ + areaIdx]
    double areaMin_{0.0}, areaMax_{0.0};
    double deformMin_{0.0}, deformMax_{0.0};
    size_t numAreaBins_{0};
    size_t numDeformBins_{0};
    bool loaded_{false};

    static constexpr size_t GRID_RESOLUTION = 200; // bins per axis
};

} // namespace backend
