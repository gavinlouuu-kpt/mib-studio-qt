// Real application-pipeline coverage for the optional Aravis consumer.
//
// This deliberately uses AppBackend's production factory, CaptureService's
// FrameStore publication, and ProcessingService's realtime loop. It does not
// replace callbacks or inject frames, so a passing test covers the software
// path: AppBackend -> Aravis Fake -> CaptureService -> FrameStore -> realtime
// processing. Physical GenTL/PL transport, SSD recording, and the desktop UI
// remain outside this Fake-camera test.

#include "backend/app/AppBackend.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

backend::services::ProcessingConfig lenientProcessing()
{
    backend::services::ProcessingConfig config;
    config.gaussian_blur_size = 3;
    config.bg_subtract_threshold = 8;
    config.morph_kernel_size = 3;
    config.morph_iterations = 1;
    config.enable_border_check = false;
    config.enable_area_range_check = false;
    config.enable_deformability_range_check = false;
    config.enable_area_ratio_check = false;
    config.enable_ring_ratio_check = false;
    config.require_single_inner_contour = false;
    config.empty_frame_pixel_threshold = 1;
    config.auto_background_enabled = false;
    config.multi_image_enabled = false;
    return config;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir temp("aravis_pipeline_e2e");

    setEnv("MIB_CAMERA_MODE", "aravis");
    setEnv("MIB_ARAVIS_DEVICE_ID", "Fake_1");
    setEnv("MIB_ARAVIS_FAKE", "1");
    // Keep unrelated persistence and hardware workers out of this software
    // pipeline test, while deliberately leaving capture and processing on.
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");

    backend::AppBackend app;
    MIB_REQUIRE(app.initialize((temp.path() / "data").string()),
                "AppBackend initializes for Aravis Fake E2E");
    const auto source = app.cameraSourceInfo();
    MIB_REQUIRE(source.requested == "aravis" && source.effective == "aravis" &&
                    source.simulated && !source.fallback,
                "AppBackend keeps explicit Aravis Fake selection truthful");

    backend::services::CaptureService::Config captureConfig;
    captureConfig.bufferPartCount = 1;
    captureConfig.numBuffers = 4;
    captureConfig.deliveryMode = camera::common::FrameDeliveryMode::EveryFrame;
    app.capture().setConfig(captureConfig);

    app.processing().setProcessingConfig(lenientProcessing());
    app.processing().setRealtimeProcessingMode(
        backend::services::ProcessingService::RealtimeProcessingMode::Inline);
    app.processing().setRealtimeDropFrames(false);
    auto store = app.getFrameStore();
    MIB_REQUIRE(static_cast<bool>(store), "AppBackend provides its FrameStore");
    app.processing().startRealtime(store);
    app.processing().setRealtimeEnabled(true);
    // Keep the realtime consumer in the existing conservation/accounting
    // contract for the first capture cycle. The current realtime cursor starts
    // at index 0 and begins admission at index 1, so the initial committed
    // index 0 is intentionally outside this run's admitted range. Every
    // admitted frame must still end in exactly one processing term after the
    // explicit drain below.
    app.processing().setExperimentAccountingContext(1, false);
    app.processing().startExperiment();

    watchdog.mark("first capture");
    const uint64_t firstCycleStartCommitted = store->committedCount();
    MIB_REQUIRE(app.capture().start(), "AppBackend capture start is accepted");
    MIB_REQUIRE(app.capture().waitForState({backend::services::CaptureLifecycleState::Running},
                                           std::chrono::seconds(10)) ==
                    backend::services::CaptureLifecycleState::Running,
                "Aravis Fake reaches CaptureService Running");
    MIB_REQUIRE(waitFor([&] { return store->committedCount() >= 8; },
                        std::chrono::seconds(10)),
                "Aravis frames commit through the production FrameStore path");

    backend::services::ProcessingService::RealtimeSnapshot snapshot;
    MIB_REQUIRE(waitFor([&] { return app.processing().getLatestSnapshot(snapshot); },
                        std::chrono::seconds(15)),
                "realtime processing publishes a snapshot");
    const uint64_t firstSnapshotIndex = snapshot.index;
    backend::playback::Frame retained;
    MIB_REQUIRE(store->getByWriteIndex(firstSnapshotIndex, retained),
                "snapshot index identifies a retained source frame");
    MIB_REQUIRE(retained.width > 0 && retained.height > 0 && !retained.data.empty(),
                "retained Aravis frame has geometry and payload");
    MIB_EXPECT(retained.pixelFormat == 0x01080001ULL,
               "retained Aravis frame is PFNC Mono8");
    MIB_EXPECT(retained.linePitch >= retained.width &&
                   retained.data.size() >= retained.linePitch * retained.height,
               "retained Aravis frame has a valid pitch and payload size");
    const auto retainedBytes = retained.data;
    MIB_EXPECT(!snapshot.mask.empty(), "realtime snapshot contains a processed mask");
    MIB_EXPECT(retained.hostTimestampUs > 0, "Aravis capture publishes host timestamps");

    const uint64_t firstCommitted = store->committedCount();
    MIB_REQUIRE(waitFor([&] { return store->committedCount() > firstCommitted + 8; },
                        std::chrono::seconds(10)),
                "capture continues to publish after the first processed snapshot");
    backend::playback::Frame retainedAgain;
    MIB_REQUIRE(store->getByWriteIndex(firstSnapshotIndex, retainedAgain),
                "initial source frame remains readable after later captures");
    MIB_EXPECT(retainedAgain.data == retainedBytes,
               "FrameStore retains copied Aravis bytes across subsequent captures");

    app.capture().stop();
    MIB_EXPECT(app.capture().lifecycleSnapshot().state ==
                   backend::services::CaptureLifecycleState::Idle,
               "first capture cycle stops at Idle");
    const uint64_t afterFirstStop = store->committedCount();
    const uint64_t firstCycleCommittedDelta = afterFirstStop - firstCycleStartCommitted;
    const uint64_t firstCycleDelivered =
        app.capture().stats().framesProcessed.load(std::memory_order_relaxed);
    MIB_EXPECT(firstCycleDelivered == firstCycleCommittedDelta,
               "CaptureService delivery count equals committed FrameStore delta");
    MIB_REQUIRE(afterFirstStop > firstCycleStartCommitted,
                "first capture cycle committed at least one frame");
    MIB_REQUIRE(waitFor([&] {
        backend::services::ProcessingService::RealtimeSnapshot drained;
        return app.processing().getLatestSnapshot(drained) &&
               drained.index >= afterFirstStop - 1;
    }, std::chrono::seconds(15)),
                "realtime processing drains through the final committed frame");
    app.processing().endExperiment();
    const auto accounting = app.processing().experimentAccountingSnapshot();
    MIB_EXPECT(accounting.admitted > 0, "processing accounting admits captured frames");
    MIB_EXPECT(accounting.hasIndexRange && accounting.firstFrameIndex == 1,
               "processing accounting starts at the current realtime cursor boundary");
    MIB_EXPECT(accounting.lastFrameIndex == afterFirstStop - 1,
               "processing accounting reaches the final committed frame");
    MIB_EXPECT(accounting.admitted + 1 == firstCycleCommittedDelta,
               "admitted frames plus the intentionally excluded index zero equal capture delta");
    MIB_EXPECT(accounting.reconciled && accounting.framesReconcile(),
               "processing frame accounting conserves every admitted frame");
    MIB_EXPECT(accounting.frameTermsSum() == accounting.admitted,
               "processing accounting terms equal admitted frames");
    MIB_EXPECT(accounting.sequenceGaps == 0 && accounting.sequenceGapFrames == 0,
               "processing accounting reports no sequence gaps");
    MIB_EXPECT(accounting.storeOverwritten == 0 && accounting.storeNotCommitted == 0 &&
                   accounting.storeMalformed == 0 && accounting.cancelledByPolicy == 0 &&
                   accounting.pendingAtStop == 0 && accounting.processingFailed == 0,
               "processing accounting reports no store loss, drops, pending, or failures");
    // HDF5 is intentionally disabled for this software-pipeline test, so
    // valid processed frames remain in the experiment buffer and are reported
    // as persistencePendingAtStop. That is an explicit unpersisted test policy,
    // not capture/processing loss; frame-level pendingAtStop above must remain
    // zero after the realtime drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MIB_EXPECT(store->committedCount() == afterFirstStop,
               "stopped capture does not continue publishing");

    // Repeat the camera lifecycle while keeping the realtime service alive,
    // so this also covers a genuine reconnect into the same FrameStore.
    watchdog.mark("capture restart");
    MIB_REQUIRE(app.capture().start(), "capture restart is accepted");
    MIB_REQUIRE(app.capture().waitForState({backend::services::CaptureLifecycleState::Running},
                                           std::chrono::seconds(10)) ==
                    backend::services::CaptureLifecycleState::Running,
                "capture restart reaches Running");
    MIB_REQUIRE(waitFor([&] { return store->committedCount() > afterFirstStop + 8; },
                        std::chrono::seconds(10)),
                "capture restart publishes fresh frames");
    backend::services::ProcessingService::RealtimeSnapshot restartedSnapshot;
    MIB_REQUIRE(waitFor([&] { return app.processing().getLatestSnapshot(restartedSnapshot) &&
                                     restartedSnapshot.index >= afterFirstStop; },
                        std::chrono::seconds(15)),
                "realtime processing publishes a snapshot from the new capture cycle");

    // Shutdown while the camera is actively streaming. AppBackend owns the
    // order: capture first, then realtime processing. It is intentionally
    // called twice to cover the idempotent shutdown contract.
    watchdog.mark("shutdown while streaming");
    app.shutdown();
    const uint64_t afterShutdown = store->committedCount();
    MIB_EXPECT(app.capture().lifecycleSnapshot().state ==
                   backend::services::CaptureLifecycleState::Idle,
               "AppBackend shutdown leaves capture Idle");
    MIB_EXPECT(!app.processing().isRealtimeRunning(),
               "AppBackend shutdown stops realtime processing");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MIB_EXPECT(store->committedCount() == afterShutdown,
               "AppBackend shutdown prevents further frame publication");
    app.shutdown();

    setEnv("MIB_CAMERA_MODE", nullptr);
    setEnv("MIB_ARAVIS_DEVICE_ID", nullptr);
    setEnv("MIB_ARAVIS_FAKE", nullptr);
    setEnv("MIB_DISABLED_SERVICES", nullptr);
    return mib::test::exitCode();
}
