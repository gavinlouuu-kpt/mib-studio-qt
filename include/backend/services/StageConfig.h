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

    struct Reference {
        // Read-only start-up is the default (Gavin, 2026-10-05): the stage
        // moves only when an operator presses Home. A rig may opt in.
        bool onStartup{false};
        double searchSpeedUmS{1000.0};
        double expectedSpanUm{6000.0};
        double spanToleranceUm{300.0};
        double searchMarginUm{500.0};
        double softLimitMarginUm{100.0};
        int powerUpTokenRegister{30054}; // 0 = hold the reference for this session only
    } reference;

    bool requireReference{true};
    // Before Home only Home and Stop are accepted unless a rig raises this.
    double maxUnreferencedJogUm{0.0};

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
