#include "backend/discovery/providers/PulseGeneratorProvider.h"

#include <string>
#include <utility>

namespace backend::discovery {

using Service = services::PulseGeneratorService;

PulseGeneratorProvider::PulseGeneratorProvider(Service& service)
    : scan_([&service](const std::string& port, const services::SerialSettings& settings,
                       std::uint8_t from, std::uint8_t to, const std::function<bool()>& cancelled,
                       int timeoutMs, Service::LinkError* error) {
          return service.scanBus(port, settings, from, to, cancelled, timeoutMs, error);
      })
{
}

PulseGeneratorProvider::PulseGeneratorProvider(Scan scan) : scan_(std::move(scan)) {}

ErrorKind PulseGeneratorProvider::mapLinkError(Service::LinkError error)
{
    switch (error) {
    case Service::LinkError::None: return ErrorKind::None;
    case Service::LinkError::PortUnavailable: return ErrorKind::OpenFailed;
    case Service::LinkError::PortBusy: return ErrorKind::Busy;
    case Service::LinkError::Timeout: return ErrorKind::Timeout;
    case Service::LinkError::CrcFrameError: return ErrorKind::MalformedResponse;
    case Service::LinkError::ModbusException: return ErrorKind::MalformedResponse;
    case Service::LinkError::AddressCollision: return ErrorKind::MalformedResponse;
    case Service::LinkError::IncompatibleDevice: return ErrorKind::Unsupported;
    case Service::LinkError::NotConnected: return ErrorKind::OpenFailed;
    case Service::LinkError::WriteFailed: return ErrorKind::OpenFailed;
    }
    return ErrorKind::ProviderException;
}

ProviderResult PulseGeneratorProvider::discover(const DiscoveryRequest& request,
                                                const ProviderContext& context)
{
    ProviderResult result;
    if (!request.serialScope || request.serialScope->portName.empty()) {
        result.complete = false;
        result.errors.push_back({kId, ErrorKind::InvalidRequest,
                                 "pulse-generator scans need an explicit port, serial settings and "
                                 "address range; broad sweeps are not performed",
                                 ""});
        return result;
    }
    if (!scan_) {
        result.complete = false;
        result.errors.push_back({kId, ErrorKind::Unsupported, "no scan implementation", ""});
        return result;
    }

    const auto& scope = *request.serialScope;
    bool cancelled = false;
    auto cancelledToken = [&]() {
        if (context.cancelled && context.cancelled()) {
            cancelled = true;
            return true;
        }
        return false;
    };
    Service::LinkError linkError = Service::LinkError::None;
    const auto hits = scan_(scope.portName, scope.settings, scope.addressFrom, scope.addressTo,
                            cancelledToken, scope.perAddressTimeoutMs, &linkError);

    if (linkError != Service::LinkError::None) {
        result.complete = false;
        result.errors.push_back({kId, mapLinkError(linkError),
                                 std::string("could not scan ") + scope.portName + ": " +
                                     Service::toString(linkError),
                                 scope.portName});
        return result;
    }

    for (const auto& hit : hits) {
        DiscoveredDevice d;
        d.kind = DeviceKind::PulseGenerator;
        d.providerId = kId;
        d.endpoint.systemPath = scope.portName;
        d.endpoint.busAddress = hit.address;
        // A Modbus slave has no serial number: identity is the (port,
        // address) pair for this session only.
        d.stableIdentity = scope.portName + "@" + std::to_string(hit.address);
        d.identityStrength = IdentityStrength::SessionLocal;
        switch (hit.kind) {
        case Service::ScanHit::Kind::PulseGenerator:
            d.displayName = "Address " + std::to_string(hit.address) + " — pulse generator";
            d.identification = IdentificationStatus::Identified;
            d.claimedBy = {kId};
            d.capabilities.push_back("channels:" + std::to_string(Service::CHANNEL_COUNT));
            break;
        case Service::ScanHit::Kind::ModbusDevice:
            d.displayName = "Address " + std::to_string(hit.address) +
                            " — Modbus device (not a pulse generator, left untouched)";
            d.identification = IdentificationStatus::Unidentified;
            break;
        case Service::ScanHit::Kind::Error:
            d.displayName = "Address " + std::to_string(hit.address) +
                            " — corrupt/inconsistent response (possible duplicate-address collision)";
            d.identification = IdentificationStatus::Unidentified;
            d.diagnostics.push_back({ErrorKind::MalformedResponse,
                                     "corrupt or inconsistent Modbus response"});
            break;
        }
        d.pulseGenerator = hit;
        result.candidates.push_back(std::move(d));
    }

    if (cancelled) {
        result.complete = false;
        result.errors.push_back({kId, ErrorKind::Cancelled, "pulse-generator scan cancelled",
                                 scope.portName});
    }
    return result;
}

} // namespace backend::discovery
