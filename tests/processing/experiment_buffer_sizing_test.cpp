// experiment_buffer_sizing_test (#833): the experiment buffer is sized from
// the camera window and the requested rate by one pure function
// (processing/ExperimentBufferSizing.h). Invariants:
//  - bytes per saved frame = W x H x images (2 without a series);
//  - target = 2 s at the requested rate, never below 2 x the flush interval;
//  - the frame cap and the byte cap agree (bytes = frames x bytes per frame);
//  - the save-path peak (buffer + 4 write-queue batches) stays under 75% of the
//    available memory: the target is cut to it, and a floor that does not fit
//    is reported (the readiness gate then fails);
//  - an explicit override replaces the target and is still cut to the ceiling;
//  - an unknown rate keeps the pre-#833 frame cap; an unknown window the
//    pre-#833 caps.
// Figures are the rig's (#828/#833): 1216x208 at 5000 fps, buffer_threshold
// 1000; the 512 MiB override reproduces the 1061-frame cap of the crash logs.

#include "backend/processing/ExperimentBufferSizing.h"

#include "support/assert.h"

#include <cmath>
#include <cstdint>

using backend::processing::ExperimentBufferPlan;
using backend::processing::ExperimentBufferSizing;
using backend::processing::ExperimentBufferSizingInputs;
using backend::processing::ExperimentBufferSource;
using backend::processing::planExperimentBuffer;
using backend::processing::savedBytesPerFrame;
using backend::processing::unknownRateFrameCap;

namespace {
constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
constexpr uint64_t MiB = 1024ULL * 1024;

ExperimentBufferPlan plan(uint64_t w, uint64_t h, double fps, uint64_t availableRam, uint64_t overrideBytes = 0,
                          size_t flushInterval = 1000)
{
    ExperimentBufferSizingInputs in;
    in.bytesPerFrame = savedBytesPerFrame(w, h, false, 1);
    in.requestedFps = fps;
    in.flushInterval = flushInterval;
    in.availableRamBytes = availableRam;
    in.overrideBytes = overrideBytes;
    return planExperimentBuffer(in);
}

bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }
} // namespace

