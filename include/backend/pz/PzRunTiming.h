#pragma once

// The sensor timing of the PZ7035 Run mode at an adjustable frame rate and exposure (bridge ABI 38).
//
// The optical outcome Gavin chose: the exposure starts ~50 µs after XVS (the qualified 5 kHz timing, SHS 64 at HMAX 58) with the qualified LED strobe
// (7 µs delay, 60 µs width, XVS-referenced; the strobe is Service-only and never part of the camera document), and "auto" lets the exposure follow the
// frame length: 150 µs at 5000 fps exactly, 949.87 µs at 1000 fps. The line timing is an implementation detail of that outcome:
//  - above ~2655 fps (the producer's own rule, choose_hmax: fps > clock / (116 * (96 + 145))) the line period is HMAX 58 and SHS 64, the qualified 5 kHz regime;
//  - at or below it HMAX 116 and SHS 32, the line period the producer measured clean for long frames (pz7035 gentl/README.md: 512x96 at 1000 Hz, HMAX 116 /
//    VMAX 640 gave 0 receiver line errors; HMAX 58 / VMAX 1280 gave millions per second). 32 x 116 = 64 x 58 line clocks, so the exposure start, the duration
//    and the strobe overlap are the same as the HMAX 58 numbers would give.
// Neither regime other than 5000 fps is qualified in Run on results14; the 1 kHz slot records PzReceiverLineErrors.
// A numeric exposure is validated against the strobe window for the active HMAX: its exposure start (SHS lines after XVS) must lie inside the strobe pulse
// [delay, delay + width], i.e. the pulse straddles the exposure start as in the qualified setting.
// The producer computes VMAX = floor(clock / (HMAX * fps)), lines = round(exposure * clock / HMAX) and SHS = VMAX - lines (pz7035-imx426
// gentl/pz7035_gentl.c mode_from_registers, SHS_MIN 4), which this mirrors.
//
// The range starts at 1000 fps: the LED driver strobes reliably only when pulsed at about 1 kHz or faster (a 2 ms gap gives 54% of the light,
// 4 to 8 ms none; pz7035 docs/LED_STROBE.md).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace backend::pz {

inline constexpr double kSensorClockHz = 74.25e6;
inline constexpr int kRunHmax = 58;                 // the line period of the qualified 5 kHz timing
inline constexpr int kRunHmaxClean = 116;           // the line period the producer picks (and measured clean) for long frames
inline constexpr int kRunShs = 64;                  // the exposure start of the qualified timing, in HMAX 58 line clocks
inline constexpr double kRunExposureStartUs = kRunShs * static_cast<double>(kRunHmax) / 74.25; // 49.99 µs after XVS
inline constexpr int kRunWindowRows = 96;           // the Run window height (producer: choose_hmax uses the window height)
// The producer's rule (choose_hmax): HMAX 58 only when the rate needs it. Integer rates up to 2655 use HMAX 116.
inline constexpr double kRunHmaxSwitchFps = 74.25e6 / (kRunHmaxClean * (kRunWindowRows + 145.0));
inline constexpr int kShsMin = 4;                   // the producer's SHS_MIN
inline constexpr double kRunFpsDefault = 5000.0;
inline constexpr double kRunFpsMin = 1000.0;
inline constexpr double kRunFpsMax = 5000.0;        // the PL profile's cap (dual U-Net engines, one frame per ~200 µs)
inline constexpr double kRunFpsQualified = 5000.0;  // the only rate qualified on the board
inline constexpr double kRunExposureDefaultUs = 150.0;

inline constexpr int runHmaxFor(double fps) { return fps > kRunHmaxSwitchFps ? kRunHmax : kRunHmaxClean; }
inline constexpr double lineUs(int hmax) { return hmax / kSensorClockHz * 1e6; }
// The "auto" exposure start in lines of `hmax`: SHS 64 at HMAX 58, 32 at HMAX 116 (the same 49.99 µs).
inline constexpr int autoShs(int hmax) { return kRunShs * kRunHmax / hmax; }

