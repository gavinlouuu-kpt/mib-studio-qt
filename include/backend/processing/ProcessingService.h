#pragma once

#include "backend/recording/RecordingAccounting.h"
#include "backend/evidence/LiveEvidence.h"
#include "backend/evidence/RetainedImage.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>
#include <opencv2/core.hpp>
#include <deque>
#include <cmath>
#include "backend/diagnostics/MemoryBudget.h"
#include "backend/processing/EModulusLut.h"
#include "backend/processing/ExperimentFrameBuffer.h"
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/HdfWriteQueue.h"
#include "backend/recording/TriggerEventRecord.h"

namespace backend { namespace playback { class FrameStore; struct Frame; } }
namespace backend::processing { struct ProviderFrame; }

namespace backend::services {

struct ProcessingStats {
    std::atomic<uint64_t> jobsQueued{0};
    std::atomic<uint64_t> jobsProcessed{0};
};

struct TargetGroupEvent {
    bool isTargetGroup{false};
    int objectId{-1};
    int trackId{-1};
    // Source-frame identity for end-to-end latency correlation (see
    // PipelineTimingRecorder). frameIndex is the FrameStore write index;
    // hostTimestampUs is the host monotonic acquisition stamp (0 if unknown).
    uint64_t frameIndex{0};
    uint64_t hostTimestampUs{0};
};

// ProcessedFrame / BufferedFrameCounts live in ProcessingTypes.h (shared with
// ExperimentFrameBuffer, issue #370).

// One unit of work handed to the experiment flush write queue.
struct ExperimentBatch {
    std::vector<ProcessedFrame> valid;
    std::vector<ProcessedFrame> invalid;
    // Sort trigger records drained from TriggerService with this batch; the
    // writer thread appends them to /trigger_events alongside the frames so
    // every HDF5 write stays on the one writer thread.
    std::vector<backend::recording::TriggerEventRecord> triggerEvents;
};

class ProcessingService {
    backend::evidence::LiveEvidence liveEvidence_;
    backend::evidence::LiveEvidence runLiveEvidence_;
    uint64_t evidenceRunSerial_{0};
    std::atomic<uint64_t> evidenceCameraSession_{0};
    std::string evidenceOwner_;
    uint64_t evidenceStoreGeneration_{0};

    friend struct ProcessingServiceFlushTestAccess;
public:
    using Job = std::function<void()>;
    // Metadata only; no durable/image-retention guarantee is implied.
    backend::evidence::LiveEvidence& liveEvidence() { return liveEvidence_; }
    backend::evidence::LiveEvidence& runLiveEvidence() { return runLiveEvidence_; }
    void setEvidenceCameraSession(uint64_t session) {
        auto change = liveEvidence_.revisionChange("camera source changed; restart realtime processing");
        auto runChange = runLiveEvidence_.revisionChange("camera source changed during run");
        evidenceCameraSession_.store(session, std::memory_order_release);
    }
    std::string liveEvidenceJson();
    std::string runEvidenceJson();
    // Exact run evidence key + existing snapshot lease; owned image copy.
    backend::evidence::RetainedImage
    readRunEvidenceImage(const backend::evidence::Identity& expected, uint64_t lease,
                         const std::string& set, const std::string& ordinal, bool mask = false);
    bool drainAndStopRealtime();


    // The pixel-to-micron factor (µm per pixel) a service starts with, and the
    // one YOFO Review's "default settings" process at.
    static constexpr double kDefaultPixelToMicronFactor = 0.4886;

    struct Roi {
        int x{0}, y{0}, w{0}, h{0};
    };

    struct RealtimeSnapshot {
        cv::Mat originalImage; // frozen source pixels; opt-in preview retention
        std::shared_ptr<const backend::playback::Frame>
            sourceFrame; // owns ROI fast-path pixels without another copy
        uint64_t sourceTimestamp{0}, hostTimestampUs{0};
        uint64_t processingSession{0}, storeGeneration{0}, captureSession{0};
        std::string recipeSha256;
        cv::Rect2d primaryBounds;
        Roi roi;
        uint64_t index{0};
        std::vector<std::vector<cv::Point>> contours;
        cv::Mat mask;
        FilterResult validation;
    };

    enum class RealtimeProcessingMode {
        Inline = 0,
        AsyncBatch = 1,
    };

    struct RealtimeBatchSettings {
        size_t batchSize{16};
        size_t maxQueuedFrames{4096};
        size_t workerCount{1};
        int maxBatchDelayMs{10};
        // Byte budget for queued input frames (issue #370); 0 = count-only.
        uint64_t maxQueuedBytes{kDefaultBatchQueueMaxBytes};
    };
    static constexpr uint64_t kDefaultBatchQueueMaxBytes = 256ULL * 1024 * 1024;
    static constexpr uint64_t kDefaultExperimentBufferMaxBytes = 512ULL * 1024 * 1024;

    ProcessingService();
    ~ProcessingService();

    class CoreOperationLease {
    public:
        CoreOperationLease() = default;
        ~CoreOperationLease();
        CoreOperationLease(const CoreOperationLease&) = delete;
        CoreOperationLease& operator=(const CoreOperationLease&) = delete;
        CoreOperationLease(CoreOperationLease&& other) noexcept;
        CoreOperationLease& operator=(CoreOperationLease&& other) noexcept;

        explicit operator bool() const noexcept { return owner_ != nullptr; }
        const backend::processing::ProcessingCoreIdentity& identity() const noexcept {
            return identity_;
        }

    private:
        friend class ProcessingService;
        CoreOperationLease(
            ProcessingService* owner,
            backend::processing::ProcessingCoreIdentity identity);
        void release() noexcept;

        ProcessingService* owner_{nullptr};
        backend::processing::ProcessingCoreIdentity identity_;
    };

    void start(size_t workerCount = std::thread::hardware_concurrency());
    void stop();

    void submit(Job job);

    const ProcessingStats& stats() const { return stats_; }

    // Processing-core selection. Activation is transactional and is rejected
    // while realtime, an experiment, or the async batch pipeline is active.
    // Callers prepare and validate a plugin with ProcessingCoreLoader first.
    // The optional preCommit callback runs under the core-selection lock after
    // every fallible activation guard and before the live kernel is swapped.
    // It must not call back into ProcessingService. Returning false preserves
    // the previous usable kernel and forwards its diagnostic through error.
    using ProcessingCoreActivationPreCommit = std::function<bool(std::string&)>;
    bool activateProcessingKernel(std::shared_ptr<backend::processing::IProcessingKernel> kernel,
                                  std::string* error = nullptr,
                                  ProcessingCoreActivationPreCommit preCommit = {});
    bool activateBundledProcessingKernel(std::string* error = nullptr);
    backend::processing::ProcessingCoreIdentity activeProcessingCoreIdentity() const;
    // ADR 0007: empty when the active core implements the configured
    // processing_contract_version; otherwise the reason processing is refused
    // (both contract numbers), suitable for the UI.
    std::string processingContractMismatch() const;
    std::string requiredProcessingCoreVersion() const { return requiredProcessingCoreVersion_; }
    bool isProcessingCorePinSatisfied() const;
    // Startup selection restoration failures fail closed until a verified
    // candidate is activated successfully.
    void markProcessingCoreSelectionUnavailable();
    // Pins the current core identity and prevents activation for the lifetime
    // of the returned lease. Long-running non-ProcessingService owners (for
    // example raw recording) must hold one for their whole operation.
    CoreOperationLease acquireProcessingCoreOperation();

