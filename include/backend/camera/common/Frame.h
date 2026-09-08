#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera::common {

/**
 * Lightweight container describing a single frame acquired from a camera source.
 *
 * This is kept independent from backend playback structures so that camera
 * implementations can normalize metadata (e.g. line pitch vs. width) before the
 * CaptureService publishes frames to the rest of the application.
 */
struct Frame {
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t pixelFormat = 0;  // Use PFNC codes to match Euresys metadata.
    size_t linePitch = 0;      // Bytes per line in the buffer (may exceed width).
    // Source timestamp in the unit/domain declared by the producing camera's
    // ICamera::timestampDescriptor() (issue #368). Never assume nanoseconds.
    uint64_t timestamp = 0;
    // Raw device counter value before the adapter's normalization (0 when the
    // source has no separate native counter). Preserved for audit.
    uint64_t rawDeviceTicks = 0;
    // Host monotonic microseconds (Tools::getTimestamp clock) stamped by
    // CaptureService when grabFrame returns. 0 when the frame never went
    // through the capture loop. Unlike `timestamp` this is comparable across
    // pipeline stages, so it anchors end-to-end latency measurements.
    uint64_t hostTimestampUs = 0;
    std::vector<uint8_t> data; // Raw image payload copied from the source buffer.
};

} // namespace camera::common


