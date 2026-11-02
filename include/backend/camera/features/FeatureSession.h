#pragma once

#include "backend/camera/features/FeatureControl.h"

#include <functional>
#include <string>

// E02 (E-2): the optional GenApi capability of a camera. A camera that owns a
// node map returns one of these from ICamera::featureSession(); a camera
// without GenApi returns nullptr so callers report "unsupported", never an
// empty tree. The object is owned by the camera and lives as long as it.
namespace camera::features {

class FeatureSession {
public:
    virtual ~FeatureSession() = default;

    // Stable identity of this camera instance's source (part of every token).
    virtual std::string source() const = 0;
    // "device" for a real node map, "fixture/mock" for a fake one.
    virtual std::string evidence() const = 0;

    // Runs `fn` with the port while holding the camera's own port guard (the
    // lock that pins the SDK handle and serializes node-map access with the
    // camera's other GenApi users). Returns false without calling `fn`, and
    // sets `reason`, when no port is open (camera not started or stopping).
    // `fn` must not call back into the camera.
    virtual bool withPort(const std::function<void(Port&)>& fn, std::string& reason) = 0;
};

} // namespace camera::features