    // Realtime processing API
    void startRealtime(std::shared_ptr<backend::playback::FrameStore> store);
    void stopRealtime();
    bool isRealtimeRunning() const { return rtRunning_.load(std::memory_order_relaxed); }
    bool isRealtimeEnabled() const { return rtEnabled_.load(std::memory_order_relaxed); }
    void setRealtimeEnabled(bool on);
    // When enabled, realtime processing will skip intermediate frames and process only the most recent frame.
    // Note: experiments still process every frame (this mode is ignored while experimentActive_ is true).
    void setRealtimeDropFrames(bool on);
    bool getRealtimeDropFrames() const { return rtDropFrames_.load(std::memory_order_relaxed); }
    void setRealtimeProcessingMode(RealtimeProcessingMode mode);
    RealtimeProcessingMode getRealtimeProcessingMode() const;
    void setRealtimeBatchSettings(const RealtimeBatchSettings& settings);
    RealtimeBatchSettings getRealtimeBatchSettings() const;
    // An explicit ROI; it also cancels a pending one.
    void setRealtimeRoi(const Roi& roi);
    Roi getRealtimeRoi() const;
    // An ROI that could not be validated when it was applied (no frame yet and
    // no known camera geometry; #398 M2c). The realtime loop applies it to the
    // first frame it sees if it fits, otherwise drops it, keeps the previous
    // ROI and says why in pendingRoiNotice(). Start is blocked while pending
    // (readiness gate processing.roi).
    void setPendingRealtimeRoi(const Roi& roi);
    bool realtimeRoiPending() const { return roiPending_.load(std::memory_order_acquire); }
    std::string pendingRoiNotice() const;
    // Applies or drops the pending ROI against a frame of this size (the
    // realtime loop's hook; public for tests). True when the ROI changed.
    bool resolvePendingRoi(uint64_t frameWidth, uint64_t frameHeight);
    void setRealtimeBackgroundGray(const cv::Mat& bg);
    // Configure before workers start; publication uses the coordinator's idle transaction.
    // Serializes ROI/background setters and calibration with experiment Start.
    void setBackgroundPublicationTransaction(std::function<bool(const std::function<void()>&)> transaction);

    cv::Mat getRealtimeBackgroundGray() const;
    // Zero-copy background accessor for hot paths; returns the shared_ptr directly.
    std::shared_ptr<const cv::Mat> getRealtimeBackgroundGrayShared() const;
    // Monotonic counter bumped by setProcessingConfig / setRealtimeRoi.
    uint64_t getConfigVersion() const;
    bool getLatestSnapshot(RealtimeSnapshot& out);
    void setProcessedPreviewEnabled(bool enabled);

    // Experiment lifecycle
    void startExperiment();
    bool endExperiment(); // false if the realtime series handoff times out

    // Frame accumulation access
    std::vector<ProcessedFrame> getValidFrames() const;
    std::vector<ProcessedFrame> getInvalidFrames() const;
    BufferedFrameCounts getBufferedFrameCounts() const;
    void clearAccumulatedFrames();
    
    // Monitoring frames (accumulated only while active; gate with setMonitoringActive)
    std::vector<ProcessedFrame> getMonitoringValidFrames() const;
    std::vector<ProcessedFrame> getMonitoringInvalidFrames() const;
    // Metrics-only copy of the valid ring (oldest -> newest): no image or
    // contour references are taken, so the lock the processing thread also
    // needs is held for a plain 1000-element copy (MonitoringDensityService).
    struct MonitoringPoint {
        uint64_t index{0};
        double area{0.0}; // pixels
        double deformability{0.0};
    };
    std::vector<MonitoringPoint> getMonitoringValidPoints() const;
    // Live cell images (#716 A6a, bridge ABI 41). Every monitoring ring slot
    // carries the start generation of the experiment run that was active when
    // its frame entered processing (0 = outside a run), read with the frame's
    // capture (below) and carried to the append. ExperimentCoordinator sets it
    // before the run's accumulation starts and resets it to 0 when the run
    // stops accumulating (or its start is rolled back). Lock-free.
    void setMonitoringRunGeneration(uint64_t startGeneration);
    uint64_t monitoringRunGeneration() const;
    // The capture a monitoring row came from (#716 A6a review rounds 1-3).
    //
    // One authority: the live cell store (MonitoringCellStore::advanceCapture)
    // owns the current capture epoch. Under its own mutex it moves the epoch
    // on, clears its strip, ends its pins (metadata only) and publishes the
    // new value here (publishMonitoringCapture). Every capture change goes
    // through that one call: a realtime start or stop and an ROI change (from
    // this service, through the authority below), a FrameStore replacement or
    // camera mode switch, a camera session opening or closing and a frame
    // buffer resize (AppBackend, the facade), and a run start (the
    // coordinator). The epoch starts at 1, only grows and is never reused; 0
    // is never a capture.
    //
    // Tag at entry: a frame's capture is decided once, when the frame enters
    // processing, from what is read for that frame there. The published epoch
    // is read together with the ROI (under rtMutex_, where an ROI change also
    // moves the epoch), the FrameStore's generation is bound to the epoch when
    // it moves, and the frame must be of that generation and of a camera
    // session at or above the published floor. A frame that fails either
    // check (another frame numbering, a closed camera session) and an async
    // batch frame measured with another ROI than the one read at its entry
    // are tagged 0: still monitoring rows (the charts count them), never
    // cells. The tag travels with the frame and its results; nothing assigns
    // "the current epoch" when a frame completes. The store compares the
    // carried epoch with its current one under its mutex on every insert and
    // every read, so a frame that entered before a change never becomes a
    // cell after it.
    uint64_t monitoringCaptureEpoch() const; // the published epoch, lock-free
    // The authority's publication: the new epoch and, for a camera session
    // change, the lowest camera session whose frames still belong to a
    // capture (the floor never goes down). Atomic stores only: the store
    // calls this under its mutex. The floor is stored before the epoch, and
    // a loop reads the epoch before the floor.
    void publishMonitoringCapture(uint64_t captureEpoch, std::optional<uint64_t> cameraSessionFloor = std::nullopt);
    // Who moves the epoch on: set once the live cell store exists (AppBackend)
    // and cleared before it goes away. Called with a static reason in plain
    // words, under the authority's own mutex (set...({}) returns once no call
    // runs), and possibly under rtMutex_ (an ROI change moves the epoch with
    // the ROI): the authority takes no lock of this service. Returns the new
    // epoch. Lock order: rtMutex_ -> the authority's mutex -> the store's.
    using MonitoringCaptureAuthority = std::function<uint64_t(const char* reason)>;
    void setMonitoringCaptureAuthority(MonitoringCaptureAuthority authority);
    // A capture change: through the authority, or (none set: a service on its
    // own, as in tests) by moving the published epoch on here.
    uint64_t requestMonitoringCaptureChange(const char* reason);
    // Test seam (review round 3): called on the processing thread with the
    // frame index right before a processed frame's rows are appended to the
    // monitoring ring (inline and async batch), outside every lock, so a test
    // can hold a frame between its entry and its completion. One relaxed load
    // per append while unset.
    void setMonitoringAppendHookForTests(std::function<void(uint64_t frameIndex)> hook);
    // What every ring slot carries besides the frame (written at append, under
    // the ring's lock): the run tag, the capture epoch, and where the row's
    // image sits in the coordinate space of its geometry. Inline rows are
    // measured on the ROI image itself (origin 0, 0); async-batch rows are
    // measured on the full frame and keep an ROI crop of it (origin = the ROI's
    // top-left), so readers subtract the origin to get image coordinates.
    struct MonitoringRowTag {
        uint64_t runGeneration{0};
        uint64_t captureEpoch{0};
        int32_t geometryOriginX{0};
        int32_t geometryOriginY{0};
    };
    // Read the ring without copying ProcessedFrames: under the ring's lock,
    // visit the newest `maxRows` rows (valid rows first, then invalid ones
    // with the remaining budget when `includeInvalid`), oldest -> newest
    // within each ring. The visitor runs under the lock the processing thread
    // takes to append, so it must only copy scalars and cv::Mat headers
    // (refcount shares of the frozen images), never pixels. Returns the ring
    // counters read under the same lock.
    struct MonitoringRingView {
        size_t validHeld{0};
        size_t invalidHeld{0};
        uint64_t validAppended{0};
        uint64_t invalidAppended{0};
        uint64_t runGeneration{0}; // the run tag a frame entering now gets
        uint64_t captureEpoch{0};  // the published capture epoch
    };
    using MonitoringVisitor =
        std::function<void(const ProcessedFrame& frame, const MonitoringRowTag& tag, bool valid)>;
    MonitoringRingView visitMonitoringNewest(size_t maxRows, bool includeInvalid,
                                             const MonitoringVisitor& visit) const;
    // A visitor that took shares of ring images and read their pixels outside
    // the lock gives the shares back here: they are dropped under the ring's
    // lock, the lock the ring's evictions take, so a free by an eviction (or by
    // the last reader) is ordered after every such reader's last access. The
    // reference count itself lives in the uninstrumented OpenCV library, so
    // this lock is the ordering ThreadSanitizer can see. Bounded: refcount
    // decrements, plus a free when the reader holds the last reference (the
    // ring already evicted the row), as an eviction does. Leaves `shares` empty.
    void releaseMonitoringShares(std::vector<cv::Mat>& shares) const;
    // Test seam: append to the monitoring ring as the realtime path does,
    // without the activity gate. A frame that carries a capture
    // (frame.monitoringCaptureEpoch != 0) keeps its carried run and capture
    // tags, as a processed frame does; any other frame enters now (the
    // published epoch and the current run). `geometryOrigin` is the tag an
    // async-batch row gets (its ROI's top-left).
    void appendMonitoringFrameForTests(const ProcessedFrame& frame, cv::Point geometryOrigin = {});
    void clearMonitoringFrames();
    // Monitoring observability (BE-5): totals appended since start/clear so a
    // consumer can compute ring-buffer evictions (appended - currently held).
    bool isMonitoringActive() const { return monitoringActive_.load(std::memory_order_relaxed); }
    uint64_t getMonitoringValidAppended() const { return monitoringValidAppended_.load(std::memory_order_relaxed); }
    uint64_t getMonitoringInvalidAppended() const { return monitoringInvalidAppended_.load(std::memory_order_relaxed); }
    static constexpr size_t getMonitoringCapacity() { return MAX_MONITORING_FRAMES; }
    // Enable/disable monitoring accumulation. When false, appendRealtimeMonitoringFrame
    // returns immediately with no clones. Wire to tab show/hide in the UI.
    void setMonitoringActive(bool active);
    
