#include "backend/discovery/providers/Zc300Provider.h"

#include "backend/services/ModbusRtu.h"
#include "backend/services/SerialBus.h"
#include "backend/stage/zc300/Zc300Protocol.h"

#include <string>

namespace backend::discovery {

namespace serialbus = services::serialbus;
namespace zc300 = stage::zc300;

namespace {

ErrorKind mapBusError(serialbus::BusError error)
{
    switch (error) {
    case serialbus::BusError::PortBusy: return ErrorKind::Busy;
    case serialbus::BusError::PortUnavailable:
    case serialbus::BusError::NotOpen:
    case serialbus::BusError::WriteFailed: return ErrorKind::OpenFailed;
    case serialbus::BusError::Timeout: return ErrorKind::Timeout;
    default: return ErrorKind::MalformedResponse;
    }
}

} // namespace

Zc300Provider::Zc300Provider(serialbus::SerialBusManager& bus) : bus_(bus) {}

ProviderResult Zc300Provider::discover(const DiscoveryRequest& request, const ProviderContext& context)
{
    ProviderResult result;
    if (!request.serialScope || request.serialScope->portName.empty()) {
        result.complete = false;
        result.errors.push_back({kId, ErrorKind::InvalidRequest,
                                 "Z stage scans need an explicit port and address range; broad sweeps are "
                                 "not performed",
                                 ""});
        return result;
    }
    const auto& scope = *request.serialScope;
    services::SerialSettings settings = scope.settings;
    settings.baudRate = zc300::kBaudRate;
    settings.dataBits = 8;
    settings.parity = 'N';
    settings.stopBits = 1;

    serialbus::BusError busError = serialbus::BusError::None;
    std::string busDetail;
    const auto bus = bus_.acquire(scope.portName, settings, &busError, &busDetail);
    if (!bus) {
        result.complete = false;
        result.errors.push_back({kId, mapBusError(busError),
                                 "could not open " + scope.portName + ": " + serialbus::toString(busError) +
                                     (busDetail.empty() ? "" : " (" + busDetail + ")"),
                                 scope.portName});
        return result;
    }

    // USB identity of the adapter, so the stage can be re-found after a
    // ttyUSB rename (identity itself comes from the controller below).
    services::SerialPortInfo adapter;
    for (const auto& port : serialbus::availablePorts()) {
        if (serialbus::SerialBusManager::samePort(port.systemName, scope.portName)) adapter = port;
    }

    for (int address = scope.addressFrom; address <= scope.addressTo; ++address) {
        if (context.cancelled && context.cancelled()) {
            result.complete = false;
            result.errors.push_back({kId, ErrorKind::Cancelled, "Z stage scan cancelled", scope.portName});
            break;
        }
        const auto addr = static_cast<std::uint8_t>(address);
        const auto model = bus->transact(zc300::buildRead(addr, zc300::kRegModel, 7), scope.perAddressTimeoutMs);
        if (model.error == serialbus::BusError::Timeout) continue; // nothing at this address

        DiscoveredDevice d;
        d.kind = DeviceKind::MotionStage;
        d.providerId = kId;
        d.endpoint.systemPath = scope.portName;
        d.endpoint.busAddress = address;
        d.endpoint.persistentId = adapter.serialNumber;
        if (adapter.vendorId || adapter.productId) {
            d.endpoint.vendorId = adapter.vendorId;
            d.endpoint.productId = adapter.productId;
        }
        services::modbus::Frame data;
        if (model.error != serialbus::BusError::None ||
            !services::modbus::extractReadData(model.response, 7, data)) {
            d.displayName = "Address " + std::to_string(address) + " — Modbus device (not a ZC300, left untouched)";
            d.stableIdentity = scope.portName + "@" + std::to_string(address);
            d.identityStrength = IdentityStrength::SessionLocal;
            d.identification = IdentificationStatus::Unidentified;
            if (model.error != serialbus::BusError::ModbusException) {
                d.diagnostics.push_back({ErrorKind::MalformedResponse, serialbus::toString(model.error)});
            }
            result.candidates.push_back(std::move(d));
            continue;
        }
        const std::string modelName = zc300::decodeAscii(data);
        if (modelName.rfind("ZC300", 0) != 0) {
            d.displayName = "Address " + std::to_string(address) + " — '" + modelName + "' (not a ZC300)";
            d.stableIdentity = scope.portName + "@" + std::to_string(address);
            d.identityStrength = IdentityStrength::SessionLocal;
            d.identification = IdentificationStatus::Unidentified;
            result.candidates.push_back(std::move(d));
            continue;
        }
        const auto ident = bus->transact(zc300::buildRead(addr, zc300::kRegSerial, 3), scope.perAddressTimeoutMs);
        services::modbus::Frame id;
        if (ident.error != serialbus::BusError::None || !services::modbus::extractReadData(ident.response, 3, id)) {
            d.displayName = modelName + " at address " + std::to_string(address) + " (serial number unreadable)";
            d.stableIdentity = scope.portName + "@" + std::to_string(address);
            d.identityStrength = IdentityStrength::SessionLocal;
            d.identification = IdentificationStatus::Unidentified;
            d.diagnostics.push_back({ErrorKind::MalformedResponse, serialbus::toString(ident.error)});
            result.candidates.push_back(std::move(d));
            continue;
        }
        const std::string serial = std::to_string(zc300::decodeLong(id.data()));
        const std::uint16_t fw = zc300::decodeU16(id.data() + 4);
        const std::string firmware = std::to_string(fw / 10) + "." + std::to_string(fw % 10);
        d.displayName = modelName + " s/n " + serial + " (fw " + firmware + ") at address " + std::to_string(address);
        d.stableIdentity = "zc300:" + serial;
        d.identityStrength = IdentityStrength::Persistent;
        d.identification = IdentificationStatus::Identified;
        d.claimedBy = {kId};
        d.capabilities = {"model:" + modelName, "serial:" + serial, "firmware:" + firmware};
        result.candidates.push_back(std::move(d));
    }
    return result;
}

} // namespace backend::discovery
