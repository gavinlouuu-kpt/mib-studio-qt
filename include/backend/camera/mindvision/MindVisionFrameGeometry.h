// Pure, SDK-free validation of MindVision output format and image geometry
// (issue #366). MindVisionCamera must prove, before any SDK conversion call,
// that the destination buffer it allocated is large enough for what the SDK
// will write. These functions are the proof; they use checked arithmetic and
// reject anything the Mono8 pipeline cannot handle.
#pragma once

#include "backend/camera/mindvision/MindVisionSdk.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace backend::camera::mindvision {

enum class GeometryFault {
    None,
    InvalidWidth,        // <= 0 or above the supported maximum
    InvalidHeight,       // <= 0 or above the supported maximum
    SizeOverflow,        // width * height * bytesPerPixel does not fit size_t/int
    UnsupportedFormat,   // effective ISP output is not Mono8
    FrameGeometryMismatch, // incoming frame header differs from the session
    DestinationTooSmall, // allocation smaller than required output bytes
};

inline const char* toString(GeometryFault f)
{
    switch (f) {
    case GeometryFault::None: return "none";
    case GeometryFault::InvalidWidth: return "invalidWidth";
    case GeometryFault::InvalidHeight: return "invalidHeight";
    case GeometryFault::SizeOverflow: return "sizeOverflow";
    case GeometryFault::UnsupportedFormat: return "unsupportedFormat";
    case GeometryFault::FrameGeometryMismatch: return "frameGeometryMismatch";
    case GeometryFault::DestinationTooSmall: return "destinationTooSmall";
    }
    return "unknown";
}

// Largest single dimension accepted. Generous for any MindVision sensor but
// keeps the checked product far below size_t limits on 32-bit hosts too.
constexpr int kMaxDimension = 65535;

// Bytes per pixel for the formats the pipeline may see. Only Mono8 is
// accepted for acquisition; the others exist so a readback can be *named*
// in the failure message instead of reported as "unknown".
inline int bytesPerPixelForMediaType(std::uint32_t mediaType)
{
    // CameraDefine.h: bits 16..23 carry the pixel occupancy in bits
    // (CAMERA_MEDIA_TYPE_OCCUPY8BIT = 0x00080000, 24BIT = 0x00180000, ...).
    const std::uint32_t occupyBits = (mediaType >> 16) & 0xFFu;
    if (occupyBits == 0) return 0;
    return static_cast<int>((occupyBits + 7) / 8);
}

// Immutable description of a validated acquisition session allocation.
struct SessionGeometry {
    int width{0};
    int height{0};
    std::uint32_t mediaType{0};
    int bytesPerPixel{0};
    std::size_t requiredBytes{0}; // width * height * bytesPerPixel (checked)
};

struct GeometryValidation {
    GeometryFault fault{GeometryFault::None};
    std::string message;
    SessionGeometry geometry{};
    bool ok() const { return fault == GeometryFault::None; }
};

// Checked width*height*bpp. Returns false on overflow (result untouched).
inline bool checkedFrameBytes(int width, int height, int bytesPerPixel, std::size_t& out)
{
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0) return false;
    const std::uint64_t w = static_cast<std::uint64_t>(width);
    const std::uint64_t h = static_cast<std::uint64_t>(height);
    const std::uint64_t b = static_cast<std::uint64_t>(bytesPerPixel);
    if (w > (std::numeric_limits<std::uint64_t>::max)() / h) return false;
    const std::uint64_t wh = w * h;
    if (wh > (std::numeric_limits<std::uint64_t>::max)() / b) return false;
    const std::uint64_t total = wh * b;
    // Must be representable as size_t and as the SDK's int-sized allocation
    // argument (CameraAlignMalloc takes int).
    if (total > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) return false;
    if (total > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) return false;
    out = static_cast<std::size_t>(total);
    return true;
}

// Validate the session allocation: the resolution read back from the SDK and
// the *effective* ISP output format (read back, not merely requested).
inline GeometryValidation validateSessionGeometry(int width, int height,
                                                  std::uint32_t effectiveMediaType)
{
    GeometryValidation v;
    if (width <= 0 || width > kMaxDimension) {
        v.fault = GeometryFault::InvalidWidth;
        v.message = "MindVision image width " + std::to_string(width) +
                    " is outside the supported range [1, " +
                    std::to_string(kMaxDimension) + "]";
        return v;
    }
    if (height <= 0 || height > kMaxDimension) {
        v.fault = GeometryFault::InvalidHeight;
        v.message = "MindVision image height " + std::to_string(height) +
                    " is outside the supported range [1, " +
                    std::to_string(kMaxDimension) + "]";
        return v;
    }
    if (effectiveMediaType != kMediaTypeMono8) {
        v.fault = GeometryFault::UnsupportedFormat;
        const int bpp = bytesPerPixelForMediaType(effectiveMediaType);
        v.message = "MindVision ISP output format 0x" + [&] {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", effectiveMediaType);
            return std::string(buf);
        }() + " (" + (bpp > 0 ? std::to_string(bpp) + " byte/px" : "unknown width") +
            ") is not Mono8; acquisition requires a verified Mono8 output";
        return v;
    }
    std::size_t bytes = 0;
    if (!checkedFrameBytes(width, height, 1, bytes)) {
        v.fault = GeometryFault::SizeOverflow;
        v.message = "MindVision frame size " + std::to_string(width) + "x" +
                    std::to_string(height) + " overflows the supported allocation";
        return v;
    }
    v.geometry.width = width;
    v.geometry.height = height;
    v.geometry.mediaType = kMediaTypeMono8;
    v.geometry.bytesPerPixel = 1;
    v.geometry.requiredBytes = bytes;
    return v;
}

// Validate one incoming SDK frame header against the session allocation
// BEFORE conversion. `destinationBytes` is the actual allocation size.
inline GeometryValidation validateIncomingFrame(const SessionGeometry& session,
                                                const SdkFrameInfo& frame,
                                                std::size_t destinationBytes)
{
    GeometryValidation v;
    v.geometry = session;
    if (frame.width != session.width || frame.height != session.height) {
        v.fault = GeometryFault::FrameGeometryMismatch;
        v.message = "MindVision frame geometry " + std::to_string(frame.width) + "x" +
                    std::to_string(frame.height) + " differs from the session allocation " +
                    std::to_string(session.width) + "x" + std::to_string(session.height);
        return v;
    }
    if (frame.width <= 0 || frame.height <= 0) {
        v.fault = frame.width <= 0 ? GeometryFault::InvalidWidth : GeometryFault::InvalidHeight;
        v.message = "MindVision frame header carries a non-positive dimension";
        return v;
    }
    if (destinationBytes < session.requiredBytes) {
        v.fault = GeometryFault::DestinationTooSmall;
        v.message = "MindVision destination buffer (" + std::to_string(destinationBytes) +
                    " bytes) is smaller than the required output (" +
                    std::to_string(session.requiredBytes) + " bytes)";
        return v;
    }
    return v;
}

} // namespace backend::camera::mindvision
