// Z stage discovery provider (#464, ADR 0013 / ADR 0005).
//
// Identity-only: over one explicitly named port and address range
// (DiscoveryRequest::serialScope), reads the ZC300 model string and, for a
// ZC300, its serial number and firmware with FC04. Never writes a register,
// never moves or homes, never connects StageService. The controller's baud
// rate is fixed at 115200, so the scope's baud rate is overridden. A bare
// Modbus reply from another device is reported Unidentified and left alone.
#pragma once

#include "backend/discovery/IDeviceDiscoveryProvider.h"

#include <string>

namespace backend::services::serialbus {
class SerialBusManager;
}

namespace backend::discovery {

class Zc300Provider final : public IDeviceDiscoveryProvider {
public:
    explicit Zc300Provider(services::serialbus::SerialBusManager& bus);

    static constexpr const char* kId = "zc300-stage";

    std::string id() const override { return kId; }
    DeviceKind kind() const override { return DeviceKind::MotionStage; }
    std::string resourceClass() const override { return "serial-probe"; }
    bool cancellable() const override { return true; }
    ProviderResult discover(const DiscoveryRequest& request, const ProviderContext& context) override;

private:
    services::serialbus::SerialBusManager& bus_;
};

} // namespace backend::discovery
