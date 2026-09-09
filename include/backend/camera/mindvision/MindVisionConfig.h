// Pure parsing + bounds validation for the MindVision camera JSON config.
// Extracted from MindVisionCamera and CameraControlService (which had drifted
// to apply different field subsets) so the parse and the safety clamps can be
// unit tested without the MVCAMSDK or a camera. Qt-free (nlohmann_json only) as
// part of the Qt -> React/Tauri backend decoupling (epic #246).
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace backend::camera::mindvision {

// All fields default to the same values the inline parsers used, so a missing
// key behaves exactly as before. Values are bounds-checked in parseConfig.
struct Config {
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

    r.config = c;
    r.ok = true;
    return r;
}

} // namespace backend::camera::mindvision
