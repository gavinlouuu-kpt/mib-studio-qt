// realtime_ring_loss_test (issue #857, and the review of PR #876)
//
// The realtime loops work through a snapshot of the frame ring. When they fall behind, the camera
// evicts frames of that snapshot before the loop reads them. What this test pins:
//
// 1. Frames evicted from the snapshot inside a pass are booked: admitted and counted as
//    storeOverwritten, with no hole in the admitted sequence, in the inline loop and in the async
//    batch loop. Before #857 the loops stepped over them with `continue`, so the bench run that
//    fell behind with the Monitoring tab open reported storeOverwritten=3 and sequenceGaps=2571
//    for about 7,900 lost frames.
// 2. Frames evicted before the pass reached them (the loop's pointer fell out of the ring) are
//    booked as storeOverwritten and are not also read as a sequence gap by the next admitted frame.
// 3. A store with no frames mid-pass (FrameStore::resize renumbers from 0: OutOfRange) is not a
//    malformed frame: the loops leave it to the pointer resync at the top of the pass, in the
//    inline loop and in the async batch loop.
// 4. Bounded background calibration does not spend its attempt budget on frames the store could
//    not give.
// 5. The monitoring rings keep what the Monitoring tab shows and no more. A row keeps an ROI crop
//    of its frame; a view into the full-frame gray image and mask pinned the whole frame (0.5 MB
//    per row at 1216x208), and with the tab open at 5000 fps that is about 1 GB. A count cap alone
//    still allowed that for a whole-frame or large ROI, so each ring is also held to
//    kMonitoringImageBytesPerRing: past it the oldest rows give up their images and keep their
//    measurements, the newest row always keeps its own. Checked for the every-frame path and for
//    drop-to-latest, with an ROI of at least half the frame, the whole frame and no ROI. Drop-to-
//    latest with an ROI already measured on ROI-sized copies; with no ROI it measures the whole
//    frame, the crop is the whole frame and the budget is what bounds it.
// 6. A realtime loop restarted in a run (#899) books only the frames the run lost while no loop
//    ran: the ones evicted before the new loop got to them. MainWindow stopped realtime processing
//    when the user left the Experiment tab mid-run and started it again on return; both loops
//    began at write index 0, so the first pass booked every frame the camera had written before
//    the ring's oldest one: storeOverwritten=2,196,816 for a 26 s run on the bench. The same start
//    from 0 booked the ring's history when realtime starts after the run does (the coordinator's
//    order for a Start without a running consumer). Checked for both loops, with the stop's frames
//    inside the ring and past it.
//
// Deterministic: a gate in the test kernel (inline) or a hook in the async loop holds the loop on a
// chosen frame while the test pushes enough frames to evict part of the loop's snapshot.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ExperimentFrameBuffer.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rec = backend::recording;
using backend::services::ProcessedFrame;
using backend::services::ProcessingService;
using backend::services::processedFrameBytes;
using Mode = ProcessingService::RealtimeProcessingMode;
using Roi = ProcessingService::Roi;

namespace {
constexpr uint64_t kNone = std::numeric_limits<uint64_t>::max();

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

// A realtime service on a small ring, fed by the test.
struct Rig {
    std::shared_ptr<backend::playback::FrameStore> store;
    ProcessingService svc;
    std::shared_ptr<mib::test::FaultKernel> kernel = std::make_shared<mib::test::FaultKernel>();
    uint64_t ts = 1;
    int w, h;

