// Experiment buffer sizing from the camera window and the frame rate (#833).
//
// The experiment buffer holds the frames accepted for saving between two
// flushes. Its size used to be a fixed 512 MiB byte cap plus a frame cap from
// the flush interval, so a camera window larger than 512x96 hit the byte cap
// within a fraction of a second at 5000 fps (#828). This header is the one
// place the size is decided: planExperimentBuffer() is pure (no service, no
// clock, no system call), and every constant it uses is in
// ExperimentBufferSizing below.
//
// Rule (frames F = flush interval, b = bytes per saved frame, r = requested
// fps, A = available physical memory):
//  - b = window W x H x images, one byte per pixel (Mono8 original + mask; a
//    multi-image series saves count originals + 1 mask);
//  - floor  = kFlushFloorMultiple x F frames (one batch being filled while the
//    previous one is handed to the writer); this is also the minimum hold;
//  - target = kTargetBacklogSeconds x r frames when r is known, else the frame
//    cap the flush interval gave before (#833 keeps it as the unknown-rate
//    default); never below the floor;
//  - the save path keeps alive the buffer plus the HDF5 write queue (up to
//    kWriteQueueBatches batches of one flush interval each, 3 queued + 1 being
//    written), so its peak is b x (N + kWriteQueueBatches x min(F, N / 2));
//  - RAM ceiling: that peak may use at most kRamCeilingFraction of A. A is read
//    while the camera runs (Start requires it), so the FrameStore ring and the
//    loaded app are already outside it; the rest of A is left for the
//    operating system's write cache and other processes;
//  - frames N = target, cut to the ceiling when it does not fit; bytes = N x b,
//    so the frame cap and the byte cap always agree and neither binds first;
//  - an explicit experiment_buffer_max_mb (override) replaces the target. It is
//    still cut to the ceiling, and the minimum-hold check still applies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace backend::processing {

struct ExperimentBufferSizing {
    // Seconds of backlog the buffer should hold at the requested rate if every
    // frame were saved (the default target).
    static constexpr double kTargetBacklogSeconds = 2.0;
    // Below this many seconds (every frame saved) the readiness gate warns.
    static constexpr double kWarnBacklogSeconds = 1.0;
    // The floor (and the minimum hold) is this many flush intervals.
    static constexpr size_t kFlushFloorMultiple = 2;
    // Share of available physical memory the save path (buffer + write
    // queue) may use. 0.75 follows BufferSaveDialog's ring guard (#370).
    static constexpr double kRamCeilingFraction = 0.75;
    // Batches the HDF5 write queue holds outside the buffer: the queue's 3
    // slots plus the batch the writer is writing (ProcessingService).
    static constexpr size_t kWriteQueueBatches = 4;
    // Images per saved frame without a series: the original and its mask.
    static constexpr uint32_t kImagesWithoutSeries = 2;
    // Frame cap for an unknown rate (the pre-#833 rule): 4 x the flush
    // interval, at least 1000 and at most 5000 frames, never below the interval.
    static constexpr size_t kUnknownRateIntervalMultiple = 4;
    static constexpr size_t kUnknownRateMinFrames = 1000;
    static constexpr size_t kUnknownRateMaxFrames = 5000;
    // Byte cap while no camera window is known yet (no frame, no camera
    // report): the pre-#833 default.
    static constexpr uint64_t kFallbackBytes = 512ULL * 1024 * 1024;
};

// Bytes one saved frame takes: width x height x images (one byte per pixel).
// `multiImageEnabled` with `multiImageCount` n saves n originals + 1 mask.
// 0 when the window is unknown or the product overflows.
uint64_t savedBytesPerFrame(uint64_t width, uint64_t height, bool multiImageEnabled, int multiImageCount);
uint32_t savedImagesPerFrame(bool multiImageEnabled, int multiImageCount);

// The pre-#833 frame cap for a flush interval (kept for an unknown rate).
size_t unknownRateFrameCap(size_t flushInterval);

struct ExperimentBufferSizingInputs {
    uint64_t bytesPerFrame{0};     // savedBytesPerFrame(); 0 = window unknown
    double requestedFps{0.0};      // 0 = unknown
    size_t flushInterval{1};       // buffer_threshold (frames)
    uint64_t availableRamBytes{0}; // physical memory available now; 0 = unknown
    uint64_t overrideBytes{0};     // experiment_buffer_max_mb in bytes; 0 = automatic
};

enum class ExperimentBufferSource {
    Computed, // from the window and the rate
    Override, // experiment_buffer_max_mb
    Fallback, // no window known yet: the pre-#833 caps
};
const char* toString(ExperimentBufferSource source);

struct ExperimentBufferPlan {
    ExperimentBufferSource source{ExperimentBufferSource::Fallback};
    uint64_t bytesPerFrame{0};
    double requestedFps{0.0};
    size_t flushInterval{1};
    size_t floorFrames{0};    // kFlushFloorMultiple x flush interval (the minimum hold)
    size_t targetFrames{0};   // what the rule (or the override) asked for
    size_t maxFrames{0};      // the frame cap applied
    uint64_t maxBytes{0};     // the byte cap applied (maxFrames x bytesPerFrame)
    uint64_t peakBytes{0};    // buffer + write queue at maxFrames
    uint64_t minimumPeakBytes{0}; // the same at the floor
    uint64_t ramCeilingBytes{0};  // kRamCeilingFraction x available; 0 = unknown
    bool ramKnown{false};
    bool minimumFitsRam{true}; // the floor's peak fits the ceiling (true when RAM is unknown)
    bool clampedByRam{false};  // the target (or override) was cut to the ceiling
    double seconds{0.0};       // maxFrames / requestedFps if every frame were saved; 0 = rate unknown

    // "4000 frames, 1.9 GiB, 0.80 s at 5000 fps"
    std::string summary() const;
};

ExperimentBufferPlan planExperimentBuffer(const ExperimentBufferSizingInputs& in);

// Bytes as text with a binary unit ("512 MiB", "1.9 GiB").
std::string formatBufferBytes(uint64_t bytes);

} // namespace backend::processing
