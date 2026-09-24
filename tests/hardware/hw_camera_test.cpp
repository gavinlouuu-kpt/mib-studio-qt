// hw_camera_test  (LABEL: hardware) — runs only against a real camera.
//
// Requires a hardware build (EGrabber or MindVision SDK) and an attached
// camera. The operator selects the camera the same way the app does, via env:
//   MIB_CAMERA_MODE=mindvision|hardware  (+ MIB_MINDVISION_CAMERA_INDEX /
//   MIB_MINDVISION_CONFIG for MindVision, or MIB_TEST_EGRABBER_IF /
//   MIB_TEST_EGRABBER_DEV for the EGrabber device, default 0/0).
// Set MIB_TEST_CAMERA=1 to enable this test; it skips otherwise. It starts
// capture and asserts real frames flow within a few seconds.

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/hardware.h"
#include "support/tempdir.h"


#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    mib::test::requireDeviceEnv("MIB_TEST_CAMERA");

    mib::test::TempDir td("mib_hw_camera");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "AppBackend initialize");

    // EGrabber/hardware mode installs the camera factory at boot but leaves the
    // device selection to the connect flow (so ConnectTab can still run
    // discovery), whereas MindVision mode records its selection at boot. Mirror
    // the connect flow here for the EGrabber path so a device is actually
    // selected before we assert configured + start capture.
    if (!backend.isCameraConfigured()) {
        const int ifIdx = mib::test::envInt("MIB_TEST_EGRABBER_IF", 0);
        const int devIdx = mib::test::envInt("MIB_TEST_EGRABBER_DEV", 0);
        backend.setHardwareCameraSelection(ifIdx, devIdx, "egrabber");
    }
    MIB_REQUIRE(backend.isCameraConfigured(),
                "a camera is configured (hardware selection, or MIB_CAMERA_MODE=mindvision)");

    MIB_REQUIRE(backend.capture().start(), "capture start");

    bool gotFrames = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (backend.capture().stats().framesProcessed.load() > 0) { gotFrames = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const uint64_t n = backend.capture().stats().framesProcessed.load();
    std::printf("camera produced %llu frames\n", static_cast<unsigned long long>(n));
    MIB_EXPECT(gotFrames, "real camera delivered frames within 5s");

    backend.capture().stop();
    if (mib::test::exitCode() == 0) std::printf("camera hardware OK\n");
    return mib::test::exitCode();
}
