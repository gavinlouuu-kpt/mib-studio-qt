#include "backend/camera/aravis/AravisCamera.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <thread>

int main()
{
    mib::test::Watchdog watchdog(30);
    using camera::aravis::AravisCamera;
    using camera::aravis::AravisCameraOptions;
    using camera::common::CameraConfig;
    using camera::common::Frame;
    using camera::common::FrameDeliveryMode;

    const auto ids = AravisCamera::enumerateDeviceIds(true);
    MIB_REQUIRE(std::find(ids.begin(), ids.end(), "Fake_1") != ids.end(), "Fake interface is discoverable when opted in");
    const auto physicalOnly = AravisCamera::enumerateDeviceIds(false);
    MIB_EXPECT(std::find(physicalOnly.begin(), physicalOnly.end(), "Fake_1") == physicalOnly.end(),
               "Fake remains filtered from physical-only discovery");

    AravisCameraOptions options;
    options.deviceId = "Fake_1";
    options.useFake = true;
    options.streamBuffers = 4;
    options.popTimeoutMs = 200;
    AravisCamera camera(options);
    CameraConfig config;
    config.bufferPartCount = 1;
    config.numBuffers = 4;
    config.deliveryMode = FrameDeliveryMode::EveryFrame;
    camera.applyConfig(config);
    MIB_REQUIRE(camera.start(), "Fake camera starts");

    Frame first;
    MIB_REQUIRE(camera.grabFrame(first), "first Fake frame delivered");
    MIB_EXPECT(first.width > 0 && first.height > 0, "frame has dimensions");
    MIB_EXPECT(first.pixelFormat == 0x01080001u, "frame is Mono8");
    MIB_EXPECT(first.data.size() >= first.width * first.height, "frame owns its payload");
    const auto firstBytes = first.data;

    // Cycle beyond the configured SDK buffer count. This proves the first
    // delivered frame survives repeated Aravis buffer reuse, not just one
    // immediate copy.
    for (int i = 0; i < 12; ++i) {
        watchdog.mark("buffer reuse");
        Frame recycled;
        MIB_REQUIRE(camera.grabFrame(recycled), "recycled Fake frame delivered");
        MIB_EXPECT(first.data == firstBytes, "owned frame is stable after SDK buffer reuse");
    }
    camera.stop();
    MIB_EXPECT(!camera.isRunning(), "camera stops");
    Frame afterStop = first;
    MIB_EXPECT(!camera.grabFrame(afterStop), "grab after stop does not republish stale frame");

    MIB_REQUIRE(camera.start(), "camera restarts"); // stop/start must be reusable.
    Frame restarted;
    MIB_EXPECT(camera.grabFrame(restarted), "restarted camera delivers");
    camera.stop();

    // Software-trigger mode gives the receive path a deterministic empty
    // queue. A timeout is a normal non-delivery while the camera remains
    // running; it must not mutate or republish the previous frame.
    AravisCameraOptions triggeredOptions = options;
    triggeredOptions.softwareTrigger = true;
    triggeredOptions.popTimeoutMs = 20;
    AravisCamera triggered(triggeredOptions);
    triggered.applyConfig(config);
    MIB_REQUIRE(triggered.start(), "software-trigger Fake camera starts");
    Frame timeoutBefore;
    MIB_EXPECT(!triggered.grabFrame(timeoutBefore), "running receive timeout before trigger is non-delivery");
    MIB_EXPECT(triggered.isRunning(), "receive timeout does not stop acquisition");
    MIB_REQUIRE(triggered.softTrigger(), "software trigger is accepted");
    Frame triggeredFrame;
    MIB_REQUIRE(triggered.grabFrame(triggeredFrame), "software trigger produces a frame");
    const auto deliveredBytes = triggeredFrame.data;
    Frame timeoutAfter = triggeredFrame;
    MIB_EXPECT(!triggered.grabFrame(timeoutAfter), "running receive timeout after delivery is non-delivery");
    MIB_EXPECT(triggered.isRunning(), "post-delivery timeout leaves acquisition running");
    MIB_EXPECT(timeoutAfter.data == deliveredBytes, "timeout does not overwrite the previous frame");
    triggered.stop();

    AravisCamera unsupported(options);
    config.deliveryMode = FrameDeliveryMode::LatestFrame;
    unsupported.applyConfig(config);
    MIB_EXPECT(!unsupported.start(), "LatestFrame is rejected");
    MIB_EXPECT(unsupported.lastFailure().code == "aravis.unsupported_delivery_mode", "unsupported mode is structured");

    AravisCamera missing({"Aravis-does-not-exist", true, 4, 50});
    missing.applyConfig(CameraConfig{});
    MIB_EXPECT(!missing.start(), "missing device is rejected");
    MIB_EXPECT(missing.lastFailure().code == "aravis.device_not_found", "missing device is structured");

    AravisCamera fakeWithoutOptIn({"Fake_1", false, 4, 50});
    fakeWithoutOptIn.applyConfig(CameraConfig{});
    MIB_EXPECT(!fakeWithoutOptIn.start(), "Fake device cannot be selected without opt-in");
    MIB_EXPECT(fakeWithoutOptIn.lastFailure().code == "aravis.fake_not_enabled", "Fake opt-in failure is structured");

    std::cout << "Aravis Fake consumer lifecycle passed\n";
    return mib::test::exitCode();
}
