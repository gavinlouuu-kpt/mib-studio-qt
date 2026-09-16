#include "backend/discovery/providers/NanopositionerProvider.h"

#include "backend/services/AutofocusService.h"
#include "backend/services/NanopositionerDiscovery.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace backend::discovery {

namespace npd = backend::services::nanopositioner;

NanopositionerProvider::NanopositionerProvider(Enumerate enumerate, Probe probe)
    : enumerate_(std::move(enumerate)), probe_(std::move(probe))
{
}

std::unique_ptr<NanopositionerProvider> NanopositionerProvider::production()
{
    return std::make_unique<NanopositionerProvider>(
        [] { return services::AutofocusService::availableEndpoints(); },
        [](const nanopositioner::Endpoint& ep) { return services::AutofocusService::probeEndpoint(ep); });
}

namespace {

const char* vendorName(nanopositioner::BackendKind kind)
{
    for (const auto& v : npd::vendors) {
        if (v.id == kind) return v.name;
    }
    return "unknown";
}

} // namespace

ProviderResult NanopositionerProvider::discover(const DiscoveryRequest& request,
                                                const ProviderContext& context)
{
    ProviderResult result;
    if (!enumerate_) return result;

    auto endpoints = enumerate_();
    const auto preferred = request.preferredNanopositioner.value_or(nanopositioner::Endpoint{});

    // Preferred endpoint first (display/probe order only), then by name.
    std::stable_sort(endpoints.begin(), endpoints.end(), [&](const auto& lhs, const auto& rhs) {
        const int lhsRank =
            (!preferred.persistentId.empty() && lhs.persistentId == preferred.persistentId) ? 0 : 1;
        const int rhsRank =
            (!preferred.persistentId.empty() && rhs.persistentId == preferred.persistentId) ? 0 : 1;
        return lhsRank == rhsRank ? lhs.displayName < rhs.displayName : lhsRank < rhsRank;
    });
    // Vendor filter from the saved preference.
    endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(),
                                   [&](const auto& ep) {
                                       return preferred.backend != nanopositioner::BackendKind::Auto &&
                                              ep.backend != preferred.backend;
                                   }),
                    endpoints.end());
    // Saved CoreMorrow serial settings apply to every candidate.
    if (request.preferredNanopositioner) {
        for (auto& ep : endpoints) {
            ep.coremorBaudRate = preferred.coremorBaudRate;
            ep.coremorAddress = preferred.coremorAddress;
        }
    }

    std::atomic<bool> cancelled{false};
    const auto candidates = npd::discover(endpoints, [&](const nanopositioner::Endpoint& ep) {
        if (cancelled.load()) return false;
        if (context.cancelled && context.cancelled()) {
            cancelled.store(true);
            return false;
        }
        return probe_ && probe_(ep);
    });

    for (const auto& c : candidates) {
        DiscoveredDevice d;
        d.kind = DeviceKind::Nanopositioner;
        d.providerId = kId;
        d.displayName = c.port.displayName.empty() ? c.port.systemPath : c.port.displayName;
        d.endpoint.systemPath = c.port.systemPath;
        d.endpoint.persistentId = c.port.persistentId;
        d.endpoint.vendorId = c.port.vendorId;
        d.endpoint.productId = c.port.productId;
        if (c.port.backend == nanopositioner::BackendKind::Coremor) {
            d.endpoint.busAddress = c.port.coremorAddress;
        }
        if (!c.port.persistentId.empty() && c.port.persistentId != c.port.systemPath) {
            d.stableIdentity = c.port.persistentId;
            d.identityStrength = IdentityStrength::Persistent;
        } else {
            d.stableIdentity = c.port.systemPath;
            d.identityStrength = IdentityStrength::SessionLocal;
        }
        if (c.identifiedVendors.size() == 1) {
            d.identification = IdentificationStatus::Identified;
        } else if (c.identifiedVendors.size() > 1) {
            d.identification = IdentificationStatus::Ambiguous;
        } else {
            d.identification = IdentificationStatus::Unidentified;
        }
        for (auto vendor : c.identifiedVendors) d.claimedBy.push_back(vendorName(vendor));
        d.capabilities.push_back(std::string("vendor:") + vendorName(c.port.backend));
        d.nanopositioner = c.port;
        result.candidates.push_back(std::move(d));
    }

    // Two vendor protocols both "identifying" the same OS path is a conflict:
    // neither is silently chosen; the operator must pick.
    for (auto& d : result.candidates) {
        if (d.identification != IdentificationStatus::Identified) continue;
        std::vector<std::string> claims;
        std::size_t identifiedAtPath = 0;
        for (const auto& other : result.candidates) {
            if (other.endpoint.systemPath != d.endpoint.systemPath) continue;
            if (other.identification != IdentificationStatus::Identified &&
                other.identification != IdentificationStatus::Ambiguous) {
                continue;
            }
            ++identifiedAtPath;
            for (const auto& c : other.claimedBy) {
                if (std::find(claims.begin(), claims.end(), c) == claims.end()) claims.push_back(c);
            }
        }
        if (identifiedAtPath > 1) {
            d.identification = IdentificationStatus::Ambiguous;
            d.claimedBy = claims;
        }
    }

    if (cancelled.load()) {
        result.complete = false;
        result.errors.push_back({kId, ErrorKind::Cancelled, "nanopositioner probe cancelled", ""});
    }
    return result;
}

} // namespace backend::discovery