    Rig(size_t capacity, int frameW, int frameH, Mode mode, const Roi& roi, bool dropFrames)
        : store(std::make_shared<backend::playback::FrameStore>(capacity)), w(frameW), h(frameH)
    {
        svc.start(1);
        std::string err;
        MIB_REQUIRE(svc.activateProcessingKernel(kernel, &err), "activate test kernel: " + err);
        configure(svc);
        svc.setRealtimeRoi(roi);
        svc.setRealtimeProcessingMode(mode);
        svc.setRealtimeDropFrames(dropFrames);
        svc.setInvalidFrameSamplingRate(1);
        svc.startRealtime(store);
        MIB_REQUIRE(svc.isRealtimeRunning(), "realtime running");
    }
    ~Rig()
    {
        svc.setRealtimeFrameHookForTests({});
        kernel->holdCall.store(0);
        kernel->release(kNone);
        svc.stopRealtime();
        svc.stop();
    }
    // A frame with one ring in it (content varies the position); `blank` = an empty channel.
    void push(uint64_t content) { pushMat(*store, mib::test::ringFrame(w, h, content), ts++); }
    void pushBlank() { pushMat(*store, cv::Mat(h, w, CV_8UC1, cv::Scalar(0)), ts++); }
    uint64_t next() const { return store->totalWritten(); } // write index of the next frame pushed
    rec::RecordingAccountingSnapshot accounting() const { return svc.experimentAccountingSnapshot(); }
};

// Holds the async batch loop on a chosen write index (the realtime frame hook).
class IndexGate {
public:
    void holdAt(uint64_t idx) { holdAt_.store(idx); }
    void operator()(uint64_t idx)
    {
        if (idx != holdAt_.load()) return;
        std::unique_lock lk(m_);
        held_.store(idx);
        cv_.wait(lk, [&] { return released_ >= idx; });
        held_.store(kNone);
    }
    void release(uint64_t through)
    {
        {
            std::scoped_lock lk(m_);
            released_ = through;
        }
        cv_.notify_all();
    }
    bool isHeldAt(uint64_t idx) const { return held_.load() == idx; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t released_{0};
    std::atomic<uint64_t> holdAt_{kNone};
    std::atomic<uint64_t> held_{kNone};
};

void printAccounting(const char* label, const rec::RecordingAccountingSnapshot& a)
{
    std::fprintf(stderr,
                 "%s: admitted=%llu processed=%llu rejected=%llu empty=%llu ow=%llu malformed=%llu "
                 "gaps=%llu gapFrames=%llu completion=%s (%s)\n",
                 label, (unsigned long long)a.admitted, (unsigned long long)a.processed,
                 (unsigned long long)a.scientificallyRejected, (unsigned long long)a.empty,
                 (unsigned long long)a.storeOverwritten, (unsigned long long)a.storeMalformed,
                 (unsigned long long)a.sequenceGaps, (unsigned long long)a.sequenceGapFrames,
                 rec::toString(a.completion), a.completionReason.c_str());
}

// Async batch warm-up: the loop starts with write index 0 counted as processed.
void warmUpBatch(Rig& rig)
{
    rig.push(0);
    rig.push(0);
    MIB_REQUIRE(waitFor([&] { return rig.svc.getBatchPipelineStats().framesProcessed >= 1; },
                        std::chrono::seconds(5)),
                "warm-up frame processed by the batch pipeline");
}

// Inline warm-up, outside the run: the first frame the loop examines is write index 1.
void warmUpInline(Rig& rig)
{
    rig.push(0);
    rig.push(0);
    MIB_REQUIRE(waitFor([&] { return rig.kernel->emptyCalls.load() == 1; }, std::chrono::seconds(5)),
                "warm-up frame examined");
}

// ---- 1a. inline loop: frames evicted inside a pass are booked, not skipped ------------------
void inlineEvictionInsidePass()
{
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, Mode::Inline, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    auto& svc = rig.svc;
    auto& kernel = *rig.kernel;
    warmUpInline(rig);

    rig.svc.setExperimentAccountingContext(/*generation=*/7, /*policyAllowsDrops=*/false);
    svc.startExperiment();
    const uint64_t base = kernel.emptyCalls.load();

    // Hold the loop on F0 while F1..F16 arrive: its next pass snapshots F1..F16.
    kernel.holdCall.store(base + 1);
    rig.push(1); // F0
    MIB_REQUIRE(waitFor([&] { return kernel.heldCall.load() == base + 1; }, std::chrono::seconds(5)),
                "loop held on F0");
    for (int i = 1; i <= 16; ++i) rig.push(i);

    // Then hold it on F1, the first frame of that pass, while F17..F24 evict F1..F8.
    kernel.holdCall.store(base + 2);
    kernel.release(base + 1);
    MIB_REQUIRE(waitFor([&] { return kernel.heldCall.load() == base + 2; }, std::chrono::seconds(5)),
                "loop held on F1, inside its pass over F1..F16");
    for (int i = 17; i <= 24; ++i) rig.push(i);
    MIB_EXPECT(rig.store->earliestAvailableIndex() == 11, "F1..F8 (write indices 3..10) evicted");
    kernel.holdCall.store(0);
    kernel.release(base + 2);

    // F0, F1, F9..F16 and F17..F24 are examined; F2..F8 were gone before the pass reached them.
    MIB_REQUIRE(waitFor([&] { return kernel.emptyCalls.load() == base + 18; }, std::chrono::seconds(10)),
                "every frame still in the ring examined");
    // Not a REQUIRE: with the old loop the count stops at 18, and the numbers below say why.
    waitFor([&] { return rig.accounting().admitted >= 25; }, std::chrono::seconds(2));
    svc.endExperiment();
    const auto a = rig.accounting();
    printAccounting("inline eviction", a);
    MIB_EXPECT(a.admitted == 25, "all 25 frames of the run admitted (F0..F24)");
    MIB_EXPECT(a.storeOverwritten == 7, "F2..F8, evicted inside the pass, booked as storeOverwritten");
    MIB_EXPECT(a.sequenceGaps == 0 && a.sequenceGapFrames == 0,
               "no hole in the admitted sequence (they were skipped silently before #857)");
    MIB_EXPECT(a.firstFrameIndex == 2 && a.lastFrameIndex == 26, "the run spans write indices 2..26");
    MIB_EXPECT(a.reconciled, "frame terms reconcile");
    MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "store loss -> incompleteLoss");
}

// ---- 1b. async batch loop: the same, in realtimeBatchLoop -----------------------------------
void asyncEvictionInsidePass()
{
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, Mode::AsyncBatch, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    IndexGate gate;
    rig.svc.setRealtimeFrameHookForTests([&](uint64_t idx) { gate(idx); });
    warmUpBatch(rig);

    rig.svc.setExperimentAccountingContext(/*generation=*/8, /*policyAllowsDrops=*/false);
    rig.svc.startExperiment();
    const uint64_t base = rig.next();

    // Hold the loop on F0 while F1..F16 arrive: its next pass is F1..F16.
    gate.holdAt(base);
    rig.push(1);
    MIB_REQUIRE(waitFor([&] { return gate.isHeldAt(base); }, std::chrono::seconds(5)),
                "async loop held on F0");
    for (int i = 1; i <= 16; ++i) rig.push(i + 1);

    // Then hold it on F1, the first frame of that pass, while F17..F24 evict F1..F8.
    gate.holdAt(base + 1);
    gate.release(base);
    MIB_REQUIRE(waitFor([&] { return gate.isHeldAt(base + 1); }, std::chrono::seconds(5)),
                "async loop held on F1, inside its pass over F1..F16");
    for (int i = 17; i <= 24; ++i) rig.push(i + 1);
    MIB_EXPECT(rig.store->earliestAvailableIndex() == base + 9, "F1..F8 evicted from the ring");
    gate.holdAt(kNone);
    gate.release(kNone);

    // F0, F1, F9..F16 and F17..F24 are read; F2..F8 were gone before the pass reached them. The
    // workers book the outcomes of the 18 frames that were queued.
    waitFor([&] {
        const auto a = rig.accounting();
        return a.admitted >= 25 && a.frameTermsSum() == a.admitted;
    }, std::chrono::seconds(10));
    rig.svc.endExperiment();
    const auto a = rig.accounting();
    printAccounting("async eviction", a);
    MIB_EXPECT(a.admitted == 25, "all 25 frames of the run admitted (F0..F24)");
    MIB_EXPECT(a.storeOverwritten == 7, "F2..F8, evicted inside the async pass, booked as storeOverwritten");
    MIB_EXPECT(a.storeMalformed == 0, "an evicted frame is not a malformed one");
    MIB_EXPECT(a.sequenceGaps == 0 && a.sequenceGapFrames == 0,
               "no hole in the admitted sequence (they were skipped silently before #857)");
    MIB_EXPECT(a.firstFrameIndex == base && a.lastFrameIndex == base + 24, "the run spans F0..F24");
    MIB_EXPECT(a.reconciled, "frame terms reconcile");
    MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "store loss -> incompleteLoss");
}

