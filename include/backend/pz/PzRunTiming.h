#pragma once

// The sensor timing of the PZ7035 Run mode at an adjustable frame rate (bridge ABI 38).
//
// Run keeps the qualified line period HMAX 58 (the producer's PzHmax, forced so that it never falls back to 116 below ~2.66 kfps) and the
// qualified exposure position SHS 64: the sensor exposes the last (VMAX - SHS) lines of a frame, and the LED strobe is referenced to XVS
// (frame start), so the pulse (7 µs delay, 60 µs width) straddles the exposure start only while SHS stays 64. Hence only VMAX grows when the rate
// falls, and the exposure follows it: lines = VMAX - 64, exposure = lines * HMAX / clock. At 5000 fps that is VMAX 256, 192 lines, 150 µs: the
// timing every earlier Run used. The producer computes VMAX = floor(clock / (HMAX * fps)) and rounds the exposure to whole lines
// (pz7035-imx426 gentl/pz7035_gentl.c mode_from_registers), which this mirrors.

#include <cmath>
#include <cstdint>
#include <string>

namespace backend::pz {

inline constexpr double kSensorClockHz = 74.25e6;
inline constexpr int kRunHmax = 58;
inline constexpr int kRunShs = 64;
inline constexpr double kRunFpsDefault = 5000.0;
inline constexpr double kRunFpsMin = 100.0;
inline constexpr double kRunFpsMax = 5000.0;       // the PL profile's cap (dual U-Net engines, one frame per ~200 µs)
inline constexpr double kRunFpsQualified = 5000.0; // the only rate qualified on the board
inline constexpr double kRunExposureDefaultUs = 150.0;

struct RunTiming {
    double fps{kRunFpsDefault};   // the configured (nominal) rate
    int hmax{kRunHmax};
    int vmax{256};
    int shs{kRunShs};
    int lines{192};               // exposure lines = vmax - shs
    double exposureUs{kRunExposureDefaultUs};
    double actualFps{kRunFpsDefault}; // clock / (hmax * vmax): the rate the sensor really runs
};

// Empty when `fps` is a Run rate: a whole number of frames per second from 100 to 5000.
inline std::string checkRunFps(double fps) {
    if (!std::isfinite(fps) || fps != std::floor(fps) || fps < kRunFpsMin || fps > kRunFpsMax)
        return "the Run rate must be a whole number of frames per second from 100 to 5000";
    return {};
}

// The timing for a valid rate (checkRunFps is empty).
inline RunTiming runTiming(double fps) {
    RunTiming t;
    t.fps = fps;
    t.vmax = static_cast<int>(std::floor(kSensorClockHz / (static_cast<double>(kRunHmax) * fps)));
    t.lines = t.vmax - kRunShs;
    // 150 µs exactly at the default rate: the producer rounds it to the same 192 lines, and every earlier profile and text says 150.
    t.exposureUs = fps == kRunFpsDefault ? kRunExposureDefaultUs : t.lines * static_cast<double>(kRunHmax) / kSensorClockHz * 1e6;
    t.actualFps = kSensorClockHz / (static_cast<double>(kRunHmax) * t.vmax);
    return t;
}

} // namespace backend::pz
