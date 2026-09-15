#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::diagnostics {

// One record per frame that reached the realtime validation/callback stage.
// All *Us fields are host monotonic microseconds from the same clock as
// Tools::getTimestamp() (CLOCK_MONOTONIC_RAW / QueryPerformanceCounter), so
// stage-to-stage differences are directly meaningful. deviceTimestamp is the
// camera/grabber tick carried on the frame and is NOT on the host clock; use
// it only for device-side inter-frame spacing.
struct FrameTimingRecord {
    uint64_t frameIndex{0};        // FrameStore absolute write index
    uint64_t deviceTimestamp{0};   // camera/grabber tick (unit depends on source)
    uint64_t grabUs{0};            // stamped in CaptureService right after grabFrame
    uint64_t algoStartUs{0};       // algorithm start (0 in async-batch mode)
    uint64_t algoEndUs{0};         // algorithm end (0 in async-batch mode)
    uint64_t triggerDispatchUs{0}; // target-group callback returned; 0 if no trigger
    uint64_t callbacksDoneUs{0};   // all realtime callbacks returned
    uint32_t validCount{0};
    uint32_t invalidCount{0};
    uint8_t isTargetGroup{0}; // 1 if this frame requested a trigger pulse
};

// One record per trigger pulse actually driven on the output line.
struct TriggerTimingRecord {
    uint64_t frameIndex{0};  // source frame write index (from TargetGroupSignal)
    uint64_t grabUs{0};      // host grab stamp of the source frame
    uint64_t requestUs{0};   // onTargetGroupResult entered (realtime thread)
    uint64_t wakeUs{0};      // trigger thread woke for this request
    uint64_t fireUs{0};      // setTriggerOutput(true) returned
    uint64_t pulseDoneUs{0}; // setTriggerOutput(false) returned
    uint32_t coalesced{0};   // extra requests merged into this pulse beyond the first
};

// Frames the realtime consumer never carried to the callback stage. Counted so
// pushed frames == frame records + empty/error records-worth + skips, i.e. no
// silent loss in the recorded data.
enum class PipelineSkipReason : size_t {
    DroppedToLatest = 0, // drop-frames mode jumped over these to the newest frame
    RingBehind,          // consumer fell out of the FrameStore retention window
    EmptyFrame,          // classified empty; algo aborted before validation
    KernelError,         // processing core returned an error for the frame
    BatchQueueRejected,  // async-batch bounded queue refused the frame
    Count
};

// Lock-free per-frame pipeline latency recorder.
//
// Purpose-built so that enabling instrumentation cannot itself delay the
// pipeline or drop frames: records go into fixed, pre-allocated rings with no
// locks, no allocation, and no I/O on any hot path. Each ring has exactly one
// writer thread (frames: realtime processing thread; triggers: trigger
// thread); skip counters are relaxed atomics and may be bumped from any
// thread. When disabled (default) every hook is a single relaxed atomic load.
//
// dumpCsv() may run concurrently with writers; if a ring wraps mid-dump the
// oldest rows can be torn. The rings hold ~65k frames / ~16k triggers, so in
// practice dump at stop (AppBackend does this) and the snapshot is exact.
class PipelineTimingRecorder {
public:
    static PipelineTimingRecorder& instance();

    void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Host monotonic microseconds (same clock as Tools::getTimestamp()).
    static uint64_t nowUs();

    // Single-writer: realtime processing thread only.
    void recordFrame(const FrameTimingRecord& record);
    // Single-writer: trigger thread only.
    void recordTrigger(const TriggerTimingRecord& record);
    // Any thread.
    void countSkipped(PipelineSkipReason reason, uint64_t n = 1);

    // Reset records and counters (leaves the enabled flag alone). Call only
    // while the capture/realtime/trigger threads are stopped.
    void clear();

