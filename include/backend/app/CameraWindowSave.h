// Saving an eGrabber camera window (#863, Qt OverviewTab::onRoiSizeChanged / updateEgrabberConfigFromRect
// / AppConfigWatcher::writeBackCameraWindow). Qt-free.
//
// The operator drags or sizes the window on the full-sensor Overview. This is what that edit does:
//   1. the window is snapped as Qt snaps it (snapEgrabberWindow: sizes down to 16/4 and clamped to
//      64..sensor x 16..sensor, offsets down to 16/4 and pulled back onto the sensor);
//   2. the Experiment script's Width/Height/OffsetX/OffsetY lines are rewritten in place;
//   3. when the applied configuration has a camera.window, the in-memory acquisition profile takes
//      the window, and when that configuration is a local profile its config.json follows, with the
//      revision the profile store remembers (profileStoreSaveCameraWindow).
// Refused while an experiment runs or raw recording is on. Nothing here touches the camera: the
// window reaches it with the next Experiment script apply.
#pragma once

#include "backend/app/ProfileStore.h"

#include <string>
#include <vector>

namespace backend {
class AppBackend;
}

namespace backend::app {

struct CameraWindowSaveResult {
    bool ok{false};
    std::string error;                 // why nothing was saved (ok == false)
    ::camera::common::CameraWindow requested;
    ::camera::common::CameraWindow window; // as snapped and saved
    int sensorWidth{0}, sensorHeight{0};   // what the snap used
    std::string scriptPath;            // the Experiment script ("" = none was given)
    bool scriptUpdated{false};         // at least one line was rewritten
    bool scriptChanged{false};         // the file's text changed
    std::vector<std::string> scriptMissing; // features the script has no active line for
    ProfileWindowEdit profile;
};

// `scriptPath` may be empty only when the applied configuration has a camera.window (the profile
// then is where the window lives). A path must be an existing regular .js file of at most 4 MiB.
CameraWindowSaveResult saveEgrabberWindow(AppBackend& backend, const std::string& scriptPath,
                                          const ::camera::common::CameraWindow& requested);

// The reply of the bridge's save_camera_window.
std::string toJson(const CameraWindowSaveResult& result);

} // namespace backend::app