    // Issue #407: true when buffered frames should be flushed to disk. Checks
    // both the frame-count interval AND a byte-budget watermark (50% of
    // maxBytes) so a flush fires even when the byte budget saturates before
    // the count threshold is reached.
    bool needsFlush() const;

    // Round-robin buffer flush (for crash resilience)
    // Returns number of frames flushed (submitted to the write queue)
    size_t flushBufferedFrames(class Hdf5Service& hdf5);

    // Drain and tear down the experiment write queue (call at experiment stop,
    // before writing experiment info). Returns false if a write error occurred.
    bool finishFlush();

    void setFlushRequestCallback(std::function<void()> cb);

    // Fatal flush-error sink: invoked (on the writer thread) when an experiment
    // flush write fails or the queue overflows. The experiment should stop.
    void setFlushErrorCallback(std::function<void(const std::string&)> cb);

    // Source of sort trigger records to persist with each flushed batch
    // (AppBackend wires TriggerService::drainEvents). Called on the flushing
    // thread; unset = no /trigger_events dataset.
    using TriggerEventSource = std::function<std::vector<backend::recording::TriggerEventRecord>()>;
    void setTriggerEventSource(TriggerEventSource source);

    // Configuration for round-robin buffer
    void setFlushInterval(size_t frames); // Flush every N frames (default: 1000)
    size_t getFlushInterval() const;
    size_t getMaxBufferedFrames() const;
    // Byte budget for the experiment buffer (issue #370): frames beyond it
    // are evicted under the same declared policy as the frame cap (sampled
    // invalid first) and accounted as persistenceCancelledByPolicy. 0 = none.
    void setMaxBufferedBytes(uint64_t bytes);
    uint64_t getMaxBufferedBytes() const;

    // ---- Memory ownership report (issue #370) -------------------------------
    // Current/peak bytes + counts per owner inside this service. Shared
    // (refcounted) images retained by two owners are counted in both — each
    // number is what that owner alone keeps alive (an upper bound).
    struct MemoryStats {
        backend::diagnostics::MemoryOwnerStats experimentBuffer; // frames awaiting flush
        backend::diagnostics::MemoryOwnerStats monitoringRings;  // presentation rings (valid+invalid)
        backend::diagnostics::MemoryOwnerStats batchQueue;       // async batch input queue
        backend::diagnostics::MemoryOwnerStats flushQueue;       // batches handed to the HDF5 writer
        backend::diagnostics::MemoryOwnerStats snapshot;         // latest realtime snapshot (mask)
        std::vector<backend::diagnostics::MemoryOwnerStats> all() const {
            return {experimentBuffer, monitoringRings, batchQueue, flushQueue, snapshot};
        }
    };
    MemoryStats memoryStats() const;
    
    // Invalid frame sampling (save every Nth invalid frame to reduce file size)
    void setInvalidFrameSamplingRate(size_t rate); // Save every Nth invalid frame (default: 100, 1 = save all)
    size_t getInvalidFrameSamplingRate() const;

    // Configuration
    void setProcessingConfig(const ProcessingConfig& config);
    ProcessingConfig getProcessingConfig() const;
    // The config as processing applies it: getProcessingConfig() plus the
    // channel band detected from the background (frame coordinates) when
    // auto_roi_from_background is on. For provenance only; never feed it back
    // into setProcessingConfig (the band is runtime state).
    ProcessingConfig getEffectiveProcessingConfig() const;
    
    // Pixel to micron conversion factor (1 pixel = X micron)
    void setPixelToMicronFactor(double factor);
    double getPixelToMicronFactor() const;
    
    // Realtime throughput metrics (1-second window). The realtime loops close a window at least
    // once a second while they run, frames or not, so the rates fall to 0 once frames stop, and
    // stopRealtime() zeroes them (#763 F17). A window that no loop has closed for
    // kRealtimeRateStaleUs (an async-batch loop without a frame store, say) reads 0 rather than
    // the last published value.
    static constexpr uint64_t kRealtimeRateStaleUs = 3'000'000;
    double getAlgoFps1s() const { return freshRealtimeRate(algoFps1s_); }
    double getValidFps1s() const { return freshRealtimeRate(validFps1s_); }
    double getInvalidFps1s() const { return freshRealtimeRate(invalidFps1s_); }
    // A rate published at `publishedUs` as read at `nowUs` (monotonic microseconds, as
    // backend::Tools::getTimestamp; `publishedUs` 0 = never published): 0 once it is older than
    // kRealtimeRateStaleUs. A publication that lands after `nowUs` was read counts as fresh.
    static double realtimeRateAt(double rate, uint64_t publishedUs, uint64_t nowUs)
    {
        if (publishedUs == 0) return 0.0;
        if (nowUs <= publishedUs) return rate;
        return nowUs - publishedUs > kRealtimeRateStaleUs ? 0.0 : rate;
    }
    void resetRealtimeMetrics() {
        algoFps1s_.store(0.0, std::memory_order_relaxed);
        validFps1s_.store(0.0, std::memory_order_relaxed);
        invalidFps1s_.store(0.0, std::memory_order_relaxed);
        algoAvgUs1s_.store(0.0, std::memory_order_relaxed);
        algoAvgUs1sUpdatedUs_.store(0, std::memory_order_relaxed);
        resetIdentificationCounters();
    }
    
