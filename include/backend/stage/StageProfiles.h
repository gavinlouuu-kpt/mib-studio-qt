// Mechanical profiles of supported stages. The controller must be configured
// to match the profile before a driver accepts motion (ADR 0013 §2).
#pragma once

#include "backend/stage/StageTypes.h"

#include <optional>
#include <string>

namespace backend::stage {

struct StageProfile {
    std::string name;
    AxisKind kind{AxisKind::Linear};
    double leadMm{0.0};        // effective travel per motor revolution
    std::int32_t pulsesPerRev{0}; // must equal the drive's microstep setting
    double travelUm{0.0};      // nominal limit-to-limit span
};

// TBZF6-60 wedge lift stage: 1 mm ball screw through a wedge gives 0.7 mm per
// revolution (datasheet 3.5 µm per full step); 6 mm travel.
inline StageProfile tbzf6_60Profile()
{
    return StageProfile{"tbzf6-60", AxisKind::Linear, 0.7, 1600, 6000.0};
}

inline std::optional<StageProfile> findStageProfile(const std::string& name)
{
    if (name == "tbzf6-60") return tbzf6_60Profile();
    return std::nullopt;
}

} // namespace backend::stage
