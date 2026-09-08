// mindvision_config_test
//
// Guards the MindVision JSON config parse + bounds validation (extracted to
// backend::camera::mindvision::parseConfig and shared by MindVisionCamera and
// CameraControlService). The inline parsers applied raw .toInt() values with no
// validation, so a negative strobe_pulse_width_us became a multi-second pulse
// once cast to the SDK's unsigned type, and width/height <= 0 produced an
// unusable ROI. These cases pin the clamps and the malformed-input handling.

#include "backend/camera/mindvision/MindVisionConfig.h"

#include "support/assert.h"

#include <string>

namespace mv = backend::camera::mindvision;

namespace {
mv::ParseResult parse(const char* json)
{
    return mv::parseConfig(std::string(json));
}
} // namespace

int main()
{
    // 1) Valid full config: parsed verbatim, no clamps.
    {
        const auto r = parse(R"({
            "width": 640, "height": 128, "offset_x": 16, "offset_y": 8,
            "exposure_time_us": 2500.0, "trigger_mode": 1, "analog_gain": 4,
            "auto_exposure_enabled": true, "ae_target_brightness": 120,
            "gamma": 90, "contrast": 110, "sharpness": 30, "frame_speed": 1,
            "flip_horizontal": true, "flip_vertical": false,
            "strobe_mode": 1, "strobe_pulse_width_us": 800,
            "strobe_delay_us": 50, "strobe_polarity": 0
        })");
        MIB_REQUIRE(r.ok, "valid config parses");
        MIB_EXPECT(r.warnings.empty(), "in-range config produces no warnings");
        MIB_EXPECT(r.config.width == 640 && r.config.height == 128, "width/height preserved");
        MIB_EXPECT(r.config.offsetX == 16 && r.config.offsetY == 8, "offsets preserved");
        MIB_EXPECT(r.config.exposureUs == 2500.0, "exposure preserved");
        MIB_EXPECT(r.config.triggerMode == 1 && r.config.analogGain == 4, "trigger/gain preserved");
        MIB_EXPECT(r.config.aeEnabled && r.config.aeTarget == 120, "AE preserved");
        MIB_EXPECT(r.config.flipHorizontal && !r.config.flipVertical, "flips preserved");
        MIB_EXPECT(r.config.strobeMode == 1 && r.config.strobePulseUs == 800 &&
                       r.config.strobeDelayUs == 50 && r.config.strobePolarity == 0,
                   "strobe fields preserved");
    }

    // 2) Empty object: every field falls back to its default, no warnings.
    {
        const auto r = parse("{}");
        MIB_REQUIRE(r.ok, "empty object parses");
        MIB_EXPECT(r.warnings.empty(), "defaults produce no warnings");
        MIB_EXPECT(r.config.width == 512 && r.config.height == 96, "default ROI");
        MIB_EXPECT(r.config.exposureUs == 3000.0, "default exposure");
        MIB_EXPECT(r.config.strobePulseUs == 500 && r.config.strobePolarity == 1,
                   "default strobe");
    }

    // 3) Malformed JSON and non-object roots are rejected with an error.
    {
        const auto bad = parse("{ not valid json ");
        MIB_EXPECT(!bad.ok && !bad.error.empty(), "malformed JSON rejected with error");

        const auto arr = parse("[1, 2, 3]");
        MIB_EXPECT(!arr.ok && !arr.error.empty(), "array root rejected");
    }

    // 4) Unusable ROI: width/height <= 0 clamp up to 1 with a warning.
    {
        const auto r = parse(R"({"width": 0, "height": -4})");
        MIB_REQUIRE(r.ok, "still a valid parse, just clamped");
        MIB_EXPECT(r.config.width == 1 && r.config.height == 1, "non-positive ROI clamped to 1");
        MIB_EXPECT(r.warnings.size() >= 2, "each clamped ROI field warns");
    }

    // 5) Negative offsets clamp to 0.
    {
        const auto r = parse(R"({"offset_x": -10, "offset_y": -1})");
        MIB_EXPECT(r.ok && r.config.offsetX == 0 && r.config.offsetY == 0,
                   "negative offsets clamped to 0");
    }

    // 6) Non-positive exposure is reset to the default, with a warning.
    {
        const auto r = parse(R"({"exposure_time_us": -5.0})");
        MIB_EXPECT(r.ok && r.config.exposureUs == 3000.0, "non-positive exposure reset");
        MIB_EXPECT(!r.warnings.empty(), "exposure reset warns");

        const auto z = parse(R"({"exposure_time_us": 0})");
        MIB_EXPECT(z.ok && z.config.exposureUs == 3000.0, "zero exposure reset");
    }

    // 7) The critical fix: a negative strobe pulse/delay clamps to 0 instead of
    //    wrapping to a huge unsigned duration.
    {
        const auto r = parse(R"({"strobe_pulse_width_us": -1, "strobe_delay_us": -100})");
        MIB_EXPECT(r.ok && r.config.strobePulseUs == 0 && r.config.strobeDelayUs == 0,
                   "negative strobe pulse/delay clamped to 0");
        MIB_EXPECT(r.warnings.size() >= 2, "each negative strobe field warns");
    }

    // 8) Out-of-range enum-ish fields clamp into their valid set.
    {
        const auto r = parse(R"({"strobe_polarity": 5, "trigger_mode": 99, "analog_gain": 0})");
        MIB_EXPECT(r.ok, "out-of-range scalars still parse");
        MIB_EXPECT(r.config.strobePolarity == 1, "polarity clamped to {0,1}");
        MIB_EXPECT(r.config.triggerMode == 2, "trigger_mode clamped to max");
        MIB_EXPECT(r.config.analogGain == 1, "analog_gain clamped to min 1");
    }

    // 9) Absurdly large ROI clamps to the upper bound rather than overflowing.
    {
        const auto r = parse(R"({"width": 999999, "height": 1000000})");
        MIB_EXPECT(r.ok && r.config.width == 65535 && r.config.height == 65535,
                   "oversize ROI clamped to upper bound");
    }

    // 10) Acquisition-trigger fields: defaults when absent.
    {
        const auto r = parse("{}");
        MIB_REQUIRE(r.ok, "empty object parses");
        MIB_EXPECT(r.config.extTrigSignalType == 0 && r.config.extTrigJitterUs == 0 &&
                       r.config.acqTriggerDelayUs == 0 && r.config.triggerCount == 1,
                   "acquisition-trigger defaults");
    }

    // 11) Acquisition-trigger fields: full ext-trigger config round-trips.
    {
        const auto r = parse(R"({
            "trigger_mode": 2, "ext_trig_signal_type": 1,
            "ext_trig_jitter_us": 10, "acq_trigger_delay_us": 25, "trigger_count": 3
        })");
        MIB_REQUIRE(r.ok, "ext-trigger config parses");
        MIB_EXPECT(r.warnings.empty(), "in-range trigger config produces no warnings");
        MIB_EXPECT(r.config.triggerMode == 2 && r.config.extTrigSignalType == 1 &&
                       r.config.extTrigJitterUs == 10 && r.config.acqTriggerDelayUs == 25 &&
                       r.config.triggerCount == 3,
                   "ext-trigger fields preserved");
    }

    // 12) strobe_mode range widened to [0,3] (3 = always low).
    {
        const auto ok3 = parse(R"({"strobe_mode": 3})");
        MIB_EXPECT(ok3.ok && ok3.config.strobeMode == 3 && ok3.warnings.empty(),
                   "strobe_mode 3 accepted without warning");
        const auto over = parse(R"({"strobe_mode": 4})");
        MIB_EXPECT(over.ok && over.config.strobeMode == 3 && !over.warnings.empty(),
                   "strobe_mode 4 clamps to 3 with warning");
    }

    // 13) ext_trig_signal_type clamps into [0,4].
    {
        const auto r = parse(R"({"ext_trig_signal_type": 5})");
        MIB_EXPECT(r.ok && r.config.extTrigSignalType == 4, "signal type clamped to 4");
    }

    // 14) trigger_count 0 would silently capture nothing per trigger — clamps
    //     up to 1 with a warning.
    {
        const auto r = parse(R"({"trigger_count": 0})");
        MIB_EXPECT(r.ok && r.config.triggerCount == 1, "trigger_count clamped to 1");
        MIB_EXPECT(!r.warnings.empty(), "trigger_count clamp warns");
    }

    // 15) Negative jitter/delay clamp to 0 (same unsigned-cast hazard as strobe).
    {
        const auto r = parse(R"({"ext_trig_jitter_us": -5, "acq_trigger_delay_us": -1})");
        MIB_EXPECT(r.ok && r.config.extTrigJitterUs == 0 && r.config.acqTriggerDelayUs == 0,
                   "negative jitter/delay clamped to 0");
        MIB_EXPECT(r.warnings.size() >= 2, "each negative trigger field warns");
    }

    if (mib::test::exitCode() == 0) {
        std::printf("MindVision config parse/bounds validation verified\n");
    }
    return mib::test::exitCode();
}