    // ---- Identification funnel + loss counters (monotonic, always-on) ----
    // Accumulated on the realtime inline path once per processed frame, off the
    // trigger-critical section. Cheap relaxed atomics, readable from any thread.
    // Rates are derived by the consumer from deltas against a wall clock. This
    // is the quantitative basis for "loss of target identification": the funnel
    // (frames -> objects -> valid -> target-group -> served) plus the per-reason
    // rejection histogram, plus unserved extra targets a single frame produced.
    struct IdentificationCounters {
        uint64_t framesProcessed{0};   // frames reaching object validation
        uint64_t framesWithObjects{0}; // frames with >=1 detected object
        uint64_t validObjects{0};      // objects passing all range gates
        uint64_t invalidObjects{0};    // objects failing >=1 gate
        uint64_t targetGroupObjects{0};         // valid objects in the sort target group
        uint64_t unservedTargetGroupObjects{0}; // target-group objects beyond the frame's
                                                // first — no pulse is dispatched for them
        // Invalid-reason histogram, indexed by science::InvalidReasonCode:
        // {NoContour, Border, Area, Ring, Deform, AreaRatio, Laplacian, Channel}.
        uint64_t reasonCounts[8]{};
    };
    IdentificationCounters getIdentificationCounters() const;
    void resetIdentificationCounters();

    // Totals for current experiment
    uint64_t getTotalValidFlushed() const { return totalValidFlushed_.load(std::memory_order_relaxed); }
    uint64_t getTotalInvalidFlushed() const { return totalInvalidFlushed_.load(std::memory_order_relaxed); }
    uint64_t getDroppedValidFrames() const { return droppedValidFrames_.load(std::memory_order_relaxed); }
    uint64_t getDroppedInvalidFrames() const { return droppedInvalidFrames_.load(std::memory_order_relaxed); }
    // Average algorithm processing time per frame over last 1s window (microseconds)
    double getAlgoAvgUs1s() const { return algoAvgUs1s_.load(std::memory_order_relaxed); }
    // Monotonic timestamp (microseconds) when algoAvgUs1s_ was last published; 0 if never
    uint64_t getAlgoAvgUs1sUpdatedUs() const { return algoAvgUs1sUpdatedUs_.load(std::memory_order_relaxed); }

    // Helper function to check if a raw frame is empty (for filtering during save)
    // Returns true if frame is empty (pixel count below threshold)
    static bool isFrameEmpty(const backend::playback::Frame& frame,
                            const ProcessingConfig& config,
                            const Roi& roi,
                            const cv::Mat& background = cv::Mat());
    // Hot-path overload: extracts only the ROI (no full-frame copy) and reads
    // background via shared_ptr (no clone). Semantically identical to the above.
    static bool isFrameEmpty(const backend::playback::Frame& frame,
                            const ProcessingConfig& config,
                            const Roi& roi,
                            const std::shared_ptr<const cv::Mat>& background);

    // Selected-core variant used by runtime paths. The static overloads above
    // remain as compatibility helpers and use bundled behavior.
    // Compatibility wrapper over classifyFrameWithActiveKernel: returns true
    // for Empty AND for Malformed/ProcessingFailed (callers that need to tell
    // those apart must use the typed API — issue #367).
    bool isFrameEmptyWithActiveKernel(
        const backend::playback::Frame& frame,
        const ProcessingConfig& config,
        const Roi& roi,
        const std::shared_ptr<const cv::Mat>& background) const;

    // Typed empty-frame classification (issue #367). A processing failure
    // or a malformed frame is never reported as a valid empty frame.
    struct FrameClassification {
        enum class Kind { Empty, Candidate, Malformed, ProcessingFailed };
        Kind kind{Kind::ProcessingFailed};
        std::string detail; // populated for Malformed / ProcessingFailed
        bool isEmpty() const { return kind == Kind::Empty; }
        bool isCandidate() const { return kind == Kind::Candidate; }
    };
    FrameClassification classifyFrameWithActiveKernel(
        const backend::playback::Frame& frame,
        const ProcessingConfig& config,
        const Roi& roi,
        const std::shared_ptr<const cv::Mat>& background) const;

    // Frames whose processing core call failed on the realtime path (never
    // counted as empty). Monotonic for the service lifetime.
    uint64_t getProcessingFailureCount() const {
        return processingFailures_.load(std::memory_order_relaxed);
    }

    // Experiment frame accounting (issue #367). Reset by startExperiment();
    // set the context (capture session generation, declared drop policy)
    // before starting. The snapshot derives the persistence pending/failed
    // terms from the current buffer + flush-queue state, so it is exact
    // after finishFlush().
    void setExperimentAccountingContext(uint64_t captureGeneration, bool policyAllowsDrops);
    backend::recording::RecordingAccountingSnapshot experimentAccountingSnapshot() const;

    // ---- Background identity + bounded calibration (issue #369) ----------
    // Generation increments on every successful background publication
    // (manual set, auto-capture, or calibration); 0 = never set.
    uint64_t backgroundGeneration() const { return backgroundGeneration_.load(std::memory_order_acquire); }
    // SHA-256 of the active background bytes (empty when none).
    std::string backgroundSha256() const;

    struct BackgroundCalibrationRequest {
        uint32_t requiredAccepted{10};  // empty frames to average
        uint32_t maxAttempts{200};      // frames examined before giving up
        uint64_t timeoutMs{5000};       // wall-clock bound
    };
    enum class BackgroundCalibrationState {
        Idle, Running, Succeeded, FailedInsufficient, FailedTimeout, FailedProcessing, Cancelled
    };
    struct BackgroundCalibrationStatus {
        BackgroundCalibrationState state{BackgroundCalibrationState::Idle};
        uint64_t operationGeneration{0};   // increments per start
        uint64_t frozenConfigVersion{0};   // config version the recipe was frozen at
        uint32_t attempted{0};
        uint32_t accepted{0};
        uint32_t rejectedNonEmpty{0};      // contaminated frames
        uint32_t rejectedProcessingFailed{0};
        uint64_t publishedBackgroundGeneration{0}; // set on success
        std::string publishedSha256;
        std::string message;
        bool finished() const {
            return state != BackgroundCalibrationState::Idle && state != BackgroundCalibrationState::Running;
        }
    };
    // Finite, cancellable background acquisition on the realtime path: the
    // recipe (config version) is frozen; empty frames are accepted and
    // averaged, non-empty/failed frames rejected and counted; success
    // publishes the candidate atomically (previous background stays active
    // until then). Ends with an explicit result at requiredAccepted,
    // maxAttempts, timeout, or cancel. Returns false if realtime is not
    // running or another calibration is active.
    bool startBackgroundCalibration(const BackgroundCalibrationRequest& request, std::string* error = nullptr);
    // PL science (ADR 0008): no host frame is classified, so the background is
    // the per-pixel median of `requiredAccepted` distinct preview frames from
    // the store (cells passing through are rejected by the median); same
    // status, cancel and publication as startBackgroundCalibration. Detects
    // the channel band from it when auto_roi_from_background is on.
    bool startPreviewBackgroundCalibration(std::shared_ptr<backend::playback::FrameStore> store,
                                           const BackgroundCalibrationRequest& request, std::string* error);
    void cancelBackgroundCalibration();
    void stopBackgroundCalibration();
    BackgroundCalibrationStatus backgroundCalibrationStatus() const;

    // ---- Batch mask generation ----
    // Pure pipeline: Gaussian blur -> (optional) background subtract -> binary
    // threshold -> morphology -> contour validation. Produces a full-frame mask
    // (zero outside ROI) so outputs round-trip through Hdf5Service without
    // special handling. Does NOT touch realtime state, monitoring buffers,
    // experiment accumulation, or any callback.
    //
    // Inputs:
    //   grayInput          - CV_8UC1 image (or any type that can be converted)
    //   backgroundGray     - full-size CV_8UC1 background (empty = no subtract)
    //   config             - processing thresholds
    //   roi                - ROI to analyze; {0,0,0,0} means full frame
    //   index, timestampNs - copied into the ProcessedFrame
    //
    // Returns a ProcessedFrame with originalImage (full gray clone),
    // processedImage (full-size mask, CV_8UC1), and validation metrics filled.
    ProcessedFrame computeProcessedFrame(
        const cv::Mat& grayInput,
        const cv::Mat& backgroundGray,
        const ProcessingConfig& config,
        const Roi& roi,
        uint64_t index = 0,
        uint64_t timestampNs = 0);

