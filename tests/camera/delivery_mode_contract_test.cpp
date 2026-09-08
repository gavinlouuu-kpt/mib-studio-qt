// Contract-level tests for FrameDeliveryMode: serialization helpers,
// CaptureService mode plumbing (requested vs backend-confirmed), acquisition
// queue telemetry propagation, and the unsupported-mode error path.

#include "support/assert.h"
#include "support/queue_camera.h"
#include "support/watchdog.h"

#include "backend/camera/common/ICamera.h"
#include "backend/services/CaptureService.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using backend::services::CaptureService;
using camera::common::FrameDeliveryMode;
using mib::test::QueueBackedTestCamera;

namespace {

// Poll until `pred` holds or ~2 s elapse; keeps timing gates ratio-free.
template <typename Pred>
bool eventually(Pred pred)
{
    for (int i = 0; i < 300; ++i) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

} // namespace

int main()
{
    mib::test::Watchdog dog(30);

    // --- Serialization helpers ----------------------------------------------
    dog.mark("serialization");
    MIB_EXPECT(std::string(camera::common::toString(FrameDeliveryMode::EveryFrame)) ==
                   "everyFrame",
               "EveryFrame serializes as everyFrame");
    MIB_EXPECT(std::string(camera::common::toString(FrameDeliveryMode::LatestFrame)) ==
                   "latestFrame",
               "LatestFrame serializes as latestFrame");
    MIB_EXPECT(camera::common::frameDeliveryModeFromString("latestFrame") ==
                   FrameDeliveryMode::LatestFrame,
               "round trip latestFrame");
    MIB_EXPECT(camera::common::frameDeliveryModeFromString("everyFrame") ==
                   FrameDeliveryMode::EveryFrame,
               "round trip everyFrame");
    // Deterministic migration: unknown/legacy values load as EveryFrame.
    MIB_EXPECT(camera::common::frameDeliveryModeFromString("") ==
                   FrameDeliveryMode::EveryFrame,
               "missing value maps to EveryFrame");
    MIB_EXPECT(camera::common::frameDeliveryModeFromString("garbage") ==
                   FrameDeliveryMode::EveryFrame,
               "unknown value maps to EveryFrame");

    // --- CaptureService plumbing: LatestFrame confirmed + telemetry ----------
    dog.mark("captureservice latestframe");
    {
        CaptureService service;
        QueueBackedTestCamera* rawCamera = nullptr;
        service.setCameraFactory([&rawCamera]() {
            QueueBackedTestCamera::Options options;
            options.produceInterval = std::chrono::microseconds(500);
            auto camera = std::make_unique<QueueBackedTestCamera>(options);
            rawCamera = camera.get();
            return std::unique_ptr<camera::common::ICamera>(std::move(camera));
        });

        CaptureService::Config cfg;
        cfg.numBuffers = 8;
        cfg.deliveryMode = FrameDeliveryMode::LatestFrame;
        service.setConfig(cfg);

        std::atomic<uint64_t> callbackFrames{0};
        service.setFrameCallback([&callbackFrames](const uint8_t*, size_t, uint64_t, uint64_t,
                                                   uint64_t) {
            ++callbackFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // slow consumer
        });

        MIB_REQUIRE(service.start(), "capture service must start");
        MIB_EXPECT(eventually([&] {
                       return service.stats().deliveryModeConfirmed.load() &&
                              callbackFrames.load() >= 5;
                   }),
                   "capture loop must confirm mode and deliver frames");
        MIB_EXPECT(service.stats().requestedDeliveryMode.load() ==
                       static_cast<int>(FrameDeliveryMode::LatestFrame),
                   "requested mode recorded");
        MIB_EXPECT(service.activeDeliveryMode() == FrameDeliveryMode::LatestFrame,
                   "backend-confirmed active mode is LatestFrame");
        // Queue telemetry propagates on the 1 s stats poll.
        MIB_EXPECT(eventually([&] { return service.stats().queueStatsValid.load(); }),
                   "queue stats must reach CaptureStats");
        MIB_EXPECT(eventually([&] {
                       return service.stats().intentionallyDiscardedFrames.load() > 0;
                   }),
                   "intentional discards must propagate under overload");
        MIB_EXPECT(service.stats().frameAgeValid.load(),
                   "frame age must be computed for host-comparable timestamps");
        service.stop();
        MIB_EXPECT(!service.isRunning(), "service stops cleanly");
    }

    // --- CaptureService plumbing: EveryFrame stays ordered, no discards ------
    dog.mark("captureservice everyframe");
    {
        CaptureService service;
        service.setCameraFactory([]() -> std::unique_ptr<camera::common::ICamera> {
            QueueBackedTestCamera::Options options;
            options.produceInterval = std::chrono::microseconds(500);
            return std::make_unique<QueueBackedTestCamera>(options);
        });

        CaptureService::Config cfg;
        cfg.numBuffers = 8;
        cfg.deliveryMode = FrameDeliveryMode::EveryFrame;
        service.setConfig(cfg);

        MIB_REQUIRE(service.start(), "capture service must start");
        MIB_EXPECT(eventually([&] { return service.stats().deliveryModeConfirmed.load(); }),
                   "mode confirmation must land");
        MIB_EXPECT(service.activeDeliveryMode() == FrameDeliveryMode::EveryFrame,
                   "backend-confirmed active mode is EveryFrame");
        MIB_EXPECT(service.stats().intentionallyDiscardedFrames.load() == 0,
                   "EveryFrame must not intentionally discard");
        service.stop();
    }

    // --- Unsupported mode: actionable failure, no capture -------------------
    dog.mark("unsupported mode");
    {
        CaptureService service;
        service.setCameraFactory([]() -> std::unique_ptr<camera::common::ICamera> {
            QueueBackedTestCamera::Options options;
            options.supportsLatestFrame = false;
            return std::make_unique<QueueBackedTestCamera>(options);
        });

        CaptureService::Config cfg;
        cfg.deliveryMode = FrameDeliveryMode::LatestFrame;
        service.setConfig(cfg);

        service.start(); // capture thread starts, then rejects the mode
        MIB_EXPECT(eventually([&] { return !service.isRunning(); }),
                   "unsupported mode must stop the capture service");
        MIB_EXPECT(!service.stats().deliveryModeConfirmed.load(),
                   "a rejected mode must never be reported as confirmed");
        MIB_EXPECT(service.stats().framesProcessed.load() == 0,
                   "no frames may be captured in a rejected mode");
        service.stop();
    }

    return mib::test::exitCode();
}
