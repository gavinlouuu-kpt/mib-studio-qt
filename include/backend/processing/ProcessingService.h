#pragma once

#include "backend/recording/RecordingAccounting.h"

#include <algorithm>
#include <atomic>
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

namespace backend { namespace playback { class FrameStore; struct Frame; } }

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
};

class ProcessingService {
public:
    using Job = std::function<void()>;

    struct Roi {
        int x{0}, y{0}, w{0}, h{0};
    };

    struct RealtimeSnapshot {
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
    void setRealtimeEnabled(bool on);
    // When enabled, realtime processing will skip intermediate frames and process only the most recent frame.
    // Note: experiments still process every frame (this mode is ignored while experimentActive_ is true).
    void setRealtimeDropFrames(bool on);
    bool getRealtimeDropFrames() const { return rtDropFrames_.load(std::memory_order_relaxed); }
    void setRealtimeProcessingMode(RealtimeProcessingMode mode);
    RealtimeProcessingMode getRealtimeProcessingMode() const;
    void setRealtimeBatchSettings(const RealtimeBatchSettings& settings);
    RealtimeBatchSettings getRealtimeBatchSettings() const;
    void setRealtimeRoi(const Roi& roi);
    Roi getRealtimeRoi() const;
    void setRealtimeBackgroundGray(const cv::Mat& bg);
    cv::Mat getRealtimeBackgroundGray() const;
    // Zero-copy background accessor for hot paths; returns the shared_ptr directly.
    std::shared_ptr<const cv::Mat> getRealtimeBackgroundGrayShared() const;
    // Monotonic counter bumped by setProcessingConfig / setRealtimeRoi.
    uint64_t getConfigVersion() const;
    bool getLatestSnapshot(RealtimeSnapshot& out);

    // Experiment lifecycle
    void startExperiment();
    void endExperiment();
    
    // Frame accumulation access
    std::vector<ProcessedFrame> getValidFrames() const;
    std::vector<ProcessedFrame> getInvalidFrames() const;
    BufferedFrameCounts getBufferedFrameCounts() const;
    void clearAccumulatedFrames();
    
    // Monitoring frames (accumulated only while active; gate with setMonitoringActive)
    std::vector<ProcessedFrame> getMonitoringValidFrames() const;
    std::vector<ProcessedFrame> getMonitoringInvalidFrames() const;
    void clearMonitoringFrames();
    // Enable/disable monitoring accumulation. When false, appendRealtimeMonitoringFrame
    // returns immediately with no clones. Wire to tab show/hide in the UI.
    void setMonitoringActive(bool active);
    
    // Round-robin buffer flush (for crash resilience)
    // Returns number of frames flushed (submitted to the write queue)
    size_t flushBufferedFrames(class Hdf5Service& hdf5);

    // Drain and tear down the experiment write queue (call at experiment stop,
    // before writing experiment info). Returns false if a write error occurred.
    bool finishFlush();

    // Fatal flush-error sink: invoked (on the writer thread) when an experiment
    // flush write fails or the queue overflows. The experiment should stop.
    void setFlushErrorCallback(std::function<void(const std::string&)> cb);

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
    
    // Pixel to micron conversion factor (1 pixel = X micron)
    void setPixelToMicronFactor(double factor);
    double getPixelToMicronFactor() const;
    
    // Realtime throughput metrics (1-second window)
    double getAlgoFps1s() const { return algoFps1s_.load(std::memory_order_relaxed); }
    double getValidFps1s() const { return validFps1s_.load(std::memory_order_relaxed); }
    double getInvalidFps1s() const { return invalidFps1s_.load(std::memory_order_relaxed); }
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
        // {NoContour, Border, Area, Ring, Deform, AreaRatio}.
        uint64_t reasonCounts[6]{};
    };
    IdentificationCounters getIdentificationCounters() const;
    void resetIdentificationCounters();

    // Totals for current experiment
    uint64_t getTotalValidFlushed() const { return totalValidFlushed_.load(std::memory_order_relaxed); }
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
    void cancelBackgroundCalibration();
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
        backend::processing::ProcessingCoreIdentity* processingCore = nullptr);

    struct BatchPipelineConfig {
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
                           uint64_t hostTimestampUs = 0);
    bool enqueueBatchFrame(const backend::playback::Frame& frame, uint64_t index);
    BatchPipelineStats getBatchPipelineStats() const;

    // Ring ratio callback for autofocus (called when validated frames are processed)
    using RingRatioCallback = std::function<void(double ringRatio, int64_t timestampNs)>;
    void setRingRatioCallback(RingRatioCallback callback);

    // Target group trigger callback (one deterministic event per source frame)
    using TargetGroupCallback = std::function<void(const TargetGroupEvent& event)>;
    void setTargetGroupCallback(TargetGroupCallback callback);
    TargetGroupEvent selectTargetGroupTriggerOwner(const std::vector<FilterResult>& validations) const;

    // Young's modulus LUT loading
    bool loadEModulusLut(const std::string& path);

    // Background capture callback for auto-capture (called when background is auto-captured)
    using BackgroundCaptureCallback = std::function<void(const cv::Mat& background, uint64_t frameIndex)>;
    void setBackgroundCaptureCallback(BackgroundCaptureCallback callback);