// ---- 2. frames evicted before the pass: booked once, no sequence gap on top -----------------
void evictionBeforePass(Mode mode)
{
    const bool async = mode == Mode::AsyncBatch;
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, mode, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    IndexGate gate;
    if (async) rig.svc.setRealtimeFrameHookForTests([&](uint64_t idx) { gate(idx); });
    if (async) warmUpBatch(rig); else warmUpInline(rig);
    rig.svc.setExperimentAccountingContext(/*generation=*/9, /*policyAllowsDrops=*/false);
    rig.svc.startExperiment();
    const uint64_t base = rig.kernel->emptyCalls.load();
    const uint64_t first = rig.next();

    // Hold the loop on F0, then push 20 more frames: the 16-slot ring keeps F5..F20, so when the
    // loop is released its pointer (F0) has fallen out of the ring by F1..F4.
    if (async) {
        gate.holdAt(first);
    } else {
        rig.kernel->holdCall.store(base + 1);
    }
    rig.push(1);
    MIB_REQUIRE(waitFor([&] { return async ? gate.isHeldAt(first) : rig.kernel->heldCall.load() == base + 1; },
                        std::chrono::seconds(5)),
                "loop held on F0");
    for (int i = 1; i <= 20; ++i) rig.push(i + 1);
    if (async) {
        gate.holdAt(kNone);
        gate.release(kNone);
    } else {
        rig.kernel->holdCall.store(0);
        rig.kernel->release(base + 1);
    }

    waitFor([&] {
        const auto a = rig.accounting();
        return a.admitted >= 21 && (!async || a.frameTermsSum() == a.admitted);
    }, std::chrono::seconds(10));
    if (!async) {
        MIB_EXPECT(rig.kernel->emptyCalls.load() == base + 17, "F0 and the 16 frames still in the ring examined");
    }
    rig.svc.endExperiment();
    const auto a = rig.accounting();
    printAccounting(async ? "eviction before the pass, async" : "eviction before the pass, inline", a);
    MIB_EXPECT(a.admitted == 21, "F0, the four lost frames and the 16 read frames admitted");
    MIB_EXPECT(a.storeOverwritten == 4, "F1..F4 booked as storeOverwritten");
    MIB_EXPECT(a.sequenceGaps == 0 && a.sequenceGapFrames == 0,
               "the booked loss is not also counted as a sequence gap");
    MIB_EXPECT(a.firstFrameIndex == first && a.lastFrameIndex == first + 20, "the run spans F0..F20");
    MIB_EXPECT(a.reconciled, "frame terms reconcile");
}

