// Camera / framegrabber discovery providers (issue #419, ADR 0005).
//
// Thin wrappers over the existing CameraControlService enumeration: one
// provider per SDK ("mindvision", "egrabber") plus one for eGrabber
// framegrabber streams. The enumerators are injectable so headless tests run
// without vendor SDKs; production wiring uses the static factories below.
// Camera SDK enumeration cannot be interrupted once entered (cancellable()
// is false) and every camera provider shares the "camera-sdk" resource class
// so two jobs never enumerate concurrently (one GenTL handle per process).
#pragma once

#include "backend/discovery/IDeviceDiscoveryProvider.h"
#include "backend/services/CameraControlService.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backend::discovery {

class CameraEnumerationProvider final : public IDeviceDiscoveryProvider {
public:
    using CameraEnumerator = std::function<std::vector<services::DiscoveredCamera>()>;
    using FramegrabberEnumerator = std::function<std::vector<services::DiscoveredFramegrabber>()>;

    // A camera provider owns exactly one CameraType; foreign entries returned
    // by the enumerator are dropped. `sdkAvailable=false` reports MissingSdk
    // without calling the enumerator.
    static std::unique_ptr<CameraEnumerationProvider> cameras(std::string id,
                                                              services::CameraType type,
                                                              CameraEnumerator enumerate,
                                                              bool sdkAvailable = true);
    static std::unique_ptr<CameraEnumerationProvider> framegrabbers(std::string id,
                                                                    FramegrabberEnumerator enumerate,
                                                                    bool sdkAvailable = true);

    // Production wiring over the shared CameraControlService instance.
    static std::unique_ptr<CameraEnumerationProvider> mindVision(services::CameraControlService& control);
    static std::unique_ptr<CameraEnumerationProvider> eGrabber(services::CameraControlService& control);
    static std::unique_ptr<CameraEnumerationProvider>
    eGrabberFramegrabbers(services::CameraControlService& control);

    static constexpr const char* kMindVisionId = "mindvision";
    static constexpr const char* kEGrabberId = "egrabber";
    static constexpr const char* kEGrabberFramegrabberId = "egrabber-framegrabber";

    std::string id() const override { return id_; }
    DeviceKind kind() const override { return kind_; }
    std::string resourceClass() const override { return "camera-sdk"; }
    bool cancellable() const override { return false; }
    ProviderResult discover(const DiscoveryRequest& request, const ProviderContext& context) override;

private:
    CameraEnumerationProvider() = default;

    std::string id_;
    DeviceKind kind_{DeviceKind::Camera};
    services::CameraType type_{services::CameraType::EGrabber};
    CameraEnumerator cameras_;
    FramegrabberEnumerator framegrabbers_;
    bool sdkAvailable_{true};
};

} // namespace backend::discovery
