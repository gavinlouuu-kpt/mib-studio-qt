// e2e_pipeline_timing_test
//
// End-to-end invariant coverage for the pipeline latency instrumentation
// (PipelineTimingRecorder) used to diagnose realtime-pipeline and
// trigger-service delay:
//
//   Phase 1 (inline, every-frame): drives the REAL realtime inline loop from a
//   FrameStore producer that stamps hostTimestampUs like CaptureService does,
//   then asserts per-frame stage stamps are monotonic
//   (grab <= algoStart <= algoEnd <= callbacksDone), record indices are
//   strictly increasing, and frame accounting is conserved:
//   pushed frames == timing records + counted skips (no silent loss).
//
//   Phase 2 (inline, drop-frames): capture overruns processing; asserts
//   dropped-to-latest skips are counted and accounting still conserves.
//
//   Phase 3 (trigger correlation): drives TriggerService with a stub camera
//   and asserts each pulse's record carries the source frame identity
//   (frameIndex + grab stamp echo) with monotonic
//   request <= wake <= fire <= pulseDone stamps, and that with the
//   per-request pending queue (issue #283) paced requests neither coalesce
//   nor drop — exactly one pulse per request.
//
//   Phase 4 (trigger queue overflow): a burst faster than the pulse rate
//   must drop the OLDEST requests with counted conservation
//   (pulses + dropped == requests) and the newest request always fires.
//
// Timing gates are ordering/accounting invariants only (no absolute-ms
// thresholds), per docs/architecture/testing-strategy.md.

#include "backend/camera/common/ICamera.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/TriggerService.h"

#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using backend::diagnostics::PipelineSkipReason;
using backend::diagnostics::PipelineTimingRecorder;
using backend::playback::FrameStore;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;
using backend::services::TargetGroupSignal;
using backend::services::TriggerService;
using Clock = std::chrono::steady_clock;

