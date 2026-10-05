#include "backend/camera/aravis/AravisCamera.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

int main()
{
    mib::test::Watchdog watchdog(30);

    backend::services::CaptureService service;
    camera::aravis::AravisCameraOptions options;
    options.deviceId = "Fake_1";
    options.useFake = true;
    options.streamBuffers = 4;
    options.popTimeoutMs = 50;
    service.setCameraFactory([options] {
        return std::make_unique<camera::aravis::AravisCamera>(options);
    });
    std::atomic<unsigned> frames{0};
    service.setFrameCallback([&](const uint8_t*, size_t, uint64_t, uint64_t, uint64_t) {
        frames.fetch_add(1, std::memory_order_relaxed);
    });

    MIB_REQUIRE(service.start(), "CaptureService starts with Aravis Fake");
    const auto state = service.waitForState(
        {backend::services::CaptureLifecycleState::Running,
         backend::services::CaptureLifecycleState::Faulted},
        std::chrono::seconds(5));
    MIB_REQUIRE(state == backend::services::CaptureLifecycleState::Running, "CaptureService reaches Running");

    for (int i = 0; i < 50 && frames.load(std::memory_order_relaxed) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MIB_REQUIRE(frames.load(std::memory_order_relaxed) > 0, "CaptureService publishes a frame");

    watchdog.mark("concurrent service stop");
    service.stop();
    MIB_EXPECT(service.lifecycleSnapshot().state == backend::services::CaptureLifecycleState::Idle, "CaptureService returns Idle");
    return mib::test::exitCode();
}
