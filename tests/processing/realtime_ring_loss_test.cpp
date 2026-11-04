// realtime_ring_loss_test (issue #857)
//
// The realtime inline loop works through a snapshot of the frame ring. When it
// falls behind, the camera evicts frames of that snapshot before the loop reads
// them. Those frames must be booked: admitted and counted as storeOverwritten,
// with no hole in the admitted sequence. Before #857 the loop stepped over them
// with `continue`, so the bench run that fell behind with the Monitoring tab open
// reported storeOverwritten=3 and sequenceGaps=2571 for about 7,900 lost frames.
//
// It also proves the monitoring ring keeps compact ROI crops: a row that keeps a
// view into the full-frame gray image and mask pins the whole frame (0.5 MB per
// row at 1216x208), and with the tab open at 5000 fps that is about 1 GB.
//
// Deterministic: a gate in the test kernel holds the loop on a chosen frame
// while the test pushes enough frames to evict part of the loop's snapshot.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace rec = backend::recording;
using backend::services::ProcessingService;

namespace {
bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

void pushMat(backend::playback::FrameStore& s, const cv::Mat& m, uint64_t ts)
{
    s.pushFrame(m.data, m.total(), m.cols, m.rows, m.cols, 0x01080001, ts, ts);
}

void configure(ProcessingService& svc)
{
    auto cfg = svc.getProcessingConfig();
    cfg.empty_frame_pixel_threshold = 1;
    cfg.bg_subtract_threshold = 100;
    cfg.enable_border_check = false;
    cfg.enable_area_range_check = false;
    cfg.enable_deformability_range_check = false;
    cfg.enable_ring_ratio_check = false;
    cfg.enable_area_ratio_check = false;
    cfg.require_single_inner_contour = false;
    cfg.auto_background_enabled = false;
    svc.setProcessingConfig(cfg);
}
} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(60);

    constexpr size_t kCapacity = 16;
    constexpr int kW = 96;
    constexpr int kH = 96;
    auto store = std::make_shared<backend::playback::FrameStore>(kCapacity);
    ProcessingService svc;
    svc.start(1);
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string err;
    MIB_REQUIRE(svc.activateProcessingKernel(kernel, &err), "activate test kernel: " + err);
    configure(svc);
    svc.setRealtimeRoi(ProcessingService::Roi{0, 0, kW, kH});
    svc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    svc.setInvalidFrameSamplingRate(1);
    svc.startRealtime(store);
    MIB_REQUIRE(svc.isRealtimeRunning(), "realtime running");

    uint64_t ts = 1;
    // Warm-up, outside the run. The loop starts with write index 0 counted as processed, so the
    // first frame it examines is index 1.
    pushMat(*store, mib::test::ringFrame(kW, kH, 0), ts++);
    pushMat(*store, mib::test::ringFrame(kW, kH, 0), ts++);
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() == 1; }, std::chrono::seconds(5)),
                "warm-up frame examined");

    // ---- 1. Frames evicted from the loop's snapshot are booked, not skipped ----
    wd.mark("eviction inside a pass");
    svc.setExperimentAccountingContext(/*generation=*/7, /*policyAllowsDrops=*/false);
    svc.startExperiment();
    const uint64_t base = kernel->emptyCalls.load();

    // Hold the loop on F0 while F1..F16 arrive: its next pass snapshots F1..F16.
    kernel->holdCall.store(base + 1);
    pushMat(*store, mib::test::ringFrame(kW, kH, 1), ts++); // F0
    MIB_REQUIRE(waitFor([&] { return kernel->heldCall.load() == base + 1; }, std::chrono::seconds(5)),
                "loop held on F0");
    for (int i = 1; i <= 16; ++i) pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);

    // Then hold it on F1, the first frame of that pass, while F17..F24 evict F1..F8.
    kernel->holdCall.store(base + 2);
    kernel->release(base + 1);
    MIB_REQUIRE(waitFor([&] { return kernel->heldCall.load() == base + 2; }, std::chrono::seconds(5)),
                "loop held on F1, inside its pass over F1..F16");
    for (int i = 17; i <= 24; ++i) pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);
    MIB_EXPECT(store->earliestAvailableIndex() == 11, "F1..F8 (write indices 3..10) evicted");
    kernel->holdCall.store(0);
    kernel->release(base + 2);

    // F0, F1, F9..F16 and F17..F24 are examined; F2..F8 were gone before the pass reached them.
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() == base + 18; }, std::chrono::seconds(10)),
                "every frame still in the ring examined");
    // Not a REQUIRE: with the old loop the count stops at 18, and the numbers below say why.
    waitFor([&] { return svc.experimentAccountingSnapshot().admitted >= 25; }, std::chrono::seconds(2));
    svc.endExperiment();
    {
        const auto a = svc.experimentAccountingSnapshot();
        std::fprintf(stderr,
                     "eviction: admitted=%llu processed=%llu rejected=%llu empty=%llu ow=%llu gaps=%llu "
                     "gapFrames=%llu completion=%s (%s)\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.processed,
                     (unsigned long long)a.scientificallyRejected, (unsigned long long)a.empty,
                     (unsigned long long)a.storeOverwritten, (unsigned long long)a.sequenceGaps,
                     (unsigned long long)a.sequenceGapFrames, rec::toString(a.completion),
                     a.completionReason.c_str());
        MIB_EXPECT(a.admitted == 25, "all 25 frames of the run admitted (F0..F24)");
        MIB_EXPECT(a.storeOverwritten == 7, "F2..F8, evicted inside the pass, booked as storeOverwritten");
        MIB_EXPECT(a.sequenceGaps == 0 && a.sequenceGapFrames == 0,
                   "no hole in the admitted sequence (they were skipped silently before #857)");
        MIB_EXPECT(a.firstFrameIndex == 2 && a.lastFrameIndex == 26, "the run spans write indices 2..26");
        MIB_EXPECT(a.reconciled, "frame terms reconcile");
        MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "store loss -> incompleteLoss");
    }
    svc.clearAccumulatedFrames();

    // ---- 2. Monitoring rows own their ROI crop, not the whole frame ------------
    // The every-frame path (the one a run uses; live view drops to the latest frame and already
    // works on an ROI copy) measured on views of the full-frame gray image and mask.
    wd.mark("monitoring crop");
    const ProcessingService::Roi roi{16, 20, 64, 48};
    svc.setRealtimeDropFrames(false);
    svc.setRealtimeRoi(roi);
    svc.clearMonitoringFrames();
    svc.setMonitoringActive(true);
    const uint64_t before = kernel->emptyCalls.load();
    for (int i = 0; i < 4; ++i) pushMat(*store, mib::test::ringFrame(kW, kH, 100 + i), ts++);
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() >= before + 4; }, std::chrono::seconds(5)),
                "monitoring frames examined");
    MIB_REQUIRE(waitFor([&] {
                    return !svc.getMonitoringValidFrames().empty() || !svc.getMonitoringInvalidFrames().empty();
                }, std::chrono::seconds(5)),
                "monitoring rows appended");
    auto rows = svc.getMonitoringValidFrames();
    const auto invalid = svc.getMonitoringInvalidFrames();
    rows.insert(rows.end(), invalid.begin(), invalid.end());
    for (const auto& row : rows) {
        for (const cv::Mat* m : {&row.originalImage, &row.processedImage}) {
            MIB_EXPECT(m->cols == roi.w && m->rows == roi.h, "row keeps the ROI-sized crop");
            const auto held = static_cast<size_t>(m->dataend - m->datastart);
            MIB_EXPECT(!m->isSubmatrix() && held == m->total() * m->elemSize(),
                       "row owns only its crop (" + std::to_string(held) + " bytes held for a " +
                           std::to_string(m->total() * m->elemSize()) + "-byte crop)");
        }
    }
    svc.setMonitoringActive(false);

    svc.stopRealtime();
    svc.stop();
    return mib::test::exitCode();
}
