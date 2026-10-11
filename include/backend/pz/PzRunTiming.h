#pragma once

// The sensor timing of the PZ7035 Run mode at an adjustable frame rate (bridge ABI 38).
//
// Only the frame rate changes. Run keeps the qualified line period HMAX 58 (the producer's PzHmax, forced so that it never falls back to 116
// below ~2.66 kfps) and the qualified exposure of 150 µs (192 lines) at every rate. The producer computes VMAX = floor(clock / (HMAX * fps)) and
// SHS = VMAX - exposure lines (pz7035-imx426 gentl/pz7035_gentl.c mode_from_registers), which this mirrors: the exposure occupies the last 150 µs of
// the frame, [frame period - 150 µs, frame period], relative to XVS. At 5000 fps that is VMAX 256, SHS 64: the timing every earlier Run used.
//
// The LED strobe is referenced to XVS (frame start): delay 7 µs, width 60 µs in Run. It overlaps the exposure only while the exposure starts
// before the strobe ends, i.e. up to a frame period of about 217 µs (above ~4.6 kfps). Below that the frames are unlit: strobeCheck() says so and
// the readiness gate and the UI warn. Nothing here changes the LED; whether a lower rate may move the exposure or the strobe is Gavin's decision.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace backend::pz {

inline constexpr double kSensorClockHz = 74.25e6;
inline constexpr int kRunHmax = 58;
inline constexpr double kRunFpsDefault = 5000.0;
inline constexpr double kRunFpsMin = 100.0;
inline constexpr double kRunFpsMax = 5000.0;       // the PL profile's cap (dual U-Net engines, one frame per ~200 µs)
inline constexpr double kRunFpsQualified = 5000.0; // the only rate qualified on the board
inline constexpr double kRunExposureDefaultUs = 150.0;

struct RunTiming {
    double fps{kRunFpsDefault};   // the configured (nominal) rate
    int hmax{kRunHmax};
    int vmax{256};
    int shs{64};                  // vmax - lines
    int lines{192};               // exposure lines
    double exposureUs{kRunExposureDefaultUs};
    double actualFps{kRunFpsDefault}; // clock / (hmax * vmax): the rate the sensor really runs
};

// Empty when `fps` is a Run rate: a whole number of frames per second from 100 to 5000.
inline std::string checkRunFps(double fps) {
    if (!std::isfinite(fps) || fps != std::floor(fps) || fps < kRunFpsMin || fps > kRunFpsMax)
        return "the Run rate must be a whole number of frames per second from 100 to 5000";
    return {};
}

// The timing for a valid rate (checkRunFps is empty): the exposure stays 150 µs, SHS follows the frame length.
inline RunTiming runTiming(double fps) {
    RunTiming t;
    t.fps = fps;
    t.vmax = static_cast<int>(std::floor(kSensorClockHz / (static_cast<double>(kRunHmax) * fps)));
    t.lines = static_cast<int>(std::lround(kRunExposureDefaultUs * 1e-6 * kSensorClockHz / kRunHmax)); // 192
    t.shs = t.vmax - t.lines;
    t.exposureUs = kRunExposureDefaultUs;
    t.actualFps = kSensorClockHz / (static_cast<double>(kRunHmax) * t.vmax);
    return t;
}

// Where the exposure and the LED strobe fall in a frame, relative to XVS (frame start), in µs.
struct StrobeCheck {
    double exposureStartUs{0}, exposureEndUs{0};
    double strobeStartUs{0}, strobeEndUs{0};
    double overlapUs{0};
    bool overlaps{false};
};

inline StrobeCheck strobeCheck(const RunTiming& t, double strobeDelayUs, double strobeWidthUs) {
    StrobeCheck c;
    const double lineUs = static_cast<double>(t.hmax) / kSensorClockHz * 1e6;
    c.exposureStartUs = t.shs * lineUs;
    c.exposureEndUs = t.vmax * lineUs;
    c.strobeStartUs = strobeDelayUs;
    c.strobeEndUs = strobeDelayUs + strobeWidthUs;
    c.overlapUs = std::fmax(0.0, std::fmin(c.exposureEndUs, c.strobeEndUs) - std::fmax(c.exposureStartUs, c.strobeStartUs));
    c.overlaps = c.overlapUs > 0.0;
    return c;
}

// The warning for a rate whose strobe misses the exposure; empty when they overlap.
inline std::string strobeWarning(const StrobeCheck& c) {
    if (c.overlaps) return {};
    char buf[200];
    std::snprintf(buf, sizeof buf, "strobe outside the exposure at this rate: frames will be unlit (exposure %.0f to %.0f µs after XVS, strobe %.0f to %.0f µs)",
                  c.exposureStartUs, c.exposureEndUs, c.strobeStartUs, c.strobeEndUs);
    return buf;
}

} // namespace backend::pz
