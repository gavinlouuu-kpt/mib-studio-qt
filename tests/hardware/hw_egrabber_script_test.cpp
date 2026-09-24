// hw_egrabber_script_test  (LABEL: hardware) — runs only against a real EGrabber
// camera. Applies a camera script (the EGrabber LED/strobe control mechanism)
// and asserts it is accepted by the device.
//
// Set MIB_TEST_EGRABBER_SCRIPT to a script file path (e.g. an LED-on script).
// Optional: MIB_TEST_EGRABBER_IF / MIB_TEST_EGRABBER_DEV (default 0/0).
// Skips when MIB_TEST_EGRABBER_SCRIPT is absent or on a non-EGrabber build.

#include "backend/app/AppBackend.h"

#include "support/assert.h"
#include "support/hardware.h"
#include "support/tempdir.h"


#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    const char* scriptPath = mib::test::requireDeviceEnv("MIB_TEST_EGRABBER_SCRIPT");

#if !MIB_HAS_EGRABBER
    std::printf("SKIP: built without the EGrabber SDK\n");
    return mib::test::kSkipExitCode;
#else
    const int ifIdx = mib::test::envInt("MIB_TEST_EGRABBER_IF", 0);
    const int devIdx = mib::test::envInt("MIB_TEST_EGRABBER_DEV", 0);

    mib::test::TempDir td("mib_hw_egrabber_script");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "AppBackend initialize");

    backend.setHardwareCameraSelection(ifIdx, devIdx, "egrabber");
    MIB_REQUIRE(backend.isMindVisionCameraSelected() == false, "hardware (EGrabber) selected");

    std::string err;
    const bool ok = backend.applyCameraScriptFromFile(scriptPath, &err);
    MIB_EXPECT(ok, std::string("apply EGrabber LED/camera script: ") + err);

    if (mib::test::exitCode() == 0) std::printf("EGrabber camera-script (LED) applied OK\n");
    return mib::test::exitCode();
#endif
}
