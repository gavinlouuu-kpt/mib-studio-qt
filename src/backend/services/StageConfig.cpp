#include "backend/services/StageConfig.h"

#include "backend/stage/StageProfiles.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace backend::services {

namespace {

using J = nlohmann::json;

double number(const J& o, const char* key, double fallback, double lo, double hi)
{
    if (!o.contains(key)) return fallback;
    const auto& v = o.at(key);
    if (!v.is_number()) throw std::runtime_error(std::string("stage.") + key + " must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d) || d < lo || d > hi)
        throw std::runtime_error(std::string("stage.") + key + " is out of range");
    return d;
}

int integer(const J& o, const char* key, int fallback, int lo, int hi)
{
    if (!o.contains(key)) return fallback;
    const auto& v = o.at(key);
    if (!v.is_number_integer()) throw std::runtime_error(std::string("stage.") + key + " must be an integer");
    const auto i = v.get<long long>();
    if (i < lo || i > hi) throw std::runtime_error(std::string("stage.") + key + " is out of range");
    return static_cast<int>(i);
}

bool boolean(const J& o, const char* key, bool fallback)
{
    if (!o.contains(key)) return fallback;
    if (!o.at(key).is_boolean()) throw std::runtime_error(std::string("stage.") + key + " must be a boolean");
    return o.at(key).get<bool>();
}

std::string text(const J& o, const char* key, const std::string& fallback)
{
    if (!o.contains(key)) return fallback;
    if (!o.at(key).is_string()) throw std::runtime_error(std::string("stage.") + key + " must be a string");
    return o.at(key).get<std::string>();
}

const J& object(const J& o, const char* key)
{
    static const J empty = J::object();
    if (!o.contains(key)) return empty;
    if (!o.at(key).is_object()) throw std::runtime_error(std::string("stage.") + key + " must be an object");
    return o.at(key);
}

} // namespace

StageConfig parseStageConfig(const J& block)
{
    if (!block.is_object()) throw std::runtime_error("stage must be an object");
    StageConfig c;
    c.enabled = boolean(block, "enabled", c.enabled);

    const auto& ep = object(block, "endpoint");
    c.endpoint.usbSerial = text(ep, "usb_serial", c.endpoint.usbSerial);
    c.endpoint.systemPort = text(ep, "port", c.endpoint.systemPort);
    c.endpoint.modbusAddress = static_cast<std::uint8_t>(integer(ep, "address", 1, 1, 255));
    c.endpoint.axis = integer(ep, "axis", 0, 0, 2);

    c.profile = text(block, "profile", c.profile);
    if (!stage::findStageProfile(c.profile)) throw std::runtime_error("stage.profile '" + c.profile + "' is unknown");

    const auto& ref = object(block, "reference");
    auto& r = c.reference;
    // Keys of the removed Home (on_startup, search_*, span_tolerance_um,
    // require_reference, max_unreferenced_jog_um) are ignored, never acted on.
    r.expectedSpanUm = number(ref, "expected_span_um", r.expectedSpanUm, 1.0, 1e6);
    r.softLimitMarginUm = number(ref, "soft_limit_margin_um", r.softLimitMarginUm, 0.0, 1e6);
    r.powerUpTokenRegister = integer(ref, "power_up_token_register", r.powerUpTokenRegister, 0, 65535);
    if (r.powerUpTokenRegister != 0 && r.powerUpTokenRegister != 30054)
        throw std::runtime_error("stage.reference.power_up_token_register must be 30054 or 0");
    r.allowSessionOnlyZero = boolean(ref, "allow_session_only_zero", r.allowSessionOnlyZero);
    if (r.powerUpTokenRegister == 0 && !r.allowSessionOnlyZero)
        throw std::runtime_error(
            "stage.reference.power_up_token_register 0 turns off power-cycle detection; it is for hardware "
            "acceptance only and needs stage.reference.allow_session_only_zero: true");
    if (2 * r.softLimitMarginUm >= r.expectedSpanUm)
        throw std::runtime_error("stage.reference.soft_limit_margin_um leaves no travel");

    // The default envelope can be lowered by a rig, never raised.
    c.envelope.defaultUm = number(object(block, "envelope"), "default_um", c.envelope.defaultUm, 1.0, 1000.0);

    const auto& ap = object(block, "approach");
    const std::string dir = text(ap, "direction", "positive");
    if (dir == "positive") c.approach.direction = stage::Direction::Positive;
    else if (dir == "negative") c.approach.direction = stage::Direction::Negative;
    else throw std::runtime_error("stage.approach.direction must be 'positive' or 'negative'");
    c.approach.overshootUm = number(ap, "overshoot_um", c.approach.overshootUm, 0.0, 1000.0);

    c.speedUmS = number(block, "speed_um_s", c.speedUmS, 1.0, 7000.0);
    c.accelUmS2 = number(block, "accel_um_s2", c.accelUmS2, 1.0, 1e6);
    const auto& poll = object(block, "poll_ms");
    c.pollMovingMs = integer(poll, "moving", c.pollMovingMs, 10, 10000);
    c.pollIdleMs = integer(poll, "idle", c.pollIdleMs, 10, 60000);
    c.moveTimeoutMargin = number(block, "move_timeout_margin", c.moveTimeoutMargin, 1.0, 100.0);
    return c;
}

} // namespace backend::services