// What the camera document sets: the rate and the exposure ("auto" = SHS 64, or a number of µs).
struct RunSetting {
    double fps{kRunFpsDefault};
    bool autoExposure{true};
    double exposureUs{0.0}; // the requested exposure when !autoExposure
    bool operator==(const RunSetting& o) const {
        return fps == o.fps && autoExposure == o.autoExposure && (autoExposure || exposureUs == o.exposureUs);
    }
    bool operator!=(const RunSetting& o) const { return !(*this == o); }
};

struct RunTiming {
    double fps{kRunFpsDefault};   // the configured (nominal) rate
    int hmax{kRunHmax};
    int vmax{256};
    int shs{kRunShs};
    int lines{192};               // exposure lines = vmax - shs
    double exposureUs{kRunExposureDefaultUs}; // lines x line period (150 exactly at the default setting)
    double actualFps{kRunFpsDefault}; // clock / (hmax * vmax): the rate the sensor really runs
    bool autoExposure{true};
};

// Where the exposure and the LED strobe fall in a frame, relative to XVS (frame start), in µs.
struct StrobeCheck {
    double exposureStartUs{0}, exposureEndUs{0};
    double strobeStartUs{0}, strobeEndUs{0};
    double overlapUs{0};
    bool overlaps{false};
    bool straddlesStart{false}; // the exposure start lies inside the strobe pulse: the qualified geometry
};

inline int vmaxFor(double fps) { return static_cast<int>(std::floor(kSensorClockHz / (static_cast<double>(runHmaxFor(fps)) * fps))); }

// Empty when `fps` is a Run rate: a whole number of frames per second from 1000 to 5000.
inline std::string checkRunFps(double fps) {
    if (!std::isfinite(fps) || fps != std::floor(fps) || fps < kRunFpsMin || fps > kRunFpsMax)
        return "the Run rate must be a whole number of frames per second from 1000 to 5000";
    return {};
}

// The timing for a valid rate with "auto" exposure: the exposure starts ~50 µs after XVS, it follows the frame length.
inline RunTiming runTiming(double fps) {
    RunTiming t;
    t.fps = fps;
    t.hmax = runHmaxFor(fps);
    t.vmax = vmaxFor(fps);
    t.shs = autoShs(t.hmax);
    t.lines = t.vmax - t.shs;
    // 150 µs exactly at the default rate: the producer rounds it to the same 192 lines, and every earlier profile and text says 150.
    t.exposureUs = fps == kRunFpsDefault ? kRunExposureDefaultUs : t.lines * lineUs(t.hmax);
    t.actualFps = kSensorClockHz / (static_cast<double>(t.hmax) * t.vmax);
    t.autoExposure = true;
    return t;
}

// The timing for a numeric exposure (whole lines, as the producer rounds): the exposure start is SHS lines after XVS. The exposure is clamped to the frame
// in floating point before it becomes an integer (an absurd value is refused by checkRunSetting before it gets here).
inline RunTiming runTimingForExposure(double fps, double exposureUs) {
    RunTiming t = runTiming(fps);
    double lines = std::isfinite(exposureUs) ? std::floor(exposureUs * kSensorClockHz / 1e6 / t.hmax + 0.5) : 1.0;
    lines = std::fmax(1.0, std::fmin(lines, static_cast<double>(t.vmax - kShsMin)));
    t.lines = static_cast<int>(lines);
    t.shs = t.vmax - t.lines;
    t.exposureUs = t.lines * lineUs(t.hmax);
    t.autoExposure = false;
    return t;
}

inline RunTiming runTiming(const RunSetting& s) { return s.autoExposure ? runTiming(s.fps) : runTimingForExposure(s.fps, s.exposureUs); }

inline StrobeCheck strobeCheck(const RunTiming& t, double strobeDelayUs, double strobeWidthUs) {
    StrobeCheck c;
    c.exposureStartUs = t.shs * lineUs(t.hmax);
    c.exposureEndUs = t.vmax * lineUs(t.hmax);
    c.strobeStartUs = strobeDelayUs;
    c.strobeEndUs = strobeDelayUs + strobeWidthUs;
    c.overlapUs = std::fmax(0.0, std::fmin(c.exposureEndUs, c.strobeEndUs) - std::fmax(c.exposureStartUs, c.strobeStartUs));
    c.overlaps = c.overlapUs > 0.0;
    c.straddlesStart = c.exposureStartUs >= c.strobeStartUs && c.exposureStartUs <= c.strobeEndUs;
    return c;
}

