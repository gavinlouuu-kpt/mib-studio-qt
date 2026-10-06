// stage_config_test
//
// The "stage" block of config.json (#464, ADR 0013): defaults (read-only
// start-up, Home and Stop only before referencing), full parsing, and
// rejection of invalid values with the offending key named.

#include "backend/services/StageConfig.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <stdexcept>
#include <string>

using backend::services::parseStageConfig;
using backend::services::StageConfig;
using J = nlohmann::json;

namespace {
bool rejects(const J& block, const std::string& keyInMessage)
{
    try {
        parseStageConfig(block);
    } catch (const std::runtime_error& e) {
        return std::string(e.what()).find(keyInMessage) != std::string::npos;
    }
    return false;
}
} // namespace

int main()
{
    // 1) Defaults: the safe policy (ADR 0013 Amendment 1: no homing).
    {
        const StageConfig c = parseStageConfig(J::object());
        MIB_EXPECT(!c.enabled, "disabled by default");
        MIB_EXPECT(c.envelope.defaultUm == 1000.0, "+/-1000 um around the operator's zero by default");
        MIB_EXPECT(c.reference.powerUpTokenRegister == 30054, "power-up token on 30054");
        MIB_EXPECT(c.profile == "tbzf6-60" && c.reference.expectedSpanUm == 6000.0, "TBZF6-60 defaults");
    }

    // 2) The plan's example block parses field by field.
    {
        const J block = J::parse(R"({
            "enabled": true,
            "endpoint": {"usb_serial": "A10RB8XC", "port": "", "address": 1, "axis": 0},
            "profile": "tbzf6-60",
            "reference": {"expected_span_um": 6000, "soft_limit_margin_um": 150, "power_up_token_register": 0,
                          "allow_session_only_zero": true},
            "envelope": {"default_um": 400},
            "approach": {"direction": "negative", "overshoot_um": 30},
            "speed_um_s": 1500, "accel_um_s2": 2500,
            "poll_ms": {"moving": 40, "idle": 400}, "move_timeout_margin": 3
        })");
        const StageConfig c = parseStageConfig(block);
        MIB_EXPECT(c.enabled && c.endpoint.usbSerial == "A10RB8XC" && c.endpoint.modbusAddress == 1,
                   "endpoint parsed");
        MIB_EXPECT(c.reference.softLimitMarginUm == 150 && c.reference.powerUpTokenRegister == 0 &&
                       c.envelope.defaultUm == 400,
                   "reference and envelope blocks parsed");
        MIB_EXPECT(c.approach.direction == backend::stage::Direction::Negative && c.approach.overshootUm == 30,
                   "approach parsed");
        MIB_EXPECT(c.speedUmS == 1500 && c.pollMovingMs == 40 && c.pollIdleMs == 400 && c.moveTimeoutMargin == 3,
                   "motion and polling parsed");
    }

    // 3) Invalid values are rejected and name their key.
    {
        MIB_EXPECT(rejects(J::array(), "stage must be an object"), "non-object block");
        MIB_EXPECT(rejects(J{{"profile", "pi-m112"}}, "profile"), "unknown profile");
        MIB_EXPECT(rejects(J{{"endpoint", {{"address", 0}}}}, "address"), "address 0");
        MIB_EXPECT(rejects(J{{"endpoint", {{"axis", 3}}}}, "axis"), "axis 3");
        MIB_EXPECT(rejects(J{{"reference", {{"power_up_token_register", 30060}}}}, "power_up_token_register"),
                   "token register other than 30054/0");
        MIB_EXPECT(rejects(J{{"reference", {{"soft_limit_margin_um", 3000}}}}, "soft_limit_margin_um"),
                   "margin that leaves no travel");
        MIB_EXPECT(rejects(J{{"speed_um_s", 9000}}, "speed_um_s"), "speed above the stage maximum");
        MIB_EXPECT(rejects(J{{"envelope", {{"default_um", 1001}}}}, "default_um"),
                   "a default envelope can be lowered, never raised above 1000 um");
        MIB_EXPECT(rejects(J{{"envelope", {{"default_um", 0}}}}, "default_um"), "an empty envelope");
        MIB_EXPECT(rejects(J{{"approach", {{"direction", "up"}}}}, "direction"), "bad approach direction");
        MIB_EXPECT(rejects(J{{"enabled", "yes"}}, "enabled"), "non-boolean flag");
    }

    // 3b) Session-only zero (token register 0) turns off power-cycle detection: only
    // with the explicit acceptance flag.
    {
        MIB_EXPECT(rejects(J{{"reference", {{"power_up_token_register", 0}}}}, "allow_session_only_zero"),
                   "token register 0 needs the acceptance flag");
        const StageConfig c = parseStageConfig(
            J{{"reference", {{"power_up_token_register", 0}, {"allow_session_only_zero", true}}}});
        MIB_EXPECT(c.reference.powerUpTokenRegister == 0 && c.reference.allowSessionOnlyZero, "accepted with it");
        MIB_EXPECT(!parseStageConfig(J::object()).reference.allowSessionOnlyZero, "off by default");
    }

    // 4) Keys of the removed Home are ignored, never acted on: an old config
    // that asked for a start-up Home just connects, read-only.
    {
        const J old = J::parse(R"({"enabled": true,
            "reference": {"on_startup": true, "search_speed_um_s": 800, "span_tolerance_um": 250,
                          "search_margin_um": 400},
            "require_reference": false, "max_unreferenced_jog_um": 5000})");
        const StageConfig c = parseStageConfig(old);
        MIB_EXPECT(c.enabled && c.envelope.defaultUm == 1000.0, "old Home keys do not break parsing or widen anything");
    }

    if (mib::test::exitCode() == 0) std::printf("stage config verified\n");
    return mib::test::exitCode();
}