    struct BatchProgress {
        size_t done{0};
        size_t total{0};
    };
    using BatchProgressCallback = std::function<void(const BatchProgress&)>;

    // Analysis inputs of one processBatch call that otherwise come from the
    // service's live state (#758). An unset member keeps the live value: the
    // factor setPixelToMicronFactor set, the channel band detected from the
    // published background (getChannelBand). Offline reanalysis sets both, so
    // a job validates at its own source's calibration and never reads or
    // changes what a live run or another job uses.
    struct BatchAnalysisInputs {
        // µm per pixel for area validity, the target group and the reported
        // analysisPixelToMicronFactor; must be finite and > 0 when set.
        std::optional<double> pixelToMicronFactor;
        // Frame coordinates; h == 0 is no band (the config's channel_band_*
        // then apply as given). Used only with auto_roi_from_background.
        std::optional<Roi> channelBand;
        // The E-LUT for Young's modulus and an E-gated target group (#764:
        // a source run's own table in reanalysis). Unset: the service's own.
        // Set: this table, where null or an unloaded table computes no E (as
        // a run that loaded none: E stays 0, E gates cannot pass). The caller
        // keeps the table alive for the call.
        std::optional<const EModulusLut*> eModulusLut;
    };

    // computeProcessedFrame with per-call analysis inputs, mapped as in
    // processBatch (#779: the wheel's compute_processed_frame(pixel_to_micron=)
    // validates at the caller's factor). A set factor must be finite and > 0
    // (std::invalid_argument otherwise); an unset member keeps the live value.
    ProcessedFrame computeProcessedFrame(const cv::Mat& grayInput, const cv::Mat& backgroundGray,
                                         const ProcessingConfig& config, const Roi& roi, uint64_t index,
                                         uint64_t timestampNs, const BatchAnalysisInputs& inputs);

    // Process each image in order and emit one ProcessedFrame per detected
    // object candidate. Multiple records can share the same source index and
    // timestamp. Does not modify realtime config, monitoring buffers,
    // experiment state, or fire any callback. Safe to call from any thread.
    std::vector<ProcessedFrame> processBatch(
        const std::vector<cv::Mat>& grayImages,
        const ProcessingConfig& config,
        const cv::Mat& background = cv::Mat{},
        const Roi& roi = Roi{0, 0, 0, 0},
        BatchProgressCallback progress = {},
        backend::processing::ProcessingCoreIdentity* processingCore = nullptr,
        const BatchAnalysisInputs& inputs = {});

    struct BatchPipelineConfig {
        std::string previewRecipeSha256;
        size_t batchSize{64};
        size_t maxQueuedFrames{4096};
        size_t workerCount{1};
        int maxBatchDelayMs{10};
        uint64_t maxQueuedBytes{kDefaultBatchQueueMaxBytes}; // 0 = count-only (issue #370)
        ProcessingConfig processing;
        cv::Mat background;
        Roi roi{0, 0, 0, 0};
    };

    struct BatchPipelineStats {
        uint64_t framesAccepted{0};
        uint64_t framesDropped{0};
        uint64_t framesDroppedByByteBudget{0}; // subset of framesDropped (issue #370)
        uint64_t framesProcessed{0};
        uint64_t batchesProcessed{0};
        size_t currentQueueDepth{0};
        size_t maxQueueDepth{0};
        uint64_t currentQueueBytes{0};
        uint64_t maxQueueBytes{0};
        size_t batchSize{0};
        size_t workerCount{0};
        size_t queueCapacity{0}; // configured maxQueuedFrames
        bool running{false};
    };

    using BatchResultCallback = std::function<void(std::vector<ProcessedFrame>)>;

    // Async batch pipeline for capture-loop integration. enqueueBatchFrame()
    // copies the frame into a bounded queue and returns immediately. Dedicated
    // workers form configured-size batches, call computeProcessedFrame(), and
    // emit completed batches through the callback.
    bool startBatchPipeline(BatchPipelineConfig config, BatchResultCallback callback);
    void stopBatchPipeline();
    bool enqueueBatchFrame(const cv::Mat& grayImage, uint64_t index, uint64_t timestampNs = 0,
                           uint64_t hostTimestampUs = 0, uint64_t storeGeneration = 0, uint64_t captureSession = 0);
    bool enqueueBatchFrame(const backend::playback::Frame& frame, uint64_t index);
    BatchPipelineStats getBatchPipelineStats() const;

    // Autofocus feeds, chosen by the active contract: Contract 1 publishes the
    // ring ratio of each valid object (finite and > 0); Contracts 2 and 3
    // publish each valid object's finite Laplacian variance instead.
    using RingRatioCallback = std::function<void(double ringRatio, int64_t timestampNs)>;
    void setRingRatioCallback(RingRatioCallback callback);
    using FocusSampleCallback = std::function<void(double laplacianVariance, int64_t timestampNs,
                                                   uint64_t frameIndex, int objectId, int trackId)>;
    void setFocusSampleCallback(FocusSampleCallback callback);

    // PL science (ADR 0008, YOFO S1): one frame of results from an execution
    // provider, called on the provider thread. Feeds what the inline loop
    // feeds after metrics: run accounting (admitted; Empty, Processed /
    // RejectedByScientificFilter, or StoreMalformed for an ingress-error
    // FRAME.INVALID/PARTIAL), the identification funnel and reason histogram
    // (from the PL's reasons), and monitoring rows (without images). It never
    // calls the target-group callback: the PL owns the trigger, so a PL
    // decision must not cause a second pulse from the PS.
    void ingestProviderFrame(const backend::processing::ProviderFrame& frame);

    // Target group trigger callback (one deterministic event per source frame)
    using TargetGroupCallback = std::function<void(const TargetGroupEvent& event)>;
    void setTargetGroupCallback(TargetGroupCallback callback);
    TargetGroupEvent selectTargetGroupTriggerOwner(const std::vector<FilterResult>& validations) const;

    // Young's modulus LUT loading
    bool loadEModulusLut(const std::string& path);
    // The loaded LUT (read-only; loaded at bootstrap), e.g. for the PZ7035
    // profile compiler's E-modulus table.
    const EModulusLut& eModulusLut() const { return eModulusLut_; }
    // Identity of the loaded LUT (#712, run snapshot v3). loadEModulusLut
    // records the file it read and the SHA-256 of its bytes (empty when the
    // path does not name a file the hash could read); the shell adds what the
    // LUT catalog says about it (setEModulusLutCatalogIdentity). Empty strings
    // are unknown. A failed load clears it (loaded = false).
    struct EModulusLutIdentity {
        bool loaded{false};
        std::string path;
        std::string sha256;
        std::string lutId;
        std::string revision;
        std::string source;          // catalog source type
        std::string catalogSha256;   // the catalog's declared hash
        std::string checksumStatus;  // the catalog's verdict
    };
    EModulusLutIdentity eModulusLutIdentity() const;
    void setEModulusLutCatalogIdentity(std::string lutId, std::string revision, std::string source,
                                       std::string catalogSha256, std::string checksumStatus);

    // Background capture callback for auto-capture (called when background is auto-captured)
    using BackgroundCaptureCallback = std::function<void(const cv::Mat& background, uint64_t frameIndex)>;
    void setBackgroundCaptureCallback(BackgroundCaptureCallback callback);

    // Channel-band callback: fired when auto_roi_from_background detects the
    // channel band (full width, wall rows excluded, frame coordinates) in a
    // captured background. The band gates objects by centroid; the ROI is not
    // changed. Lets the UI draw the detected band.
    using SuggestedRoiCallback = std::function<void(const Roi& roi, uint64_t frameIndex)>;
    void setSuggestedRoiCallback(SuggestedRoiCallback callback);

