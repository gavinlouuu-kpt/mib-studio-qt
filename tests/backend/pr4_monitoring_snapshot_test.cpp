// pr4_monitoring_snapshot_test
//
// Verifies PR4 invariants through the public realtime-loop API:
//   1. MONITORING GATING: monitoring buffers stay empty when setMonitoringActive(false)
//      (default); fill when setMonitoringActive(true); stop filling when disabled again.
//   2. SNAPSHOT SHARING: getLatestSnapshot returns a valid snapshot (no clone required);
//      verified indirectly by running the realtime loop and confirming the snapshot is
//      populated and readable without crash after PR4 pointer-swap change.

#include "backend/processing/ProcessingService.h"
#include "backend/playback/FrameStore.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using PS = backend::services::ProcessingService;
using backend::playback::FrameStore;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kW = 128;
constexpr int kH = 128;
constexpr uint64_t kMono8 = 0x01080001ULL;

backend::services::ProcessingConfig lenient() {
    backend::services::ProcessingConfig c;
    c.gaussian_blur_size = 3;
    c.bg_subtract_threshold = 8;
    c.morph_kernel_size = 3;
    c.morph_iterations = 1;
    c.enable_border_check = false;
    c.enable_area_range_check = false;
    c.enable_deformability_range_check = false;
    c.enable_area_ratio_check = false;
    c.enable_ring_ratio_check = false;
    c.require_single_inner_contour = false;
    c.empty_frame_pixel_threshold = 1; // almost nothing classifies as empty
    c.auto_background_enabled = false;
    c.multi_image_enabled = false;
    return c;
}

// Push a synthetic blob frame into the FrameStore
void pushBlob(FrameStore& store) {
    cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
    cv::circle(m, cv::Point(64, 64), 20, cv::Scalar(220), -1);
    store.pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH,
                    static_cast<size_t>(kW), kMono8, 0);
}

