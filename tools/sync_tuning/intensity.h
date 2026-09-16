#pragma once

#include "backend/playback/FrameStore.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mib::sync {
struct Region {
    int x, y, width, height;
};

inline Region orientRegion(Region region, uint64_t width, uint64_t height, bool flipX, bool flipY) {
    if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
        throw std::runtime_error("Frame geometry exceeds coordinate range");
    if (flipX) region.x = static_cast<int>(width) - region.x - region.width;
    if (flipY) region.y = static_cast<int>(height) - region.y - region.height;
    return region;
}

struct Intensity {
    double mean = 0, spatialSd = 0, clippedFraction = 0, darkFraction = 0;
    std::array<double, 3> bands{};
};

// Coordinates are local to the delivered image. Ignore line padding and sample
// the same sensor pixels in Overview and the experiment crop.
inline Intensity measure(const backend::playback::Frame& f, Region r) {
    if (r.x < 0 || r.y < 0 || r.width < 4 || r.height < 6 ||
        static_cast<uint64_t>(r.x) + r.width > f.width ||
        static_cast<uint64_t>(r.y) + r.height > f.height || f.pixelFormat != 0x01080001 ||
        f.linePitch < f.width || f.height > std::numeric_limits<size_t>::max() / f.linePitch ||
        f.data.size() < f.linePitch * f.height)
        throw std::runtime_error("Invalid MONO8 frame or measurement region");
    double sum = 0, square = 0;
    size_t count = 0, clipped = 0, dark = 0;
    std::array<size_t, 3> bandCount{};
    Intensity result;
    for (int y = 0; y < r.height; y += 2) {
        for (int x = 0; x < r.width; x += 4) {
            const double value = f.data[(r.y + y) * f.linePitch + r.x + x];
            sum += value;
            square += value * value;
            ++count;
            clipped += value >= 250;
            dark += value <= 10;
            const auto band = std::min(2, y * 3 / r.height);
            result.bands[band] += value;
            ++bandCount[band];
        }
    }
    result.mean = sum / count;
    result.spatialSd = std::sqrt(std::max(0.0, square / count - result.mean * result.mean));
    result.clippedFraction = static_cast<double>(clipped) / count;
    result.darkFraction = static_cast<double>(dark) / count;
    for (size_t b = 0; b < 3; ++b)
        result.bands[b] /= bandCount[b];
    return result;
}
} // namespace mib::sync
