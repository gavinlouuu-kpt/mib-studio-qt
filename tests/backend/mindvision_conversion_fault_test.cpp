// mindvision_conversion_fault_test (issue #366)
//
// Fault injection through the MindVision SDK seam. Proves the camera fails
// closed before any conversion can write into a destination buffer that was
// not proven large enough:
//   - CameraSetIspOutFormat failure          -> start() false, no alloc/play
//   - readback reports RGB8/BGR8/Mono16      -> start() false, no alloc/play
//   - readback unavailable                   -> start() false
//   - zero / negative / oversized dimensions -> start() false
//   - checked-size overflow (pure validator)
//   - incoming frame geometry != session     -> frame rejected BEFORE
//                                               CameraImageProcess, stream
//                                               faulted with structured code
//   - geometry change after N good frames    -> same, N frames delivered first
//   - normal 512x96 Mono8 acquisition        -> byte-identical frames
//   - stop() while a grab is in flight       -> CameraUnInit only after the
//                                               in-flight call returned
//   - wedged driver                          -> handle abandoned, no UnInit
//                                               under a live call
// Also the SDK-unavailable build: start() reports "mindvision.sdk_unavailable".

#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/camera/mindvision/MindVisionFrameGeometry.h"

#include "support/assert.h"
#include "support/fake_mindvision_sdk.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace mv = backend::camera::mindvision;
using camera::common::MindVisionCamera;
using mib::test::FakeMindVisionSdk;

namespace {
constexpr std::uint32_t kRgb8 = 0x02180014u;   // CAMERA_MEDIA_TYPE_RGB8
constexpr std::uint32_t kBgr8 = 0x02180015u;   // CAMERA_MEDIA_TYPE_BGR8
constexpr std::uint32_t kMono16 = 0x01100007u; // CAMERA_MEDIA_TYPE_MONO16
} // namespace

