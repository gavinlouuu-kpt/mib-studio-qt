// Pure selection/connection decisions over a discovery snapshot (issue #419,
// ADR 0005). Discovery finds devices; this policy decides; the existing
// services select/connect. Nothing here touches hardware.
//
// Rules (behaviour-compatible with the pre-#419 DeviceInitManager):
//  - only a Completed, complete, non-overflowed snapshot is decidable;
//  - one fully identified physical camera may be selected; framegrabbers and
//    the synthetic mock never count; more than one requires user selection;
//  - one identified nanopositioner may be connected; any ambiguous endpoint
//    or more than one identified device requires user selection, even when a
//    saved preference matches one of them (preferences reorder only).
#pragma once

#include "backend/discovery/DeviceDiscoveryTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace backend::discovery {

bool isDecidable(const DiscoverySnapshot& snapshot);

struct CameraDecision {
    enum class Kind { NotDecidable, NoneFound, SelectUnique, RequireSelection };
    Kind kind{Kind::NotDecidable};
    std::optional<DiscoveredDevice> device; // set for SelectUnique
    std::size_t physicalCount{0};           // identified, non-synthetic cameras
};

CameraDecision decideCamera(const DiscoverySnapshot& snapshot);

struct NanopositionerDecision {
    enum class Kind { NotDecidable, NotFound, ConnectUnique, RequireSelection };
    Kind kind{Kind::NotDecidable};
    std::optional<DiscoveredDevice> device;              // set for ConnectUnique
    std::optional<nanopositioner::Endpoint> endpoint;    // set for ConnectUnique
    std::size_t identifiedCount{0};
    std::size_t ambiguousCount{0};
};

NanopositionerDecision decideNanopositioner(const DiscoverySnapshot& snapshot,
                                            const std::string& preferredIdentity = {});

// Stable partition: candidates whose stable identity or persistent endpoint ID
// equals `preferredIdentity` come first. Never changes any decision.
std::vector<DiscoveredDevice> orderByPreference(std::vector<DiscoveredDevice> candidates,
                                                const std::string& preferredIdentity);

} // namespace backend::discovery