    // Derive a wall-avoiding ROI from a background image using the current
    // ProcessingConfig auto-ROI settings. Returns an empty/full-frame ROI when
    // detection is disabled or the background is unusable. Pure w.r.t. service
    // state (does not apply the result); exposed for reuse and testing.
    Roi computeAutoRoiFromBackground(const cv::Mat& backgroundGray) const;
    // The same detection with `config`'s auto-ROI settings instead of the
    // service's (#758: an offline run detects the band from the background it
    // uses). Empty when auto_roi_from_background is off or no background.
    static Roi detectChannelBand(const cv::Mat& backgroundGray, const ProcessingConfig& config);
    // What an offline run (reanalysis, regenerate masks, #758) passes to
    // processBatch instead of the live state: `pixelToMicronFactor` and the
    // band detectChannelBand finds in `background`. With
    // auto_roi_from_background on, `config`'s channel_band_* become that band
    // (0 when none), as getEffectiveProcessingConfig fills them, so the config
    // the output records is the one applied. Pure.
    static BatchAnalysisInputs offlineAnalysisInputs(ProcessingConfig& config, const cv::Mat& background,
                                                     double pixelToMicronFactor);

    // Channel band detected from the latest background, in frame coordinates.
    // Empty (h == 0) when auto_roi_from_background is off or no background.
    Roi getChannelBand() const;

private:
    struct DroppedFrameCounts {
        size_t valid{0};
        size_t invalid{0};
    };

    struct QueuedBatchFrame {
        uint64_t storeGeneration{0}, captureSession{0};
        // Monitoring entry (#716 A6a review 3): run and capture read at the
        // enqueue, with the ROI read with them; the worker keeps the capture
        // only when it measures the frame with that ROI.
        uint64_t monitoringRunGeneration{0}, monitoringCaptureEpoch{0};
        Roi monitoringRoi{};
        cv::Mat gray;
        uint64_t index{0};
        uint64_t timestampNs{0};
        uint64_t hostTimestampUs{0};
    };

    // Per-frame stamps handed to publishRealtimeValidationCallbacks so the
    // shared callback chokepoint can emit one PipelineTimingRecorder record
    // per processed frame. All fields are host monotonic microseconds;
    // algoStartUs/algoEndUs are 0 in async-batch mode (aggregate batch timing
    // only). present=false (the default) records nothing.
    struct RealtimeFrameTiming {
        bool present{false};
        uint64_t frameIndex{0};
        uint64_t grabUs{0};
        uint64_t algoStartUs{0};
        uint64_t algoEndUs{0};
    };

    void workerLoop();
    void batchWorkerLoop();
    void realtimeLoop();
    void realtimeInlineLoop();
    void realtimeBatchLoop();
    BatchPipelineConfig makeRealtimeBatchPipelineConfig() const;
    void refreshRealtimeBatchPipelineConfig();
    void publishRealtimeBatchFrame(ProcessedFrame&& frame);
    void publishRealtimeValidationCallbacks(const std::vector<FilterResult>& validations,
                                            uint64_t timestampNs,
                                            const RealtimeFrameTiming& timing);
    // Update the identification funnel + invalid-reason histogram from one
    // frame's validations. Called off the trigger-critical path (after the
    // trigger callback has already fired) with the loop's cached config.
    void accumulateIdentificationCounters(const std::vector<FilterResult>& validations,
                                          const ProcessingConfig& config,
                                          double pixelToMicronFactor);
    void appendProviderMonitoringRow(uint64_t index, uint64_t timestampNs, const FilterResult& validation);
    void accumulateProviderIdentification(const backend::processing::ProviderFrame& frame);
    // The run and capture a frame's monitoring rows carry, decided when the
    // frame entered processing (#716 A6a review 3). captureEpoch 0: never a cell.
    struct MonitoringEntry {
        uint64_t runGeneration{0};
        uint64_t captureEpoch{0};
    };
    // A realtime loop's view of the capture: the published epoch as last read
    // with the ROI under rtMutex_, the ROI read with it, and the FrameStore
    // generation bound to the epoch when it moved (read after the epoch).
    struct MonitoringEntryCache {
        uint64_t captureEpoch{0};
        uint64_t storeGeneration{0};
        Roi roi{};
    };
    // Bind a freshly read epoch (and the ROI read with it under rtMutex_):
    // when the epoch moved, rtStore_'s generation is read now, after it.
    void bindMonitoringEntry(MonitoringEntryCache& cache, uint64_t captureEpoch, const Roi& roi) const;
    // A fetched frame's entry: the cached epoch when the frame is of the bound
    // store generation and of a camera session at or above the floor, else 0;
    // the run generation read now.
    MonitoringEntry monitoringEntryFor(const MonitoringEntryCache& cache, uint64_t storeGeneration,
                                       uint64_t captureSession) const;
    void runMonitoringAppendHook(uint64_t frameIndex);
    void appendRealtimeMonitoringFrame(uint64_t index,
                                       uint64_t timestampNs,
                                       const FilterResult& validation,
                                       const cv::Mat& originalImage,
                                       const cv::Mat& processedImage,
                                       const MonitoringEntry& entry);
    // The realtime batch loop's enqueue: the frame carries its entry and the ROI read with it.
    bool enqueueRealtimeBatchFrame(const backend::playback::Frame& frame, uint64_t index,
                                   const MonitoringEntry& entry, const Roi& entryRoi);
    bool enqueueBatchFrameImpl(const cv::Mat& grayImage, uint64_t index, uint64_t timestampNs,
                               uint64_t hostTimestampUs, uint64_t storeGeneration, uint64_t captureSession,
                               const MonitoringEntry& entry, const Roi& entryRoi);
    bool appendExperimentFrame(ProcessedFrame&& frame, bool isValid);
    void logDroppedExperimentFrames(const DroppedFrameCounts& dropped, size_t bufferedTotal, size_t maxBufferedFrames);
    FilterResult filterProcessedImage(const cv::Mat& processedImage, const cv::Rect& roi,
                                      const ProcessingConfig& config, const cv::Mat& originalImage,
                                      const cv::Mat& background = cv::Mat(),
                                      const BatchAnalysisInputs& inputs = {});
    // maskOrigin is the frame position of the mask's (0,0), so the frame-space
    // channel band can be expressed in the mask's coordinates. `background`
    // matches originalImage; an ABI v2 core needs it to run its own science.
    // `inputs` replaces the live factor / channel band where set (#758).
    std::vector<FilterResult> filterProcessedObjects(const cv::Mat& processedImage, const cv::Rect& roi,
                                                     const ProcessingConfig& config, const cv::Mat& originalImage,
                                                     cv::Point maskOrigin = {},
                                                     const cv::Mat& background = cv::Mat(),
                                                     const BatchAnalysisInputs& inputs = {});
    // computeProcessedFrame with per-call analysis inputs (processBatch).
    ProcessedFrame computeProcessedFrameWith(const cv::Mat& grayInput, const cv::Mat& backgroundGray,
                                             const ProcessingConfig& config, const Roi& roi, uint64_t index,
                                             uint64_t timestampNs, const BatchAnalysisInputs& inputs);
    // Batch track matching routed through the selected kernel; -1 = new track.
    int matchTrackWithActiveKernel(const std::vector<BatchTrack>& tracks,
                                   const std::vector<bool>& matchedThisFrame,
                                   const FilterResult& detection,
                                   uint64_t frameIndex,
                                   int frameWidth) const;
    // Fails closed (with *error) when the active kernel does not implement
    // config.processing_contract_version. Caller holds processingKernelMutex_.
    bool activeKernelServesContractLocked(const ProcessingConfig& config,
                                          std::string* error) const;
    bool processMaskWithActiveKernel(const cv::Mat& gray,
                                     const cv::Mat& background,
                                     const ProcessingConfig& config,
                                     const Roi& roi,
                                     cv::Mat& mask,
                                     std::string* error = nullptr) const;
    bool isImageEmptyWithActiveKernel(const cv::Mat& gray,
                                      const cv::Mat& background,
                                      const ProcessingConfig& config,
                                      const Roi& roi,
                                      bool absoluteBackgroundDifference,
                                      bool& empty,
                                      std::string* error = nullptr) const;
    void releaseProcessingCoreOperation() noexcept;

    std::vector<std::thread> workers_;
    std::queue<Job> queue_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::atomic<bool> running_{false};