// ---- 3. a store without frames mid-pass (resize) is not a malformed frame ------------------
// FrameStore::resize to fewer slots than it holds clears the ring and renumbers from 0, so a read
// of an old index returns OutOfRange. The loops booked it as storeMalformed and admitted the old
// index; they break and let the resync at the top of the pass deal with the new numbering.
void outOfRangeMidPass(Mode mode)
{
    const bool async = mode == Mode::AsyncBatch;
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, mode, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    IndexGate gate;
    if (async) rig.svc.setRealtimeFrameHookForTests([&](uint64_t idx) { gate(idx); });
    if (async) warmUpBatch(rig); else warmUpInline(rig);
    rig.svc.setExperimentAccountingContext(/*generation=*/10, /*policyAllowsDrops=*/false);
    rig.svc.startExperiment();
    const uint64_t base = rig.kernel->emptyCalls.load();
    const uint64_t first = rig.next();

    // Hold the loop on F0, queue F1..F10 as its next pass, then hold it on F1 inside that pass.
    if (async) gate.holdAt(first); else rig.kernel->holdCall.store(base + 1);
    rig.push(1);
    MIB_REQUIRE(waitFor([&] { return async ? gate.isHeldAt(first) : rig.kernel->heldCall.load() == base + 1; },
                        std::chrono::seconds(5)),
                "loop held on F0");
    for (int i = 1; i <= 10; ++i) rig.push(i + 1);
    if (async) {
        gate.holdAt(first + 1);
        gate.release(first);
    } else {
        rig.kernel->holdCall.store(base + 2);
        rig.kernel->release(base + 1);
    }
    MIB_REQUIRE(waitFor([&] { return async ? gate.isHeldAt(first + 1) : rig.kernel->heldCall.load() == base + 2; },
                        std::chrono::seconds(5)),
                "loop held on F1, inside its pass over F1..F10");

    MIB_REQUIRE(rig.store->resize(2), "resize to fewer slots than held clears the ring");
    MIB_EXPECT(rig.store->totalWritten() == 0, "the ring is empty and renumbers from 0");
    if (async) {
        gate.holdAt(kNone);
        gate.release(kNone);
    } else {
        rig.kernel->holdCall.store(0);
        rig.kernel->release(base + 2);
    }

    // The regressed loop books F2..F10 as storeMalformed within a few ms; the fixed one books
    // nothing, so this wait runs out. New frames come only after it (they would end the old pass).
    const bool booked = waitFor([&] { return rig.accounting().storeMalformed > 0; }, std::chrono::milliseconds(300));
    MIB_EXPECT(!booked, "OutOfRange frames are not booked as storeMalformed");

    // The loop recovers on the new numbering: the first new frame is the resync point, the next one
    // is read.
    auto progress = [&]() -> uint64_t {
        return async ? rig.svc.getBatchPipelineStats().framesAccepted : rig.kernel->emptyCalls.load();
    };
    const uint64_t before = progress();
    for (int i = 0; i < 20 && progress() == before; ++i) {
        rig.push(30 + i);
        waitFor([&] { return progress() > before; }, std::chrono::milliseconds(100));
    }
    MIB_EXPECT(progress() > before, "the loop reads frames again after the resize");
    const auto a = rig.accounting();
    printAccounting(async ? "out of range, async batch" : "out of range, inline", a);
    MIB_EXPECT(a.storeMalformed == 0, "no frame booked as malformed across the renumbering");
    MIB_EXPECT(a.admitted <= 2 + (progress() - before) + (async ? 1u : 0u),
               "none of F2..F10 admitted: only F0, F1 and the frames read after the resize");
    rig.svc.endExperiment();
}

