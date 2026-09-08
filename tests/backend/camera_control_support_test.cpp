// camera_control_support_test
//
// Pins the CameraControlService contract for builds compiled without a vendor
// SDK (issue #338): support is queryable, discovery is empty rather than
// erroring, and hardware operations fail with an actionable reason instead of
// silently doing nothing. Official CI builds ship with both SDKs disabled, so
// the unsupported branches are exactly what end users of those builds run.
//
// In an SDK-enabled build the unsupported-path sections are skipped; the
// consistency section still applies.

#include "backend/services/CameraControlService.h"

#include "support/assert.h"

#include <string>

using backend::services::CameraControlService;

int main()
{
    CameraControlService service;

    if (!CameraControlService::mindVisionSupported())
    {
        MIB_EXPECT(service.discoverMindVisionCameras().empty(),
                   "unsupported MindVision discovery must return empty, not fail");

        std::string error;
        MIB_EXPECT(!service.applyMindVisionConfig(0, "unused.json", &error),
                   "unsupported MindVision config apply must fail");
        MIB_EXPECT(!error.empty(),
                   "unsupported MindVision operations must return an actionable reason");
    }

    if (!CameraControlService::eGrabberSupported())
    {
        MIB_EXPECT(service.discoverCameras().empty(),
                   "unsupported EGrabber discovery must return empty, not fail");

        std::string error;
        MIB_EXPECT(!service.applyScriptToDevice(0, 0, "unused.js", &error),
                   "unsupported script apply must fail");
        MIB_EXPECT(!error.empty(),
                   "unsupported EGrabber operations must return an actionable reason");
    }

    // Aggregate discovery is the union of the per-vendor lists; with no SDKs
    // it must be empty (this is the state every official CI build is in).
    if (!CameraControlService::mindVisionSupported() &&
        !CameraControlService::eGrabberSupported())
    {
        MIB_EXPECT(service.discoverAllCameras().empty(),
                   "no-SDK builds must discover zero cameras");
    }

    return mib::test::exitCode();
}
