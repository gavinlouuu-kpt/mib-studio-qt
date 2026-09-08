// camera_script_apply_test
//
// Hardware-independent coverage of the EGrabber camera-script entry point
// (AppBackend::applyCameraScriptFromFile) — the path used to drive the LED /
// strobe on the EGrabber camera. Verifies the precondition guards return cleanly
// with actionable errors and never touch the device for invalid input. The
// actual on-device script run is covered by hardware.egrabber_script.

#include "backend/app/AppBackend.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char* argv[])
{
    // Route the startup LUT lookup to a file: URL so initialize() does no
    // network I/O (keeps this unit test fast/offline).
#ifdef _WIN32
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/lut.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/lut.json", 1);
#endif
    (void)argc;
    (void)argv;

    mib::test::TempDir td("mib_camera_script");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "AppBackend initialize");

    // No camera selected -> clean refusal, no device access.
    {
        std::string err;
        MIB_EXPECT(!backend.applyCameraScriptFromFile((td / "led.js").string(), &err),
                   "apply script with no camera selected returns false");
        MIB_EXPECT(err == "No hardware camera selected", "clear 'no camera' error");
    }

#if MIB_HAS_EGRABBER
    // With a camera selected (SDK build only), a missing script file must fail at
    // the file precheck WITHOUT opening the device.
    {
        backend.setHardwareCameraSelection(0, 0, "test-cam");
        std::string err;
        const std::string missing = (td / "does_not_exist.js").string();
        MIB_EXPECT(!backend.applyCameraScriptFromFile(missing, &err),
                   "apply missing script returns false");
        MIB_EXPECT(err.find("not found") != std::string::npos,
                   "missing-script error names the problem");
    }
#endif

    if (mib::test::exitCode() == 0) {
        std::printf("camera-script (LED) apply guards verified\n");
    }
    return mib::test::exitCode();
}
