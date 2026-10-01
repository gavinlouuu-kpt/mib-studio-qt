// AravisCamera against the PZ7035 GenTL producer's synthetic pattern device
// (pz7035-imx426 gentl/, PZ_GENTL_PATTERN=1). Registered only when
// MIB_PZ7035_GENTL_CTI names the producer, so it never needs PZ7035 hardware.
//
// Checks the YOFO Studio adapter contract (ADR 0008, impl spec S4): vendor
// auto-selection, region/rate/exposure read-back, the reported rate limit,
// the banded Overview delivered rate, LatestFrame preview delivery and the
// host-steady timestamps.
#include "backend/camera/aravis/AravisCamera.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

int main()
{
    mib::test::Watchdog watchdog(60);
    using camera::aravis::AravisCamera;
    using camera::aravis::AravisCameraOptions;
    using camera::aravis::AravisRegion;
    using camera::common::CameraConfig;
    using camera::common::ClockDomain;
    using camera::common::Frame;
    using camera::common::FrameDeliveryMode;

    CameraConfig config;
    config.numBuffers = 4;

    // ROI 1 preview: 512 x 96 at the default window, 1000 Hz, 900 us.
    AravisCameraOptions preview;
    preview.region = AravisRegion{152, 256, 512, 96};
    preview.frameRateHz = 1000.0;
    preview.exposureUs = 900.0;
    preview.popTimeoutMs = 1000;
    AravisCamera roi(preview);
    config.deliveryMode = FrameDeliveryMode::LatestFrame;
    roi.applyConfig(config);
    MIB_REQUIRE(roi.start(), "pattern device starts with the ROI 1 preview settings");
    auto info = roi.sessionInfo();
    MIB_EXPECT(info.vendor == "YOFO", "auto-selection picks the YOFO producer");
    MIB_EXPECT(info.region.width == 512 && info.region.height == 96 && info.region.x == 152 &&
                   info.region.y == 256,
               "ROI 1 window is applied exactly");
    MIB_EXPECT(!info.frameRateClamped && std::abs(info.frameRateHz - 1000.0) < 1.0, "1000 Hz is applied");
    MIB_EXPECT(!info.exposureClamped && std::abs(info.exposureUs - 900.0) < 1.0, "900 us is applied");
    MIB_EXPECT(info.pzFeatures && info.bandCount == 1, "a ROI 1 image is one band");
    // Previews are samples while the PL processes every frame: at 1 kHz the
    // PS copy and re-arm (1.9 ms) plus the active time spans three frame
    // periods, so the producer reports at least 333 images/s (417 measured on
    // the PS, pz7035-imx426 gentl/README.md).
    MIB_EXPECT(info.deliveredFrameRateLimit == "BandReadout", "readout binds the preview rate");
    MIB_EXPECT(std::abs(info.deliveredFrameRateHz - 333.4) < 2.0, "at least 333 preview images/s");
    // 96 lines would allow ~5.3 kHz; the classical profile caps the sensor at 5 kHz.
    MIB_EXPECT(std::abs(info.frameRateMaxHz - 5000.0) < 1.0, "96-line maximum is the 5 kHz profile cap");
    MIB_EXPECT(info.frameRateLimitReason == "Profile", "the limit reason is reported");
    const auto ts = roi.timestampDescriptor();
    MIB_EXPECT(ts.isValid() && ts.domain == ClockDomain::HostSteadyNs && ts.ticksPerSecond == 1000000000ULL,
               "producer timestamps are host steady nanoseconds");
    Frame first;
    MIB_REQUIRE(roi.grabFrame(first), "ROI 1 frame delivered");
    MIB_EXPECT(first.width == 512 && first.height == 96, "frame has the ROI 1 geometry");
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    MIB_EXPECT(first.timestamp <= now && now - first.timestamp < 1000000000ULL,
               "frame timestamp is a recent steady-clock instant");
    roi.stop();

    // A rate above the 96-line maximum is clamped by the device and reported.
    AravisCameraOptions fast = preview;
    fast.frameRateHz = 100000.0;
    fast.exposureUs.reset();
    AravisCamera clamped(fast);
    config.deliveryMode = FrameDeliveryMode::EveryFrame;
    clamped.applyConfig(config);
    MIB_REQUIRE(clamped.start(), "over-limit request still starts");
    info = clamped.sessionInfo();
    MIB_EXPECT(info.frameRateClamped && std::abs(info.frameRateHz - info.frameRateMaxHz) < 1.0,
               "rate is clamped to the reported maximum");
    clamped.stop();

    // Overview: full field 816 x 624 arrives in bands. At 300 Hz the blanking
    // covers the band readout, so every band catches the next frame.
    AravisCameraOptions overview;
    overview.region = AravisRegion{0, 0, 816, 624};
    overview.frameRateHz = 300.0;
    AravisCamera slowField(overview);
    config.deliveryMode = FrameDeliveryMode::LatestFrame;
    slowField.applyConfig(config);
    MIB_REQUIRE(slowField.start(), "300 Hz Overview starts");
    info = slowField.sessionInfo();
    MIB_EXPECT(info.bandCount == 11, "a full-field image is 11 bands of the 48 KiB grabber");
    MIB_EXPECT(info.deliveredFrameRateLimit == "SensorRate", "300 Hz bands catch consecutive frames");
    MIB_EXPECT(info.frameRateLimitReason == "SensorGeometry", "624 lines bind the sensor maximum");
    slowField.stop();

    // The YOFO Overview preset (830 Hz, lit by the 1 kHz-class LED driver)
    // has too little blanking: bands skip frames and the readout binds.
    overview.frameRateHz = 830.0;
    overview.popTimeoutMs = 1000;
    AravisCamera field(overview);
    config.deliveryMode = FrameDeliveryMode::LatestFrame;
    field.applyConfig(config);
    MIB_REQUIRE(field.start(), "full-field Overview starts");
    info = field.sessionInfo();
    MIB_EXPECT(!info.frameRateClamped, "830 Hz is within the full-field maximum");
    MIB_EXPECT(info.deliveredFrameRateLimit == "BandReadout", "band readout binds at the preset");
    MIB_EXPECT(info.deliveredFrameRateHz > 15.0 && info.deliveredFrameRateHz < 30.0,
               "about 25 full-field images/s are delivered (25.9 measured on the PS)");
    Frame image;
    MIB_REQUIRE(field.grabFrame(image), "full-field image delivered");
    MIB_EXPECT(image.width == 816 && image.height == 624, "image has the full field");
    field.stop();

    std::cout << "PZ7035 pattern producer: Overview max " << info.frameRateMaxHz << " Hz; Overview "
              << info.bandCount << " bands, " << info.deliveredFrameRateHz << " images/s at "
              << info.frameRateHz << " Hz\n";
    return mib::test::exitCode();
}
