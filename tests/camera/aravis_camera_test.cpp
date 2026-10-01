#include "backend/camera/aravis/AravisCamera.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

    // LatestFrame (preview) delivery: completed buffers that queued up while
    // the consumer was busy are returned to the producer, newest one wins.
    AravisCamera latest(options);
    config.deliveryMode = FrameDeliveryMode::LatestFrame;
    latest.applyConfig(config);
    MIB_EXPECT(latest.deliveryCapabilities().supportsLatestFrame, "LatestFrame is advertised");
    MIB_REQUIRE(latest.start(), "LatestFrame session starts");
    MIB_EXPECT(latest.activeDeliveryMode() == FrameDeliveryMode::LatestFrame, "LatestFrame is active");
    Frame newest;
    MIB_REQUIRE(latest.grabFrame(newest), "LatestFrame delivers");
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let several Fake frames complete
    MIB_REQUIRE(latest.grabFrame(newest), "LatestFrame delivers after a stall");
    camera::common::AcquisitionQueueStats latestStats;
    MIB_REQUIRE(latest.pollAcquisitionQueueStats(latestStats), "queue stats available");
    MIB_EXPECT(latestStats.intentionallyDiscardedFrames > 0, "stale frames are discarded, not queued");
    latest.stop();
    config.deliveryMode = FrameDeliveryMode::EveryFrame;

    // Settings are applied in region -> rate -> exposure order and read back;
    // a rate above the maximum is clamped by the device and reported.
    AravisCameraOptions tuned = options;
    tuned.region = camera::aravis::AravisRegion{16, 8, 256, 128};
    tuned.frameRateHz = 1.0e6;
    tuned.exposureUs = 1000.0;
    AravisCamera tunedCamera(tuned);
    tunedCamera.applyConfig(config);
    MIB_REQUIRE(tunedCamera.start(), "camera starts with explicit settings");
    const auto info = tunedCamera.sessionInfo();
    MIB_EXPECT(info.deviceId == "Fake_1", "session names the device");
    MIB_EXPECT(info.region.x == 16 && info.region.y == 8 && info.region.width == 256 && info.region.height == 128,
               "region is applied exactly");
    MIB_EXPECT(info.frameRateClamped && info.frameRateHz <= info.frameRateMaxHz + 1e-6,
               "frame-rate clamp is reported");
    MIB_EXPECT(std::abs(info.exposureUs - 1000.0) < 1.0 && !info.exposureClamped, "exposure is applied");
    MIB_EXPECT(!info.pzFeatures && info.deliveredFrameRateHz == info.frameRateHz,
               "devices without the PZ7035 model deliver at the sensor rate");
    Frame tunedFrame;
    MIB_REQUIRE(tunedCamera.grabFrame(tunedFrame), "tuned camera delivers");
    MIB_EXPECT(tunedFrame.width == 256 && tunedFrame.height == 128, "frame has the requested region");
    tunedCamera.stop();

    AravisCameraOptions badRegion = options;
    badRegion.region = camera::aravis::AravisRegion{0, 0, 1 << 20, 1 << 20};
    AravisCamera badRegionCamera(badRegion);
    badRegionCamera.applyConfig(config);
    // The Fake device stores any region and then reports no payload; a
    // strict device fails at the region read-back instead.
    MIB_EXPECT(!badRegionCamera.start(), "an impossible region does not start");
    MIB_EXPECT(badRegionCamera.lastFailure().code.rfind("aravis.", 0) == 0, "region failure is structured");

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