    uint64_t frameRecordCount() const { return frameCount_.load(std::memory_order_acquire); }
    uint64_t triggerRecordCount() const { return triggerCount_.load(std::memory_order_acquire); }
    uint64_t skippedCount(PipelineSkipReason reason) const {
        return skipped_[static_cast<size_t>(reason)].load(std::memory_order_relaxed);
    }

    // Snapshot copies, oldest retained -> newest.
    std::vector<FrameTimingRecord> frameRecords() const;
    std::vector<TriggerTimingRecord> triggerRecords() const;

    // Per-stage latency distribution over one set of records.
    struct StageStats {
        uint64_t count{0}; // samples contributing (pairs with both stamps set)
        double avgUs{0.0};
        uint64_t maxUs{0};
        uint64_t p50Us{0};
        uint64_t p95Us{0};
        uint64_t p99Us{0};
    };
    // Latency percentiles derived from the retained rings. This is the detailed
    // tier: it is populated only for frames/triggers that were recorded while
    // instrumentation was enabled. Allocates and sorts locally, so call it off
    // any hot path (e.g. a ~1 Hz UI tick or at capture stop). sampleLimit caps
    // how many of the newest records are scanned (0 = all retained).
    struct LatencySummary {
        StageStats endToEndTarget; // fireUs - grabUs      (acquisition -> pulse onset)
        StageStats requestToFire;  // fireUs - requestUs    (trigger queue wait + wake)
        StageStats endToEndFrame;  // callbacksDoneUs - grabUs
        StageStats frameAge;       // algoStartUs - grabUs  (consumer lag)
        StageStats algo;           // algoEndUs - algoStartUs
        StageStats dispatch;       // triggerDispatchUs - algoEndUs (trigger frames only)
    };
    LatencySummary summarize(size_t sampleLimit = 0) const;

    // ---- Always-on live end-to-end target latency (acquisition -> pulse) ----
    // Updated unconditionally on every driven pulse, independent of the
    // enabled flag, so the headline "target seen -> sorted" latency is visible
    // live without turning on the detailed recorder. nowUs()-based; cheap.
    void noteTargetLatency(uint64_t latencyUs);
    uint64_t lastTargetLatencyUs() const {
        return liveTargetLatencyLastUs_.load(std::memory_order_relaxed);
    }
    double avgTargetLatencyUs() const {
        return liveTargetLatencyEwmaUs_.load(std::memory_order_relaxed);
    }
    uint64_t maxTargetLatencyUs() const {
        return liveTargetLatencyMaxUs_.load(std::memory_order_relaxed);
    }
    // Clear the live gauges (leaves the detailed rings alone). Safe any time.
    void resetLiveLatency();

    // Write pipeline_frames.csv, pipeline_triggers.csv and pipeline_skips.csv
    // into `directory` (created if missing; files overwritten). Analyse with
    // scripts/analyze_pipeline_timing.py.
    bool dumpCsv(const std::string& directory, std::string* error = nullptr) const;

    static constexpr size_t kFrameCapacity = size_t{1} << 16;   // ~64k frames retained
    static constexpr size_t kTriggerCapacity = size_t{1} << 14; // ~16k pulses retained

private:
    PipelineTimingRecorder();

    std::atomic<bool> enabled_{false};
    std::vector<FrameTimingRecord> frames_;
    std::vector<TriggerTimingRecord> triggers_;
    // Monotonic totals; slot = (count % capacity). The writer fills the slot
    // first, then publishes with a release store so dump readers never see an
    // unwritten slot below the count they loaded.
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> triggerCount_{0};
    std::atomic<uint64_t> skipped_[static_cast<size_t>(PipelineSkipReason::Count)]{};

    // Always-on live end-to-end target latency gauge (see noteTargetLatency).
    std::atomic<uint64_t> liveTargetLatencyLastUs_{0};
    std::atomic<double> liveTargetLatencyEwmaUs_{0.0};
    std::atomic<uint64_t> liveTargetLatencyMaxUs_{0};
};

const char* pipelineSkipReasonName(PipelineSkipReason reason);

} // namespace backend::diagnostics