    ProcessingStats stats_{};

    // Shared for calls, exclusive for activation/start transitions. A shared
    // pointer additionally keeps a loaded module resident for any in-flight
    // call even during service teardown.
    mutable std::shared_mutex processingKernelMutex_;
    std::shared_ptr<backend::processing::IProcessingKernel> processingKernel_;
    std::string requiredProcessingCoreVersion_;
    std::atomic<bool> processingCoreSelectionAvailable_{true};
    std::atomic<uint32_t> activeSynchronousCoreOperations_{0};

    // Async batch processing state
    std::vector<std::thread> batchWorkers_;
    std::queue<QueuedBatchFrame> batchQueue_;
    mutable std::mutex batchMutex_;
    std::condition_variable batchCv_;
    BatchPipelineConfig batchConfig_{};
    BatchResultCallback batchResultCallback_;
    std::atomic<bool> batchRunning_{false};
    std::atomic<uint64_t> batchFramesAccepted_{0};
    std::atomic<uint64_t> batchFramesDropped_{0};
    std::atomic<uint64_t> batchFramesProcessed_{0};
    // Frames dequeued by a batch worker whose callback has not returned yet
    // (endExperiment() drains on it together with the queue).
    std::atomic<uint64_t> batchFramesInFlight_{0};
    // Experiment accounting settlement (issue #367): endExperiment() books
    // still-unprocessed admitted frames as PendingAtStop under the exclusive
    // lock; outcome/validation counting takes it shared, so no outcome can
    // land after the settlement. Reset by startExperiment().
    std::shared_mutex experimentSettleMutex_;
    // Guarded by experimentSettleMutex_; shared by evidence and legacy recording.
    uint64_t experimentFirstFrame_{0};
    std::atomic<bool> experimentSettled_{false};
    std::atomic<uint64_t> batchBatchesProcessed_{0};
    std::atomic<uint64_t> batchAlgoMicrosTotal_{0};
    std::atomic<size_t> batchMaxQueueDepth_{0};
    std::atomic<size_t> batchWorkerCount_{0};
    // Queued input bytes (issue #370): add on enqueue, subtract on dequeue.
    backend::diagnostics::ByteAccountant batchQueueBytes_;
    std::atomic<uint64_t> batchFramesDroppedByBytes_{0};

    // Realtime processing state
    std::thread realtimeThread_;
    std::atomic<bool> rtRunning_{false};
    std::atomic<bool> experimentDrainRequested_{false};
    std::atomic<bool> rtEnabled_{true};
    // Default ON so live view processes only the newest frame and the processed
    // overlay (mask/contours/target-group) cannot accumulate a backlog behind
    // the capture write head when capture outpaces processing. Experiments are
    // unaffected: the realtime loop ignores this flag while experimentActive_ is
    // true (gated by `rtDropFrames_ && !experimentActive_`), so every frame is
    // still processed/recorded during an experiment. Users can still turn it off
    // via ProcessingSettingsDialog. See e2e_live_view_latency_test.
    std::atomic<bool> rtDropFrames_{true};
    std::atomic<int> rtProcessingMode_{static_cast<int>(RealtimeProcessingMode::Inline)};
    mutable std::mutex rtBatchSettingsMutex_;
    RealtimeBatchSettings rtBatchSettings_{};
    std::atomic<bool> rtBatchPipelineActive_{false};
    std::shared_ptr<backend::playback::FrameStore> rtStore_;
    mutable std::mutex rtMutex_;
    Roi rtRoi_{};
    std::optional<Roi> pendingRoi_;  // under rtMutex_
    std::string pendingRoiNotice_;   // under rtMutex_
    std::atomic<bool> roiPending_{false};
    std::shared_ptr<cv::Mat> rtBgGray_; // shared_ptr to avoid cloning on access
    std::atomic<uint64_t> rtLastProcessed_{0};

    mutable std::mutex snapshotMutex_;
    std::shared_ptr<const RealtimeSnapshot> latestSnapshot_; // pointer-swap on publish (no mutex-held copy)

    // Frame accumulation for experiment (issue #370: extracted, bounded by
    // frames AND bytes, every eviction reported).
    ExperimentFrameBuffer experimentBuffer_;
    std::atomic<uint64_t> maxBufferedBytes_{kDefaultExperimentBufferMaxBytes};

    // Experiment flush write queue (decouples HDF5 writes from frame
    // accumulation). Created lazily on the first flush, drained by finishFlush.
    mutable std::mutex flushQueueMutex_;
    std::unique_ptr<backend::recording::HdfWriteQueue<ExperimentBatch>> flushQueue_;
    // Bytes of batches submitted to the writer and not yet written (issue #370).
    backend::diagnostics::ByteAccountant flushQueueBytes_;
    std::function<void(const std::string&)> flushErrorCb_;
    mutable std::mutex triggerEventSourceMutex_;
    TriggerEventSource triggerEventSource_;
    std::function<void()> flushRequestCb_;
    std::atomic<bool> experimentActive_{false};
    // The queue is destroyed after every flush; retain fatal write state for
    // the lifetime of the run so accounting survives that destruction.
    std::atomic<bool> experimentPersistenceFailed_{false};
    // Issue #367: per-experiment frame accounting + lifetime processing
    // failure counter. Written by the realtime thread, the flush writer, and
    // appendExperimentFrame; read by experimentAccountingSnapshot().
    backend::recording::RecordingAccounting experimentAccounting_;
    std::atomic<uint32_t> providerLossLogged_{0}; // gap and malformed-frame log lines in this run (capped)
    std::atomic<uint64_t> processingFailures_{0};
    uint64_t experimentAccountingGeneration_{0};
    bool experimentAccountingPolicyAllowsDrops_{false};
    void noteRealtimeOutcome(uint64_t idx, backend::recording::FrameOutcome outcome,
                             const backend::playback::Frame* frame = nullptr);
    // Background calibration state (issue #369); bgCalMutex_ guards the
    // accumulator, status_ fields are read under it too.
    mutable std::mutex bgCalMutex_;
    BackgroundCalibrationStatus bgCalStatus_;
    BackgroundCalibrationRequest bgCalRequest_;
    uint64_t bgCalOperationCounter_{0};
    cv::Mat bgCalAccumulator_; // CV_64FC1 running sum of accepted frames
    std::thread bgCalPreviewThread_; // PL science: preview-median calibration
    void runPreviewBackgroundCalibration(std::shared_ptr<backend::playback::FrameStore> store, uint64_t generation);
    // Install a calibrated background under the idle transaction; temporarily
    // releases the caller's bgCalMutex_ lock to preserve coordinator lock order.
    bool publishCalibratedBackgroundLocked(cv::Mat background, std::unique_lock<std::mutex>& lock);
    std::function<bool(const std::function<void()>&)> backgroundPublicationTransaction_;
    std::chrono::steady_clock::time_point bgCalDeadline_{};
    std::atomic<bool> bgCalActive_{false};
    std::atomic<bool> processedPreviewEnabled_{false};
    std::atomic<uint64_t> processingSession_{0};
    std::atomic<uint64_t> backgroundGeneration_{0};
    void bgCalObserve(backend::recording::FrameOutcome outcome, const backend::playback::Frame* frame);
    void bgCalFinishLocked(BackgroundCalibrationState state, const std::string& message);
    void noteRealtimeAdmitted(uint64_t idx, const backend::playback::Frame* frame = nullptr);
    void noteRealtimeLost(uint64_t first, uint64_t endExclusive);
    void noteRealtimeValidation(uint64_t idx, const std::vector<FilterResult>& validations);
    
    // Monitoring frames (always accumulated, separate from experiment)
    mutable std::mutex monitoringFramesMutex_;

    // Simple circular buffer for ProcessedFrame (fixed capacity, keeps the most recent frames)
    class FrameRingBuffer {
    public:
        explicit FrameRingBuffer(size_t capacity = 1000)
            : capacity_(capacity), data_(capacity), tags_(capacity) {}