// ---- 4. bounded background calibration is not used up by unreadable frames -----------------
void calibrationIgnoresStoreLoss()
{
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, Mode::Inline, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    auto& kernel = *rig.kernel;
    warmUpInline(rig);

    // 3 empty frames needed within 5 attempts. The pass below reads F0 and F1, loses F2..F8 to the
    // camera inside the pass, then reads F9... Counting the 7 lost frames as contaminated attempts
    // used up the budget (F0, F1, F2, F3, F4: "only 2/3 empty frames in 5 attempts").
    ProcessingService::BackgroundCalibrationRequest request;
    request.requiredAccepted = 3;
    request.maxAttempts = 5;
    request.timeoutMs = 30000;
    std::string err;

    const uint64_t base = kernel.emptyCalls.load();
    kernel.holdCall.store(base + 1);
    rig.pushBlank(); // F0
    MIB_REQUIRE(waitFor([&] { return kernel.heldCall.load() == base + 1; }, std::chrono::seconds(5)),
                "loop held on F0");
    // Start the calibration only now. The loop is inside F0's examination, so the warm-up frame's
    // outcome has been booked: warmUpInline returns once the kernel is entered, and a slow build
    // (the sanitizer lanes) books that outcome after a calibration started before this point, which
    // then counts the warm-up ring frame as a contaminated attempt.
    MIB_REQUIRE(rig.svc.startBackgroundCalibration(request, &err), "calibration starts: " + err);
    for (int i = 1; i <= 16; ++i) rig.pushBlank();
    kernel.holdCall.store(base + 2);
    kernel.release(base + 1);
    MIB_REQUIRE(waitFor([&] { return kernel.heldCall.load() == base + 2; }, std::chrono::seconds(5)),
                "loop held on F1, inside its pass");
    for (int i = 17; i <= 24; ++i) rig.pushBlank();
    kernel.holdCall.store(0);
    kernel.release(base + 2);

    MIB_EXPECT(waitFor([&] { return rig.svc.backgroundCalibrationStatus().finished(); }, std::chrono::seconds(10)),
               "calibration finished");
    const auto st = rig.svc.backgroundCalibrationStatus();
    std::fprintf(stderr, "calibration: state=%d attempted=%u accepted=%u contaminated=%u (%s)\n",
                 static_cast<int>(st.state), st.attempted, st.accepted, st.rejectedNonEmpty, st.message.c_str());
    MIB_EXPECT(st.state == ProcessingService::BackgroundCalibrationState::Succeeded,
               "calibration succeeds: " + st.message);
    MIB_EXPECT(st.rejectedNonEmpty == 0, "no unreadable frame counted as contaminated");
    MIB_EXPECT(st.attempted == 3 && st.accepted == 3, "only the three empty frames the loop read were attempts");
    rig.svc.cancelBackgroundCalibration();
}

// ---- 6. a realtime restart in a run books the run's real gap only (#899) --------------------
// The camera runs before the run: 40 frames, each read before the next is pushed, so the 16-slot
// ring has wrapped well past write index 0 (the bench ring was millions of frames in).
void wrapRingBeforeRun(Rig& rig, bool async)
{
    auto progress = [&]() -> uint64_t {
        return async ? rig.svc.getBatchPipelineStats().framesAccepted : rig.kernel->emptyCalls.load();
    };
    for (int i = 0; i < 40; ++i) {
        const uint64_t before = progress();
        rig.push(100 + i);
        MIB_REQUIRE(waitFor([&] { return progress() > before; }, std::chrono::seconds(5)),
                    "pre-run frame read before the next one is pushed");
    }
    MIB_REQUIRE(rig.store->earliestAvailableIndex() > 16, "the ring wrapped before the run");
}

// Every admitted frame of the run has its outcome (the async workers book them after the loop).
bool runSettled(const Rig& rig, uint64_t admitted)
{
    const auto a = rig.accounting();
    return a.admitted >= admitted && a.frameTermsSum() == a.admitted;
}