int main()
{
    // ---- bytes per saved frame ----------------------------------------------------------
    MIB_EXPECT(savedBytesPerFrame(512, 96, false, 1) == 98304, "512x96: original + mask");
    MIB_EXPECT(savedBytesPerFrame(1216, 208, false, 1) == 505856, "1216x208: the rig's 505,856 B/frame");
    MIB_EXPECT(savedBytesPerFrame(1920, 1080, false, 1) == 4147200, "1920x1080");
    MIB_EXPECT(savedBytesPerFrame(1216, 208, true, 3) == 1216ull * 208 * 4, "a series of 3 saves 3 originals + 1 mask");
    MIB_EXPECT(savedBytesPerFrame(0, 208, false, 1) == 0, "unknown window");

    // ---- the pre-#833 frame cap is kept for an unknown rate --------------------------------
    MIB_EXPECT(unknownRateFrameCap(1) == 1000 && unknownRateFrameCap(1000) == 4000 &&
                   unknownRateFrameCap(2000) == 5000 && unknownRateFrameCap(10000) == 10000,
               "4 x interval, clamped to 1000..5000, never below the interval");

    // ---- 512x96 at 5000 fps: 2 s fits easily ----------------------------------------------------
    {
        const auto p = plan(512, 96, 5000, 16 * GiB);
        MIB_EXPECT(p.source == ExperimentBufferSource::Computed, "computed");
        MIB_EXPECT(p.floorFrames == 2000 && p.targetFrames == 10000 && p.maxFrames == 10000, "2 s = 10000 frames");
        MIB_EXPECT(p.maxBytes == 10000ull * 98304, "byte cap = frames x bytes per frame");
        MIB_EXPECT(p.peakBytes == 98304ull * 14000, "peak = buffer + 4 batches of 1000 frames");
        MIB_EXPECT(near(p.seconds, 2.0) && !p.clampedByRam && p.minimumFitsRam, "2.00 s, not cut");
    }
    // ---- 1216x208 at 5000 fps (the rig) ---------------------------------------------------------
    {
        const auto p = plan(1216, 208, 5000, 16 * GiB);
        MIB_EXPECT(p.maxFrames == 10000 && p.maxBytes == 10000ull * 505856, "10000 frames, 4.7 GiB");
        MIB_EXPECT(near(p.seconds, 2.0) && !p.clampedByRam && p.minimumFitsRam, "2.00 s on a 16 GiB host");
        MIB_EXPECT(p.ramCeilingBytes == 12 * GiB, "ceiling = 75% of the available memory");
        MIB_EXPECT(p.summary().find("10000 frames") != std::string::npos &&
                       p.summary().find("2.00 s at 5000 fps") != std::string::npos,
                   "summary names frames and seconds: " + p.summary());
    }
    // ---- 1920x1080 at 5000 fps: the floor alone needs 23 GiB with the queue ---------------------
    {
        const auto small = plan(1920, 1080, 5000, 16 * GiB);
        MIB_EXPECT(small.minimumPeakBytes == 4147200ull * 6000, "floor 2000 + 4 batches of 1000");
        MIB_EXPECT(!small.minimumFitsRam, "the minimum hold does not fit 12 GiB");
        MIB_EXPECT(small.clampedByRam && small.peakBytes <= small.ramCeilingBytes, "cut to the ceiling anyway");
        const auto large = plan(1920, 1080, 5000, 64 * GiB);
        MIB_EXPECT(large.minimumFitsRam && large.clampedByRam, "the floor fits 48 GiB, 2 s does not");
        MIB_EXPECT(large.maxFrames == 8427, "largest cap whose peak fits 48 GiB: (48 GiB / 4147200) - 4000");
        MIB_EXPECT(large.peakBytes <= large.ramCeilingBytes &&
                       4147200ull * (large.maxFrames + 1 + 4000) > large.ramCeilingBytes,
                   "one more frame would not fit");
        MIB_EXPECT(near(large.seconds, 8427.0 / 5000.0), "seconds follow the cut cap");
    }
    // ---- a RAM ceiling hit on the rig's window ----------------------------------------------------
    {
        const auto p = plan(1216, 208, 5000, 4 * GiB);
        MIB_EXPECT(p.minimumFitsRam && p.clampedByRam, "floor fits 3 GiB, 2 s does not");
        MIB_EXPECT(p.maxFrames == 2367, "3 GiB / 505856 - 4000");
        MIB_EXPECT(p.seconds < ExperimentBufferSizing::kWarnBacklogSeconds, "less than the warn threshold");
        const auto tiny = plan(1216, 208, 5000, 2 * GiB);
        MIB_EXPECT(!tiny.minimumFitsRam, "1.5 GiB cannot hold the floor");
    }
    // ---- manual override ---------------------------------------------------------------------------
    {
        const auto p = plan(1216, 208, 5000, 16 * GiB, 512 * MiB);
        MIB_EXPECT(p.source == ExperimentBufferSource::Override, "override");
        MIB_EXPECT(p.maxFrames == 1061, "512 MiB holds the rig's 1061 frames");
        MIB_EXPECT(p.maxBytes == 1061ull * 505856, "byte cap agrees with the frame cap");
        MIB_EXPECT(near(p.seconds, 1061.0 / 5000.0) && !p.clampedByRam, "0.21 s, not cut");
        const auto cut = plan(1216, 208, 5000, 2 * GiB, 64 * GiB);
        MIB_EXPECT(cut.clampedByRam && cut.peakBytes <= cut.ramCeilingBytes, "an override is cut to the ceiling");
        MIB_EXPECT(!cut.minimumFitsRam, "the minimum hold check still applies to an override");
        const auto unknownRam = plan(1216, 208, 5000, 0, 64 * GiB);
        MIB_EXPECT(!unknownRam.ramKnown && unknownRam.minimumFitsRam && !unknownRam.clampedByRam,
                   "unknown RAM: no ceiling, nothing reported as not fitting");
    }
    // ---- unknown rate, unknown window ----------------------------------------------------------------
    {
        const auto p = plan(1216, 208, 0, 16 * GiB);
        MIB_EXPECT(p.maxFrames == 4000 && p.maxBytes == 4000ull * 505856 && p.seconds == 0.0,
                   "unknown rate: the pre-#833 4000 frames, with a byte cap that agrees");
        MIB_EXPECT(p.summary().find("frame rate unknown") != std::string::npos, "summary says the rate is unknown");
        ExperimentBufferSizingInputs none;
        none.flushInterval = 1000;
        const auto f = planExperimentBuffer(none);
        MIB_EXPECT(f.source == ExperimentBufferSource::Fallback && f.maxFrames == 4000 &&
                       f.maxBytes == ExperimentBufferSizing::kFallbackBytes,
                   "no window: the pre-#833 caps");
    }
    // ---- the floor follows the flush interval ----------------------------------------------------------
    {
        const auto p = plan(512, 96, 500, 16 * GiB, 0, 5000);
        MIB_EXPECT(p.floorFrames == 10000 && p.targetFrames == 10000, "1000 frames for 2 s is raised to 2 x 5000");
        MIB_EXPECT(near(p.seconds, 20.0), "20 s at 500 fps");
    }
    return mib::test::exitCode();
}
