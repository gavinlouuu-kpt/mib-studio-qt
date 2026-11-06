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
#include <vector>

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

// The eGrabber camera's window steps and limits, as the Qt Overview tab hard-codes them
// (EgrabberConfigParser ROI_*_STEP, OverviewTab: W 64..1920, H 16..1080): offset X in steps of
// 16, Y of 4, width of 16, height of 4. The sensor is the camera's reported size when the
// backend knows it, else 1920x1080 (what Qt assumes).
constexpr int kEgrabberOffsetXStep = 16;
constexpr int kEgrabberOffsetYStep = 4;
constexpr int kEgrabberWidthStep = 16;
constexpr int kEgrabberHeightStep = 4;
constexpr int kEgrabberMinWidth = 64;
constexpr int kEgrabberMinHeight = 16;
constexpr int kEgrabberDefaultSensorWidth = 1920;
constexpr int kEgrabberDefaultSensorHeight = 1080;

// The window Qt's Overview tab keeps from a requested one (OverviewTab::onRoiSizeChanged and
// EgrabberConfigParser::updateRoiOffsets): the size is snapped DOWN to the width/height steps and
// clamped to 64..sensor x 16..sensor; the offsets are snapped down to their steps (not below 0)
// and an offset that would push the window off the sensor is pulled back to the last step that
// fits.
::camera::common::CameraWindow snapEgrabberWindow(const ::camera::common::CameraWindow& requested,
                                                  int sensorWidth = kEgrabberDefaultSensorWidth,
                                                  int sensorHeight = kEgrabberDefaultSensorHeight);

// The camera script with its Width/Height/OffsetX/OffsetY lines set to `window`. Same line shape
// as parseScriptCameraWindow and Qt's EgrabberConfigParser (`g.RemotePort.set("Width", 512);`):
// every active (not commented) line of a feature gets the number, the rest of the line (indent,
// receiver, trailing text, line ending) is kept; a feature without an active line is left alone
// (Qt never adds lines) and named in `missing`.
struct ScriptWindowRewrite {
    std::string text;
    std::vector<std::string> rewritten; // "Width", "Height", "OffsetX", "OffsetY" with an active line
    std::vector<std::string> missing;   // the features that have none
};
ScriptWindowRewrite rewriteScriptCameraWindow(const std::string& scriptText,
                                              const ::camera::common::CameraWindow& window);

// The egrabberConfig.js beside the applied local profile's config.json (ProfileStore::apply records the
// profile's config.json path in the `profile_selection` of the applied configuration text); empty when
// the applied configuration is not a local profile. The Experiment script the desktop falls back to when
// the operator chose none, and where the Overview reads the script's window before any script was applied.
std::string appliedProfileScriptPath(const std::string& appliedConfigJson);

// Which window the Overview shows for an eGrabber camera and where it came from (#846): the
// applied profile's camera.window wins (it is applied after the Experiment script), else the
// Experiment script's own window, else none.
enum class WindowSource { None, Script, Profile };
const char* toString(WindowSource source);
struct ChosenWindow {
    ::camera::common::CameraWindow window;
    WindowSource source{WindowSource::None};
};
ChosenWindow chooseEgrabberWindow(const std::optional<ProfileCameraWindow>& profile,
                                  const std::optional<::camera::common::CameraWindow>& script);

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
    // The camera's sensor size read while the script ran (0 = the camera did not say, #846).
    int sensorWidth{0}, sensorHeight{0};
};

} // namespace backend::app
