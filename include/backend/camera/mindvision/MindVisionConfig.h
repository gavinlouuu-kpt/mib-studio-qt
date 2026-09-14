// Pure parsing + bounds validation for the MindVision camera JSON config.
// Extracted from MindVisionCamera and CameraControlService (which had drifted
// to apply different field subsets) so the parse and the safety clamps can be
// unit tested without the MVCAMSDK or a camera. Qt-free (nlohmann_json only) as
// part of the Qt -> React/Tauri backend decoupling (epic #246).
#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace backend::camera::mindvision {

// Saved `live_view` block (issue #413): the pulse-generator link and trigger
// train that a coordinated illuminated capture owns. Parsed and validated in
// parseConfig so the capture factory, the camera and the settings UI share one
// rule set; nothing here is applied to hardware by the parser.
struct LiveViewSettings {
    std::string port{"auto"};  // system port name, or "auto" for discovery
    int address{1};            // Modbus slave address 1..247
    int channel{1};            // generator channel 1..4 (1-based, as saved)
    double frequencyHz{5000.0};
    double dutyPercent{10.0};
    int baud{9600};
    int dataBits{8};
    char parity{'N'};
    int stopBits{1};

    double periodUs() const { return frequencyHz > 0.0 ? 1e6 / frequencyHz : 0.0; }
    double triggerPulseUs() const { return periodUs() * dutyPercent / 100.0; }
};

// All fields default to the same values the inline parsers used, so a missing
// key behaves exactly as before. Values are bounds-checked in parseConfig.
struct Config {
    bool illuminatedLive{false};
    LiveViewSettings liveView{}; // meaningful only when illuminatedLive
    int width{512};
    int height{96};
    int offsetX{0};
    int offsetY{0};
    double exposureUs{3000.0};
    int triggerMode{0};
    int analogGain{1};
    bool aeEnabled{false};
    int aeTarget{100};
    int gamma{100};
    int contrast{100};
    int sharpness{0};
    int frameSpeed{2};
    bool flipHorizontal{false};
    bool flipVertical{false};
    int strobeMode{0};
    int strobePulseUs{500};
    int strobeDelayUs{0};
    int strobePolarity{1};
    int extTrigSignalType{0};  // MVSDK edge select: 0=leading(rising) edge,
                               // 1=trailing(falling), 2=high level, 3=low level,
                               // 4=double edge
    int extTrigJitterUs{0};    // de-glitch filter on the trigger input line
    int acqTriggerDelayUs{0};  // delay from external trigger edge to exposure start
    int triggerCount{1};       // frames captured per trigger (software + hardware)
};

struct ParseResult {
    bool ok{false};
    Config config{};
    std::string error;                 // set when ok == false
    std::vector<std::string> warnings; // bounds clamps applied to a valid parse
};

namespace detail {
// Clamp an int into [lo, hi], recording a warning naming the field if it moved.
inline int clampInt(int value, int lo, int hi, const char* field,
                    std::vector<std::string>& warnings)
{
    int clamped = value;
    if (clamped < lo) clamped = lo;
    if (clamped > hi) clamped = hi;
    if (clamped != value) {
        warnings.push_back(std::string("MindVision config: ") + field + "=" +
                           std::to_string(value) + " out of range [" +
                           std::to_string(lo) + "," + std::to_string(hi) +
                           "], clamped to " + std::to_string(clamped));
    }
    return clamped;
}

inline std::string formatUs(double value)
{
    // Whole microseconds read naturally in operator-facing messages; keep one
    // decimal only when the value is fractional.
    char buffer[32];
    if (std::fabs(value - std::round(value)) < 0.05) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    }
    return buffer;
}

