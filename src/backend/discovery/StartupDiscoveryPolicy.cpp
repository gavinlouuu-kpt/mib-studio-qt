#include "backend/discovery/StartupDiscoveryPolicy.h"

#include <algorithm>

namespace backend::discovery {

bool isDecidable(const DiscoverySnapshot& snapshot)
{
    return snapshot.state == JobState::Completed && snapshot.complete && !snapshot.overflow;
}

CameraDecision decideCamera(const DiscoverySnapshot& snapshot)
{
    CameraDecision decision;
    if (!isDecidable(snapshot)) return decision;

    const DiscoveredDevice* unique = nullptr;
    for (const auto& c : snapshot.candidates) {
        if (c.kind != DeviceKind::Camera || c.synthetic) continue;
        if (c.identification != IdentificationStatus::Identified) continue;
        ++decision.physicalCount;
        unique = &c;
    }
    if (decision.physicalCount == 0) {
        decision.kind = CameraDecision::Kind::NoneFound;
    } else if (decision.physicalCount == 1) {
        decision.kind = CameraDecision::Kind::SelectUnique;
        decision.device = *unique;
    } else {
        decision.kind = CameraDecision::Kind::RequireSelection;
    }
    return decision;
}

std::vector<DiscoveredDevice> orderByPreference(std::vector<DiscoveredDevice> candidates,
                                                const std::string& preferredIdentity)
{
    if (preferredIdentity.empty()) return candidates;
    std::stable_partition(candidates.begin(), candidates.end(), [&](const DiscoveredDevice& d) {
        return d.stableIdentity == preferredIdentity || d.endpoint.persistentId == preferredIdentity;
    });
    return candidates;
}

NanopositionerDecision decideNanopositioner(const DiscoverySnapshot& snapshot,
                                            const std::string& preferredIdentity)
{
    NanopositionerDecision decision;
    if (!isDecidable(snapshot)) return decision;

    const auto ordered = orderByPreference(snapshot.candidates, preferredIdentity);
    const DiscoveredDevice* unique = nullptr;
    for (const auto& c : ordered) {
        if (c.kind != DeviceKind::Nanopositioner || c.synthetic) continue;
        if (c.identification == IdentificationStatus::Ambiguous) {
            ++decision.ambiguousCount;
        } else if (c.identification == IdentificationStatus::Identified) {
            ++decision.identifiedCount;
            unique = &c;
        }
    }
    if (decision.ambiguousCount > 0 || decision.identifiedCount > 1) {
        decision.kind = NanopositionerDecision::Kind::RequireSelection;
    } else if (decision.identifiedCount == 1) {
        decision.kind = NanopositionerDecision::Kind::ConnectUnique;
        decision.device = *unique;
        if (unique->nanopositioner) {
            decision.endpoint = *unique->nanopositioner;
        } else {
            nanopositioner::Endpoint ep;
            ep.persistentId = unique->endpoint.persistentId.empty() ? unique->stableIdentity
                                                                     : unique->endpoint.persistentId;
            ep.systemPath = unique->endpoint.systemPath;
            ep.displayName = unique->displayName;
            ep.vendorId = unique->endpoint.vendorId;
            ep.productId = unique->endpoint.productId;
            decision.endpoint = ep;
        }
    } else {
        decision.kind = NanopositionerDecision::Kind::NotFound;
    }
    return decision;
}

} // namespace backend::discovery
