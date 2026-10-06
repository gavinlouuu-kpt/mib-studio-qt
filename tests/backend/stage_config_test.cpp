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
    // 1) Defaults: the safe policy decided on 2026-10-05.
    {
        const StageConfig c = parseStageConfig(J::object());
        MIB_EXPECT(!c.enabled, "disabled by default");
        MIB_EXPECT(!c.reference.onStartup, "read-only start-up by default");
        MIB_EXPECT(c.requireReference && c.maxUnreferencedJogUm == 0.0, "only Home and Stop before Home");
        MIB_EXPECT(c.reference.powerUpTokenRegister == 30054, "power-up token on 30054");
        MIB_EXPECT(c.profile == "tbzf6-60" && c.reference.expectedSpanUm == 6000.0, "TBZF6-60 defaults");
    }

    // 2) The plan's example block parses field by field.
    {
        const J block = J::parse(R"({
            "enabled": true,
            "endpoint": {"usb_serial": "A10RB8XC", "port": "", "address": 1, "axis": 0},
            "profile": "tbzf6-60",
            "reference": {"on_startup": false, "search_speed_um_s": 800, "expected_span_um": 6000,
                          "span_tolerance_um": 250, "search_margin_um": 400,
                          "soft_limit_margin_um": 150, "power_up_token_register": 0},
            "require_reference": true, "max_unreferenced_jog_um": 0,
            "approach": {"direction": "negative", "overshoot_um": 30},
            "speed_um_s": 1500, "accel_um_s2": 2500,
            "poll_ms": {"moving": 40, "idle": 400}, "move_timeout_margin": 3
        })");
        const StageConfig c = parseStageConfig(block);
        MIB_EXPECT(c.enabled && c.endpoint.usbSerial == "A10RB8XC" && c.endpoint.modbusAddress == 1,
                   "endpoint parsed");
        MIB_EXPECT(c.reference.searchSpeedUmS == 800 && c.reference.spanToleranceUm == 250 &&
                       c.reference.softLimitMarginUm == 150 && c.reference.powerUpTokenRegister == 0,
                   "reference block parsed");
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
        MIB_EXPECT(rejects(J{{"approach", {{"direction", "up"}}}}, "direction"), "bad approach direction");
        MIB_EXPECT(rejects(J{{"enabled", "yes"}}, "enabled"), "non-boolean flag");
        MIB_EXPECT(rejects(J{{"max_unreferenced_jog_um", -1}}, "max_unreferenced_jog_um"), "negative jog bound");
    }

    if (mib::test::exitCode() == 0) std::printf("stage config verified\n");
    return mib::test::exitCode();
}