// Realtime processing stops mid-run (the user left the Experiment tab) while the camera writes
// `whileStopped` frames, then starts again (the user came back). The 16-slot ring keeps the last 16
// of them; the others were evicted before any loop could read them and are the run's real loss.
void restartMidRun(Mode mode, int whileStopped)
{
    const bool async = mode == Mode::AsyncBatch;
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, mode, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    if (async) warmUpBatch(rig); else warmUpInline(rig);
    wrapRingBeforeRun(rig, async);

    rig.svc.setExperimentAccountingContext(/*generation=*/11, /*policyAllowsDrops=*/false);
    rig.svc.startExperiment();
    const uint64_t first = rig.next();
    for (int i = 0; i < 5; ++i) rig.push(i + 1); // F0..F4, read by the first loop
    MIB_REQUIRE(waitFor([&] { return runSettled(rig, 5); }, std::chrono::seconds(10)),
                "F0..F4 read and settled before the stop");

    rig.svc.stopRealtime();
    MIB_REQUIRE(!rig.svc.isRealtimeRunning(), "realtime stopped mid-run");
    for (int i = 0; i < whileStopped; ++i) rig.push(10 + i); // F5.. while no loop runs
    rig.svc.startRealtime(rig.store);
    MIB_REQUIRE(rig.svc.isRealtimeRunning(), "realtime restarted in the run");

    const uint64_t total = 5 + static_cast<uint64_t>(whileStopped);
    const uint64_t lost = whileStopped > static_cast<int>(kCapacity)
                              ? static_cast<uint64_t>(whileStopped) - kCapacity
                              : 0;
    // Not a REQUIRE: the regressed loop overshoots at once, and the numbers below say by how much.
    waitFor([&] { return runSettled(rig, total); }, std::chrono::seconds(10));
    rig.svc.endExperiment();
    const auto a = rig.accounting();
    const std::string label = std::string(async ? "restart mid-run, async" : "restart mid-run, inline") +
                              ", " + std::to_string(whileStopped) + " frames while stopped";
    printAccounting(label.c_str(), a);
    MIB_EXPECT(a.admitted == total, label + ": every frame of the run admitted once, none from before it");
    MIB_EXPECT(a.storeOverwritten == lost,
               label + ": storeOverwritten is the frames evicted while stopped (" + std::to_string(lost) +
                   "), not every frame since write index 0");
    MIB_EXPECT(a.sequenceGaps == 0 && a.sequenceGapFrames == 0, label + ": no hole in the admitted sequence");
    MIB_EXPECT(a.firstFrameIndex == first && a.lastFrameIndex == first + total - 1,
               label + ": the run spans F0 to its last frame");
    MIB_EXPECT(a.reconciled, label + ": frame terms reconcile");
    MIB_EXPECT((a.completion == rec::RunCompletionState::IncompleteLoss) == (lost > 0),
               label + ": incompleteLoss only for a real loss");
}

// Realtime starts after the run started: the coordinator's order when no consumer runs yet
// (ExperimentCoordinator: startExperiment, then startRealtime). The new loop reads the ring from its
// oldest frame as before; the frames older than the ring were never the run's and are not booked.
void startAfterRun(Mode mode)
{
    const bool async = mode == Mode::AsyncBatch;
    constexpr size_t kCapacity = 16;
    constexpr int kW = 96, kH = 96;
    Rig rig(kCapacity, kW, kH, mode, Roi{0, 0, kW, kH}, /*dropFrames=*/false);
    if (async) warmUpBatch(rig); else warmUpInline(rig);
    wrapRingBeforeRun(rig, async);
    rig.svc.stopRealtime();

    rig.svc.setExperimentAccountingContext(/*generation=*/12, /*policyAllowsDrops=*/false);
    rig.svc.startExperiment();
    rig.svc.startRealtime(rig.store);
    // The new loop works through the ring first; the frames pushed after it evict only frames it read.
    MIB_REQUIRE(waitFor([&] { return runSettled(rig, kCapacity); }, std::chrono::seconds(10)),
                "the ring read by the new loop");
    for (int i = 0; i < 3; ++i) rig.push(i + 1);
    waitFor([&] { return runSettled(rig, kCapacity + 3); }, std::chrono::seconds(10));
    rig.svc.endExperiment();
    const auto a = rig.accounting();
    const char* label = async ? "realtime started after the run, async" : "realtime started after the run, inline";
    printAccounting(label, a);
    MIB_EXPECT(a.storeOverwritten == 0, std::string(label) + ": nothing booked for frames older than the ring");
    MIB_EXPECT(a.admitted <= kCapacity + 3, std::string(label) + ": at most the ring and the new frames admitted");
    MIB_EXPECT(a.sequenceGaps == 0, std::string(label) + ": no hole in the admitted sequence");
    MIB_EXPECT(a.reconciled, std::string(label) + ": frame terms reconcile");
    MIB_EXPECT(a.completion != rec::RunCompletionState::IncompleteLoss, std::string(label) + ": not incompleteLoss");
}

// ---- 5. the monitoring rings stay within their byte budget -----------------------------------
struct RetentionCase {
    const char* label;
    int frameW, frameH;
    Roi roi;            // {0, 0, 0, 0}: no ROI, the whole frame
    bool dropFrames;    // drop-to-latest (live view) instead of every frame
    int frames;
    int cropW, cropH;   // what a row must hold
};

