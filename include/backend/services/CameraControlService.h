#pragma once

#include "backend/app/CameraWindowProfile.h"

#include <optional>
#include <string>
#include <vector>

namespace backend::services {

enum class CameraType {
    EGrabber,
    MindVision,
};

struct DiscoveredCamera {
    CameraType cameraType = CameraType::EGrabber;
    int cameraIndex = -1; // MindVision index; -1 for EGrabber entries.
    int interfaceIndex = -1;
    int deviceIndex = -1;
    std::string interfaceID;
    std::string deviceID;
    std::string modelName;
    std::string firmwareVersion; // "Unknown" if not available
    std::string label; // interfaceID/deviceID (model) [Firmware: version]
};

struct DiscoveredFramegrabber {
    int interfaceIndex = -1;
    int deviceIndex = -1;
    int streamIndex = -1;
    std::string interfaceID;
    std::string deviceID;
    std::string streamID;
    std::string modelName;
    std::string label; // interfaceID/deviceID/streamID (model)
};

/**
 * Camera utility service for:
 *  - Enumerating available cameras
 *  - Applying a JS configuration script to a specific device
 *
 * Note: This service does not own or interact with the CaptureService thread.
 */
class CameraControlService {
public:
    CameraControlService() = default;
    ~CameraControlService() = default;

    std::vector<DiscoveredCamera> discoverCameras();
    std::vector<DiscoveredCamera> discoverMindVisionCameras();
    std::vector<DiscoveredCamera> discoverAllCameras();
    std::vector<DiscoveredFramegrabber> discoverFramegrabbers();

    // Runs the camera script, then stops acquisition. With `window` (the
    // profile's camera window, #833) the window is written to the camera
    // through the same handle afterwards, so the profile wins over the
    // script's Width/Height/Offset lines; `report` receives what the script
    // had set, what was applied, a model refusal or the SDK error. The return
    // value is the script's: a refused or rejected window is reported, and the
    // camera.window readiness gate blocks Start on it.
    bool applyScriptToDevice(int interfaceIndex,
                             int deviceIndex,
                             const std::string& scriptPath,
                             std::string* errorOut = nullptr,
                             const backend::app::ProfileCameraWindow* window = nullptr,
                             backend::app::CameraWindowApplyReport* report = nullptr);

    bool applyMindVisionConfig(int cameraIndex,
                               const std::string& jsonPath,
                               std::string* errorOut = nullptr);

    // Issue GenICam SFNC DeviceReset to a specific device.
    // Best effort stops acquisition first; returns true on success.
    bool deviceReset(int interfaceIndex,
                     int deviceIndex,
                     std::string* errorOut = nullptr);
};

} // namespace backend::services