// Run the realtime loop for durationMs, push frames at ~1kfps, return when done.
// Fills monitoringFrameCount with the monitoring buffer total at the end.
void runAndCheck(const char* label,
                 bool monitoringOn,
                 size_t* outMonValidCount,
                 size_t* outMonInvalidCount,
                 size_t* outSnapshots,
                 PS::RealtimeProcessingMode mode=PS::RealtimeProcessingMode::Inline)
{
    auto store = std::make_shared<FrameStore>(2000);
    PS proc;
    proc.setProcessingConfig(lenient());
    proc.setRealtimeProcessingMode(mode);
    proc.setPixelToMicronFactor(0.5);
    proc.setRealtimeDropFrames(true);
    proc.setMonitoringActive(monitoringOn);
    proc.start(1);
    proc.startRealtime(store);
    proc.setRealtimeEnabled(true);

    // Producer: push blob frames for 200ms
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            pushBlob(*store);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    if (producer.joinable()) producer.join();

    proc.stopRealtime();
    proc.stop();

    // A later calibration change must never relabel already analysed rows.
    proc.setPixelToMicronFactor(2.0);
    for(const auto& frame:proc.getMonitoringValidFrames())
        MIB_EXPECT(frame.validation.analysisPixelToMicronFactor==0.5,"valid monitoring row retains exact analysis-time calibration");
    for(const auto& frame:proc.getMonitoringInvalidFrames())
        MIB_EXPECT(frame.validation.analysisPixelToMicronFactor==0.5,"invalid monitoring row retains exact analysis-time calibration");
    // Count what accumulated
    *outMonValidCount   = proc.getMonitoringValidFrames().size();
    *outMonInvalidCount = proc.getMonitoringInvalidFrames().size();

    PS::RealtimeSnapshot snap;
    *outSnapshots = proc.getLatestSnapshot(snap) ? 1 : 0;

    std::printf("  %s: mon_valid=%zu mon_invalid=%zu snapshot=%zu\n",
                label, *outMonValidCount, *outMonInvalidCount, *outSnapshots);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: monitoring gating
// ─────────────────────────────────────────────────────────────────────────────
void testMonitoringGating() {
    std::printf("testMonitoringGating\n");

    mib::test::Watchdog wd(15);

    size_t monValid = 0, monInvalid = 0, snaps = 0;

    // --- INACTIVE (default) ---
    runAndCheck("inactive", false, &monValid, &monInvalid, &snaps);
    MIB_EXPECT(monValid   == 0, "monitoring valid must stay 0 when inactive");
    MIB_EXPECT(monInvalid == 0, "monitoring invalid must stay 0 when inactive");
    MIB_EXPECT(snaps      == 1, "snapshot should be populated even when monitoring is inactive");

    // --- ACTIVE ---
    runAndCheck("active", true, &monValid, &monInvalid, &snaps);
    MIB_EXPECT(monValid + monInvalid > 0,
               "at least some frames must accumulate in monitoring when active");
    MIB_EXPECT(snaps == 1, "snapshot should be populated when monitoring is active");

    std::printf("testMonitoringGating: done\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: snapshot is readable without tearing after pointer-swap change
// ─────────────────────────────────────────────────────────────────────────────
void testSnapshotReadable() {
    std::printf("testSnapshotReadable\n");

    mib::test::Watchdog wd(15);

    auto store = std::make_shared<FrameStore>(2000);
    PS proc;
    proc.setProcessingConfig(lenient());
    proc.setRealtimeProcessingMode(PS::RealtimeProcessingMode::Inline);
    proc.setRealtimeDropFrames(true);
    proc.start(1);
    proc.startRealtime(store);
    proc.setRealtimeEnabled(true);

    // Push frames
    for (int i = 0; i < 200; ++i) {
        pushBlob(*store);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    proc.stopRealtime();
    proc.stop();

    PS::RealtimeSnapshot snap;
    bool got = proc.getLatestSnapshot(snap);
    MIB_EXPECT(got, "getLatestSnapshot must return true after processing frames");
    if (got) {
        MIB_EXPECT(snap.originalImage.empty(), "source-pixel retention is opt-in");
        MIB_EXPECT(!snap.mask.empty(),  "snapshot mask must not be empty");
        MIB_EXPECT(snap.mask.cols == kW, "snapshot mask width matches frame width");
        MIB_EXPECT(snap.mask.rows == kH, "snapshot mask height matches frame height");
        MIB_EXPECT(snap.index > 0,      "snapshot index must be > 0");
    }

    // A second read must be consistent (same index, same mask dims)
    PS::RealtimeSnapshot snap2;
    bool got2 = proc.getLatestSnapshot(snap2);
    if (got && got2) {
        MIB_EXPECT(snap.index == snap2.index, "two consecutive reads must see same snapshot index");
        MIB_EXPECT(snap.mask.cols == snap2.mask.cols, "snapshot mask cols stable");
        MIB_EXPECT(snap.mask.rows == snap2.mask.rows, "snapshot mask rows stable");
    }

    std::printf("testSnapshotReadable: done\n");
}

} // namespace

int main() {
    testMonitoringGating();
    testSnapshotReadable();
    size_t valid=0,invalid=0,snapshots=0;
    runAndCheck("batch calibration",true,&valid,&invalid,&snapshots,PS::RealtimeProcessingMode::AsyncBatch);
    MIB_EXPECT(valid+invalid>0,"batch monitoring exercises stamped calibration");
    PS process;
    cv::Mat blob(kH,kW,CV_8UC1,cv::Scalar(0));cv::circle(blob,{64,64},20,cv::Scalar(220),-1);
    process.setPixelToMicronFactor(0.25);
    const auto first=process.processBatch({blob},lenient());
    process.setPixelToMicronFactor(2.0);
    const auto second=process.processBatch({blob},lenient());
    MIB_REQUIRE(!first.empty()&&!second.empty(),"batch rows available");
    MIB_EXPECT(first.front().validation.analysisPixelToMicronFactor==0.25,"old batch result retains old calibration");
    MIB_EXPECT(second.front().validation.analysisPixelToMicronFactor==2.0,"new batch result uses new calibration");

    if (mib::test::failureCount() == 0) {
        std::printf("All PR4 monitoring/snapshot invariant tests passed\n");
    } else {
        std::printf("%d test(s) FAILED\n", mib::test::failureCount());
    }
    return mib::test::exitCode();
}
