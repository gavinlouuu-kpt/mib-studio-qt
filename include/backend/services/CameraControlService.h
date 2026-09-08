#pragma once

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

    // Compile-time SDK availability. UIs use these to distinguish "no
    // hardware found" from "support not included in this build" — official
    // CI builds ship with both vendor SDKs disabled (issue #338), so an
    // empty discovery result alone is ambiguous.
    static bool eGrabberSupported();
    static bool mindVisionSupported();

    std::vector<DiscoveredCamera> discoverCameras();
    std::vector<DiscoveredCamera> discoverMindVisionCameras();
    std::vector<DiscoveredCamera> discoverAllCameras();
    std::vector<DiscoveredFramegrabber> discoverFramegrabbers();

    bool applyScriptToDevice(int interfaceIndex,
                             int deviceIndex,
                             const std::string& scriptPath,
                             std::string* errorOut = nullptr);

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

