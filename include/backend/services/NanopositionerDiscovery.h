#pragma once

#include "backend/nanopositioner/INanopositionerBackend.h"

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace backend::services::nanopositioner {

using Vendor = backend::nanopositioner::BackendKind;
using Endpoint = backend::nanopositioner::Endpoint;
struct VendorInfo {
    Vendor id;
    const char* name;
    bool protocolAvailable;
};

inline constexpr std::array<VendorInfo, 2> vendors{{
    {Vendor::Coremor, "CoreMorrow / XMT", true},
    {Vendor::Oeabt, "OEABT", true},
}};

struct Candidate {
    Endpoint port;
    // Empty means unidentified, including USB adapters whose vendor is known.
    std::vector<Vendor> identifiedVendors;
};

// A driver supplies a read-only, protocol-validating probe. Enumeration and
// adapter VID/PID never establish instrument identity. No connection or motion
// writes occur here; the caller may connect only after resolving a unique match.
using Probe = std::function<bool(const Endpoint&)>;
inline std::vector<Candidate> discover(const std::vector<Endpoint>& ports,
                                       const Probe& probe) {
    std::vector<Candidate> result;
    for (const auto& port : ports) {
        bool duplicate = false;
        for (const auto& existing : result) {
            if (existing.port.systemPath == port.systemPath && existing.port.backend == port.backend) duplicate = true;
        }
        if (duplicate) continue;
        Candidate candidate{port, {}};
        for (const auto& vendor : vendors) {
            if (vendor.id == port.backend && vendor.protocolAvailable && probe && probe(port)) {
                candidate.identifiedVendors.push_back(vendor.id);
            }
        }
        result.push_back(std::move(candidate));
    }
    return result;
}

// A remembered port only affects display/probe order, never ambiguity handling.
inline const Candidate* uniqueMatch(const std::vector<Candidate>& candidates) {
    const Candidate* match = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.identifiedVendors.empty()) continue;
        if (match || candidate.identifiedVendors.size() != 1) return nullptr;
        match = &candidate;
    }
    return match;
}

} // namespace backend::services::nanopositioner