// Reads the enabled `live_view` block into c.liveView. Returns an empty string
// on success, otherwise a message naming the offending field. Missing keys
// take the bundled preset's defaults; a wrong JSON type is an error rather
// than a silent fallback, because these values drive hardware.
inline std::string parseLiveView(const nlohmann::json& live, Config& c)
{
    auto& lv = c.liveView;
    auto number = [&live](const char* key, double& out) -> bool {
        const auto it = live.find(key);
        if (it == live.end()) return true;
        if (!it->is_number()) return false;
        out = it->get<double>();
        return true;
    };
    auto integer = [&live](const char* key, int& out) -> bool {
        const auto it = live.find(key);
        if (it == live.end()) return true;
        if (!it->is_number_integer()) return false;
        out = it->get<int>();
        return true;
    };

    if (const auto it = live.find("port"); it != live.end()) {
        if (!it->is_string()) return "live_view.port must be a serial port name or \"auto\"";
        lv.port = it->get<std::string>();
    }
    if (lv.port.empty()) return "live_view.port must be a serial port name or \"auto\"";
    if (!integer("address", lv.address)) return "live_view.address must be an integer";
    if (lv.address < 1 || lv.address > 247)
        return "live_view.address must be a Modbus address between 1 and 247";
    if (!integer("channel", lv.channel)) return "live_view.channel must be an integer";
    if (lv.channel < 1 || lv.channel > 4) return "live_view.channel must be between 1 and 4";
    if (!number("frequency_hz", lv.frequencyHz)) return "live_view.frequency_hz must be a number";
    if (!std::isfinite(lv.frequencyHz) || lv.frequencyHz < 400.0 || lv.frequencyHz > 40000.0)
        return "Requested FPS must be between 400 and 40000 (pulse generator range)";
    if (!number("duty_percent", lv.dutyPercent)) return "live_view.duty_percent must be a number";
    if (!std::isfinite(lv.dutyPercent) || lv.dutyPercent <= 0.0 || lv.dutyPercent >= 100.0)
        return "live_view.duty_percent must be above 0 and below 100 (a continuous level cannot trigger)";
    if (!integer("baud", lv.baud)) return "live_view.baud must be an integer";
    if (lv.baud <= 0) return "live_view.baud must be positive";
    if (!integer("data_bits", lv.dataBits)) return "live_view.data_bits must be an integer";
    if (lv.dataBits < 5 || lv.dataBits > 8) return "live_view.data_bits must be between 5 and 8";
    if (const auto it = live.find("parity"); it != live.end()) {
        if (!it->is_string() || it->get<std::string>().size() != 1)
            return "live_view.parity must be one of N, E, O";
        lv.parity = it->get<std::string>()[0];
    }
    if (lv.parity != 'N' && lv.parity != 'E' && lv.parity != 'O')
        return "live_view.parity must be one of N, E, O";
    if (!integer("stop_bits", lv.stopBits)) return "live_view.stop_bits must be an integer";
    if (lv.stopBits < 1 || lv.stopBits > 2) return "live_view.stop_bits must be 1 or 2";
    return {};
}

// Static timing rules for one trigger period. These are necessary, not
// sufficient: the camera's readout time at the ROI decides the achievable
// frame rate, and only an oscilloscope establishes physical LED timing.
inline std::string validateLiveViewTiming(const Config& c)
{
    const auto& lv = c.liveView;
    const double period = lv.periodUs();
    const std::string fps = formatUs(lv.frequencyHz);
    if (c.exposureUs > period) {
        return "Requested FPS " + fps + " gives a " + formatUs(period) +
               " us trigger period; exposure " + formatUs(c.exposureUs) +
               " us must not exceed it. Lower the FPS or the exposure.";
    }
    const double strobeEnd = static_cast<double>(c.strobeDelayUs + c.strobePulseUs);
    if (strobeEnd >= period) {
        return "Requested FPS " + fps + " gives a " + formatUs(period) +
               " us trigger period; strobe delay + width (" + formatUs(strobeEnd) +
               " us) must be shorter than it. Lower the FPS or the strobe width.";
    }
    if (lv.triggerPulseUs() < 1.0) {
        return "Trigger pulse of " + formatUs(lv.triggerPulseUs()) +
               " us is too short; increase live_view.duty_percent.";
    }
    return {};
}
} // namespace detail

