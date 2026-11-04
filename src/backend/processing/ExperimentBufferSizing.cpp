#include "backend/processing/ExperimentBufferSizing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace backend::processing {
namespace {

constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();

uint64_t saturatingMul(uint64_t a, uint64_t b)
{
    if (a == 0 || b == 0) return 0;
    return a > kU64Max / b ? kU64Max : a * b;
}

uint64_t saturatingAdd(uint64_t a, uint64_t b)
{
    return a > kU64Max - b ? kU64Max : a + b;
}

// Bytes the save path keeps alive with a frame cap of `frames`: the buffer and
// the write queue's batches (each one flush interval, or half the buffer when
// the byte watermark flushes first).
uint64_t savePathPeak(uint64_t bytesPerFrame, size_t frames, size_t flushInterval)
{
    const uint64_t batchFrames = std::min<uint64_t>(flushInterval, frames / 2);
    const uint64_t queued = saturatingMul(ExperimentBufferSizing::kWriteQueueBatches, batchFrames);
    return saturatingMul(bytesPerFrame, saturatingAdd(frames, queued));
}

// The largest frame cap in [0, upper] whose save-path peak fits `ceiling`
// (the peak grows with the cap, so a binary search finds it exactly).
size_t largestFittingFrames(uint64_t bytesPerFrame, size_t flushInterval, uint64_t ceiling, size_t upper)
{
    size_t lo = 0, hi = upper;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        if (savePathPeak(bytesPerFrame, mid, flushInterval) <= ceiling)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

// A frame count from seconds at a rate, saturated far below overflow of the
// peak arithmetic.
size_t framesFor(double seconds, double fps)
{
    constexpr double kLimit = static_cast<double>(std::numeric_limits<uint32_t>::max());
    const double frames = std::ceil(seconds * fps);
    if (!(frames > 0)) return 0;
    return frames >= kLimit ? static_cast<size_t>(kLimit) : static_cast<size_t>(frames);
}

} // namespace

uint32_t savedImagesPerFrame(bool multiImageEnabled, int multiImageCount)
{
    if (!multiImageEnabled) return ExperimentBufferSizing::kImagesWithoutSeries;
    return static_cast<uint32_t>(std::max(1, multiImageCount)) + 1;
}

uint64_t savedBytesPerFrame(uint64_t width, uint64_t height, bool multiImageEnabled, int multiImageCount)
{
    if (width == 0 || height == 0) return 0;
    const uint64_t images = savedImagesPerFrame(multiImageEnabled, multiImageCount);
    if (width > kU64Max / height || width * height > kU64Max / images) return 0;
    return width * height * images;
}

size_t unknownRateFrameCap(size_t flushInterval)
{
    using S = ExperimentBufferSizing;
    const size_t interval = std::max<size_t>(1, flushInterval);
    const size_t scaled = interval > std::numeric_limits<size_t>::max() / S::kUnknownRateIntervalMultiple
                              ? std::numeric_limits<size_t>::max()
                              : interval * S::kUnknownRateIntervalMultiple;
    const size_t preferred = std::max(S::kUnknownRateMinFrames, scaled);
    return std::max(interval, std::min(preferred, S::kUnknownRateMaxFrames));
}

const char* toString(ExperimentBufferSource source)
{
    switch (source) {
    case ExperimentBufferSource::Computed: return "computed";
    case ExperimentBufferSource::Override: return "override";
    case ExperimentBufferSource::Fallback: return "fallback";
    }
    return "unknown";
}

ExperimentBufferPlan planExperimentBuffer(const ExperimentBufferSizingInputs& in)
{
    using S = ExperimentBufferSizing;
    ExperimentBufferPlan p;
    p.bytesPerFrame = in.bytesPerFrame;
    p.requestedFps = std::isfinite(in.requestedFps) && in.requestedFps > 0 ? in.requestedFps : 0.0;
    p.flushInterval = std::max<size_t>(1, in.flushInterval);
    p.floorFrames = p.flushInterval > std::numeric_limits<size_t>::max() / S::kFlushFloorMultiple
                        ? std::numeric_limits<size_t>::max()
                        : p.flushInterval * S::kFlushFloorMultiple;
    if (in.availableRamBytes > 0) {
        p.ramKnown = true;
        p.ramCeilingBytes = static_cast<uint64_t>(S::kRamCeilingFraction * static_cast<double>(in.availableRamBytes));
    }

    if (p.bytesPerFrame == 0) {
        // No window known yet (no frame, no camera report): the pre-#833 caps,
        // replaced as soon as the window is known.
        p.source = ExperimentBufferSource::Fallback;
        p.targetFrames = p.maxFrames = unknownRateFrameCap(p.flushInterval);
        p.maxBytes = in.overrideBytes > 0 ? in.overrideBytes : S::kFallbackBytes;
        if (p.requestedFps > 0) p.seconds = static_cast<double>(p.maxFrames) / p.requestedFps;
        return p;
    }

    p.minimumPeakBytes = savePathPeak(p.bytesPerFrame, p.floorFrames, p.flushInterval);
    p.minimumFitsRam = !p.ramKnown || p.minimumPeakBytes <= p.ramCeilingBytes;

    if (in.overrideBytes > 0) {
        p.source = ExperimentBufferSource::Override;
        p.targetFrames = static_cast<size_t>(std::max<uint64_t>(1, in.overrideBytes / p.bytesPerFrame));
    } else {
        p.source = ExperimentBufferSource::Computed;
        const size_t wanted = p.requestedFps > 0 ? framesFor(S::kTargetBacklogSeconds, p.requestedFps)
                                                 : unknownRateFrameCap(p.flushInterval);
        p.targetFrames = std::max(wanted, p.floorFrames);
    }

    size_t frames = p.targetFrames;
    if (p.ramKnown && savePathPeak(p.bytesPerFrame, frames, p.flushInterval) > p.ramCeilingBytes) {
        frames = largestFittingFrames(p.bytesPerFrame, p.flushInterval, p.ramCeilingBytes, frames);
        p.clampedByRam = true;
    }
    p.maxFrames = std::max<size_t>(1, frames);
    p.maxBytes = saturatingMul(p.maxFrames, p.bytesPerFrame);
    p.peakBytes = savePathPeak(p.bytesPerFrame, p.maxFrames, p.flushInterval);
    if (p.requestedFps > 0) p.seconds = static_cast<double>(p.maxFrames) / p.requestedFps;
    return p;
}

std::string formatBufferBytes(uint64_t bytes)
{
    constexpr double kKiB = 1024.0, kMiB = kKiB * 1024.0, kGiB = kMiB * 1024.0;
    const double b = static_cast<double>(bytes);
    char text[48];
    if (b >= kGiB)
        std::snprintf(text, sizeof text, "%.1f GiB", b / kGiB);
    else if (b >= 10 * kMiB)
        std::snprintf(text, sizeof text, "%.0f MiB", b / kMiB);
    else if (b >= kMiB)
        std::snprintf(text, sizeof text, "%.1f MiB", b / kMiB);
    else if (b >= kKiB)
        std::snprintf(text, sizeof text, "%.0f KiB", b / kKiB);
    else
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

std::string ExperimentBufferPlan::summary() const
{
    std::string text = std::to_string(maxFrames) + " frames, " + formatBufferBytes(maxBytes);
    char tail[96];
    if (requestedFps > 0) {
        std::snprintf(tail, sizeof tail, ", %.2f s at %.0f fps", seconds, requestedFps);
        text += tail;
    } else {
        text += " (frame rate unknown)";
    }
    return text;
}

} // namespace backend::processing
