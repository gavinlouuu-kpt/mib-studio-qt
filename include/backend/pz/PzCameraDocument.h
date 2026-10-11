#pragma once

// The PZ7035 camera document (bridge ABI 38): the JSON file edited in Config -> Script and applied to the instrument, the way an EGrabber script is applied
// to a MIB camera. It sets the Run frame rate and exposure only:
//
//   { "schema": "pz7035-camera/1", "run": { "frame_rate_hz": 5000, "exposure_us": "auto" } }
//
// frame_rate_hz: a whole number from 1000 to 5000 (default 5000). exposure_us: "auto" (SHS 64: 150 us at 5000 fps, about 950 us at 1000 fps) or a number of
// microseconds that starts the exposure inside the LED strobe window (PzRunTiming.h). The LED strobe is Service-only: no strobe, LED, delay or width key is
// accepted. Unknown keys are errors, so a typo cannot silently leave a setting unchanged.

#include "backend/pz/PzRunTiming.h"

#include <string>

namespace backend::pz {

inline constexpr std::size_t kCameraDocumentMaxBytes = 64 * 1024;

struct CameraDocumentResult {
    bool ok{false};
    std::string error;  // the first problem, naming the key
    RunSetting setting;
};

// Parses and validates `text` against the Run preset's strobe window (7 us delay, 60 us width).
CameraDocumentResult parseCameraDocument(const std::string& text, double strobeDelayUs, double strobeWidthUs);

} // namespace backend::pz
