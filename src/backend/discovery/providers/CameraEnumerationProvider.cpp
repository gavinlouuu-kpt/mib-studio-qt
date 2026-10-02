#include "backend/discovery/providers/CameraEnumerationProvider.h"

#include <utility>

namespace backend::discovery {

std::unique_ptr<CameraEnumerationProvider>
CameraEnumerationProvider::cameras(std::string id, services::CameraType type,
                                   CameraEnumerator enumerate, bool sdkAvailable)
{
    std::unique_ptr<CameraEnumerationProvider> p(new CameraEnumerationProvider());
    p->id_ = std::move(id);
    p->kind_ = DeviceKind::Camera;
    p->type_ = type;
    p->cameras_ = std::move(enumerate);
    p->sdkAvailable_ = sdkAvailable;
    return p;
}

std::unique_ptr<CameraEnumerationProvider>
CameraEnumerationProvider::framegrabbers(std::string id, FramegrabberEnumerator enumerate,
                                         bool sdkAvailable)
{
    std::unique_ptr<CameraEnumerationProvider> p(new CameraEnumerationProvider());
    p->id_ = std::move(id);
    p->kind_ = DeviceKind::Framegrabber;
    p->framegrabbers_ = std::move(enumerate);
    p->sdkAvailable_ = sdkAvailable;
    return p;
}

std::unique_ptr<CameraEnumerationProvider>
CameraEnumerationProvider::mindVision(services::CameraControlService& control)
{
    return cameras(kMindVisionId, services::CameraType::MindVision,
                   [&control] { return control.discoverMindVisionCameras(); },
#if MIB_HAS_MINDVISION
                   true
#else
                   false
#endif
    );
}

std::unique_ptr<CameraEnumerationProvider>
CameraEnumerationProvider::eGrabber(services::CameraControlService& control)
{
    return cameras(kEGrabberId, services::CameraType::EGrabber,
                   [&control] { return control.discoverCameras(); },
#if MIB_HAS_EGRABBER
                   true
#else
                   false
#endif
    );
}

std::unique_ptr<CameraEnumerationProvider>
CameraEnumerationProvider::eGrabberFramegrabbers(services::CameraControlService& control)
{
    return framegrabbers(kEGrabberFramegrabberId,
                         [&control] { return control.discoverFramegrabbers(); },
#if MIB_HAS_EGRABBER
                         true
#else
                         false
#endif
    );
}

ProviderResult CameraEnumerationProvider::discover(const DiscoveryRequest&, const ProviderContext&)
{
    ProviderResult result;
    if (!sdkAvailable_) {
        result.complete = false;
        result.errors.push_back({id_, ErrorKind::MissingSdk,
                                 "camera SDK is not compiled into this build", ""});
        return result;
    }

    if (kind_ == DeviceKind::Framegrabber) {
        if (!framegrabbers_) return result;
        for (auto& fg : framegrabbers_()) {
            DiscoveredDevice d;
            d.kind = DeviceKind::Framegrabber;
            d.providerId = id_;
            d.displayName = fg.label;
            d.endpoint.interfaceIndex = fg.interfaceIndex;
            d.endpoint.deviceIndex = fg.deviceIndex;
            d.endpoint.streamIndex = fg.streamIndex;
            if (!fg.interfaceID.empty() && !fg.deviceID.empty()) {
                d.stableIdentity = fg.interfaceID + "/" + fg.deviceID + "/" + fg.streamID;
                d.identityStrength = IdentityStrength::Persistent;
            } else {
                d.identityStrength = IdentityStrength::SessionLocal;
            }
            d.identification = IdentificationStatus::Identified;
            d.claimedBy = {id_};
            if (!fg.modelName.empty()) d.capabilities.push_back("model:" + fg.modelName);
            d.framegrabber = std::move(fg);
            result.candidates.push_back(std::move(d));
        }
        return result;
    }

    if (!cameras_) return result;
    for (auto& cam : cameras_()) {
        if (cam.cameraType != type_) continue;
        DiscoveredDevice d;
        d.kind = DeviceKind::Camera;
        d.providerId = id_;
        d.displayName = cam.label;
        d.endpoint.sdkIndex = cam.cameraIndex;
        d.endpoint.interfaceIndex = cam.interfaceIndex;
        d.endpoint.deviceIndex = cam.deviceIndex;
        if (cam.cameraType == services::CameraType::EGrabber && !cam.interfaceID.empty() &&
            !cam.deviceID.empty()) {
            // GenTL interface/device IDs survive re-enumeration.
            d.stableIdentity = cam.interfaceID + "/" + cam.deviceID;
            d.identityStrength = IdentityStrength::Persistent;
        } else {
            // The MindVision enumeration index is an SDK ordinal: valid for
            // this session only and never persisted as identity.
            d.identityStrength = IdentityStrength::SessionLocal;
        }
        d.identification = IdentificationStatus::Identified;
        d.claimedBy = {id_};
        if (!cam.modelName.empty()) d.capabilities.push_back("model:" + cam.modelName);
        if (!cam.firmwareVersion.empty() && cam.firmwareVersion != "Unknown") {
            d.capabilities.push_back("firmware:" + cam.firmwareVersion);
        }
        d.camera = std::move(cam);
        result.candidates.push_back(std::move(d));
    }
    return result;
}

} // namespace backend::discovery