// Parse + validate. Returns ok=false (with error set) for malformed JSON or a
// non-object document. Otherwise applies defaults for missing keys and clamps
// every numeric field to a safe range, collecting one warning per clamp. The
// critical clamps guard against an unusable ROI (width/height <= 0) and the
// negative strobe/exposure values that previously became enormous unsigned
// durations when cast for the SDK.
inline ParseResult parseConfig(const std::string& jsonBytes)
{
    ParseResult r;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(jsonBytes);
    } catch (const nlohmann::json::parse_error& e) {
        r.error = std::string("JSON parse error: ") + e.what();
        return r;
    }
    if (!doc.is_object()) {
        r.error = "MindVision config root is not a JSON object";
        return r;
    }

    const nlohmann::json& obj = doc;
    Config c;
    auto& w = r.warnings;

    // Lenient accessors that mirror QJsonValue::toInt/toDouble/toBool(default):
    // a missing key, or a value of the wrong JSON type, falls back to `def`
    // rather than throwing.
    auto getInt = [&obj](const char* key, int def) -> int {
        const auto it = obj.find(key);
        return (it != obj.end() && it->is_number()) ? it->get<int>() : def;
    };
    auto getDouble = [&obj](const char* key, double def) -> double {
        const auto it = obj.find(key);
        return (it != obj.end() && it->is_number()) ? it->get<double>() : def;
    };
    auto getBool = [&obj](const char* key, bool def) -> bool {
        const auto it = obj.find(key);
        return (it != obj.end() && it->is_boolean()) ? it->get<bool>() : def;
    };

    // ROI must be positive; an upper bound large enough for any current sensor
    // keeps a garbage value from overflowing SDK fields.
    c.width = detail::clampInt(getInt("width", c.width), 1, 65535, "width", w);
    c.height = detail::clampInt(getInt("height", c.height), 1, 65535, "height", w);
    c.offsetX = detail::clampInt(getInt("offset_x", c.offsetX), 0, 65535, "offset_x", w);
    c.offsetY = detail::clampInt(getInt("offset_y", c.offsetY), 0, 65535, "offset_y", w);

    c.exposureUs = getDouble("exposure_time_us", c.exposureUs);
    if (!(c.exposureUs > 0.0)) {
        w.push_back("MindVision config: exposure_time_us=" +
                    std::to_string(c.exposureUs) + " must be > 0, reset to 3000");
        c.exposureUs = 3000.0;
    }

    c.triggerMode = detail::clampInt(getInt("trigger_mode", c.triggerMode), 0, 2, "trigger_mode", w);
    c.analogGain = detail::clampInt(getInt("analog_gain", c.analogGain), 1, 256, "analog_gain", w);
    c.aeEnabled = getBool("auto_exposure_enabled", c.aeEnabled);
    c.aeTarget = detail::clampInt(getInt("ae_target_brightness", c.aeTarget), 0, 255, "ae_target_brightness", w);
    c.gamma = detail::clampInt(getInt("gamma", c.gamma), 0, 1000, "gamma", w);
    c.contrast = detail::clampInt(getInt("contrast", c.contrast), 0, 1000, "contrast", w);
    c.sharpness = detail::clampInt(getInt("sharpness", c.sharpness), 0, 100, "sharpness", w);
    c.frameSpeed = detail::clampInt(getInt("frame_speed", c.frameSpeed), 0, 2, "frame_speed", w);
    c.flipHorizontal = getBool("flip_horizontal", c.flipHorizontal);
    c.flipVertical = getBool("flip_vertical", c.flipVertical);

    // Strobe modes per MVSDK: 0 = auto sync with exposure, 1 = manual
    // (delay + pulse width), 2 = always high, 3 = always low.
    c.strobeMode = detail::clampInt(getInt("strobe_mode", c.strobeMode), 0, 3, "strobe_mode", w);
    // Strobe pulse/delay are cast to unsigned for the SDK — a negative here
    // previously became a multi-second pulse. Clamp to >= 0.
    c.strobePulseUs = detail::clampInt(getInt("strobe_pulse_width_us", c.strobePulseUs), 0, 1000000, "strobe_pulse_width_us", w);
    c.strobeDelayUs = detail::clampInt(getInt("strobe_delay_us", c.strobeDelayUs), 0, 1000000, "strobe_delay_us", w);
    c.strobePolarity = detail::clampInt(getInt("strobe_polarity", c.strobePolarity), 0, 1, "strobe_polarity", w);

    // Acquisition-trigger extras (trigger_mode 1 = software, 2 = external).
    // Jitter/delay share the strobe fields' 1s ceiling; trigger_count's floor
    // of 1 prevents a config that silently captures zero frames per trigger
    // (the 1000 ceiling is arbitrary — the SDK maximum is not documented).
    c.extTrigSignalType = detail::clampInt(getInt("ext_trig_signal_type", c.extTrigSignalType), 0, 4, "ext_trig_signal_type", w);
    c.extTrigJitterUs = detail::clampInt(getInt("ext_trig_jitter_us", c.extTrigJitterUs), 0, 1000000, "ext_trig_jitter_us", w);
    c.acqTriggerDelayUs = detail::clampInt(getInt("acq_trigger_delay_us", c.acqTriggerDelayUs), 0, 1000000, "acq_trigger_delay_us", w);
    c.triggerCount = detail::clampInt(getInt("trigger_count", c.triggerCount), 1, 1000, "trigger_count", w);

    if (doc.contains("live_view")) {
        const auto& live = doc["live_view"];
        if (!live.is_object() || !live.contains("enabled") || !live["enabled"].is_boolean()) {
            r.error = "live_view must contain a boolean enabled";
            return r;
        }
        c.illuminatedLive = live["enabled"].get<bool>();
        if (c.illuminatedLive) {
            const std::string error = detail::parseLiveView(live, c);
            if (!error.empty()) {
                r.error = error;
                return r;
            }
        }
    }
    if (c.illuminatedLive && (c.triggerMode != 2 || c.extTrigSignalType != 2 || c.strobeMode != 1 ||
                              c.strobePolarity != 1 || c.aeEnabled || c.triggerCount != 1 ||
                              c.strobePulseUs <= 0 || !w.empty())) {
        r.error = "Illuminated Live View requires external high-level trigger, manual exposure, "
                  "one frame per trigger and active-high manual strobe. Check Hardware Setup.";
        return r;
    }
    if (c.illuminatedLive) {
        const std::string error = detail::validateLiveViewTiming(c);
        if (!error.empty()) {
            r.error = error;
            return r;
        }
    }
    r.config = c;
    r.ok = true;
    return r;
}

} // namespace backend::camera::mindvision