// The exposures (µs) a rate allows against the strobe window: the exposure start must lie in [delay, delay + width], i.e. SHS in
// [ceil(delay / line), floor((delay + width) / line)] (and at least the producer's SHS_MIN). lo <= hi unless the window is empty.
struct ExposureRange {
    double loUs{0}, hiUs{0};
    int shsMin{0}, shsMax{0};
    bool empty{false};
};

inline ExposureRange allowedExposure(double fps, double strobeDelayUs, double strobeWidthUs) {
    ExposureRange r;
    const int hmax = runHmaxFor(fps), vmax = vmaxFor(fps);
    const double line = lineUs(hmax);
    r.shsMin = static_cast<int>(std::ceil(strobeDelayUs / line - 1e-9));
    if (r.shsMin < kShsMin) r.shsMin = kShsMin;
    r.shsMax = static_cast<int>(std::floor((strobeDelayUs + strobeWidthUs) / line + 1e-9));
    r.empty = r.shsMin > r.shsMax || r.shsMax >= vmax;
    r.loUs = (vmax - r.shsMax) * line;
    r.hiUs = (vmax - r.shsMin) * line;
    return r;
}

// Empty when the setting is a valid Run setting against the strobe window (the Run preset's delay and width).
inline std::string checkRunSetting(const RunSetting& s, double strobeDelayUs, double strobeWidthUs) {
    if (auto bad = checkRunFps(s.fps); !bad.empty()) return bad;
    if (s.autoExposure) return {};
    if (!std::isfinite(s.exposureUs) || s.exposureUs <= 0.0) return "the exposure must be \"auto\" or a number of microseconds above 0";
    if (s.exposureUs > 1e6 / s.fps) return "the exposure is longer than the frame at this rate";
    const auto t = runTimingForExposure(s.fps, s.exposureUs);
    const auto c = strobeCheck(t, strobeDelayUs, strobeWidthUs);
    if (c.straddlesStart) return {};
    const auto r = allowedExposure(s.fps, strobeDelayUs, strobeWidthUs);
    char buf[260];
    std::snprintf(buf, sizeof buf,
                  "exposure %.1f µs at %.0f fps starts %.1f µs after XVS, outside the LED strobe window (%.0f to %.0f µs): at this rate use \"auto\" or an exposure from %.0f to %.0f µs",
                  s.exposureUs, s.fps, c.exposureStartUs, c.strobeStartUs, c.strobeEndUs, r.loUs, r.hiUs);
    return buf;
}

// The warning for a rate whose strobe misses the exposure; empty when they overlap. (Never shown for a validated setting: a safety check.)
inline std::string strobeWarning(const StrobeCheck& c) {
    if (c.overlaps) return {};
    char buf[200];
    std::snprintf(buf, sizeof buf, "strobe outside the exposure at this rate: frames will be unlit (exposure %.0f to %.0f µs after XVS, strobe %.0f to %.0f µs)",
                  c.exposureStartUs, c.exposureEndUs, c.strobeStartUs, c.strobeEndUs);
    return buf;
}

// What a Run entry stages in the Aravis profile (templated on AppBackend's private AravisProfile so a test can use a stand-in): the window, the rate
// with its exposure, the forced line period and the exposure setting. Every field here is observable by the tests.
template <class Profile>
void stageRunProfile(Profile& p, int x, int y, int width, int height, const RunSetting& s, const RunTiming& t) {
    p.hasRoi = true;
    p.x = x;
    p.y = y;
    p.width = width;
    p.height = height;
    p.experimentFps = t.fps;
    p.experimentExposureUs = t.exposureUs;
    p.experimentHmax = t.hmax;
    p.experimentExposureAuto = s.autoExposure;
    p.experimentExposureSetting = s.autoExposure ? 0.0 : s.exposureUs;
}

// The line period the producer is told: the overview's in Align, the experiment's (58 in Run) otherwise.
template <class Profile>
int producerHmax(const Profile& p, bool overview) {
    return overview ? p.overviewHmax : p.experimentHmax;
}

} // namespace backend::pz
