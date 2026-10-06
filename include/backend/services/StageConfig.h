// Configuration of the motorized Z stage service (ADR 0013, #464): the
// "stage" block of config.json. Every distance is in micrometres.
#pragma once

#include "backend/stage/StageTypes.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace backend::services {

struct StageConfig {
    bool enabled{false};
    stage::StageEndpoint endpoint; // usb_serial / port / address / axis
    std::string profile{"tbzf6-60"};

    // The stage is never homed (ADR 0013 Amendment 1, 2026-10-06): the
    // operator sets zero, and travel is bounded around it. Nothing moves
    // at start-up.
    struct Reference {
        double expectedSpanUm{6000.0}; // the stage's travel, from the profile
        double softLimitMarginUm{100.0}; // widest envelope = +/-(span/2 - margin)
        int powerUpTokenRegister{30054}; // 0 = hold the zero for this session only
    } reference;

    struct Envelope {
        // Travel around the operator's zero unless they declare mid-travel.
        // A rig may only lower it (maximum 1000 um).
        double defaultUm{1000.0};
    } envelope;

    struct Approach {
        stage::Direction direction{stage::Direction::Positive};
        double overshootUm{20.0}; // 0 disables the one-sided approach
    } approach;

    double speedUmS{1000.0};
    double accelUmS2{2000.0};
    int pollMovingMs{50};
    int pollIdleMs{500};
    double moveTimeoutMargin{2.0}; // deadline = distance / speed * margin + 1 s
};

// Parses and validates a "stage" block; absent keys keep their defaults.
// Throws std::runtime_error naming the offending key.
StageConfig parseStageConfig(const nlohmann::json& block);

} // namespace backend::services
