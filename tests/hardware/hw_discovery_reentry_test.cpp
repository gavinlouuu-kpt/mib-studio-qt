// hw_discovery_reentry_test  (LABEL: hardware) — replays the app's boot-time
// camera discovery sequence against a real EGrabber device and asserts that
// EGrabber discovery keeps working afterwards.
//
// Boot sequence being replayed (ConnectTab::refresh on the GUI thread, then
// DeviceInitManager's QtConcurrent worker 400 ms later):
//   1. discoverFramegrabbers()          (main thread)
//   2. discoverCameras()                (main thread)
//   3. discoverMindVisionCameras()      (main thread, only with MindVision)
//   4. discoverAllCameras()             (worker thread)
//
// Regression guard for the GenTL -1004 "GCInitLib: resource already in use"
// lockout seen on v1.0.7-beta.fa6e6ba: the MindVision SDK's CoaXPress plugin
// opened coaxlink.cti during step 3, so step 4 and every later EGrabber open
// in the process failed. Fixed by the shared GenTL handle (GenTLHolder.h),
// which MindVision enumeration reserves first.
//
// Set MIB_TEST_CAMERA=1 to enable (skips otherwise). Optional switches:
//   MIB_TEST_DISCOVERY_SKIP_MINDVISION=1   omit step 3
//   MIB_TEST_DISCOVERY_SAME_THREAD=1       run step 4 on the main thread
//   MIB_TEST_DISCOVERY_MINDVISION_FIRST=1  run a MindVision enumeration before
//                                          any EGrabber call (worst-case order)

#include "backend/services/CameraControlService.h"

#include "support/assert.h"
#include "support/hardware.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using backend::services::CameraControlService;
using backend::services::DiscoveredCamera;

namespace {

bool flag(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' && *v != '0';
}

std::size_t egrabberCount(const std::vector<DiscoveredCamera>& cams)
{
    std::size_t n = 0;
    for (const auto& c : cams) {
        if (c.cameraType == backend::services::CameraType::EGrabber) ++n;
    }
    return n;
}

} // namespace

int main()
{
    mib::test::requireDeviceEnv("MIB_TEST_CAMERA");
    const bool skipMindVision = flag("MIB_TEST_DISCOVERY_SKIP_MINDVISION");
    const bool sameThread = flag("MIB_TEST_DISCOVERY_SAME_THREAD");
    const bool mindVisionFirst = flag("MIB_TEST_DISCOVERY_MINDVISION_FIRST");

    CameraControlService cc;

    if (mindVisionFirst) {
        (void)cc.discoverMindVisionCameras();
        std::printf("step 0: MindVision enumeration before any EGrabber call\n");
    }

    const auto grabbers = cc.discoverFramegrabbers();
    MIB_REQUIRE(!grabbers.empty(), "step 1: at least one framegrabber discovered");

    const auto first = cc.discoverCameras();
    const std::size_t baseline = egrabberCount(first);
    MIB_REQUIRE(baseline > 0, "step 2: at least one EGrabber camera discovered");

    if (!skipMindVision) {
        (void)cc.discoverMindVisionCameras();
        std::printf("step 3: MindVision enumeration done\n");
    } else {
        std::printf("step 3: skipped (MIB_TEST_DISCOVERY_SKIP_MINDVISION)\n");
    }

    std::vector<DiscoveredCamera> second;
    if (sameThread) {
        second = cc.discoverAllCameras();
    } else {
        std::thread worker([&] { second = cc.discoverAllCameras(); });
        worker.join();
    }
    std::printf("step 4 (%s thread): %zu EGrabber camera(s), baseline %zu\n",
                sameThread ? "same" : "worker", egrabberCount(second), baseline);
    MIB_EXPECT(egrabberCount(second) == baseline,
               "step 4: EGrabber discovery still finds the camera after the boot sequence");

    // Any later EGenTL construction must also still work.
    const auto third = cc.discoverCameras();
    MIB_EXPECT(egrabberCount(third) == baseline,
               "step 5: a later EGrabber discovery on the main thread still works");

    if (mib::test::exitCode() == 0) std::printf("discovery re-entry OK\n");
    return mib::test::exitCode();
}
