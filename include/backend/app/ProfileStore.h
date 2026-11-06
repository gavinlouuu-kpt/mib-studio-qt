#pragma once
#include "backend/app/CameraWindowProfile.h"

#include <functional>
#include <string>
namespace backend {
class AppBackend;
}
namespace backend::app {
// Portable local profile files; Qt-compatible config.json/egrabberConfig.js layout.
// Caller serializes mutations. Revision covers all three managed files.
std::string profileStoreCommand(AppBackend& backend, const std::string& base,
                                const std::string& request);

// What an eGrabber camera-window edit did to the applied configuration (#863; Qt
// AppConfigWatcher::writeBackCameraWindow).
struct ProfileWindowEdit {
    bool configHasWindow{false}; // the applied configuration carries a camera.window
    bool localProfile{false};    // ...and it came from a local profile that is still where it was applied from
    bool memoryUpdated{false};   // the in-memory acquisition profile now holds the window
    bool configUpdated{false};   // the profile's config.json camera.window now holds it
    std::string profileName;
    std::string reason;          // why config.json was not rewritten ("" when it was, or nothing was to be)
};

// The window edit's persistence, in one transaction (the caller holds the coordinator's idle
// configuration): `writeScript` rewrites the Experiment script (it may live in the profile folder, so
// it runs after the profile's recorded revision was checked and before config.json is written; an
// exception from it leaves everything else unchanged). Then, only when the applied configuration
// already has a camera.window (Qt writes an existing section only), the in-memory acquisition profile
// takes the window and the recorded configuration text shows it; and when that configuration is a local
// profile whose files are still the ones it applied, its config.json camera.window is rewritten
// atomically. Whenever the profile's files changed (config.json, or the script in its folder) the
// revision the store remembers (.selection.json, the startup pointer, the runtime profile_selection)
// follows, so that the next start restores the edited profile instead of refusing a "changed" one.
ProfileWindowEdit profileStoreSaveCameraWindow(AppBackend& backend, const ::camera::common::CameraWindow& window,
                                               const std::function<void()>& writeScript);
} // namespace backend::app
