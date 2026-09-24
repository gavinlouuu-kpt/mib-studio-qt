// Provider seam of the device-discovery service (issue #419, ADR 0005).
// A provider owns SDK/protocol enumeration and read-only identification for
// one device kind; the service owns job lifetime, cancellation, deadlines,
// dedup and result bounds.
#pragma once

#include "backend/discovery/DeviceDiscoveryTypes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace backend::discovery {

struct ProviderContext {
    // True once the owning job was cancelled or the service is shutting down.
    // Providers check it between bounded steps (ports, addresses, SDK calls).
    std::function<bool()> cancelled;
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t jobId{0};
    int attempt{1};
};

struct ProviderResult {
    std::vector<DiscoveredDevice> candidates;
    std::vector<DiscoveryError> errors;
    bool complete{true}; // false when coverage stopped early (cancel, timeout, busy)
};

class IDeviceDiscoveryProvider {
public:
    virtual ~IDeviceDiscoveryProvider() = default;

    virtual std::string id() const = 0;
    virtual DeviceKind kind() const = 0;
    // Providers sharing a non-empty resource class ("camera-sdk",
    // "serial:COM7") never run concurrently across jobs.
    virtual std::string resourceClass() const { return {}; }
    // False when the underlying vendor call cannot be interrupted once entered
    // (the service documents and tests the resulting cancellation bound).
    virtual bool cancellable() const { return false; }

    // Must never issue motion, capture-start, generator-enable, configuration
    // writes or experiment commands. Exceptions become ProviderException.
    virtual ProviderResult discover(const DiscoveryRequest& request,
                                    const ProviderContext& context) = 0;
};

} // namespace backend::discovery