namespace {

int failures = 0;

#define CHECK_MSG(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond << " — " << msg    \
                      << "\n";                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK(cond) CHECK_MSG(cond, "")

constexpr int kFrameW = 128;
constexpr int kFrameH = 128;
constexpr uint64_t kPixelFormatMono8 = 0x01080001ULL;

ProcessingConfig makeLenientConfig() {
    ProcessingConfig c;
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
    c.empty_frame_pixel_threshold = 1;
    c.auto_background_enabled = false;
    c.multi_image_enabled = false;
    return c;
}

// Guaranteed non-empty so every processed frame reaches the callback stage.
cv::Mat makeBlobFrame(uint64_t i) {
    cv::Mat img(kFrameH, kFrameW, CV_8UC1, cv::Scalar(0));
    const int cx = 32 + static_cast<int>(i % 32);
    cv::circle(img, cv::Point(cx, 64), 24, cv::Scalar(220), -1);
    return img;
}

uint64_t totalSkips(const PipelineTimingRecorder& rec) {
    uint64_t total = 0;
    for (size_t i = 0; i < static_cast<size_t>(PipelineSkipReason::Count); ++i) {
        total += rec.skippedCount(static_cast<PipelineSkipReason>(i));
    }
    return total;
}

// Push `count` frames like CaptureService does (device tick + host stamp).
// Returns number pushed.
uint64_t produceFrames(FrameStore& store, uint64_t count, int paceEveryN) {
    for (uint64_t i = 0; i < count; ++i) {
        const cv::Mat f = makeBlobFrame(i);
        store.pushFrame(f.data, static_cast<size_t>(f.total()), kFrameW, kFrameH, kFrameW,
                        kPixelFormatMono8,
                        /*deviceTimestamp=*/(i + 1) * 1000ULL,
                        /*hostTimestampUs=*/PipelineTimingRecorder::nowUs());
        if (paceEveryN > 0 && (i % static_cast<uint64_t>(paceEveryN)) == 0) {
            std::this_thread::yield();
        }
    }
    return count;
}

bool waitUntil(mib::test::Watchdog& watchdog, const char* step, int timeoutMs,
               const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        watchdog.mark(step);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void runInlinePhase(mib::test::Watchdog& watchdog, bool dropFrames, uint64_t frameCount) {
    auto& rec = PipelineTimingRecorder::instance();
    rec.clear();
    rec.setEnabled(true);

    auto store = std::make_shared<FrameStore>(4096);
    ProcessingService proc;
    proc.setProcessingConfig(makeLenientConfig());
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeDropFrames(dropFrames);
    proc.startRealtime(store);
    proc.setRealtimeEnabled(true);

    const uint64_t pushed = produceFrames(*store, frameCount, dropFrames ? 0 : 4);

    // Wait until the consumer has accounted for every pushed frame. Frame
    // index 0 is the realtime loop's "nothing processed yet" sentinel and is
    // never consumed, hence the -1 tolerance.
    const bool settled = waitUntil(watchdog, "inline-settle", 15000, [&] {
        return rec.frameRecordCount() + totalSkips(rec) >= pushed - 1;
    });
    proc.setRealtimeEnabled(false);
    proc.stopRealtime();

    const uint64_t records = rec.frameRecordCount();
    const uint64_t skips = totalSkips(rec);
    std::cout << (dropFrames ? "[drop-frames]" : "[every-frame]") << " pushed=" << pushed
              << " records=" << records << " skips=" << skips
              << " (dropped_to_latest=" << rec.skippedCount(PipelineSkipReason::DroppedToLatest)
              << ", ring_behind=" << rec.skippedCount(PipelineSkipReason::RingBehind)
              << ", empty=" << rec.skippedCount(PipelineSkipReason::EmptyFrame) << ")\n";

    CHECK_MSG(settled, "realtime loop did not account for all frames in time");
    // Frame accounting conserved: every pushed frame is either recorded or
    // explicitly counted as skipped (index-0 sentinel tolerance).
    CHECK_MSG(records + skips >= pushed - 1 && records + skips <= pushed,
              "accounting records=" << records << " skips=" << skips << " pushed=" << pushed);

    const auto frames = rec.frameRecords();
    CHECK(frames.size() == records);
    uint64_t previousIndex = 0;
    bool havePrevious = false;
    for (const auto& r : frames) {
        CHECK_MSG(r.grabUs > 0, "frame " << r.frameIndex << " missing grab stamp");
        CHECK_MSG(r.algoStartUs >= r.grabUs, "frame " << r.frameIndex << " algoStart < grab");
        CHECK_MSG(r.algoEndUs >= r.algoStartUs, "frame " << r.frameIndex << " algoEnd < start");
        CHECK_MSG(r.callbacksDoneUs >= r.algoEndUs,
                  "frame " << r.frameIndex << " callbacksDone < algoEnd");
        CHECK_MSG(r.validCount + r.invalidCount >= 1,
                  "frame " << r.frameIndex << " has no validations");
        if (havePrevious) {
            CHECK_MSG(r.frameIndex > previousIndex, "record indices not strictly increasing");
        }
        previousIndex = r.frameIndex;
        havePrevious = true;
    }

    if (dropFrames) {
        CHECK_MSG(rec.skippedCount(PipelineSkipReason::DroppedToLatest) > 0,
                  "overrun with drop-frames ON should count dropped_to_latest skips");
    } else {
        CHECK_MSG(rec.skippedCount(PipelineSkipReason::DroppedToLatest) == 0,
                  "every-frame mode must not drop to latest");
    }
}

// Stub camera that acknowledges trigger output (records nothing itself; the
// recorder's trigger ring is the artifact under test).
class AckCamera : public camera::common::ICamera {
public:
    void applyConfig(const camera::common::CameraConfig&) override {}
    bool start() override { return true; }
    void stop() override {}
    bool isRunning() const override { return true; }
    bool grabFrame(camera::common::Frame&) override { return false; }
    bool pollStats(camera::common::CameraStats&) const override { return false; }
    bool setTriggerOutput(bool high) override {
        if (high) fireCount_.fetch_add(1, std::memory_order_release);
        return true;
    }
    uint64_t fireCount() const { return fireCount_.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> fireCount_{0};
};

void runTriggerPhase(mib::test::Watchdog& watchdog) {
    auto& rec = PipelineTimingRecorder::instance();
    rec.clear();
    rec.setEnabled(true);

    AckCamera camera;
    TriggerService trigger;
    trigger.setPulseDurationUs(5);
    trigger.setCamera(&camera);
    trigger.start();

    constexpr uint64_t kRequests = 50;
    std::map<uint64_t, uint64_t> sentGrabUs;
    for (uint64_t i = 0; i < kRequests; ++i) {
        watchdog.mark("trigger-requests");
        TargetGroupSignal signal;
        signal.isTargetGroup = true;
        signal.objectId = static_cast<int>(i);
        signal.trackId = static_cast<int>(i);
        signal.frameIndex = 1000 + i;
        signal.hostTimestampUs = PipelineTimingRecorder::nowUs();
        sentGrabUs[signal.frameIndex] = signal.hostTimestampUs;
        trigger.onTargetGroupResult(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Settle on the exact completion condition: every request either fired a
    // pulse or was counted as an overflow drop, and each pulse's record has
    // been written. No stability heuristic — nothing arrives after the loop.
    waitUntil(watchdog, "trigger-settle", 5000, [&] {
        return camera.fireCount() + trigger.getDroppedRequestCount() == kRequests &&
               rec.triggerRecordCount() == camera.fireCount();
    });
    trigger.stop();

    const auto records = rec.triggerRecords();
    uint64_t coalescedTotal = 0;
    for (const auto& r : records) {
        coalescedTotal += r.coalesced;
        CHECK_MSG(r.requestUs > 0, "trigger record missing request stamp");
        CHECK_MSG(r.wakeUs >= r.requestUs, "wake < request");
        CHECK_MSG(r.fireUs >= r.wakeUs, "fire < wake");
        CHECK_MSG(r.pulseDoneUs >= r.fireUs, "pulseDone < fire");
        const auto sent = sentGrabUs.find(r.frameIndex);
        CHECK_MSG(sent != sentGrabUs.end(),
                  "trigger record has unknown frameIndex " << r.frameIndex);
        if (sent != sentGrabUs.end()) {
            CHECK_MSG(r.grabUs == sent->second, "grab stamp not echoed for frame " << r.frameIndex);
        }
    }
    std::cout << "[trigger] requests=" << kRequests << " pulses=" << camera.fireCount()
              << " records=" << records.size() << " coalesced=" << coalescedTotal
              << " dropped=" << trigger.getDroppedRequestCount() << "\n";
    // Per-request queue (issue #283): paced requests each get their own pulse
    // — no coalescing, no drops, exact one-record-per-request accounting.
    CHECK_MSG(coalescedTotal == 0, "paced requests must not coalesce with the request queue");
    CHECK_MSG(trigger.getDroppedRequestCount() == 0, "paced requests must not overflow the queue");
    CHECK_MSG(records.size() == kRequests,
              "trigger accounting records=" << records.size() << " requests=" << kRequests);
    CHECK(records.size() == camera.fireCount());
}

// Deliberate overflow of the bounded pending-request queue (issue #283): a
// burst far faster than the pulse rate must drop the OLDEST requests, count
// them, and keep accounting conserved (pulses + drops == requests).
void runTriggerOverflowPhase(mib::test::Watchdog& watchdog) {
    auto& rec = PipelineTimingRecorder::instance();
    rec.clear();
    rec.setEnabled(true);

    AckCamera camera;
    TriggerService trigger;
    // Long pulses so the burst below outruns the drain deterministically:
    // draining even one entry takes 5 ms while the whole burst is enqueued in
    // well under a millisecond.
    trigger.setPulseDurationUs(5000);
    trigger.setCamera(&camera);
    trigger.start();

    constexpr uint64_t kBurst = TriggerService::kMaxPendingRequests + 12;
    for (uint64_t i = 0; i < kBurst; ++i) {
        TargetGroupSignal signal;
        signal.isTargetGroup = true;
        signal.objectId = static_cast<int>(i);
        signal.trackId = static_cast<int>(i);
        signal.frameIndex = 5000 + i;
        signal.hostTimestampUs = PipelineTimingRecorder::nowUs();
        trigger.onTargetGroupResult(signal);
    }

    // Settle on the exact completion condition (see runTriggerPhase): every
    // burst request either fired or was counted dropped, records flushed.
    waitUntil(watchdog, "overflow-settle", 5000, [&] {
        return camera.fireCount() + trigger.getDroppedRequestCount() == kBurst &&
               rec.triggerRecordCount() == camera.fireCount();
    });
    trigger.stop();

    const uint64_t pulses = camera.fireCount();
    const uint64_t dropped = trigger.getDroppedRequestCount();
    std::cout << "[trigger-overflow] burst=" << kBurst << " pulses=" << pulses
              << " dropped=" << dropped << "\n";
    CHECK_MSG(dropped > 0, "burst must overflow the bounded queue");
    CHECK_MSG(pulses + dropped == kBurst,
              "overflow accounting pulses=" << pulses << " dropped=" << dropped
                                            << " burst=" << kBurst);
    // The newest request always survives drop-oldest, so the LAST pulse must
    // carry the final frame of the burst. (Early pulses legitimately carry
    // the oldest frames — they fired before the queue overflowed — so the
    // exact surviving set in between is timing-dependent.)
    const auto records = rec.triggerRecords();
    CHECK(!records.empty());
    if (!records.empty()) {
        CHECK_MSG(records.back().frameIndex == 5000 + kBurst - 1,
                  "last pulse should carry the newest request, got frame "
                      << records.back().frameIndex);
    }
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(30);

    std::cout << "=== e2e pipeline timing instrumentation ===\n";
    watchdog.mark("phase1-every-frame");
    runInlinePhase(watchdog, /*dropFrames=*/false, /*frameCount=*/300);
    watchdog.mark("phase2-drop-frames");
    runInlinePhase(watchdog, /*dropFrames=*/true, /*frameCount=*/2000);
    watchdog.mark("phase3-trigger");
    runTriggerPhase(watchdog);
    watchdog.mark("phase4-trigger-overflow");
    runTriggerOverflowPhase(watchdog);

    PipelineTimingRecorder::instance().setEnabled(false);
    PipelineTimingRecorder::instance().clear();

    if (failures == 0) {
        std::cout << "e2e_pipeline_timing_test: OK\n";
        return 0;
    }
    std::cerr << "e2e_pipeline_timing_test: " << failures << " failure(s)\n";
    return 1;
}
