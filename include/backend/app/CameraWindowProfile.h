// The camera window (cROI) a profile carries (#833). Qt-free.
//
// config.json section:
//
//   "camera": {
//     "window": {
//       "width": 1216, "height": 208, "offset_x": 368, "offset_y": 616,
//       "camera_vendor": "SVS-Vistek GmbH", "camera_model": "MC2066"
//     }
//   }
//
// width/height/offset_x/offset_y are sensor pixels (the GenICam Width, Height,
// OffsetX, OffsetY). camera_vendor/camera_model name the camera the window was
// made for (the remote device's DeviceVendorName / DeviceModelName); a window
// for another model is refused, not clipped. Both are optional: a window that
// names no camera is applied and flagged by the camera.window gate.
//
// Precedence with the camera script (egrabberConfig.js also sets Width/Height/
// OffsetX/OffsetY): the experiment script runs first, then the profile's window
// is written to the camera through the same handle, so the profile wins. What
// the script had set is read back before the window is written; when the two
// disagree the apply is logged and the camera.window gate warns with both
// windows. Saving a profile writes the window into the section and the same
// values into the script's Width/Height/Offset lines, so they agree again.
// A profile without the section leaves the window to the script (as before).
#pragma once

#include "backend/camera/common/ICamera.h"

#include <cstdint>
#include <optional>
#include <string>

namespace backend::app {

struct ProfileCameraWindow {
    ::camera::common::CameraWindow window;
    std::string cameraVendor; // the camera the window was made for; empty = not recorded
    std::string cameraModel;
    bool namesCamera() const { return !cameraModel.empty(); }
};

// The profile values the experiment's acquisition depends on (#833).
struct AcquisitionProfile {
    std::optional<ProfileCameraWindow> window; // camera.window
    double targetFps{0.0};                     // target_fps; 0 = not set
};

// Reads camera.window and target_fps from a config.json document. Throws
// std::runtime_error with the reason when a present section is malformed
// (missing or non-integer size, negative offset, zero size); absent sections
// give an empty window / a zero rate.
AcquisitionProfile parseAcquisitionProfile(const std::string& configJson);

// Width/Height/OffsetX/OffsetY from an eGrabber camera script's
// `g.RemotePort.set("<Feature>", <n>);` lines (comment lines skipped; the last
// assignment wins). nullopt unless at least Width and Height are set.
std::optional<::camera::common::CameraWindow> parseScriptCameraWindow(const std::string& scriptText);

// Non-empty when the window was made for another camera than the connected one
// (case-sensitive model; the vendor is compared only when both are known).
// Empty when the window names no camera or the connected model is unknown.
std::string cameraModelMismatch(const ProfileCameraWindow& profile, const std::string& connectedVendor,
                                const std::string& connectedModel);

// "1216x208 at (368, 616)"
std::string describeWindow(const ::camera::common::CameraWindow& window);
// "SVS-Vistek GmbH MC2066", or "an unnamed camera"
std::string describeCamera(const std::string& vendor, const std::string& model);

// What applying the profile window after the camera script did (AppBackend::
// applyCameraScriptFromFile, Experiment role). Kept for the camera.window gate.
struct CameraWindowApplyReport {
    uint64_t sequence{0};       // increments with each script apply (0 = never applied)
    bool requested{false};      // the profile had a window when the script was applied
    bool applied{false};        // the window is in the camera
    ProfileCameraWindow profile;
    ::camera::common::CameraWindow scriptWindow; // what the camera had after the script
    bool scriptWindowKnown{false};
    ::camera::common::CameraWindow result;       // read back after the apply
    bool resultKnown{false};
    std::string connectedVendor, connectedModel;
    std::string refusal; // the window was made for another camera
    std::string error;   // the camera rejected the window (SDK error)
};

} // namespace backend::app
