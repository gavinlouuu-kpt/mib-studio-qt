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
//
// One probe is one sample, and a healthy NVMe gave 15.2 MB/s once among eight
// readings of 469-700 MB/s (the disk briefly busy, a scanner on the new file;
// #841), close to the 12.6 MB/s floor of 1216x208 at 5000 fps. So a slow sample
// is measured again (measureBestWriteRate): at most kMaxProbes probes, the best
// kept, and the coordinator does not cache a result that still fails the floor.
// Time bound: a healthy destination takes one probe (about 0.1 s). Only a
// suspect result (below kSuspectBelowFloorMultiple x the floor, or one that ran
// into kMaxSeconds) is repeated, and no new probe starts once the probes have
// spent kMaxTotalSeconds (kMaxProbes x kMaxSeconds = 2.25 s) of timed writing,
// so the worst case is about 3 x (0.75 s + the chunk in flight), roughly 3 s.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace backend::app {

struct DestinationWriteRate {
    static constexpr uint64_t kMaxBytes = 32ULL * 1024 * 1024;
    static constexpr double kMaxSeconds = 0.75;
    static constexpr uint64_t kChunkBytes = 4ULL * 1024 * 1024;
    // #841: probes per measurement, the margin over the floor below which a
    // sample is not trusted, and the cap on the time the repeats may spend.
    static constexpr int kMaxProbes = 3;
    static constexpr double kSuspectBelowFloorMultiple = 4.0;
    static constexpr double kMaxTotalSeconds = kMaxProbes * kMaxSeconds;
};

struct WriteRateMeasurement {
    bool ok{false};
    double bytesPerSecond{0.0};
    uint64_t bytes{0};
    double seconds{0.0};
    int probes{1};         // probes taken for this result (the best of them is reported)
    std::string directory; // where it was measured
    std::string note;      // "32 MiB of 505856-byte frames in 0.21 s at <dir>", or why it failed
};

// `outputPath` is the destination file (UTF-8); `bytesPerFrame` the saved
// frame size (at least 1). Never throws. One probe.
WriteRateMeasurement measureDestinationWriteRate(const std::string& outputPath, uint64_t bytesPerFrame);

// A successful measurement that cannot carry the floor stream (the storage.throughput
// gate's Fail). `floorBytesPerSecond` 0 = unknown: nothing fails.
bool writeRateFailsFloor(const WriteRateMeasurement& m, double floorBytesPerSecond);

// A successful measurement not to be trusted on its own: below
// kSuspectBelowFloorMultiple x the floor (when the floor is known), or one that ran
// into kMaxSeconds (a destination that slow, or one that stalled mid-probe).
bool writeRateIsSuspect(const WriteRateMeasurement& m, double floorBytesPerSecond);

// Runs `probe` (one measureDestinationWriteRate, or a fake) and, while the best result so far
// is suspect, again: up to kMaxProbes probes in all and kMaxTotalSeconds of timed writing,
// keeping the highest rate. A failed first probe is returned as it is (nothing to repeat);
// a failed repeat keeps the best earlier result. The result's `probes` says how many were
// taken and its note lists their rates.
WriteRateMeasurement measureBestWriteRate(const std::function<WriteRateMeasurement()>& probe,
                                          double floorBytesPerSecond);

// The directory a destination's rate is measured in and cached by.
std::string writeRateDirectory(const std::string& outputPath);

} // namespace backend::app
