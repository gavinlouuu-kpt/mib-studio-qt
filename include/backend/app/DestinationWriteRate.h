// Sustained write rate of a recording destination (#833, storage.throughput).
//
// A short, bounded measurement beside the destination: a temporary file in the
// destination's directory (the nearest existing one) receives frame-size
// records (original + mask, the bytes a saved frame takes) in chunks of about
// kChunkBytes, each chunk followed by a flush to the device (fsync /
// _commit), so the operating system's write cache does not count. It stops at
// kMaxBytes or kMaxSeconds, whichever comes first, and the file is removed.
// The rate is the bytes written over the time including the flushes. The
// HDF5 writer adds a little per-frame overhead on top, so this is the disk's
// rate, an upper bound for the writer; the coordinator compares it with the
// floor save stream, which is far below it on a working destination.
#pragma once

#include <cstdint>
#include <string>

namespace backend::app {

struct DestinationWriteRate {
    static constexpr uint64_t kMaxBytes = 32ULL * 1024 * 1024;
    static constexpr double kMaxSeconds = 0.75;
    static constexpr uint64_t kChunkBytes = 4ULL * 1024 * 1024;
};

struct WriteRateMeasurement {
    bool ok{false};
    double bytesPerSecond{0.0};
    uint64_t bytes{0};
    double seconds{0.0};
    std::string directory; // where it was measured
    std::string note;      // "32 MiB of 505856-byte frames in 0.21 s at <dir>", or why it failed
};

// `outputPath` is the destination file (UTF-8); `bytesPerFrame` the saved
// frame size (at least 1). Never throws.
WriteRateMeasurement measureDestinationWriteRate(const std::string& outputPath, uint64_t bytesPerFrame);

// The directory a destination's rate is measured in and cached by.
std::string writeRateDirectory(const std::string& outputPath);

} // namespace backend::app
