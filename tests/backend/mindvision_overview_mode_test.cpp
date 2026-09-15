#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/services/CaptureService.h"
#include "support/fake_mindvision_sdk.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <thread>

namespace mv = backend::camera::mindvision;
using camera::common::MindVisionCamera;

int main() {
    mib::test::Watchdog wd(90);
    mv::Config experiment;
    experiment.width = 512;
    experiment.height = 96;
    experiment.offsetX = 64;
    experiment.offsetY = 48;
    experiment.illuminatedLive = true;
    experiment.liveView.frequencyHz = 1000;
    experiment.liveView.dutyPercent = 2;
    experiment.exposureUs = 2;
    experiment.strobePulseUs = 100;
    const auto overview = mv::overviewConfig(experiment);
    MIB_EXPECT(overview.liveView.frequencyHz == 400, "generator minimum overview frequency");
    MIB_EXPECT(std::abs(overview.liveView.dutyPercent - 0.8) < 1e-9,
               "20 us pulse becomes 0.8 percent at 400 Hz");
    MIB_EXPECT(std::abs(overview.liveView.triggerPulseUs() - 20) < 1e-9,
               "pulse duration preserved");
    MIB_EXPECT(experiment.liveView.frequencyHz == 1000 && experiment.offsetX == 64,
               "saved config unchanged");
    auto quantized = experiment;
    quantized.liveView.frequencyHz = 1234;
    quantized.liveView.dutyPercent = 2.33;
    const auto q = mv::overviewConfig(quantized);
    MIB_EXPECT(std::abs(q.liveView.triggerPulseUs() - quantized.liveView.triggerPulseUs()) <=
                   0.1251,
               "pulse quantization bounded by half a generator duty step");

    // Capture pipeline repeatedly switches between native full sensor and hardware crop.
    // A fake with a differently sized sensor catches hard-coded eGrabber geometry.
    experiment.illuminatedLive = false;
    experiment.requireExactGeometry = true;
    for (int cycle = 0; cycle < 30; ++cycle) {
        wd.mark("alternating overview/experiment pipeline");
        const bool full = cycle % 2 == 0;
        mib::test::FakeMindVisionSdk fake;
        auto sdk = std::make_shared<mv::SdkOps>(*fake.ops());
        mv::Config applied;
        sdk->getCapability = [](int, mv::SdkCapability& cap) {
            cap = {true, 1280, 1024, 16, 4};
            return mv::kSdkSuccess;
        };
        sdk->applyConfig = [&](int, const mv::Config& cfg) {
            applied = cfg;
            fake.width = cfg.width;
            fake.height = cfg.height;
            return true;
        };
        const int width = full ? 1280 : 512, height = full ? 1024 : 96;
        for (int frame = 0; frame < 12; ++frame)
            fake.frames.push_back(
                mib::test::FakeMindVisionSdk::frame(width, height, 31, frame + 1));
        auto store = std::make_shared<backend::playback::FrameStore>(8);
        backend::services::CaptureService capture;
        capture.setFrameStore(store);
        const auto config = full ? mv::overviewConfig(experiment) : experiment;
        capture.setCameraFactory(
            [&] { return std::make_unique<MindVisionCamera>(0, "", sdk, nullptr, full, config); });
        MIB_REQUIRE(capture.start(), "capture starts");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (capture.stats().framesProcessed.load() < 12 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        capture.stop();
        MIB_EXPECT(applied.width == width && applied.height == height,
                   "mode applies correct dimensions");
        MIB_EXPECT(applied.offsetX == (full ? 0 : 64) && applied.offsetY == (full ? 0 : 48),
                   "crop restored");
        MIB_EXPECT(capture.stats().framesProcessed == 12 && store->totalWritten() == 12,
                   "all captured frames published");
        MIB_EXPECT(store->availableCount() == 8 && store->earliestAvailableIndex() == 4,
                   "retained plus overwritten equals captured");
        backend::playback::Frame frame;
        MIB_REQUIRE(store->getLatest(frame), "preview frame exists");
        MIB_EXPECT(frame.width == width && frame.height == height,
                   "frame matches destination mode");
        MIB_EXPECT(fake.outstandingBuffers == 0 && fake.allocCalls == fake.freeCalls,
                   "buffers released every switch");
        MIB_EXPECT(!fake.unInitWhileGrabInFlight, "SDK lifecycle remains serialized");
    }

    for (int fault = 0; fault < 3; ++fault) {
        mib::test::FakeMindVisionSdk fake;
        auto sdk = std::make_shared<mv::SdkOps>(*fake.ops());
        sdk->getCapability = [fault](int, mv::SdkCapability& cap) {
            cap = {true, fault == 0 ? 0 : 1280, 1024, 16, 4};
            return mv::kSdkSuccess;
        };
        sdk->applyConfig = [fault](int, const mv::Config&) { return fault != 1; };
        // Fault 2 leaves readback at the old crop even though apply returned success.
        MindVisionCamera camera(0, "", sdk, nullptr, true, mv::overviewConfig(experiment));
        MIB_EXPECT(!camera.start(),
                   "unknown sensor, rejected config or stale readback fails closed");
        MIB_EXPECT(fake.playCalls == 0 && fake.allocCalls == 0, "never stream incorrect geometry");
    }
    return mib::test::exitCode();
}