int main()
{
    mib::test::Watchdog wd(30);

    // ---- Pure validator -----------------------------------------------------
    {
        wd.mark("validator");
        std::size_t bytes = 0;
        MIB_EXPECT(mv::checkedFrameBytes(512, 96, 1, bytes) && bytes == 512 * 96, "512x96 mono8");
        MIB_EXPECT(!mv::checkedFrameBytes(0, 96, 1, bytes), "zero width rejected");
        MIB_EXPECT(!mv::checkedFrameBytes(512, -1, 1, bytes), "negative height rejected");
        MIB_EXPECT(!mv::checkedFrameBytes(1 << 30, 1 << 30, 1, bytes), "product overflow rejected");
        MIB_EXPECT(!mv::checkedFrameBytes(65535, 65535, 3, bytes), "int-range overflow rejected");
        MIB_EXPECT(mv::bytesPerPixelForMediaType(mv::kMediaTypeMono8) == 1, "mono8 = 1 byte");
        MIB_EXPECT(mv::bytesPerPixelForMediaType(kRgb8) == 3, "rgb8 = 3 bytes");
        MIB_EXPECT(mv::bytesPerPixelForMediaType(kMono16) == 2, "mono16 = 2 bytes");

        auto ok = mv::validateSessionGeometry(512, 96, mv::kMediaTypeMono8);
        MIB_EXPECT(ok.ok() && ok.geometry.requiredBytes == 512 * 96, "512x96 mono8 valid");
        MIB_EXPECT(mv::validateSessionGeometry(512, 96, kRgb8).fault == mv::GeometryFault::UnsupportedFormat,
                   "rgb8 unsupported");
        MIB_EXPECT(mv::validateSessionGeometry(0, 96, mv::kMediaTypeMono8).fault == mv::GeometryFault::InvalidWidth,
                   "zero width");
        MIB_EXPECT(mv::validateSessionGeometry(512, 0, mv::kMediaTypeMono8).fault == mv::GeometryFault::InvalidHeight,
                   "zero height");
        MIB_EXPECT(mv::validateSessionGeometry(-5, 96, mv::kMediaTypeMono8).fault == mv::GeometryFault::InvalidWidth,
                   "negative width");
        MIB_EXPECT(mv::validateSessionGeometry(70000, 96, mv::kMediaTypeMono8).fault == mv::GeometryFault::InvalidWidth,
                   "oversized width");

        mv::SdkFrameInfo head{};
        head.width = 512;
        head.height = 96;
        MIB_EXPECT(mv::validateIncomingFrame(ok.geometry, head, 512 * 96).ok(), "matching frame ok");
        head.width = 640;
        MIB_EXPECT(mv::validateIncomingFrame(ok.geometry, head, 512 * 96).fault ==
                       mv::GeometryFault::FrameGeometryMismatch,
                   "width mismatch");
        head.width = 512;
        MIB_EXPECT(mv::validateIncomingFrame(ok.geometry, head, 512 * 96 - 1).fault ==
                       mv::GeometryFault::DestinationTooSmall,
                   "undersized destination");
    }

    // ---- Start-time fail-closed cases -------------------------------------
    auto expectStartFails = [&](const char* label, FakeMindVisionSdk& sdk, const std::string& code,
                                int expectedPlayCalls = 0) {
        MindVisionCamera cam(0, {}, sdk.ops());
        const bool started = cam.start();
        MIB_EXPECT(!started, std::string(label) + ": start must fail");
        const auto f = cam.lastFailure();
        MIB_EXPECT(f.code == code, std::string(label) + ": code " + f.code + " expected " + code);
        MIB_EXPECT(!f.message.empty(), std::string(label) + ": message present");
        MIB_EXPECT(!cam.isRunning(), std::string(label) + ": not running");
        MIB_EXPECT(sdk.playCalls.load() == expectedPlayCalls,
                   std::string(label) + ": CameraPlay call count");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 0, std::string(label) + ": no conversion");
        MIB_EXPECT(sdk.unInitCalls.load() == 1, std::string(label) + ": handle closed");
        camera::common::Frame frame;
        MIB_EXPECT(!cam.grabFrame(frame), std::string(label) + ": grab refused");
    };

    {
        wd.mark("format set failure");
        FakeMindVisionSdk sdk;
        sdk.setIspOutFormatStatus = -3;
        expectStartFails("SetIspOutFormat fails", sdk, "mindvision.isp_format_rejected");
        MIB_EXPECT(sdk.allocCalls.load() == 0, "no destination allocated");
    }
    for (auto [fmt, name] : {std::pair{kRgb8, "rgb8"}, std::pair{kBgr8, "bgr8"}, std::pair{kMono16, "mono16"}}) {
        wd.mark("readback not mono8");
        FakeMindVisionSdk sdk;
        sdk.readbackOverrideEnabled = true;
        sdk.readbackFormatOverride = fmt;
        expectStartFails((std::string("readback ") + name).c_str(), sdk,
                         "mindvision.geometry.unsupportedFormat");
        MIB_EXPECT(sdk.allocCalls.load() == 0, "no destination allocated for non-mono8");
    }
    {
        wd.mark("readback unavailable");
        FakeMindVisionSdk sdk;
        sdk.getIspOutFormatStatus = -9;
        expectStartFails("readback unavailable", sdk, "mindvision.isp_format_unverified");
    }
    {
        wd.mark("bad dimensions");
        for (auto [w, h, code] : {
                 std::tuple{0, 96, "mindvision.geometry.invalidWidth"},
                 std::tuple{512, 0, "mindvision.geometry.invalidHeight"},
                 std::tuple{-1, 96, "mindvision.geometry.invalidWidth"},
                 std::tuple{512, -7, "mindvision.geometry.invalidHeight"},
                 std::tuple{100000, 96, "mindvision.geometry.invalidWidth"},
             }) {
            FakeMindVisionSdk sdk;
            sdk.width = w;
            sdk.height = h;
            expectStartFails("dimensions", sdk, code);
            MIB_EXPECT(sdk.allocCalls.load() == 0, "no allocation for invalid geometry");
        }
    }
    {
        wd.mark("alloc failure");
        FakeMindVisionSdk sdk;
        sdk.allocFails = true;
        expectStartFails("alloc fails", sdk, "mindvision.alloc_failed");
    }
    {
        wd.mark("play failure");
        FakeMindVisionSdk sdk;
        sdk.playStatus = -4;
        expectStartFails("play fails", sdk, "mindvision.play_failed", /*expectedPlayCalls=*/1);
        MIB_EXPECT(sdk.freeCalls.load() == 1, "buffer freed on play failure");
    }
    {
        wd.mark("no devices");
        FakeMindVisionSdk sdk;
        sdk.deviceCount = 0;
        MindVisionCamera cam(0, {}, sdk.ops());
        MIB_EXPECT(!cam.start(), "no devices -> fail");
        MIB_EXPECT(cam.lastFailure().code == "mindvision.no_devices", "no_devices code");
    }

    // ---- Normal Mono8 acquisition: byte-identical ---------------------------
    {
        wd.mark("normal mono8");
        FakeMindVisionSdk sdk;
        for (int i = 0; i < 5; ++i) sdk.frames.push_back(FakeMindVisionSdk::frame(512, 96, static_cast<std::uint8_t>(i * 7), 10 + i));
        auto expected = sdk.frames; // copy for comparison
        MindVisionCamera cam(0, {}, sdk.ops());
        MIB_REQUIRE(cam.start(), "normal start");
        MIB_EXPECT(sdk.setIspOutFormatCalls.load() == 1 && sdk.getIspOutFormatCalls.load() == 1,
                   "format set and read back once");
        MIB_EXPECT(sdk.requestedFormat == mv::kMediaTypeMono8, "mono8 requested");
        MIB_EXPECT(sdk.lastAllocBytes.load() == 512 * 96, "destination sized to validated bytes");
        const auto geom = cam.sessionGeometry();
        MIB_EXPECT(geom.width == 512 && geom.height == 96 && geom.requiredBytes == 512 * 96,
                   "session geometry recorded");
        for (int i = 0; i < 5; ++i) {
            camera::common::Frame frame;
            MIB_REQUIRE(cam.grabFrame(frame), "grab " + std::to_string(i));
            MIB_EXPECT(frame.width == 512 && frame.height == 96 && frame.linePitch == 512, "frame geometry");
            MIB_EXPECT(frame.pixelFormat == 0x01080001u, "PFNC mono8");
            MIB_EXPECT(frame.data.size() == 512 * 96, "payload size");
            MIB_EXPECT(std::memcmp(frame.data.data(), expected[i].payload.data(), 512 * 96) == 0,
                       "payload byte-identical");
            MIB_EXPECT(frame.timestamp == static_cast<std::uint64_t>(10 + i) * 100'000ULL,
                       "0.1 ms ticks -> ns");
        }
        MIB_EXPECT(sdk.imageProcessCalls.load() == 5, "one conversion per frame");
        MIB_EXPECT(sdk.releaseCalls.load() == 5 && sdk.outstandingBuffers.load() == 0,
                   "release exactly once per acquired buffer");
        MIB_EXPECT(cam.geometryRejectedFrames() == 0, "no rejections");
        cam.stop();
        MIB_EXPECT(sdk.unInitCalls.load() == 1 && sdk.freeCalls.load() == 1, "clean teardown");
        MIB_EXPECT(!cam.isRunning(), "stopped");
    }

    // ---- Mid-session geometry change: rejected before conversion -----------
    {
        wd.mark("geometry change");
        FakeMindVisionSdk sdk;
        sdk.frames.push_back(FakeMindVisionSdk::frame(512, 96, 1));
        sdk.frames.push_back(FakeMindVisionSdk::frame(512, 96, 2));
        sdk.frames.push_back(FakeMindVisionSdk::frame(640, 480, 3)); // larger than allocation
        sdk.frames.push_back(FakeMindVisionSdk::frame(512, 96, 4));  // would be fine, but stream is faulted
        MindVisionCamera cam(0, {}, sdk.ops());
        MIB_REQUIRE(cam.start(), "start");
        camera::common::Frame frame;
        MIB_REQUIRE(cam.grabFrame(frame), "frame 1");
        MIB_REQUIRE(cam.grabFrame(frame), "frame 2");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 2, "two conversions so far");
        MIB_EXPECT(!cam.grabFrame(frame), "mismatched frame rejected");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 2, "conversion NOT invoked for the mismatched frame");
        MIB_EXPECT(cam.geometryRejectedFrames() == 1, "rejection counted");
        MIB_EXPECT(!cam.isRunning(), "stream faulted");
        const auto f = cam.lastFailure();
        MIB_EXPECT(f.code == "mindvision.frame.frameGeometryMismatch", "structured code: " + f.code);
        MIB_EXPECT(f.message.find("640x480") != std::string::npos, "message names the geometry");
        MIB_EXPECT(sdk.outstandingBuffers.load() == 0, "rejected buffer released");
        MIB_EXPECT(!cam.grabFrame(frame), "no further frames after fault (controlled stop)");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 2, "still no conversion after fault");
        cam.stop();
        MIB_EXPECT(sdk.unInitCalls.load() == 1, "handle closed on stop after fault");
        // Controlled reconfiguration: a fresh start re-validates and works.
        sdk.frames.clear();
        sdk.width = 640;
        sdk.height = 480;
        sdk.frames.push_back(FakeMindVisionSdk::frame(640, 480, 9));
        MIB_REQUIRE(cam.start(), "restart after reconfiguration");
        MIB_EXPECT(cam.sessionGeometry().requiredBytes == 640 * 480, "new allocation validated");
        MIB_REQUIRE(cam.grabFrame(frame), "frame at new geometry");
        MIB_EXPECT(frame.data.size() == 640 * 480, "new size");
        cam.stop();
    }

    // ---- Smaller frame than allocation is still a mismatch (never silently
    //      accepted as a differently shaped Mono8 image) ---------------------
    {
        wd.mark("smaller frame");
        FakeMindVisionSdk sdk;
        sdk.frames.push_back(FakeMindVisionSdk::frame(256, 96, 1));
        MindVisionCamera cam(0, {}, sdk.ops());
        MIB_REQUIRE(cam.start(), "start");
        camera::common::Frame frame;
        MIB_EXPECT(!cam.grabFrame(frame), "smaller frame rejected");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 0, "no conversion");
        cam.stop();
    }

    // ---- Stop while a grab is in flight: UnInit only after it returns ------
    {
        wd.mark("stop while grab in flight");
        FakeMindVisionSdk sdk;
        sdk.blockGrab = true;
        MindVisionCamera cam(0, {}, sdk.ops());
        MIB_REQUIRE(cam.start(), "start");
        std::thread grabber([&] {
            camera::common::Frame frame;
            cam.grabFrame(frame); // blocks in the fake SDK until stop()
        });
        MIB_REQUIRE(sdk.waitUntilBlocked(std::chrono::seconds(5)), "grab parked in SDK");
        cam.stop(); // must wait for the in-flight op before UnInit
        grabber.join();
        MIB_EXPECT(sdk.stopCalls.load() == 1, "CameraStop issued to unblock the grab");
        MIB_EXPECT(sdk.unInitCalls.load() == 1, "handle uninitialized");
        MIB_EXPECT(!sdk.unInitWhileGrabInFlight.load(), "UnInit never ran under a live SDK call");
        MIB_EXPECT(cam.lastFailure().empty(), "clean stop records no failure");
    }

    // ---- Wedged driver: bounded wait, handle abandoned, no UnInit ----------
    {
        wd.mark("wedged driver");
        FakeMindVisionSdk sdk;
        sdk.blockGrab = true;
        sdk.ignoreStopWhileBlocked = true;
        MindVisionCamera cam(0, {}, sdk.ops());
        cam.setInFlightDrainTimeout(std::chrono::milliseconds(100));
        MIB_REQUIRE(cam.start(), "start");
        std::thread grabber([&] {
            camera::common::Frame frame;
            cam.grabFrame(frame);
        });
        MIB_REQUIRE(sdk.waitUntilBlocked(std::chrono::seconds(5)), "grab parked");
        const auto t0 = std::chrono::steady_clock::now();
        cam.stop();
        MIB_EXPECT(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "stop is bounded");
        MIB_EXPECT(sdk.unInitCalls.load() == 0, "handle abandoned, not uninitialized under a live call");
        MIB_EXPECT(cam.lastFailure().code == "mindvision.inflight_drain_timeout", "drain timeout reported");
        sdk.releaseGrab(); // let the wedged call return so the thread exits
        grabber.join();
        MIB_EXPECT(!sdk.unInitWhileGrabInFlight.load(), "no UnInit under live call, ever");
    }

    // ---- LatestFrame via newest API path also validates geometry ----------
    {
        wd.mark("latest frame path");
        FakeMindVisionSdk sdk;
        sdk.provideNewestApi = true;
        sdk.frames.push_back(FakeMindVisionSdk::frame(512, 96, 5));
        sdk.frames.push_back(FakeMindVisionSdk::frame(512, 200, 6));
        MindVisionCamera cam(0, {}, sdk.ops());
        camera::common::CameraConfig cfg;
        cfg.deliveryMode = camera::common::FrameDeliveryMode::LatestFrame;
        cam.applyConfig(cfg);
        MIB_REQUIRE(cam.start(), "start latest");
        MIB_EXPECT(cam.activeDeliveryMode() == camera::common::FrameDeliveryMode::LatestFrame, "mode confirmed");
        camera::common::Frame frame;
        MIB_REQUIRE(cam.grabFrame(frame), "good frame");
        MIB_EXPECT(!cam.grabFrame(frame), "bad frame rejected on newest path");
        MIB_EXPECT(sdk.imageProcessCalls.load() == 1, "single conversion");
        cam.stop();
    }

    // ---- Build without the SDK: explicit unavailable failure ---------------
    {
        wd.mark("sdk availability");
        if (!mv::mindVisionSdkAvailable()) {
            MindVisionCamera cam(0);
            MIB_EXPECT(!cam.start(), "unavailable SDK: start fails");
            MIB_EXPECT(cam.lastFailure().code == "mindvision.sdk_unavailable", "sdk_unavailable code");
        }
    }

    return mib::test::exitCode();
}