private:
    struct DroppedFrameCounts {
        size_t valid{0};
        size_t invalid{0};
    };

    struct QueuedBatchFrame {
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
    void appendRealtimeMonitoringFrame(uint64_t index,
                                       uint64_t timestampNs,
                                       const FilterResult& validation,
                                       const cv::Mat& originalImage,
                                       const cv::Mat& processedImage);
    bool appendExperimentFrame(ProcessedFrame&& frame, bool isValid);
    void logDroppedExperimentFrames(const DroppedFrameCounts& dropped, size_t bufferedTotal, size_t maxBufferedFrames);
    FilterResult filterProcessedImage(const cv::Mat& processedImage, const cv::Rect& roi, 
                                      const ProcessingConfig& config, const cv::Mat& originalImage);
    std::vector<FilterResult> filterProcessedObjects(const cv::Mat& processedImage, const cv::Rect& roi,
                                                     const ProcessingConfig& config, const cv::Mat& originalImage);
    // Batch track matching routed through the selected kernel; -1 = new track.
    int matchTrackWithActiveKernel(const std::vector<BatchTrack>& tracks,
                                   const std::vector<bool>& matchedThisFrame,
                                   const FilterResult& detection,
                                   uint64_t frameIndex,
                                   int frameWidth) const;
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
    std::atomic<bool> experimentActive_{false};
    // Issue #367: per-experiment frame accounting + lifetime processing
    // failure counter. Written by the realtime thread, the flush writer, and
    // appendExperimentFrame; read by experimentAccountingSnapshot().
    backend::recording::RecordingAccounting experimentAccounting_;
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
    std::chrono::steady_clock::time_point bgCalDeadline_{};
    std::atomic<bool> bgCalActive_{false};
    std::atomic<uint64_t> backgroundGeneration_{0};
    void bgCalObserve(backend::recording::FrameOutcome outcome, const backend::playback::Frame* frame);
    void bgCalFinishLocked(BackgroundCalibrationState state, const std::string& message);
    void noteRealtimeAdmitted(uint64_t idx);
    void noteRealtimeLost(uint64_t count);
    void noteRealtimeValidation(uint64_t idx, const std::vector<FilterResult>& validations);
    
    // Monitoring frames (always accumulated, separate from experiment)
    mutable std::mutex monitoringFramesMutex_;

    // Simple circular buffer for ProcessedFrame (fixed capacity, keeps the most recent frames)
    class FrameRingBuffer {
    public:
        explicit FrameRingBuffer(size_t capacity = 1000)
            : capacity_(capacity), data_(capacity) {}

        void clear() {
            for (auto& f : data_) f = ProcessedFrame{}; // release retained images
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

        void push_back(ProcessedFrame&& frame) {
            const uint64_t incoming = processedFrameBytes(frame);
            if (size_ == capacity_) {
                bytes_ -= std::min(bytes_, processedFrameBytes(data_[head_]));
                ++replaced_;
            }
            data_[head_] = std::move(frame);
            bytes_ += incoming;
            if (bytes_ > peakBytes_) peakBytes_ = bytes_;
            head_ = (head_ + 1) % capacity_;
            if (size_ < capacity_) {
                ++size_;
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

    mutable std::mutex targetGroupCallbackMutex_;
    TargetGroupCallback targetGroupCallback_;

    // Background capture callback for auto-capture
    mutable std::mutex backgroundCaptureCallbackMutex_;
    BackgroundCaptureCallback backgroundCaptureCallback_;
    
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
    std::atomic<uint64_t> algoAvgUs1sUpdatedUs_{0}; // monotonic us when algoAvgUs1s_ was last published
    
    // Experiment totals
    std::atomic<uint64_t> totalValidFlushed_{0};
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
    std::atomic<uint64_t> idReasonCounts_[6]{};
    
    // Pixel to micron conversion factor (default: 0.4886)
    std::atomic<double> pixelToMicronFactor_{0.4886};

    // Young's modulus LUT (read-only after loading, thread-safe)
    EModulusLut eModulusLut_;
};

} // namespace backend::services
