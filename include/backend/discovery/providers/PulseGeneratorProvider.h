// Pulse-generator discovery provider (issue #419, ADR 0005).
//
// Wraps PulseGeneratorService::scanBus: an explicit, read-only (FC03 only)
// Modbus identity scan over one named port and address range through the
// shared SerialBusManager session. The scope must be explicit
// (DiscoveryRequest::serialScope); a request without it is refused so no
// broad serial sweep can ever start. Cancellation is honoured between
// addresses; busy/unopenable ports and incompatible settings are structured
// errors. Never connects or enables outputs.
#pragma once

#include "backend/discovery/IDeviceDiscoveryProvider.h"
#include "backend/services/PulseGeneratorService.h"

#include <functional>
#include <string>
#include <vector>

namespace backend::discovery {

class PulseGeneratorProvider final : public IDeviceDiscoveryProvider {
public:
    using Scan = std::function<std::vector<services::PulseGeneratorService::ScanHit>(
        const std::string& portName, const services::SerialSettings& settings, std::uint8_t from,
        std::uint8_t to, const std::function<bool()>& cancelled, int perAddressTimeoutMs,
        services::PulseGeneratorService::LinkError* error)>;

    explicit PulseGeneratorProvider(services::PulseGeneratorService& service);
    explicit PulseGeneratorProvider(Scan scan);

    static constexpr const char* kId = "pulse-generator";

    std::string id() const override { return kId; }
    DeviceKind kind() const override { return DeviceKind::PulseGenerator; }
    std::string resourceClass() const override { return "serial-probe"; }
    bool cancellable() const override { return true; }
    ProviderResult discover(const DiscoveryRequest& request, const ProviderContext& context) override;

    static ErrorKind mapLinkError(services::PulseGeneratorService::LinkError error);

private:
    Scan scan_;
};

} // namespace backend::discovery