        void clear() {
            for (auto& f : data_) f = ProcessedFrame{}; // release retained images
            std::fill(tags_.begin(), tags_.end(), MonitoringRowTag{});
            size_ = 0;
            head_ = 0;
            bytes_ = 0;
        }

        size_t size() const { return size_; }
        size_t capacity() const { return capacity_; }
        // Image bytes the ring keeps alive (issue #370); bounded by capacity.
        uint64_t bytes() const { return bytes_; }
        uint64_t peakBytes() const { return peakBytes_; }
        uint64_t replaced() const { return replaced_; }

        // `tag`: the slot's MonitoringRowTag (#716 A6a); a 24-byte store in
        // the preallocated parallel array, under the caller's lock.
        void push_back(ProcessedFrame&& frame, const MonitoringRowTag& tag) {
            const uint64_t incoming = processedFrameBytes(frame);
            if (size_ == capacity_) {
                bytes_ -= std::min(bytes_, processedFrameBytes(data_[head_]));
                ++replaced_;
            }
            data_[head_] = std::move(frame);
            tags_[head_] = tag;
            bytes_ += incoming;
            if (bytes_ > peakBytes_) peakBytes_ = bytes_;
            head_ = (head_ + 1) % capacity_;
            if (size_ < capacity_) {
                ++size_;
            }
        }

        // Visit oldest -> newest without copying frames.
        template <class F>
        void forEach(F&& visit) const {
            const size_t start = (head_ + capacity_ - size_) % capacity_;
            for (size_t i = 0; i < size_; ++i) {
                visit(data_[(start + i) % capacity_]);
            }
        }

        // Visit the newest `count` entries, oldest -> newest, with their tags.
        template <class F>
        void forEachNewest(size_t count, F&& visit) const {
            const size_t n = std::min(count, size_);
            const size_t start = (head_ + capacity_ - n) % capacity_;
            for (size_t i = 0; i < n; ++i) {
                const size_t at = (start + i) % capacity_;
                visit(data_[at], tags_[at]);
            }
        }

        // Copy out as oldest -> newest
        std::vector<ProcessedFrame> toVector() const {
            std::vector<ProcessedFrame> out;
            out.reserve(size_);
            const size_t start = (head_ + capacity_ - size_) % capacity_;
            for (size_t i = 0; i < size_; ++i) {
                out.push_back(data_[(start + i) % capacity_]);
            }
            return out;
        }

    private:
        size_t capacity_{0};
        std::vector<ProcessedFrame> data_;
        std::vector<MonitoringRowTag> tags_; // per slot, parallel to data_
        size_t size_{0};
        size_t head_{0}; // next write position
        uint64_t bytes_{0};
        uint64_t peakBytes_{0};
        uint64_t replaced_{0}; // entries overwritten by the bounded ring
    };

    FrameRingBuffer monitoringValidFrames_{1000};
    FrameRingBuffer monitoringInvalidFrames_{1000};
    static constexpr size_t MAX_MONITORING_FRAMES = 1000; // Keep last 1000 frames for monitoring
    std::atomic<bool> monitoringActive_{false}; // gating: no clones when no consumer is active
    std::atomic<uint64_t> monitoringRunGeneration_{0}; // read at a frame's entry (#716 A6a)
    std::atomic<uint64_t> monitoringCaptureEpoch_{1};  // published by the capture authority; never reused
    std::atomic<uint64_t> monitoringSessionFloor_{0};  // camera sessions below it are closed
    mutable std::mutex monitoringCaptureAuthorityMutex_; // held while the authority runs
    MonitoringCaptureAuthority monitoringCaptureAuthority_;
    std::atomic<bool> monitoringAppendHookArmed_{false};
    std::mutex monitoringAppendHookMutex_;
    std::function<void(uint64_t)> monitoringAppendHook_;
    // Totals appended to the monitoring ring buffers since start/clear (BE-5)
    std::atomic<uint64_t> monitoringValidAppended_{0};
    std::atomic<uint64_t> monitoringInvalidAppended_{0};
    mutable ProcessingConfig processingConfig_;
    mutable std::mutex configMutex_;
    std::atomic<uint64_t> configVersion_{0}; // bumped by setProcessingConfig / setRealtimeRoi
    
    // Round-robin buffer for periodic flushing
    std::atomic<size_t> flushInterval_{100}; // Flush every 100 frames by default
    std::atomic<size_t> framesSinceLastFlush_{0};
    std::atomic<size_t> maxBufferedFrames_{1000};
    
    // Invalid frame sampling
    std::atomic<size_t> invalidFrameSamplingRate_{100}; // Save every 100th invalid frame by default
    std::atomic<size_t> invalidFrameCounter_{0}; // Counter for sampling
    
    // Ring ratio callback for autofocus
    mutable std::mutex ringRatioCallbackMutex_;
    RingRatioCallback ringRatioCallback_;
    FocusSampleCallback focusSampleCallback_; // guarded by ringRatioCallbackMutex_
    // processing_contract_version of processingConfig_, read lock-free by the
    // realtime callback publisher to choose the autofocus feed.
    std::atomic<int> activeContract_{1};

    mutable std::mutex targetGroupCallbackMutex_;
    TargetGroupCallback targetGroupCallback_;

    // Background capture callback for auto-capture
    mutable std::mutex backgroundCaptureCallbackMutex_;
    BackgroundCaptureCallback backgroundCaptureCallback_;

    // Channel-band callback (fired when auto_roi_from_background detects a band)
    mutable std::mutex suggestedRoiCallbackMutex_;
    SuggestedRoiCallback suggestedRoiCallback_;

    // Channel band from the latest background (frame coordinates; h == 0: none)
    mutable std::mutex channelBandMutex_;
    Roi channelBand_{};
    
    // Auto-capture state tracking
    std::atomic<uint64_t> consecutiveEmptyFrames_{0};
    std::atomic<uint64_t> lastAutoBackgroundFrame_{0};
    cv::Mat previousFrameForAutoCapture_; // Store previous frame for frame-to-frame diff when no background
    std::mutex previousFrameMutex_; // Protect previous frame access
    
    // Realtime throughput metrics (published once per ~1s window)
    std::atomic<double> algoFps1s_{0.0};
    std::atomic<double> validFps1s_{0.0};
    std::atomic<double> invalidFps1s_{0.0};
    std::atomic<double> algoAvgUs1s_{0.0};
    std::atomic<uint64_t> algoAvgUs1sUpdatedUs_{0}; // monotonic us when the window (all four) was last published
    double freshRealtimeRate(const std::atomic<double>& rate) const; // realtimeRateAt(rate, published, now)
    
    // Experiment totals
    std::atomic<uint64_t> totalValidFlushed_{0};
    std::atomic<uint64_t> totalInvalidFlushed_{0};
    std::atomic<uint64_t> droppedValidFrames_{0};
    std::atomic<uint64_t> droppedInvalidFrames_{0};
    std::atomic<uint64_t> lastDropLogUs_{0};

    // Identification funnel + loss counters (see IdentificationCounters). The
    // reason array is indexed by science::InvalidReasonCode (size checked with
    // a static_assert in the .cpp).
    std::atomic<uint64_t> idFramesProcessed_{0};
    std::atomic<uint64_t> idFramesWithObjects_{0};
    std::atomic<uint64_t> idValidObjects_{0};
    std::atomic<uint64_t> idInvalidObjects_{0};
    std::atomic<uint64_t> idTargetGroupObjects_{0};
    std::atomic<uint64_t> idUnservedTargetGroupObjects_{0};
    std::atomic<uint64_t> idReasonCounts_[8]{};
    
    // Pixel to micron conversion factor (default: 0.4886)
    std::atomic<double> pixelToMicronFactor_{kDefaultPixelToMicronFactor};

    // Young's modulus LUT (read-only after loading, thread-safe)
    EModulusLut eModulusLut_;
    mutable std::mutex eModulusLutIdentityMutex_;
    EModulusLutIdentity eModulusLutIdentity_;
};

} // namespace backend::services
