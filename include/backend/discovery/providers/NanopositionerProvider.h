// Nanopositioner discovery provider (issue #419, ADR 0005).
//
// Wraps the existing enumeration (AutofocusService::availableEndpoints) and
// read-only identity probe (AutofocusService::probeEndpoint) through
// backend::services::nanopositioner::discover. Moves the pre-#419
// DeviceInitManager worker rules into the backend: the preferred endpoint is
// probed first, a vendor filter restricts probing, saved CoreMorrow serial
// settings apply to every endpoint, unidentified adapters stay in the
// inventory, conflicting vendor identities are ambiguous, and cancellation is
// honoured between endpoints. Never connects persistently or moves a stage.
#pragma once

#include "backend/discovery/IDeviceDiscoveryProvider.h"
#include "backend/nanopositioner/INanopositionerBackend.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backend::discovery {

class NanopositionerProvider final : public IDeviceDiscoveryProvider {
public:
    using Enumerate = std::function<std::vector<nanopositioner::Endpoint>()>;
    using Probe = std::function<bool(const nanopositioner::Endpoint&)>;

    NanopositionerProvider(Enumerate enumerate, Probe probe);
    // Production wiring over AutofocusService's static enumeration/probe.
    static std::unique_ptr<NanopositionerProvider> production();

    static constexpr const char* kId = "nanopositioner";

    std::string id() const override { return kId; }
    DeviceKind kind() const override { return DeviceKind::Nanopositioner; }
    // Serial probes open ports exclusively; keep them serialized with the
    // pulse-generator scan which shares the same adapters.
    std::string resourceClass() const override { return "serial-probe"; }
    bool cancellable() const override { return true; }
    ProviderResult discover(const DiscoveryRequest& request, const ProviderContext& context) override;

private:
    Enumerate enumerate_;
    Probe probe_;
};

} // namespace backend::discovery