void retention(const RetentionCase& c)
{
    Rig rig(32, c.frameW, c.frameH, Mode::Inline, c.roi, c.dropFrames);
    auto& svc = rig.svc;
    auto appended = [&] { return svc.getMonitoringValidAppended() + svc.getMonitoringInvalidAppended(); };

    // Hidden: nothing is retained and nothing is cloned for the tab.
    rig.push(0);
    for (int i = 1; i <= 6; ++i) {
        rig.push(i);
        waitFor([&] { return rig.kernel->emptyCalls.load() >= static_cast<uint64_t>(i); }, std::chrono::seconds(5));
    }
    MIB_EXPECT(appended() == 0, std::string(c.label) + ": Monitoring hidden keeps no rows");

    svc.setMonitoringActive(true);
    svc.clearMonitoringFrames();
    for (int i = 1; i <= c.frames; ++i) {
        const uint64_t want = appended() + 1;
        rig.push(100 + i);
        MIB_REQUIRE(waitFor([&] { return appended() >= want; }, std::chrono::seconds(10)),
                    std::string(c.label) + ": frame " + std::to_string(i) + " produced a row");
    }
    // Settle: a frame can append more than one row (one per object).
    for (uint64_t last = ~0ull; last != appended();) {
        last = appended();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    const uint64_t budget = ProcessingService::kMonitoringImageBytesPerRing;
    const auto valid = svc.getMonitoringValidFrames();
    const auto invalid = svc.getMonitoringInvalidFrames();
    MIB_EXPECT(valid.size() + invalid.size() == appended(), std::string(c.label) + ": every row kept its measurements");
    const uint64_t rowBytes = static_cast<uint64_t>(c.cropW) * c.cropH * 2; // gray crop + mask crop

    size_t imagelessTotal = 0;
    uint64_t imagedBytesTotal = 0;
    size_t imagedMax = 0;
    auto checkRing = [&](const std::vector<ProcessedFrame>& rows, const char* ring) {
        const std::string tag = std::string(c.label) + " " + ring + ": ";
        if (rows.empty()) return;
        size_t firstImaged = rows.size();
        uint64_t imagedBytes = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const uint64_t b = processedFrameBytes(rows[i]);
            if (b > 0 && firstImaged == rows.size()) firstImaged = i;
            if (b == 0) MIB_EXPECT(firstImaged == rows.size(), tag + "imageless rows are the oldest ones, in a block");
            imagedBytes += b;
            if (i > 0) MIB_EXPECT(rows[i].index >= rows[i - 1].index, tag + "rows stay in frame order");
        }
        const size_t imaged = rows.size() - firstImaged;
        MIB_EXPECT(processedFrameBytes(rows.back()) > 0, tag + "the newest row keeps its images");
        MIB_EXPECT(imagedBytes <= budget, tag + std::to_string(imagedBytes) + " image bytes kept, budget " +
                                              std::to_string(budget));
        MIB_EXPECT(imaged == std::min<size_t>(rows.size(), static_cast<size_t>(budget / rowBytes)),
                   tag + std::to_string(imaged) + " of " + std::to_string(rows.size()) + " rows imaged, expected " +
                       std::to_string(std::min<uint64_t>(rows.size(), budget / rowBytes)));
        MIB_EXPECT(imaged >= std::min<size_t>(rows.size(), 25), tag + "at least what the thumbnails show keeps images");
        for (size_t i = firstImaged; i < rows.size(); ++i) {
            for (const cv::Mat* m : {&rows[i].originalImage, &rows[i].processedImage}) {
                MIB_EXPECT(m->cols == c.cropW && m->rows == c.cropH, tag + "a row keeps the ROI-sized crop");
                const auto held = static_cast<size_t>(m->dataend - m->datastart);
                MIB_EXPECT(!m->isSubmatrix() && held == m->total() * m->elemSize(),
                           tag + "a row owns only its crop (" + std::to_string(held) + " bytes held for a " +
                               std::to_string(m->total() * m->elemSize()) + "-byte crop)");
            }
        }
        imagelessTotal += firstImaged;
        imagedBytesTotal += imagedBytes;
        imagedMax = std::max(imagedMax, imaged);
    };
    checkRing(valid, "valid");
    checkRing(invalid, "invalid");

    const auto mem = svc.memoryStats().monitoringRings;
    MIB_EXPECT(mem.currentBytes == imagedBytesTotal, std::string(c.label) + ": reported bytes are what the rows hold");
    MIB_EXPECT(mem.capacityBytes == 2 * budget && !mem.overBudget(), std::string(c.label) + ": rings within their budget");
    MIB_EXPECT(mem.evictedByBudget >= imagelessTotal, std::string(c.label) + ": released images are counted");
    size_t tagged = 0;
    svc.visitMonitoringNewest(~size_t{0}, true,
                              [&](const ProcessedFrame&, const ProcessingService::MonitoringRowTag& t, bool) {
                                  if (t.imagesReleased) ++tagged;
                              });
    MIB_EXPECT(tagged == imagelessTotal, std::string(c.label) + ": rows that gave up their images say so");
    // The budget is what is under test: enough frames went through to exceed it.
    MIB_EXPECT(imagelessTotal > 0, std::string(c.label) + ": the run exceeded the budget (" +
                                       std::to_string(valid.size() + invalid.size()) + " rows of " +
                                       std::to_string(rowBytes) + " bytes)");
    std::fprintf(stderr, "retention %-34s rows=%zu imageless=%zu imaged<=%zu image_bytes=%llu (budget %llu per ring)\n",
                 c.label, valid.size() + invalid.size(), imagelessTotal, imagedMax,
                 (unsigned long long)imagedBytesTotal, (unsigned long long)budget);

    // Giving the oldest rows' pictures up must not lose the Monitoring tab's copy of the budget either:
    // the tab trims its rolling copy with the same rule.
    auto copy = valid.empty() ? invalid : valid;
    if (copy.empty()) {
        svc.setMonitoringActive(false);
        return;
    }
    for (auto& row : copy) {
        if (row.originalImage.empty()) row.originalImage = cv::Mat(c.cropH, c.cropW, CV_8UC1, cv::Scalar(1));
        if (row.processedImage.empty()) row.processedImage = cv::Mat(c.cropH, c.cropW, CV_8UC1, cv::Scalar(1));
    }
    const size_t released = ProcessingService::releaseImagesBeyondBudget(copy, budget);
    uint64_t kept = 0;
    for (const auto& row : copy) kept += processedFrameBytes(row);
    MIB_EXPECT(kept <= budget && processedFrameBytes(copy.back()) > 0 &&
                   released == copy.size() - std::min<size_t>(copy.size(), budget / rowBytes),
               std::string(c.label) + ": the tab's copy is trimmed to the same budget, keeping the newest row");
    svc.setMonitoringActive(false);
}
} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(120);

    wd.mark("inline eviction");
    inlineEvictionInsidePass();
    wd.mark("async eviction");
    asyncEvictionInsidePass();
    wd.mark("eviction before the pass, inline");
    evictionBeforePass(Mode::Inline);
    wd.mark("eviction before the pass, async");
    evictionBeforePass(Mode::AsyncBatch);
    wd.mark("out of range, inline");
    outOfRangeMidPass(Mode::Inline);
    wd.mark("out of range, async");
    outOfRangeMidPass(Mode::AsyncBatch);
    wd.mark("calibration");
    calibrationIgnoresStoreLoss();
    wd.mark("restart mid-run, inline, gap inside the ring");
    restartMidRun(Mode::Inline, 10);
    wd.mark("restart mid-run, inline, gap past the ring");
    restartMidRun(Mode::Inline, 30);
    wd.mark("restart mid-run, async, gap inside the ring");
    restartMidRun(Mode::AsyncBatch, 10);
    wd.mark("restart mid-run, async, gap past the ring");
    restartMidRun(Mode::AsyncBatch, 30);
    wd.mark("realtime started after the run, inline");
    startAfterRun(Mode::Inline);
    wd.mark("realtime started after the run, async");
    startAfterRun(Mode::AsyncBatch);

    // The bench geometry: 1216x208. A row of the whole frame is 506 KB (gray + mask); the 1000-row
    // count cap alone is 0.5 GB per ring.
    constexpr int kW = 1216, kH = 208;
    wd.mark("retention: every frame, whole-frame ROI");
    retention({"every frame, whole-frame ROI", kW, kH, Roi{0, 0, kW, kH}, false, 200, kW, kH});
    wd.mark("retention: every frame, no ROI");
    retention({"every frame, no ROI", kW, kH, Roi{0, 0, 0, 0}, false, 200, kW, kH});
    wd.mark("retention: every frame, ROI over half");
    retention({"every frame, 1100x190 ROI", kW, kH, Roi{40, 8, 1100, 190}, false, 240, 1100, 190});
    wd.mark("retention: drop-to-latest, ROI");
    retention({"drop-to-latest, 1100x190 ROI", kW, kH, Roi{40, 8, 1100, 190}, true, 240, 1100, 190});
    wd.mark("retention: drop-to-latest, no ROI");
    retention({"drop-to-latest, no ROI", kW, kH, Roi{0, 0, 0, 0}, true, 200, kW, kH});
    return mib::test::exitCode();
}
