#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/playback/FrameStore.h"
#include "backend/app/Tools.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <opencv2/core.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5 moved contour geometry out of imgproc.hpp
#endif
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace backend::services {

namespace {

size_t defaultMaxBufferedFrames(size_t flushInterval) {
    constexpr size_t kMinBufferedFrames = 1000;
    constexpr size_t kSoftMaxBufferedFrames = 5000;

    const size_t scaled = flushInterval > (std::numeric_limits<size_t>::max() / 4)
                              ? std::numeric_limits<size_t>::max()
                              : flushInterval * 4;
    const size_t preferred = std::max(kMinBufferedFrames, scaled);
    return std::max(flushInterval, std::min(preferred, kSoftMaxBufferedFrames));
}

void applyTrackState(ProcessedFrame& frame, const BatchTrack& track) {
    frame.validation.trackId = track.id;
    frame.validation.trackFirstFrame = track.firstFrame;
    frame.validation.trackLastFrame = track.lastFrame;
    frame.validation.trackObservationCount = track.observations;
}

} // namespace

ProcessingService::ProcessingService()
    : processingKernel_(backend::processing::makeBundledProcessingKernel()) {
    if (const char* pin = std::getenv("MIB_STUDIO_PROCESSING_CORE_VERSION")) {
        requiredProcessingCoreVersion_ = pin;
    }
}

ProcessingService::CoreOperationLease::CoreOperationLease(
    ProcessingService* owner, backend::processing::ProcessingCoreIdentity identity)
    : owner_(owner), identity_(std::move(identity)) {}

ProcessingService::CoreOperationLease::~CoreOperationLease() {
    release();
}

ProcessingService::CoreOperationLease::CoreOperationLease(CoreOperationLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), identity_(std::move(other.identity_)) {}

ProcessingService::CoreOperationLease&
ProcessingService::CoreOperationLease::operator=(CoreOperationLease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        identity_ = std::move(other.identity_);
    }
    return *this;
}

void ProcessingService::CoreOperationLease::release() noexcept {
    if (!owner_) return;
    owner_->releaseProcessingCoreOperation();
    owner_ = nullptr;
}

ProcessingService::~ProcessingService() {
    // A joinable realtimeThread_ at destruction would std::terminate; do not
    // rely on the GUI teardown path having called stopRealtime() first.
    stopRealtime();
    stopBatchPipeline();
    stop();
}

void ProcessingService::start(size_t workerCount) {
    if (running_.load()) return;
    running_.store(true);
    if (workerCount == 0) workerCount = 1;
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back(&ProcessingService::workerLoop, this);
    }
    {
        auto& m = backend::diagnostics::CrashStateMirror::instance().processing;
        m.running.store(true);
        m.workerCount.store(static_cast<int>(workerCount));
    }
    SPDLOG_INFO("ProcessingService started with {} workers", workerCount);
}

void ProcessingService::stop() {
    if (!running_.load()) return;
    running_.store(false);
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    // drain queue
    {
        std::scoped_lock lk(mutex_);
        std::queue<Job> empty;
        queue_.swap(empty);
    }
    {
        auto& m = backend::diagnostics::CrashStateMirror::instance().processing;
        m.running.store(false);
        m.workerCount.store(0);
    }
    SPDLOG_INFO("ProcessingService stopped");
}

void ProcessingService::submit(Job job) {
    if (!running_.load()) return;
    {
        std::scoped_lock lk(mutex_);
        queue_.push(std::move(job));
        stats_.jobsQueued.fetch_add(1, std::memory_order_relaxed);
        backend::diagnostics::CrashStateMirror::instance().processing.jobsQueued.store(
            stats_.jobsQueued.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    cv_.notify_one();
}

void ProcessingService::workerLoop() {
    while (running_.load()) {
        Job job;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) break;
            if (queue_.empty()) continue;
            job = std::move(queue_.front());
            queue_.pop();
        }
        if (job) {
            try {
                job();
            } catch (const std::exception& ex) {
                SPDLOG_ERROR("ProcessingService: worker job threw: {} — job skipped", ex.what());
            } catch (...) {
                SPDLOG_ERROR("ProcessingService: worker job threw unknown exception — job skipped");
            }
            stats_.jobsProcessed.fetch_add(1, std::memory_order_relaxed);
            backend::diagnostics::CrashStateMirror::instance().processing.jobsProcessed.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void ProcessingService::startRealtime(std::shared_ptr<backend::playback::FrameStore> store) {
    std::unique_lock coreLock(processingKernelMutex_);
    if (rtRunning_.load()) return;
    if (realtimeThread_.joinable()) {
        coreLock.unlock();
        realtimeThread_.join();
        coreLock.lock();
        if (rtRunning_.load()) return;
    }
    rtStore_ = std::move(store);
    rtRunning_.store(true);
    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
    lastAutoBackgroundFrame_.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock prevFrameLk(previousFrameMutex_);
        previousFrameForAutoCapture_.release();
    }
    realtimeThread_ = std::thread(&ProcessingService::realtimeLoop, this);
    backend::diagnostics::CrashStateMirror::instance().processing.realtimeRunning.store(true);
    SPDLOG_INFO("ProcessingService: realtime processing started");
}

bool ProcessingService::activateProcessingKernel(
    std::shared_ptr<backend::processing::IProcessingKernel> kernel, std::string* error,
    ProcessingCoreActivationPreCommit preCommit) {
    if (!kernel) {
        if (error) *error = "processing kernel is null";
        return false;
    }
    if (!requiredProcessingCoreVersion_.empty() &&
        kernel->identity().version != requiredProcessingCoreVersion_) {
        if (error) {
            *error = "administrator pin requires processing core " + requiredProcessingCoreVersion_;
        }
        return false;
    }
    std::unique_lock coreLock(processingKernelMutex_);
    if (rtRunning_.load(std::memory_order_acquire)) {
        if (error) *error = "realtime processing is running";
        return false;
    }
    if (experimentActive_.load(std::memory_order_acquire)) {
        if (error) *error = "an experiment is active";
        return false;
    }
    if (batchRunning_.load(std::memory_order_acquire)) {
        if (error) *error = "a batch pipeline is running";
        return false;
    }
    if (activeSynchronousCoreOperations_.load(std::memory_order_acquire) != 0) {
        if (error) *error = "an offline processing operation is active";
        return false;
    }

    // Reset is part of the quiescent activation transaction. In particular,
    // the candidate may be the currently active resident module, so resetting
    // it before taking this lock could mutate a context held by an operation
    // whose lease will subsequently cause activation to be rejected.
    std::string resetError;
    if (!kernel->reset(&resetError)) {
        if (error) *error = "processing kernel reset failed: " + resetError;
        return false;
    }

    if (preCommit) {
        std::string commitError;
        try {
            if (!preCommit(commitError)) {
                if (error) {
                    *error = commitError.empty() ? "processing core activation commit was refused"
                                                 : commitError;
                }
                return false;
            }
        } catch (const std::exception& ex) {
            if (error) {
                *error = "processing core activation commit failed: " + std::string(ex.what());
            }
            return false;
        } catch (...) {
            if (error) *error = "processing core activation commit failed";
            return false;
        }
    }

    const auto previous = processingKernel_ ? processingKernel_->identity()
                                            : backend::processing::ProcessingCoreIdentity{};
    const auto next = kernel->identity();
    processingKernel_ = std::move(kernel);
    processingCoreSelectionAvailable_.store(true, std::memory_order_release);

    {
        std::scoped_lock snapshotLock(snapshotMutex_);
        latestSnapshot_.reset();
    }
    clearAccumulatedFrames();
    clearMonitoringFrames();
    {
        std::scoped_lock realtimeLock(rtMutex_);
        rtBgGray_.reset();
    }
    configVersion_.fetch_add(1, std::memory_order_release);
    {
        std::scoped_lock previousFrameLock(previousFrameMutex_);
        previousFrameForAutoCapture_.release();
    }
    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
    lastAutoBackgroundFrame_.store(0, std::memory_order_relaxed);
    coreLock.unlock();
    SPDLOG_INFO("ProcessingService: activated processing core {} (contract={}, abi={}, source={}); "
                "previous core was {}",
                next.version, next.contractVersion, next.engineAbiVersion, next.source,
                previous.version);
    return true;
}

bool ProcessingService::activateBundledProcessingKernel(std::string* error) {
    return activateProcessingKernel(backend::processing::makeBundledProcessingKernel(), error);
}

backend::processing::ProcessingCoreIdentity
ProcessingService::activeProcessingCoreIdentity() const {
    std::shared_lock lock(processingKernelMutex_);
    return processingKernel_ ? processingKernel_->identity()
                             : backend::processing::bundledProcessingCoreIdentity();
}

bool ProcessingService::isProcessingCorePinSatisfied() const {
    const auto identity = activeProcessingCoreIdentity();
    return processingCoreSelectionAvailable_.load(std::memory_order_acquire) &&
           (requiredProcessingCoreVersion_.empty() ||
            identity.version == requiredProcessingCoreVersion_);
}

void ProcessingService::markProcessingCoreSelectionUnavailable() {
    processingCoreSelectionAvailable_.store(false, std::memory_order_release);
}

ProcessingService::CoreOperationLease ProcessingService::acquireProcessingCoreOperation() {
    std::shared_lock coreLock(processingKernelMutex_);
    activeSynchronousCoreOperations_.fetch_add(1, std::memory_order_acq_rel);
    return CoreOperationLease(this, processingKernel_
                                        ? processingKernel_->identity()
                                        : backend::processing::bundledProcessingCoreIdentity());
}

void ProcessingService::releaseProcessingCoreOperation() noexcept {
    activeSynchronousCoreOperations_.fetch_sub(1, std::memory_order_acq_rel);
}

void ProcessingService::stopRealtime() {
    const bool wasRunning = rtRunning_.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning && !realtimeThread_.joinable()) return;
    if (realtimeThread_.joinable()) realtimeThread_.join();
    backend::diagnostics::CrashStateMirror::instance().processing.realtimeRunning.store(false);
    SPDLOG_INFO("ProcessingService: realtime processing stopped");
}

void ProcessingService::setRealtimeEnabled(bool on) {
    rtEnabled_.store(on);
}

void ProcessingService::setRealtimeDropFrames(bool on) {
    rtDropFrames_.store(on, std::memory_order_relaxed);
}

void ProcessingService::setRealtimeProcessingMode(RealtimeProcessingMode mode) {
    const int normalized = static_cast<int>(mode);
    const int previous = rtProcessingMode_.exchange(normalized, std::memory_order_acq_rel);
    if (previous == normalized) {
        return;
    }

    SPDLOG_INFO("ProcessingService: realtime processing mode set to {}",
                mode == RealtimeProcessingMode::AsyncBatch ? "async_batch" : "inline");

    if (rtRunning_.load(std::memory_order_acquire)) {
        auto store = rtStore_;
        stopRealtime();
        startRealtime(store);
    }
}

ProcessingService::RealtimeProcessingMode ProcessingService::getRealtimeProcessingMode() const {
    const int mode = rtProcessingMode_.load(std::memory_order_acquire);
    return mode == static_cast<int>(RealtimeProcessingMode::AsyncBatch)
               ? RealtimeProcessingMode::AsyncBatch
               : RealtimeProcessingMode::Inline;
}

void ProcessingService::setRealtimeBatchSettings(const RealtimeBatchSettings& settings) {
    RealtimeBatchSettings normalized = settings;
    normalized.batchSize = std::max<size_t>(1, normalized.batchSize);
    normalized.maxQueuedFrames = std::max(normalized.batchSize, normalized.maxQueuedFrames);
    normalized.workerCount = std::max<size_t>(1, normalized.workerCount);
    normalized.maxBatchDelayMs = std::max(1, normalized.maxBatchDelayMs);

    bool restart = false;
    {
        std::scoped_lock lk(rtBatchSettingsMutex_);
        restart = rtRunning_.load(std::memory_order_acquire) &&
                  getRealtimeProcessingMode() == RealtimeProcessingMode::AsyncBatch &&
                  rtBatchSettings_.workerCount != normalized.workerCount;
        rtBatchSettings_ = normalized;
    }

    SPDLOG_INFO("ProcessingService: realtime batch settings batch_size={}, max_queue={}, "
                "workers={}, max_delay_ms={}",
                normalized.batchSize, normalized.maxQueuedFrames, normalized.workerCount,
                normalized.maxBatchDelayMs);

    if (restart) {
        auto store = rtStore_;
        stopRealtime();
        startRealtime(store);
    } else {
        refreshRealtimeBatchPipelineConfig();
    }
}

ProcessingService::RealtimeBatchSettings ProcessingService::getRealtimeBatchSettings() const {
    std::scoped_lock lk(rtBatchSettingsMutex_);
    return rtBatchSettings_;
}

void ProcessingService::setRealtimeRoi(const Roi& roi) {
    {
        std::scoped_lock lk(rtMutex_);
        rtRoi_ = roi;
    }
    configVersion_.fetch_add(1, std::memory_order_release);
    refreshRealtimeBatchPipelineConfig();
}

ProcessingService::Roi ProcessingService::getRealtimeRoi() const {
    std::scoped_lock lk(rtMutex_);
    return rtRoi_;
}

void ProcessingService::setRealtimeBackgroundGray(const cv::Mat& bg) {
    {
        std::scoped_lock lk(rtMutex_);
        if (!bg.empty() && bg.type() == CV_8UC1) {
            rtBgGray_ = std::make_shared<cv::Mat>(bg.clone());
        } else if (!bg.empty()) {
            cv::Mat tmp;
            bg.convertTo(tmp, CV_8UC1);
            rtBgGray_ = std::make_shared<cv::Mat>(std::move(tmp));
        } else {
            rtBgGray_.reset();
        }
    }
    // Every publication (or clear) is a new background identity (issue #369).
    backgroundGeneration_.fetch_add(1, std::memory_order_acq_rel);
    configVersion_.fetch_add(
        1, std::memory_order_release); // wake cached-config refresh in realtime loop
    refreshRealtimeBatchPipelineConfig();
}

cv::Mat ProcessingService::getRealtimeBackgroundGray() const {
    std::scoped_lock lk(rtMutex_);
    if (rtBgGray_ && !rtBgGray_->empty()) {
        return rtBgGray_->clone();
    }
    return cv::Mat();
}

std::shared_ptr<const cv::Mat> ProcessingService::getRealtimeBackgroundGrayShared() const {
    std::scoped_lock lk(rtMutex_);
    return rtBgGray_; // implicit conversion shared_ptr<cv::Mat> → shared_ptr<const cv::Mat>
}

uint64_t ProcessingService::getConfigVersion() const {
    return configVersion_.load(std::memory_order_acquire);
}

bool ProcessingService::getLatestSnapshot(RealtimeSnapshot& out) {
    std::shared_ptr<const RealtimeSnapshot> snap;
    {
        std::scoped_lock lk(snapshotMutex_);
        snap = latestSnapshot_; // O(1) pointer copy inside lock
    }
    if (!snap || (snap->mask.empty() && snap->contours.empty())) return false;
    out.index = snap->index;
    out.mask = snap->mask; // shallow refcount share (read-only consumers)
    out.contours = snap->contours;
    out.validation = snap->validation;
    return true;
}

void ProcessingService::startExperiment() {
    std::unique_lock coreLock(processingKernelMutex_);
    {
        // Tear down any prior flush queue (dtor drains + joins) so a new
        // experiment starts with a clean, non-errored writer.
        std::scoped_lock qlk(flushQueueMutex_);
        flushQueue_.reset();
    }
    const size_t flushInterval = flushInterval_.load(std::memory_order_relaxed);
    const size_t maxBuffered = maxBufferedFrames_.load(std::memory_order_relaxed);
    experimentBuffer_.setPolicy({maxBuffered, maxBufferedBytes_.load(std::memory_order_relaxed)});
    experimentBuffer_.clear();
    experimentBuffer_.resetTotals();
    flushQueueBytes_.reset();
    framesSinceLastFlush_.store(0);
    invalidFrameCounter_.store(0);
    totalValidFlushed_.store(0, std::memory_order_relaxed);
    totalInvalidFlushed_.store(0, std::memory_order_relaxed);
    droppedValidFrames_.store(0, std::memory_order_relaxed);
    droppedInvalidFrames_.store(0, std::memory_order_relaxed);
    lastDropLogUs_.store(0, std::memory_order_relaxed);
    resetRealtimeMetrics();
    experimentAccounting_.reset(experimentAccountingGeneration_, experimentAccountingPolicyAllowsDrops_);
    experimentSettled_.store(false, std::memory_order_release);
    backend::diagnostics::PipelineTimingRecorder::instance().resetLiveLatency();
    // Reset auto-capture counter when experiment starts
    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
    experimentActive_.store(true);
    {
        auto& m = backend::diagnostics::CrashStateMirror::instance().processing;
        m.experimentActive.store(true);
        m.droppedValidFrames.store(0, std::memory_order_relaxed);
        m.targetGroupObjects.store(0, std::memory_order_relaxed);
        m.unservedTargetGroupObjects.store(0, std::memory_order_relaxed);
    }
    SPDLOG_INFO("ProcessingService: experiment started, frame buffers cleared (flush interval: {} "
                "frames, max buffered: {} frames / {} MB, invalid sampling: every {}th)",
                flushInterval, maxBuffered,
                maxBufferedBytes_.load(std::memory_order_relaxed) / (1024.0 * 1024.0),
                invalidFrameSamplingRate_.load());
}

void ProcessingService::endExperiment() {
    experimentActive_.store(false);
    backend::diagnostics::CrashStateMirror::instance().processing.experimentActive.store(false);
    // Let the realtime thread finish the frame it admitted under the run so
    // the accounting snapshot taken by the caller is complete (bounded).
    if (experimentAccounting_.hasAdmitted() && rtRunning_.load(std::memory_order_acquire)) {
        const uint64_t last = experimentAccounting_.lastAdmittedIndex();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        auto batchPending = [this] {
            std::scoped_lock lk(batchMutex_);
            return !batchQueue_.empty() ||
                   batchFramesInFlight_.load(std::memory_order_acquire) != 0;
        };
        while ((rtLastProcessed_.load(std::memory_order_acquire) < last ||
                (rtBatchPipelineActive_.load(std::memory_order_acquire) && batchPending())) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    // Settle the run exactly: frames admitted but still unprocessed when the
    // drain gave up (a slow batch pipeline, e.g. under a sanitizer) are
    // booked as PendingAtStop under the settlement lock, and every later
    // outcome/append for this run is dropped. The accounting always
    // reconciles; what was not processed is declared, never invented.
    {
        std::unique_lock settle(experimentSettleMutex_);
        if (!experimentSettled_.exchange(true, std::memory_order_acq_rel)) {
            const auto s = experimentAccounting_.snapshot();
            const uint64_t terms = s.frameTermsSum();
            if (s.admitted > terms) {
                const uint64_t unsettled = s.admitted - terms;
                experimentAccounting_.count(backend::recording::FrameOutcome::PendingAtStop, unsettled);
                SPDLOG_WARN("ProcessingService: {} admitted frame(s) still unprocessed at experiment stop "
                            "(booked as pendingAtStop)", unsettled);
            }
        }
    }
    const BufferedFrameCounts counts = experimentBuffer_.counts();
    SPDLOG_INFO("ProcessingService: experiment ended, valid frames: {}, invalid frames: {}",
                counts.valid, counts.invalid);
}

std::vector<ProcessedFrame> ProcessingService::getValidFrames() const {
    return experimentBuffer_.copyValid();
}

std::vector<ProcessedFrame> ProcessingService::getInvalidFrames() const {
    return experimentBuffer_.copyInvalid();
}

BufferedFrameCounts ProcessingService::getBufferedFrameCounts() const {
    return experimentBuffer_.counts();
}

void ProcessingService::clearAccumulatedFrames() {
    experimentBuffer_.clear();
    framesSinceLastFlush_.store(0, std::memory_order_relaxed);
}

void ProcessingService::setMaxBufferedBytes(uint64_t bytes) {
    maxBufferedBytes_.store(bytes, std::memory_order_relaxed);
    experimentBuffer_.setPolicy({std::max<size_t>(1, maxBufferedFrames_.load(std::memory_order_relaxed)), bytes});
    SPDLOG_INFO("Experiment buffer byte budget set to {} MB{}", bytes / (1024.0 * 1024.0),
                bytes == 0 ? " (count-only)" : "");
}

uint64_t ProcessingService::getMaxBufferedBytes() const {
    return maxBufferedBytes_.load(std::memory_order_relaxed);
}

ProcessingService::MemoryStats ProcessingService::memoryStats() const {
    MemoryStats m;
    m.experimentBuffer = experimentBuffer_.memoryStats();
    {
        std::scoped_lock lk(monitoringFramesMutex_);
        backend::diagnostics::MemoryOwnerStats o;
        o.name = "processing.monitoringRings";
        o.knowledge = backend::diagnostics::MemoryKnowledge::Measured;
        o.currentBytes = monitoringValidFrames_.bytes() + monitoringInvalidFrames_.bytes();
        o.peakBytes = monitoringValidFrames_.peakBytes() + monitoringInvalidFrames_.peakBytes();
        o.currentCount = monitoringValidFrames_.size() + monitoringInvalidFrames_.size();
        o.peakCount = o.currentCount; // rings only grow to capacity
        o.capacityCount = monitoringValidFrames_.capacity() + monitoringInvalidFrames_.capacity();
        o.capacityBytes = 0; // bounded by count; bytes follow the frame geometry
        o.evictedByBudget = monitoringValidFrames_.replaced() + monitoringInvalidFrames_.replaced();
        o.note = monitoringActive_.load(std::memory_order_relaxed)
                     ? "presentation rings (valid+invalid) for the Monitoring tab; oldest entries replaced"
                     : "presentation rings inactive (no consumer): nothing retained";
        m.monitoringRings = o;
    }
    {
        uint64_t capBytes = 0;
        size_t capCount = 0;
        {
            std::scoped_lock lk(batchMutex_);
            capBytes = batchConfig_.maxQueuedBytes;
            capCount = batchConfig_.maxQueuedFrames;
        }
        m.batchQueue = batchQueueBytes_.snapshot("processing.batchQueue",
                                                 backend::diagnostics::MemoryKnowledge::Measured, capBytes,
                                                 capCount, "async batch input frames awaiting a worker");
        m.batchQueue.evictedByBudget = batchFramesDropped_.load(std::memory_order_relaxed);
    }
    m.flushQueue = flushQueueBytes_.snapshot("processing.flushQueue",
                                             backend::diagnostics::MemoryKnowledge::Measured, 0, 3,
                                             "experiment batches submitted to the HDF5 writer, not yet written");
    {
        backend::diagnostics::MemoryOwnerStats o;
        o.name = "processing.snapshot";
        o.knowledge = backend::diagnostics::MemoryKnowledge::Measured;
        std::shared_ptr<const RealtimeSnapshot> snap;
        {
            std::scoped_lock lk(snapshotMutex_);
            snap = latestSnapshot_;
        }
        if (snap && !snap->mask.empty()) {
            o.currentBytes = static_cast<uint64_t>(snap->mask.total()) * snap->mask.elemSize();
            o.currentCount = 1;
        }
        o.peakBytes = o.currentBytes;
        o.peakCount = o.currentCount;
        o.capacityCount = 1;
        o.note = "latest realtime snapshot mask (pointer-swapped; older snapshots die with their last reader)";
        m.snapshot = o;
    }
    return m;
}

std::vector<ProcessedFrame> ProcessingService::getMonitoringValidFrames() const {
    std::scoped_lock lk(monitoringFramesMutex_);
    return monitoringValidFrames_.toVector();
}

std::vector<ProcessedFrame> ProcessingService::getMonitoringInvalidFrames() const {
    std::scoped_lock lk(monitoringFramesMutex_);
    return monitoringInvalidFrames_.toVector();
}

void ProcessingService::clearMonitoringFrames() {
    // Clear is atomic from the consumer's perspective: buffers and appended
    // totals reset under the same lock the reader snapshot takes (BE-5).
    std::scoped_lock lk(monitoringFramesMutex_);
    monitoringValidFrames_.clear();
    monitoringInvalidFrames_.clear();
    monitoringValidAppended_.store(0, std::memory_order_relaxed);
    monitoringInvalidAppended_.store(0, std::memory_order_relaxed);
}

void ProcessingService::setMonitoringActive(bool active) {
    monitoringActive_.store(active, std::memory_order_relaxed);
}

void ProcessingService::setProcessingConfig(const ProcessingConfig& config) {
    {
        std::scoped_lock lk(configMutex_);
        processingConfig_ = config;
    }
    configVersion_.fetch_add(1, std::memory_order_release);
    refreshRealtimeBatchPipelineConfig();
}

ProcessingConfig ProcessingService::getProcessingConfig() const {
    std::scoped_lock lk(configMutex_);
    return processingConfig_;
}

void ProcessingService::setPixelToMicronFactor(double factor) {
    pixelToMicronFactor_.store(factor, std::memory_order_relaxed);
}

double ProcessingService::getPixelToMicronFactor() const {
    return pixelToMicronFactor_.load(std::memory_order_relaxed);
}

bool ProcessingService::loadEModulusLut(const std::string& path) {
    return eModulusLut_.loadFromFile(path);
}

static inline cv::Mat makeGrayCopy(const backend::playback::Frame& frame) {
    if (frame.data.empty() || frame.width == 0 || frame.height == 0) {
        return cv::Mat();
    }
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const size_t step = (frame.linePitch == 0 ? static_cast<size_t>(frame.width) : frame.linePitch);
    // Geometry comes from the camera and the buffer from a separate SDK
    // query; a short buffer (pixel-format change, partial delivery on
    // disconnect) would make the clone below read out of bounds.
    const size_t requiredBytes = static_cast<size_t>(h - 1) * step + static_cast<size_t>(w);
    if (frame.data.size() < requiredBytes) {
        SPDLOG_WARN("ProcessingService: frame data ({} bytes) smaller than geometry requires "
                    "({}x{} pitch={} -> {} bytes); frame skipped",
                    frame.data.size(), frame.width, frame.height, step, requiredBytes);
        return cv::Mat();
    }
    cv::Mat view(h, w, CV_8UC1, const_cast<uint8_t*>(frame.data.data()), step);
    return view.clone();
}

// Extract ROI directly from frame data without full frame copy
static inline cv::Mat makeGrayROI(const backend::playback::Frame& frame, int roiX, int roiY,
                                  int roiW, int roiH) {
    if (frame.data.empty() || frame.width == 0 || frame.height == 0) {
        return cv::Mat();
    }

    const int frameW = static_cast<int>(frame.width);
    const int frameH = static_cast<int>(frame.height);

    // Clamp ROI to frame bounds
    const int clampedX = std::max(0, std::min(roiX, frameW - 1));
    const int clampedY = std::max(0, std::min(roiY, frameH - 1));
    const int clampedW = std::max(1, std::min(roiW, frameW - clampedX));
    const int clampedH = std::max(1, std::min(roiH, frameH - clampedY));

    const size_t srcPitch =
        (frame.linePitch == 0 ? static_cast<size_t>(frame.width) : frame.linePitch);
    // Same producer-trust issue as makeGrayCopy: validate the buffer actually
    // covers the rows the strided view will touch.
    const size_t requiredBytes = static_cast<size_t>(clampedY + clampedH - 1) * srcPitch +
                                 static_cast<size_t>(clampedX + clampedW);
    if (frame.data.size() < requiredBytes) {
        SPDLOG_WARN("ProcessingService: frame data ({} bytes) smaller than ROI requires "
                    "({}x{} pitch={} -> {} bytes); frame skipped",
                    frame.data.size(), frame.width, frame.height, srcPitch, requiredBytes);
        return cv::Mat();
    }
    const uint8_t* srcPtr = frame.data.data() + (clampedY * srcPitch) + clampedX;

    // Create ROI Mat view
    cv::Mat roiView(clampedH, clampedW, CV_8UC1, const_cast<uint8_t*>(srcPtr), srcPitch);

    // Clone to ensure contiguous memory and ownership
    return roiView.clone();
}

bool ProcessingService::isFrameEmpty(const backend::playback::Frame& frame,
                                     const ProcessingConfig& config, const Roi& roi,
                                     const cv::Mat& background) {
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        return true;
    }

    cv::Mat gray = makeGrayCopy(frame);
    if (gray.empty()) {
        return true; // undecodable frame counts as empty
    }

    // Determine ROI
    Roi effectiveRoi = roi;
    if (effectiveRoi.w <= 0 || effectiveRoi.h <= 0) {
        effectiveRoi.x = 0;
        effectiveRoi.y = 0;
        effectiveRoi.w = static_cast<int>(gray.cols);
        effectiveRoi.h = static_cast<int>(gray.rows);
    }
    effectiveRoi.x = std::max(0, std::min(effectiveRoi.x, gray.cols - 1));
    effectiveRoi.y = std::max(0, std::min(effectiveRoi.y, gray.rows - 1));
    effectiveRoi.w = std::max(1, std::min(effectiveRoi.w, gray.cols - effectiveRoi.x));
    effectiveRoi.h = std::max(1, std::min(effectiveRoi.h, gray.rows - effectiveRoi.y));

    cv::Rect cvRoi(effectiveRoi.x, effectiveRoi.y, effectiveRoi.w, effectiveRoi.h);
    cv::Mat roiCurr = gray(cvRoi);

    // Apply same processing as realtime loop
    cv::Mat blurredCurr, blurredBg, diff, thresh;
    cv::GaussianBlur(roiCurr, blurredCurr, cv::Size(3, 3), 0);

    if (!background.empty() && background.size() == gray.size() && background.type() == CV_8UC1) {
        cv::GaussianBlur(background(cvRoi), blurredBg, cv::Size(3, 3), 0);
        cv::subtract(blurredCurr, blurredBg, diff);
    } else {
        diff = blurredCurr;
    }

    cv::threshold(diff, thresh, config.bg_subtract_threshold, 255, cv::THRESH_BINARY);

    // Count non-zero pixels
    int pixelCount = cv::countNonZero(thresh);
    return pixelCount < config.empty_frame_pixel_threshold;
}

bool ProcessingService::isFrameEmpty(const backend::playback::Frame& frame,
                                     const ProcessingConfig& config, const Roi& roi,
                                     const std::shared_ptr<const cv::Mat>& background) {
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        return true;
    }

    const int frameW = static_cast<int>(frame.width);
    const int frameH = static_cast<int>(frame.height);

    Roi effectiveRoi = roi;
    if (effectiveRoi.w <= 0 || effectiveRoi.h <= 0) {
        effectiveRoi = {0, 0, frameW, frameH};
    }
    effectiveRoi.x = std::max(0, std::min(effectiveRoi.x, frameW - 1));
    effectiveRoi.y = std::max(0, std::min(effectiveRoi.y, frameH - 1));
    effectiveRoi.w = std::max(1, std::min(effectiveRoi.w, frameW - effectiveRoi.x));
    effectiveRoi.h = std::max(1, std::min(effectiveRoi.h, frameH - effectiveRoi.y));

    // ROI-only extraction — no full-frame copy
    cv::Mat roiCurr =
        makeGrayROI(frame, effectiveRoi.x, effectiveRoi.y, effectiveRoi.w, effectiveRoi.h);

    cv::Rect cvRoi(effectiveRoi.x, effectiveRoi.y, effectiveRoi.w, effectiveRoi.h);
    cv::Mat blurredCurr, blurredBg, diff, thresh;
    cv::GaussianBlur(roiCurr, blurredCurr, cv::Size(3, 3), 0);

    if (background && !background->empty() && background->cols == frameW &&
        background->rows == frameH && background->type() == CV_8UC1) {
        cv::GaussianBlur((*background)(cvRoi), blurredBg, cv::Size(3, 3), 0);
        cv::subtract(blurredCurr, blurredBg, diff);
    } else {
        diff = blurredCurr;
    }

    cv::threshold(diff, thresh, config.bg_subtract_threshold, 255, cv::THRESH_BINARY);
    return cv::countNonZero(thresh) < config.empty_frame_pixel_threshold;
}

bool ProcessingService::isFrameEmptyWithActiveKernel(
    const backend::playback::Frame& frame, const ProcessingConfig& config, const Roi& roi,
    const std::shared_ptr<const cv::Mat>& background) const {
    return !classifyFrameWithActiveKernel(frame, config, roi, background).isCandidate();
}

ProcessingService::FrameClassification ProcessingService::classifyFrameWithActiveKernel(
    const backend::playback::Frame& frame, const ProcessingConfig& config, const Roi& roi,
    const std::shared_ptr<const cv::Mat>& background) const {
    FrameClassification c;
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        c.kind = FrameClassification::Kind::Malformed;
        c.detail = "frame has zero geometry or no payload";
        return c;
    }
    const size_t stride = frame.linePitch == 0 ? static_cast<size_t>(frame.width) : frame.linePitch;
    if (stride < frame.width) {
        c.kind = FrameClassification::Kind::Malformed;
        c.detail = "line pitch smaller than width";
        return c;
    }
    const uint64_t required = (frame.height - 1u) * static_cast<uint64_t>(stride) + frame.width;
    if (required > frame.data.size()) {
        c.kind = FrameClassification::Kind::Malformed;
        c.detail = "payload shorter than geometry requires";
        return c;
    }

    cv::Mat gray(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC1,
                 const_cast<uint8_t*>(frame.data.data()), stride);
    const cv::Mat backgroundView = background ? *background : cv::Mat{};
    bool empty = true;
    std::string detail;
    try {
        if (!isImageEmptyWithActiveKernel(gray, backgroundView, config, roi, false, empty, &detail)) {
            SPDLOG_WARN("ProcessingService: active core empty-frame check failed: {}", detail);
            c.kind = FrameClassification::Kind::ProcessingFailed;
            c.detail = detail.empty() ? "processing core rejected the frame" : detail;
            return c;
        }
    } catch (const std::exception& ex) {
        c.kind = FrameClassification::Kind::ProcessingFailed;
        c.detail = std::string("processing core threw: ") + ex.what();
        return c;
    } catch (...) {
        c.kind = FrameClassification::Kind::ProcessingFailed;
        c.detail = "processing core threw an unknown exception";
        return c;
    }
    c.kind = empty ? FrameClassification::Kind::Empty : FrameClassification::Kind::Candidate;
    return c;
}

void ProcessingService::setExperimentAccountingContext(uint64_t captureGeneration,
                                                       bool policyAllowsDrops) {
    experimentAccountingGeneration_ = captureGeneration;
    experimentAccountingPolicyAllowsDrops_ = policyAllowsDrops;
}

backend::recording::RecordingAccountingSnapshot ProcessingService::experimentAccountingSnapshot() const {
    auto s = experimentAccounting_.snapshot();
    // Derive the persistence terms that only the run boundary can know:
    // frames still buffered (never handed to the writer) and frames queued
    // but not confirmed written. A latched writer error turns the latter into
    // persistence failures instead of "pending".
    const size_t buffered = experimentBuffer_.counts().total();
    bool queueErrored = false;
    {
        std::scoped_lock qlk(flushQueueMutex_);
        queueErrored = flushQueue_ && flushQueue_->hasError();
    }
    const uint64_t accountedFor =
        s.persistenceCommitted + s.persistenceCancelledByPolicy + static_cast<uint64_t>(buffered);
    const uint64_t unresolved = s.persistenceAdmitted > accountedFor ? s.persistenceAdmitted - accountedFor : 0;
    if (queueErrored) {
        s.persistenceFailed = unresolved;
        s.persistencePendingAtStop = static_cast<uint64_t>(buffered);
    } else {
        s.persistencePendingAtStop = unresolved + static_cast<uint64_t>(buffered);
    }
    return backend::recording::reconcile(s);
}

void ProcessingService::noteRealtimeAdmitted(uint64_t idx) {
    if (experimentActive_.load(std::memory_order_relaxed)) experimentAccounting_.admit(idx);
}

void ProcessingService::noteRealtimeLost(uint64_t count) {
    if (count > 0 && experimentActive_.load(std::memory_order_relaxed)) {
        experimentAccounting_.admitLost(count, backend::recording::FrameOutcome::StoreOverwritten);
    }
}

void ProcessingService::noteRealtimeOutcome(uint64_t idx, backend::recording::FrameOutcome outcome,
                                            const backend::playback::Frame* frame) {
    if (outcome == backend::recording::FrameOutcome::ProcessingFailed) {
        processingFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    // Count the outcome iff the admission was counted (not "iff active"):
    // a frame in flight across start/endExperiment is otherwise counted on
    // one side only and a clean run reads "does not reconcile". After
    // endExperiment() settled the run (settlement lock), late outcomes are
    // dropped: their frames were already booked as PendingAtStop.
    {
        std::shared_lock settle(experimentSettleMutex_);
        if (!experimentSettled_.load(std::memory_order_relaxed) && experimentAccounting_.wasAdmitted(idx)) {
            experimentAccounting_.count(outcome);
        }
    }
    if (bgCalActive_.load(std::memory_order_acquire)) bgCalObserve(outcome, frame);
}

// ---- Bounded background calibration (issue #369) --------------------------

std::string ProcessingService::backgroundSha256() const {
    std::shared_ptr<const cv::Mat> bg = getRealtimeBackgroundGrayShared();
    if (!bg || bg->empty()) return {};
    cv::Mat contiguous = bg->isContinuous() ? *bg : bg->clone();
    return backend::processing::processingCoreBytesSha256(contiguous.data, contiguous.total() * contiguous.elemSize());
}

bool ProcessingService::startBackgroundCalibration(const BackgroundCalibrationRequest& request,
                                                   std::string* error) {
    if (!rtRunning_.load(std::memory_order_acquire)) {
        if (error) *error = "realtime processing is not running";
        return false;
    }
    if (request.requiredAccepted == 0 || request.maxAttempts < request.requiredAccepted) {
        if (error) *error = "invalid calibration request (requiredAccepted must be >= 1 and <= maxAttempts)";
        return false;
    }
    std::scoped_lock lk(bgCalMutex_);
    if (bgCalStatus_.state == BackgroundCalibrationState::Running) {
        if (error) *error = "a background calibration is already running";
        return false;
    }
    bgCalRequest_ = request;
    bgCalStatus_ = BackgroundCalibrationStatus{};
    bgCalStatus_.state = BackgroundCalibrationState::Running;
    bgCalStatus_.operationGeneration = ++bgCalOperationCounter_;
    bgCalStatus_.frozenConfigVersion = configVersion_.load(std::memory_order_acquire);
    bgCalAccumulator_.release();
    bgCalDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeoutMs);
    bgCalActive_.store(true, std::memory_order_release);
    SPDLOG_INFO("Background calibration started: required={} maxAttempts={} timeoutMs={} configVersion={}",
                request.requiredAccepted, request.maxAttempts, request.timeoutMs,
                bgCalStatus_.frozenConfigVersion);
    return true;
}

void ProcessingService::cancelBackgroundCalibration() {
    std::scoped_lock lk(bgCalMutex_);
    if (bgCalStatus_.state != BackgroundCalibrationState::Running) return;
    bgCalFinishLocked(BackgroundCalibrationState::Cancelled, "cancelled by operator");
}

ProcessingService::BackgroundCalibrationStatus ProcessingService::backgroundCalibrationStatus() const {
    std::scoped_lock lk(bgCalMutex_);
    // Lazy timeout so a stalled frame source cannot leave the operation
    // apparently running forever.
    if (bgCalStatus_.state == BackgroundCalibrationState::Running &&
        std::chrono::steady_clock::now() >= bgCalDeadline_) {
        const_cast<ProcessingService*>(this)->bgCalFinishLocked(
            BackgroundCalibrationState::FailedTimeout,
            "timed out after " + std::to_string(bgCalRequest_.timeoutMs) + " ms with " +
                std::to_string(bgCalStatus_.accepted) + "/" + std::to_string(bgCalRequest_.requiredAccepted) +
                " accepted");
    }
    return bgCalStatus_;
}

void ProcessingService::bgCalFinishLocked(BackgroundCalibrationState state, const std::string& message) {
    bgCalStatus_.state = state;
    bgCalStatus_.message = message;
    bgCalActive_.store(false, std::memory_order_release);
    bgCalAccumulator_.release();
    SPDLOG_INFO("Background calibration finished: state={} attempted={} accepted={} rejectedNonEmpty={} "
                "rejectedFailed={} — {}",
                static_cast<int>(state), bgCalStatus_.attempted, bgCalStatus_.accepted,
                bgCalStatus_.rejectedNonEmpty, bgCalStatus_.rejectedProcessingFailed, message);
}

void ProcessingService::bgCalObserve(backend::recording::FrameOutcome outcome,
                                     const backend::playback::Frame* frame) {
    using backend::recording::FrameOutcome;
    std::scoped_lock lk(bgCalMutex_);
    if (bgCalStatus_.state != BackgroundCalibrationState::Running) return;
    if (std::chrono::steady_clock::now() >= bgCalDeadline_) {
        bgCalFinishLocked(BackgroundCalibrationState::FailedTimeout,
                          "timed out with " + std::to_string(bgCalStatus_.accepted) + "/" +
                              std::to_string(bgCalRequest_.requiredAccepted) + " accepted");
        return;
    }
    // The recipe is frozen: a config change mid-operation invalidates it.
    if (configVersion_.load(std::memory_order_acquire) != bgCalStatus_.frozenConfigVersion) {
        bgCalFinishLocked(BackgroundCalibrationState::FailedProcessing,
                          "processing configuration changed during calibration");
        return;
    }
    ++bgCalStatus_.attempted;
    if (outcome == FrameOutcome::Empty && frame) {
        cv::Mat gray = makeGrayCopy(*frame);
        if (!gray.empty()) {
            if (bgCalAccumulator_.empty() || bgCalAccumulator_.size() != gray.size()) {
                bgCalAccumulator_ = cv::Mat::zeros(gray.size(), CV_64FC1);
                bgCalStatus_.accepted = 0;
            }
            cv::accumulate(gray, bgCalAccumulator_);
            ++bgCalStatus_.accepted;
        } else {
            ++bgCalStatus_.rejectedProcessingFailed;
        }
    } else if (outcome == FrameOutcome::ProcessingFailed) {
        ++bgCalStatus_.rejectedProcessingFailed;
    } else {
        ++bgCalStatus_.rejectedNonEmpty; // contaminated frame
    }

    if (bgCalStatus_.accepted >= bgCalRequest_.requiredAccepted) {
        cv::Mat mean;
        bgCalAccumulator_.convertTo(mean, CV_8UC1, 1.0 / static_cast<double>(bgCalStatus_.accepted));
        // Atomic publication: the previous background stays active until the
        // candidate is installed here.
        {
            std::scoped_lock rtLk(rtMutex_);
            rtBgGray_ = std::make_shared<cv::Mat>(std::move(mean));
        }
        backgroundGeneration_.fetch_add(1, std::memory_order_acq_rel);
        configVersion_.fetch_add(1, std::memory_order_release);
        refreshRealtimeBatchPipelineConfig();
        bgCalStatus_.publishedBackgroundGeneration = backgroundGeneration_.load(std::memory_order_acquire);
        bgCalStatus_.publishedSha256 = backgroundSha256();
        bgCalFinishLocked(BackgroundCalibrationState::Succeeded,
                          "published background from " + std::to_string(bgCalStatus_.accepted) + " empty frames");
        return;
    }
    if (bgCalStatus_.attempted >= bgCalRequest_.maxAttempts) {
        bgCalFinishLocked(BackgroundCalibrationState::FailedInsufficient,
                          "only " + std::to_string(bgCalStatus_.accepted) + "/" +
                              std::to_string(bgCalRequest_.requiredAccepted) + " empty frames in " +
                              std::to_string(bgCalStatus_.attempted) + " attempts (" +
                              std::to_string(bgCalStatus_.rejectedNonEmpty) + " contaminated)");
    }
}

void ProcessingService::noteRealtimeValidation(uint64_t idx, const std::vector<FilterResult>& validations) {
    const bool anyValid = std::any_of(validations.begin(), validations.end(),
                                      [](const FilterResult& r) { return r.isValid; });
    const auto outcome = anyValid ? backend::recording::FrameOutcome::Processed
                                  : backend::recording::FrameOutcome::RejectedByScientificFilter;
    // A non-empty frame during background calibration is contamination
    // regardless of whether an experiment is active (issue #369).
    if (bgCalActive_.load(std::memory_order_acquire)) bgCalObserve(outcome, nullptr);
    std::shared_lock settle(experimentSettleMutex_);
    if (experimentSettled_.load(std::memory_order_relaxed) || !experimentAccounting_.wasAdmitted(idx)) return;
    experimentAccounting_.count(outcome);
    uint64_t objects = 0;
    for (const auto& r : validations) if (r.isValid) ++objects;
    if (objects > 0) experimentAccounting_.objectsDetected.fetch_add(objects, std::memory_order_relaxed);
}

bool ProcessingService::isImageEmptyWithActiveKernel(const cv::Mat& gray, const cv::Mat& background,
                                                     const ProcessingConfig& config, const Roi& roi,
                                                     bool absoluteBackgroundDifference, bool& empty,
                                                     std::string* error) const {
    const backend::processing::KernelConfig kernelConfig{
        config.gaussian_blur_size,          config.bg_subtract_threshold,
        config.morph_kernel_size,           config.morph_iterations,
        config.empty_frame_pixel_threshold, absoluteBackgroundDifference};
    const backend::processing::KernelRoi kernelRoi{roi.x, roi.y, roi.w, roi.h};
    std::shared_lock lock(processingKernelMutex_);
    if (!processingCoreSelectionAvailable_.load(std::memory_order_acquire)) {
        if (error) *error = "selected processing core is unavailable";
        return false;
    }
    if (!requiredProcessingCoreVersion_.empty() &&
        (!processingKernel_ ||
         processingKernel_->identity().version != requiredProcessingCoreVersion_)) {
        if (error) {
            *error =
                "required processing core " + requiredProcessingCoreVersion_ + " is not active";
        }
        return false;
    }
    if (!processingKernel_ ||
        !processingKernel_->isEmpty(gray, background, kernelConfig, kernelRoi, empty, error)) {
        return false;
    }
    return true;
}

bool ProcessingService::processMaskWithActiveKernel(const cv::Mat& gray, const cv::Mat& background,
                                                    const ProcessingConfig& config, const Roi& roi,
                                                    cv::Mat& mask, std::string* error) const {
    const backend::processing::KernelConfig kernelConfig{
        config.gaussian_blur_size, config.bg_subtract_threshold, config.morph_kernel_size,
        config.morph_iterations, config.empty_frame_pixel_threshold};
    const backend::processing::KernelRoi kernelRoi{roi.x, roi.y, roi.w, roi.h};
    std::shared_lock lock(processingKernelMutex_);
    if (!processingCoreSelectionAvailable_.load(std::memory_order_acquire)) {
        if (error) *error = "selected processing core is unavailable";
        return false;
    }
    if (!processingKernel_) {
        if (error) *error = "no active processing kernel";
        return false;
    }
    if (!requiredProcessingCoreVersion_.empty() &&
        processingKernel_->identity().version != requiredProcessingCoreVersion_) {
        if (error) {
            *error =
                "required processing core " + requiredProcessingCoreVersion_ + " is not active";
        }
        return false;
    }
    return processingKernel_->processMask(gray, background, kernelConfig, kernelRoi, mask, error);
}

ProcessedFrame ProcessingService::computeProcessedFrame(const cv::Mat& grayInput,
                                                        const cv::Mat& backgroundGray,
                                                        const ProcessingConfig& config,
                                                        const Roi& roiIn, uint64_t index,
                                                        uint64_t timestampNs) {
    auto operation = acquireProcessingCoreOperation();

    ProcessedFrame out;
    out.index = index;
    out.timestampNs = timestampNs;

    if (grayInput.empty()) {
        SPDLOG_WARN("computeProcessedFrame: empty input image");
        return out;
    }

    // Coerce input to CV_8UC1
    cv::Mat gray;
    if (grayInput.type() == CV_8UC1) {
        gray = grayInput.clone();
    } else if (grayInput.channels() == 3) {
        cv::cvtColor(grayInput, gray, cv::COLOR_BGR2GRAY);
    } else {
        grayInput.convertTo(gray, CV_8UC1);
    }
    out.originalImage = gray; // clone already via above paths

    // Clamp ROI (or default to full frame)
    Roi roi = roiIn;
    if (roi.w <= 0 || roi.h <= 0) {
        roi.x = 0;
        roi.y = 0;
        roi.w = gray.cols;
        roi.h = gray.rows;
    }
    roi.x = std::max(0, std::min(roi.x, gray.cols - 1));
    roi.y = std::max(0, std::min(roi.y, gray.rows - 1));
    roi.w = std::max(1, std::min(roi.w, gray.cols - roi.x));
    roi.h = std::max(1, std::min(roi.h, gray.rows - roi.y));
    const cv::Rect cvRoi(roi.x, roi.y, roi.w, roi.h);

    cv::Mat mask;
    std::string kernelError;
    if (!processMaskWithActiveKernel(gray, backgroundGray, config, roi, mask, &kernelError)) {
        SPDLOG_ERROR("computeProcessedFrame: active processing core failed: {}", kernelError);
        return out;
    }

    // Validation + contour/metric extraction (same helper as realtime)
    out.validation = filterProcessedImage(mask, cvRoi, config, gray);
    out.processedImage = std::move(mask);
    return out;
}

std::vector<ProcessedFrame>
ProcessingService::processBatch(const std::vector<cv::Mat>& grayImages,
                                const ProcessingConfig& config, const cv::Mat& background,
                                const Roi& roi, BatchProgressCallback progress,
                                backend::processing::ProcessingCoreIdentity* processingCore) {

    auto operation = acquireProcessingCoreOperation();
    if (processingCore) *processingCore = operation.identity();

    std::vector<ProcessedFrame> results;
    results.reserve(grayImages.size());
    std::vector<BatchTrack> tracks;
    int nextTrackId = 1;

    const auto attachSeriesImages = [&](ProcessedFrame& frame, size_t triggerIndex) {
        if (!config.multi_image_enabled || config.multi_image_count <= 1 ||
            !frame.validation.isValid) {
            return;
        }

        const size_t requested = static_cast<size_t>(config.multi_image_count);
        const size_t available = std::min(requested, grayImages.size() - triggerIndex);
        frame.seriesImages.reserve(available);
        for (size_t offset = 0; offset < available; ++offset) {
            const cv::Mat& input = grayImages[triggerIndex + offset];
            if (input.empty()) {
                break;
            }
            cv::Mat gray;
            if (input.type() == CV_8UC1) {
                gray = input.clone();
            } else if (input.channels() == 3) {
                cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
            } else {
                input.convertTo(gray, CV_8UC1);
            }
            frame.seriesImages.push_back(std::move(gray));
        }
    };

    const size_t total = grayImages.size();
    if (progress) progress(BatchProgress{0, total});

    for (size_t i = 0; i < total; ++i) {
        ProcessedFrame base = computeProcessedFrame(grayImages[i], background, config, roi,
                                                    static_cast<uint64_t>(i), 0);
        if (base.originalImage.empty() || base.processedImage.empty()) {
            results.emplace_back(std::move(base));
            if (progress) progress(BatchProgress{i + 1, total});
            continue;
        }

        Roi normalizedRoi = roi;
        if (normalizedRoi.w <= 0 || normalizedRoi.h <= 0) {
            normalizedRoi.x = 0;
            normalizedRoi.y = 0;
            normalizedRoi.w = base.originalImage.cols;
            normalizedRoi.h = base.originalImage.rows;
        }
        normalizedRoi.x = std::max(0, std::min(normalizedRoi.x, base.originalImage.cols - 1));
        normalizedRoi.y = std::max(0, std::min(normalizedRoi.y, base.originalImage.rows - 1));
        normalizedRoi.w =
            std::max(1, std::min(normalizedRoi.w, base.originalImage.cols - normalizedRoi.x));
        normalizedRoi.h =
            std::max(1, std::min(normalizedRoi.h, base.originalImage.rows - normalizedRoi.y));
        const cv::Rect cvRoi(normalizedRoi.x, normalizedRoi.y, normalizedRoi.w, normalizedRoi.h);

        auto objectResults =
            filterProcessedObjects(base.processedImage, cvRoi, config, base.originalImage);
        if (objectResults.empty()) {
            results.emplace_back(std::move(base));
        } else {
            std::vector<bool> matchedThisFrame(tracks.size(), false);
            for (auto& validation : objectResults) {
                ProcessedFrame objectFrame;
                objectFrame.index = base.index;
                objectFrame.timestampNs = base.timestampNs;
                // Issue #370: objects of one frame share the immutable source
                // + mask by refcount (frozen-Mats invariant); nothing clones
                // merely for lifetime.
                objectFrame.originalImage = base.originalImage;
                objectFrame.processedImage = base.processedImage;
                objectFrame.validation = std::move(validation);
                if (!objectFrame.validation.isValid) {
                    results.emplace_back(std::move(objectFrame));
                    continue;
                }

                const int trackIdx =
                    matchTrackWithActiveKernel(tracks, matchedThisFrame, objectFrame.validation,
                                               objectFrame.index, cvRoi.width);
                if (trackIdx >= 0) {
                    auto& track = tracks[static_cast<size_t>(trackIdx)];
                    track.lastFrame = objectFrame.index;
                    track.lastBbox =
                        backend::processing::science::resultBbox(objectFrame.validation);
                    track.lastCentroid = cv::Point2d(objectFrame.validation.centroidX,
                                                     objectFrame.validation.centroidY);
                    ++track.observations;
                    if (static_cast<size_t>(trackIdx) >= matchedThisFrame.size()) {
                        matchedThisFrame.resize(tracks.size(), false);
                    }
                    matchedThisFrame[static_cast<size_t>(trackIdx)] = true;
                    applyTrackState(results[track.outputIndex], track);
                    continue;
                }

                BatchTrack track;
                track.id = nextTrackId++;
                track.firstFrame = objectFrame.index;
                track.lastFrame = objectFrame.index;
                track.observations = 1;
                track.lastBbox = backend::processing::science::resultBbox(objectFrame.validation);
                track.lastCentroid =
                    cv::Point2d(objectFrame.validation.centroidX, objectFrame.validation.centroidY);
                track.outputIndex = results.size();
                applyTrackState(objectFrame, track);
                attachSeriesImages(objectFrame, i);
                results.emplace_back(std::move(objectFrame));
                tracks.push_back(std::move(track));
                matchedThisFrame.push_back(true);
            }
        }
        if (progress) progress(BatchProgress{i + 1, total});
    }

    SPDLOG_INFO("processBatch: processed {} images into {} records across {} tracks (roi={}x{} at "
                "{},{}, background={})",
                total, results.size(), tracks.size(), roi.w, roi.h, roi.x, roi.y,
                !background.empty());
    return results;
}

bool ProcessingService::startBatchPipeline(BatchPipelineConfig config,
                                           BatchResultCallback callback) {
    std::unique_lock coreLock(processingKernelMutex_);
    if (batchRunning_.load(std::memory_order_acquire)) {
        SPDLOG_WARN("Batch pipeline already running");
        return false;
    }

    if (config.batchSize == 0) {
        config.batchSize = 1;
    }
    if (config.maxQueuedFrames == 0) {
        config.maxQueuedFrames = config.batchSize;
    }
    if (config.workerCount == 0) {
        config.workerCount = 1;
    }
    if (config.maxBatchDelayMs <= 0) {
        config.maxBatchDelayMs = 1;
    }

    {
        std::scoped_lock lk(batchMutex_);
        std::queue<QueuedBatchFrame> empty;
        batchQueue_.swap(empty);
        batchConfig_ = std::move(config);
        batchResultCallback_ = std::move(callback);
        batchFramesAccepted_.store(0, std::memory_order_relaxed);
        batchFramesDropped_.store(0, std::memory_order_relaxed);
        batchFramesProcessed_.store(0, std::memory_order_relaxed);
        batchBatchesProcessed_.store(0, std::memory_order_relaxed);
        batchAlgoMicrosTotal_.store(0, std::memory_order_relaxed);
        batchMaxQueueDepth_.store(0, std::memory_order_relaxed);
        batchQueueBytes_.reset();
        batchFramesDroppedByBytes_.store(0, std::memory_order_relaxed);
        batchWorkerCount_.store(batchConfig_.workerCount, std::memory_order_relaxed);
    }

    batchRunning_.store(true, std::memory_order_release);
    batchWorkers_.reserve(batchWorkerCount_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < batchWorkerCount_.load(std::memory_order_relaxed); ++i) {
        batchWorkers_.emplace_back(&ProcessingService::batchWorkerLoop, this);
    }

    SPDLOG_INFO("Batch pipeline started: batch_size={}, max_queue={}, workers={}, max_delay_ms={}",
                batchConfig_.batchSize, batchConfig_.maxQueuedFrames, batchConfig_.workerCount,
                batchConfig_.maxBatchDelayMs);
    return true;
}

void ProcessingService::stopBatchPipeline() {
    if (!batchRunning_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    batchCv_.notify_all();
    for (auto& worker : batchWorkers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    batchWorkers_.clear();

    {
        std::scoped_lock lk(batchMutex_);
        std::queue<QueuedBatchFrame> empty;
        batchQueue_.swap(empty);
        batchQueueBytes_.set(0, 0);
        batchResultCallback_ = {};
        batchWorkerCount_.store(0, std::memory_order_relaxed);
    }

    SPDLOG_INFO("Batch pipeline stopped: accepted={}, processed={}, dropped={}, batches={}",
                batchFramesAccepted_.load(std::memory_order_relaxed),
                batchFramesProcessed_.load(std::memory_order_relaxed),
                batchFramesDropped_.load(std::memory_order_relaxed),
                batchBatchesProcessed_.load(std::memory_order_relaxed));
}

bool ProcessingService::enqueueBatchFrame(const cv::Mat& grayImage, uint64_t index,
                                          uint64_t timestampNs, uint64_t hostTimestampUs) {
    if (!batchRunning_.load(std::memory_order_acquire) || grayImage.empty()) {
        return false;
    }

    cv::Mat gray;
    if (grayImage.type() == CV_8UC1) {
        gray = grayImage.clone();
    } else if (grayImage.channels() == 3) {
        cv::cvtColor(grayImage, gray, cv::COLOR_BGR2GRAY);
    } else {
        grayImage.convertTo(gray, CV_8UC1);
    }

    bool shouldNotify = false;
    {
        std::scoped_lock lk(batchMutex_);
        if (!batchRunning_.load(std::memory_order_relaxed)) {
            return false;
        }
        if (batchQueue_.size() >= batchConfig_.maxQueuedFrames) {
            batchFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t frameBytes = static_cast<uint64_t>(gray.total()) * gray.elemSize();
        if (batchConfig_.maxQueuedBytes > 0 &&
            batchQueueBytes_.bytes() + frameBytes > batchConfig_.maxQueuedBytes) {
            // Issue #370: the byte budget is a declared drop policy like the
            // frame cap; the caller sees the same accepted/dropped outcome.
            batchFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            batchFramesDroppedByBytes_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        batchQueue_.push(QueuedBatchFrame{std::move(gray), index, timestampNs, hostTimestampUs});
        batchQueueBytes_.add(frameBytes, 1);
        batchFramesAccepted_.fetch_add(1, std::memory_order_relaxed);

        const size_t depth = batchQueue_.size();
        size_t observed = batchMaxQueueDepth_.load(std::memory_order_relaxed);
        while (depth > observed && !batchMaxQueueDepth_.compare_exchange_weak(
                                       observed, depth, std::memory_order_relaxed)) {
        }

        shouldNotify = depth >= batchConfig_.batchSize;
    }

    if (shouldNotify) {
        batchCv_.notify_one();
    }
    return true;
}

bool ProcessingService::enqueueBatchFrame(const backend::playback::Frame& frame, uint64_t index) {
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) {
        return false;
    }

    cv::Mat gray = makeGrayCopy(frame);
    if (gray.empty()) {
        return false;
    }
    return enqueueBatchFrame(gray, index, frame.timestamp, frame.hostTimestampUs);
}

ProcessingService::BatchPipelineStats ProcessingService::getBatchPipelineStats() const {
    BatchPipelineStats stats;
    stats.framesAccepted = batchFramesAccepted_.load(std::memory_order_relaxed);
    stats.framesDropped = batchFramesDropped_.load(std::memory_order_relaxed);
    stats.framesDroppedByByteBudget = batchFramesDroppedByBytes_.load(std::memory_order_relaxed);
    stats.currentQueueBytes = batchQueueBytes_.bytes();
    stats.maxQueueBytes = batchQueueBytes_.peakBytes();
    stats.framesProcessed = batchFramesProcessed_.load(std::memory_order_relaxed);
    stats.batchesProcessed = batchBatchesProcessed_.load(std::memory_order_relaxed);
    stats.maxQueueDepth = batchMaxQueueDepth_.load(std::memory_order_relaxed);
    stats.workerCount = batchWorkerCount_.load(std::memory_order_relaxed);
    stats.running = batchRunning_.load(std::memory_order_acquire);

    std::scoped_lock lk(batchMutex_);
    stats.currentQueueDepth = batchQueue_.size();
    stats.batchSize = batchConfig_.batchSize;
    if (stats.workerCount == 0 && stats.running) {
        stats.workerCount = batchConfig_.workerCount;
    }
    return stats;
}

void ProcessingService::batchWorkerLoop() {
    auto operation = acquireProcessingCoreOperation();
    while (true) {
        std::vector<QueuedBatchFrame> inputs;
        BatchPipelineConfig config;
        BatchResultCallback callback;

        {
            std::unique_lock lk(batchMutex_);
            batchCv_.wait_for(
                lk, std::chrono::milliseconds(std::max(1, batchConfig_.maxBatchDelayMs)), [&] {
                    const size_t batchSize = std::max<size_t>(1, batchConfig_.batchSize);
                    return !batchRunning_.load(std::memory_order_acquire) ||
                           batchQueue_.size() >= batchSize;
                });

            if (batchQueue_.empty() && !batchRunning_.load(std::memory_order_acquire)) {
                break;
            }
            if (batchQueue_.empty()) {
                continue;
            }

            const size_t batchSize = std::max<size_t>(1, batchConfig_.batchSize);
            const size_t desired = std::min(batchQueue_.size(), batchSize);

            inputs.reserve(desired);
            for (size_t i = 0; i < desired && !batchQueue_.empty(); ++i) {
                inputs.emplace_back(std::move(batchQueue_.front()));
                batchQueue_.pop();
                const auto& g = inputs.back().gray;
                batchQueueBytes_.remove(static_cast<uint64_t>(g.total()) * g.elemSize(), 1);
            }
            config = batchConfig_;
            callback = batchResultCallback_;
            batchFramesInFlight_.fetch_add(static_cast<uint64_t>(inputs.size()),
                                           std::memory_order_acq_rel);
        }
        // Everything dequeued above is settled (callback returned or the
        // batch was dropped) before the counter goes back down;
        // endExperiment() drains on it.
        struct InFlightGuard {
            std::atomic<uint64_t>& counter;
            uint64_t n;
            ~InFlightGuard() { counter.fetch_sub(n, std::memory_order_acq_rel); }
        } inFlightGuard{batchFramesInFlight_, static_cast<uint64_t>(inputs.size())};

        const auto algoStart = std::chrono::steady_clock::now();
        std::vector<ProcessedFrame> results;
        results.reserve(inputs.size());
        try {
            for (const auto& item : inputs) {
                ProcessedFrame base =
                    computeProcessedFrame(item.gray, config.background, config.processing,
                                          config.roi, item.index, item.timestampNs);
                base.hostTimestampUs = item.hostTimestampUs;
                if (base.originalImage.empty() || base.processedImage.empty()) {
                    results.emplace_back(std::move(base));
                    continue;
                }

                Roi normalizedRoi = config.roi;
                if (normalizedRoi.w <= 0 || normalizedRoi.h <= 0) {
                    normalizedRoi.x = 0;
                    normalizedRoi.y = 0;
                    normalizedRoi.w = base.originalImage.cols;
                    normalizedRoi.h = base.originalImage.rows;
                }
                normalizedRoi.x =
                    std::max(0, std::min(normalizedRoi.x, base.originalImage.cols - 1));
                normalizedRoi.y =
                    std::max(0, std::min(normalizedRoi.y, base.originalImage.rows - 1));
                normalizedRoi.w = std::max(
                    1, std::min(normalizedRoi.w, base.originalImage.cols - normalizedRoi.x));
                normalizedRoi.h = std::max(
                    1, std::min(normalizedRoi.h, base.originalImage.rows - normalizedRoi.y));
                const cv::Rect cvRoi(normalizedRoi.x, normalizedRoi.y, normalizedRoi.w,
                                     normalizedRoi.h);

                auto objectResults = filterProcessedObjects(base.processedImage, cvRoi,
                                                            config.processing, base.originalImage);
                if (objectResults.empty()) {
                    results.emplace_back(std::move(base));
                    continue;
                }

                for (auto& validation : objectResults) {
                    ProcessedFrame objectFrame;
                    objectFrame.index = base.index;
                    objectFrame.timestampNs = base.timestampNs;
                    objectFrame.hostTimestampUs = base.hostTimestampUs;
                    objectFrame.originalImage = base.originalImage;   // shared, read-only (issue #370)
                    objectFrame.processedImage = base.processedImage; // shared, read-only
                    objectFrame.validation = std::move(validation);
                    results.emplace_back(std::move(objectFrame));
                }
            }
            const auto algoEnd = std::chrono::steady_clock::now();
            const auto algoMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(algoEnd - algoStart).count();
            if (algoMicros > 0) {
                batchAlgoMicrosTotal_.fetch_add(static_cast<uint64_t>(algoMicros),
                                                std::memory_order_relaxed);
            }

            if (!results.empty()) {
                batchFramesProcessed_.fetch_add(static_cast<uint64_t>(inputs.size()),
                                                std::memory_order_relaxed);
                batchBatchesProcessed_.fetch_add(1, std::memory_order_relaxed);
                if (callback) {
                    callback(std::move(results));
                }
            }
        } catch (const std::exception& ex) {
            SPDLOG_ERROR(
                "ProcessingService: batch worker exception: {} — batch of {} frames dropped",
                ex.what(), inputs.size());
        } catch (...) {
            SPDLOG_ERROR(
                "ProcessingService: batch worker unknown exception — batch of {} frames dropped",
                inputs.size());
        }
    }
}

void ProcessingService::setRingRatioCallback(RingRatioCallback callback) {
    std::scoped_lock lk(ringRatioCallbackMutex_);
    ringRatioCallback_ = std::move(callback);
}

void ProcessingService::setTargetGroupCallback(TargetGroupCallback callback) {
    std::scoped_lock lk(targetGroupCallbackMutex_);
    targetGroupCallback_ = std::move(callback);
}

void ProcessingService::setBackgroundCaptureCallback(BackgroundCaptureCallback callback) {
    std::scoped_lock lk(backgroundCaptureCallbackMutex_);
    backgroundCaptureCallback_ = std::move(callback);
}

void ProcessingService::logDroppedExperimentFrames(const DroppedFrameCounts& dropped,
                                                   size_t bufferedTotal, size_t maxBufferedFrames) {
    if (dropped.valid == 0 && dropped.invalid == 0) {
        return;
    }

    const uint64_t nowUs = backend::Tools::getTimestamp();
    uint64_t lastUs = lastDropLogUs_.load(std::memory_order_relaxed);
    if (nowUs - lastUs < 1'000'000ULL ||
        !lastDropLogUs_.compare_exchange_strong(lastUs, nowUs, std::memory_order_relaxed)) {
        return;
    }

    SPDLOG_WARN("Experiment frame backlog capped: dropped valid={}, invalid={} "
                "(buffered={}, max={}, total_dropped_valid={}, total_dropped_invalid={})",
                dropped.valid, dropped.invalid, bufferedTotal, maxBufferedFrames,
                droppedValidFrames_.load(std::memory_order_relaxed),
                droppedInvalidFrames_.load(std::memory_order_relaxed));
}

bool ProcessingService::appendExperimentFrame(ProcessedFrame&& frame, bool isValid) {
    // Issue #370: the bounded buffer owns the retention policy (frame cap AND
    // byte budget; sampled invalid evicted first, a valid frame is refused
    // only when the backlog is entirely valid and still over the bound).
    const size_t maxBufferedFrames =
        std::max<size_t>(1, maxBufferedFrames_.load(std::memory_order_relaxed));
    const auto r = experimentBuffer_.append(std::move(frame), isValid);
    framesSinceLastFlush_.store(r.bufferedAfter, std::memory_order_relaxed);
    if (r.droppedValid > 0) {
        droppedValidFrames_.fetch_add(static_cast<uint64_t>(r.droppedValid), std::memory_order_relaxed);
    }
    if (r.droppedInvalid > 0) {
        droppedInvalidFrames_.fetch_add(static_cast<uint64_t>(r.droppedInvalid), std::memory_order_relaxed);
    }

    // Issue #367: every call is a persistence admission; frames evicted by
    // the bounded-buffer policy (including this one when not stored) are
    // CancelledByPolicy on the persistence side — never silent.
    experimentAccounting_.persistenceAdmitted.fetch_add(1, std::memory_order_relaxed);
    const uint64_t evicted = static_cast<uint64_t>(r.dropped());
    if (evicted > 0) {
        experimentAccounting_.persistenceCancelledByPolicy.fetch_add(evicted, std::memory_order_relaxed);
    }
    logDroppedExperimentFrames(DroppedFrameCounts{r.droppedValid, r.droppedInvalid}, r.bufferedAfter,
                               maxBufferedFrames);
    return r.stored;
}

size_t ProcessingService::flushBufferedFrames(class Hdf5Service& hdf5) {
    // Move the accumulated frames out (brief lock) so capture/processing never
    // blocks on the HDF5 write, then hand them to the write queue. The queue's
    // dedicated writer thread performs the slow append; capture keeps filling a
    // fresh buffer. Overflow or a write failure is fatal (stop + surface) rather
    // than a silent trim-and-drop.
    ExperimentBatch batch;
    if (experimentBuffer_.empty()) return 0;
    // Move out — cv::Mat moves are O(1) refcount transfers.
    experimentBuffer_.takeAll(batch.valid, batch.invalid);
    framesSinceLastFlush_.store(0, std::memory_order_relaxed);
    const size_t n = batch.valid.size() + batch.invalid.size();
    if (n == 0) return 0;
    uint64_t batchBytes = 0;
    for (const auto& f : batch.valid) batchBytes += processedFrameBytes(f);
    for (const auto& f : batch.invalid) batchBytes += processedFrameBytes(f);

    std::scoped_lock qlk(flushQueueMutex_);
    if (!flushQueue_) {
        Hdf5Service* h = &hdf5;
        auto writeFn = [this, h](const ExperimentBatch& b) -> bool {
            const bool ok = h->appendFrames(b.valid, b.invalid);
            uint64_t bytes = 0;
            for (const auto& f : b.valid) bytes += processedFrameBytes(f);
            for (const auto& f : b.invalid) bytes += processedFrameBytes(f);
            flushQueueBytes_.remove(bytes, 1);
            if (!ok) return false;
            experimentAccounting_.persistenceCommitted.fetch_add(
                static_cast<uint64_t>(b.valid.size() + b.invalid.size()), std::memory_order_relaxed);
            if (!b.valid.empty()) {
                totalValidFlushed_.fetch_add(static_cast<uint64_t>(b.valid.size()),
                                             std::memory_order_relaxed);
            }
            if (!b.invalid.empty()) {
                totalInvalidFlushed_.fetch_add(static_cast<uint64_t>(b.invalid.size()),
                                               std::memory_order_relaxed);
            }
            return true;
        };
        auto onError = [this](const std::string& msg) {
            if (flushErrorCb_) flushErrorCb_("Experiment save failed: " + msg);
        };
        flushQueue_ = std::make_unique<backend::recording::HdfWriteQueue<ExperimentBatch>>(
            3, writeFn, onError);
    }
    flushQueueBytes_.add(batchBytes, 1);
    if (!flushQueue_->submit(std::move(batch))) {
        flushQueueBytes_.remove(batchBytes, 1);
        return 0; // fatal error already surfaced via onError
    }
    return n;
}

bool ProcessingService::finishFlush() {
    std::unique_ptr<backend::recording::HdfWriteQueue<ExperimentBatch>> q;
    {
        std::scoped_lock qlk(flushQueueMutex_);
        q = std::move(flushQueue_);
    }
    if (!q) return true;
    return q->flushAndStop(); // drains remaining batches, then joins
}

void ProcessingService::setFlushErrorCallback(std::function<void(const std::string&)> cb) {
    flushErrorCb_ = std::move(cb);
}

void ProcessingService::setFlushInterval(size_t frames) {
    if (frames == 0) frames = 1; // Minimum 1
    flushInterval_.store(frames);
    const size_t maxBuffered = defaultMaxBufferedFrames(frames);
    maxBufferedFrames_.store(maxBuffered, std::memory_order_relaxed);
    experimentBuffer_.setPolicy({maxBuffered, maxBufferedBytes_.load(std::memory_order_relaxed)});
    SPDLOG_INFO("Flush interval set to: {} frames (max buffered backlog: {})", frames, maxBuffered);
}

size_t ProcessingService::getFlushInterval() const {
    return flushInterval_.load();
}

size_t ProcessingService::getMaxBufferedFrames() const {
    return maxBufferedFrames_.load(std::memory_order_relaxed);
}

void ProcessingService::setInvalidFrameSamplingRate(size_t rate) {
    if (rate == 0) rate = 1; // Minimum 1 (save all)
    invalidFrameSamplingRate_.store(rate);
    SPDLOG_INFO("Invalid frame sampling rate set to: every {}th frame", rate);
}

size_t ProcessingService::getInvalidFrameSamplingRate() const {
    return invalidFrameSamplingRate_.load();
}

std::vector<FilterResult> ProcessingService::filterProcessedObjects(const cv::Mat& processedImage,
                                                                    const cv::Rect& roi,
                                                                    const ProcessingConfig& config,
                                                                    const cv::Mat& originalImage) {
    // Version-sensitive science is owned by the selected kernel (A7). The
    // caller already holds a CoreOperationLease, so the kernel cannot swap
    // between the mask call and this analysis call.
    const double pixelToMicronFactor = pixelToMicronFactor_.load(std::memory_order_relaxed);
    const backend::EModulusLut* eModulusLut = eModulusLut_.isLoaded() ? &eModulusLut_ : nullptr;
    std::shared_ptr<backend::processing::IProcessingKernel> kernel;
    {
        std::shared_lock lock(processingKernelMutex_);
        kernel = processingKernel_;
    }
    std::vector<FilterResult> results;
    std::string error;
    if (!kernel || !kernel->analyzeObjects(processedImage, roi, config, originalImage,
                                           pixelToMicronFactor, eModulusLut, results, &error)) {
        SPDLOG_ERROR("filterProcessedObjects: kernel object analysis failed: {}", error);
        return {};
    }
    return results;
}

FilterResult ProcessingService::filterProcessedImage(const cv::Mat& processedImage,
                                                     const cv::Rect& roi,
                                                     const ProcessingConfig& config,
                                                     const cv::Mat& originalImage) {
    auto results = filterProcessedObjects(processedImage, roi, config, originalImage);
    if (results.empty()) {
        return {};
    }
    return std::move(results.front());
}

int ProcessingService::matchTrackWithActiveKernel(const std::vector<BatchTrack>& tracks,
                                                  const std::vector<bool>& matchedThisFrame,
                                                  const FilterResult& detection,
                                                  uint64_t frameIndex, int frameWidth) const {
    std::shared_ptr<backend::processing::IProcessingKernel> kernel;
    {
        std::shared_lock lock(processingKernelMutex_);
        kernel = processingKernel_;
    }
    int matchedTrack = -1;
    std::string error;
    if (!kernel || !kernel->matchTrack(tracks, matchedThisFrame, detection, frameIndex, frameWidth,
                                       matchedTrack, &error)) {
        SPDLOG_ERROR("matchTrackWithActiveKernel: kernel track matching failed: {}", error);
        return -1;
    }
    return matchedTrack;
}

ProcessingService::BatchPipelineConfig ProcessingService::makeRealtimeBatchPipelineConfig() const {
    BatchPipelineConfig config;
    {
        std::scoped_lock settingsLk(rtBatchSettingsMutex_);
        config.batchSize = std::max<size_t>(1, rtBatchSettings_.batchSize);
        config.maxQueuedFrames = std::max(config.batchSize, rtBatchSettings_.maxQueuedFrames);
        config.workerCount = std::max<size_t>(1, rtBatchSettings_.workerCount);
        config.maxBatchDelayMs = std::max(1, rtBatchSettings_.maxBatchDelayMs);
        config.maxQueuedBytes = rtBatchSettings_.maxQueuedBytes;
    }
    {
        std::scoped_lock cfgLk(configMutex_);
        config.processing = processingConfig_;
    }
    {
        std::scoped_lock rtLk(rtMutex_);
        config.roi = rtRoi_;
        if (rtBgGray_ && !rtBgGray_->empty()) {
            config.background = rtBgGray_->clone();
        }
    }
    return config;
}

void ProcessingService::refreshRealtimeBatchPipelineConfig() {
    if (!rtBatchPipelineActive_.load(std::memory_order_acquire)) {
        return;
    }

    BatchPipelineConfig fresh = makeRealtimeBatchPipelineConfig();
    std::scoped_lock lk(batchMutex_);
    if (!rtBatchPipelineActive_.load(std::memory_order_relaxed)) {
        return;
    }
    batchConfig_.batchSize = fresh.batchSize;
    batchConfig_.maxQueuedFrames = fresh.maxQueuedFrames;
    batchConfig_.maxQueuedBytes = fresh.maxQueuedBytes;
    batchConfig_.maxBatchDelayMs = fresh.maxBatchDelayMs;
    batchConfig_.processing = fresh.processing;
    batchConfig_.background = std::move(fresh.background);
    batchConfig_.roi = fresh.roi;
}

TargetGroupEvent ProcessingService::selectTargetGroupTriggerOwner(
    const std::vector<FilterResult>& validations) const {
    for (const auto& validation : validations) {
        if (!validation.isValid || !validation.isTargetGroup) {
            continue;
        }
        return {true, validation.objectId, validation.trackId};
    }
    return {};
}

void ProcessingService::publishRealtimeValidationCallbacks(
    const std::vector<FilterResult>& validations, uint64_t timestampNs,
    const RealtimeFrameTiming& timing) {
    // Latency-critical ordering invariant: the target-group (trigger) callback
    // fires FIRST and no heavy lock is taken before it (see
    // knowledge_map/task/2026-04-15-trigger-timing-bug.md). Timing capture
    // below is lock-free and gated to a relaxed atomic load when disabled.
    auto& timingRecorder = backend::diagnostics::PipelineTimingRecorder::instance();
    const bool recordTiming = timing.present && timingRecorder.isEnabled();
    uint64_t triggerDispatchUs = 0;

    auto targetOwner = selectTargetGroupTriggerOwner(validations);
    if (targetOwner.isTargetGroup) {
        targetOwner.frameIndex = timing.frameIndex;
        targetOwner.hostTimestampUs = timing.grabUs;
        TargetGroupCallback tgCb;
        {
            std::scoped_lock cbLk(targetGroupCallbackMutex_);
            tgCb = targetGroupCallback_;
        }
        if (tgCb) tgCb(targetOwner);
        if (recordTiming) {
            triggerDispatchUs = backend::diagnostics::PipelineTimingRecorder::nowUs();
        }
    }

    // Hoist the callback copy out of the per-object loop: one mutex-guarded
    // std::function copy per frame, not per validation object (P7).
    RingRatioCallback rrCb;
    {
        std::scoped_lock cbLk(ringRatioCallbackMutex_);
        rrCb = ringRatioCallback_;
    }
    if (rrCb) {
        for (const auto& validation : validations) {
            if (!validation.isValid || validation.ringRatio <= 0.0) {
                continue;
            }
            rrCb(validation.ringRatio, static_cast<int64_t>(timestampNs));
        }
    }

    if (recordTiming) {
        backend::diagnostics::FrameTimingRecord record;
        record.frameIndex = timing.frameIndex;
        record.deviceTimestamp = timestampNs;
        record.grabUs = timing.grabUs;
        record.algoStartUs = timing.algoStartUs;
        record.algoEndUs = timing.algoEndUs;
        record.triggerDispatchUs = triggerDispatchUs;
        record.callbacksDoneUs = backend::diagnostics::PipelineTimingRecorder::nowUs();
        for (const auto& validation : validations) {
            if (validation.isValid) {
                ++record.validCount;
            } else {
                ++record.invalidCount;
            }
        }
        record.isTargetGroup = targetOwner.isTargetGroup ? 1 : 0;
        timingRecorder.recordFrame(record);
    }
}

void ProcessingService::accumulateIdentificationCounters(
    const std::vector<FilterResult>& validations, const ProcessingConfig& config,
    double pixelToMicronFactor) {
    namespace science = backend::processing::science;
    static_assert(science::kInvalidReasonCount == 6,
                  "idReasonCounts_ / IdentificationCounters.reasonCounts size must match "
                  "science::kInvalidReasonCount");

    idFramesProcessed_.fetch_add(1, std::memory_order_relaxed);

    bool anyObject = false;
    uint64_t targetGroupThisFrame = 0;
    for (const auto& v : validations) {
        // filterProcessedObjects emits at least one (possibly empty) result per
        // frame; a real detection has a contour. objectCount > 0 marks frames
        // that actually contained something to classify.
        if (v.objectCount > 0 || v.innerContourCount > 0) {
            anyObject = true;
        }
        if (v.isValid) {
            idValidObjects_.fetch_add(1, std::memory_order_relaxed);
            if (v.isTargetGroup) {
                ++targetGroupThisFrame;
            }
        } else {
            idInvalidObjects_.fetch_add(1, std::memory_order_relaxed);
            for (auto reason : science::classifyInvalidReasons(v, config, pixelToMicronFactor)) {
                idReasonCounts_[static_cast<size_t>(reason)].fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
        }
    }

    if (anyObject) {
        idFramesWithObjects_.fetch_add(1, std::memory_order_relaxed);
    }
    if (targetGroupThisFrame > 0) {
        idTargetGroupObjects_.fetch_add(targetGroupThisFrame, std::memory_order_relaxed);
        // selectTargetGroupTriggerOwner dispatches only the first target-group
        // object per frame; the rest are identified sort targets that never get
        // a pulse. Count them as an identification loss.
        idUnservedTargetGroupObjects_.fetch_add(targetGroupThisFrame - 1,
                                                std::memory_order_relaxed);
    }

    // Mirror the headline loss values so crash reports carry live state (cheap
    // relaxed stores; off the trigger-critical path).
    auto& mirror = backend::diagnostics::CrashStateMirror::instance().processing;
    mirror.targetGroupObjects.store(idTargetGroupObjects_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    mirror.unservedTargetGroupObjects.store(
        idUnservedTargetGroupObjects_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mirror.droppedValidFrames.store(droppedValidFrames_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
}

ProcessingService::IdentificationCounters ProcessingService::getIdentificationCounters() const {
    IdentificationCounters c;
    c.framesProcessed = idFramesProcessed_.load(std::memory_order_relaxed);
    c.framesWithObjects = idFramesWithObjects_.load(std::memory_order_relaxed);
    c.validObjects = idValidObjects_.load(std::memory_order_relaxed);
    c.invalidObjects = idInvalidObjects_.load(std::memory_order_relaxed);
    c.targetGroupObjects = idTargetGroupObjects_.load(std::memory_order_relaxed);
    c.unservedTargetGroupObjects = idUnservedTargetGroupObjects_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 6; ++i) {
        c.reasonCounts[i] = idReasonCounts_[i].load(std::memory_order_relaxed);
    }
    return c;
}

void ProcessingService::resetIdentificationCounters() {
    idFramesProcessed_.store(0, std::memory_order_relaxed);
    idFramesWithObjects_.store(0, std::memory_order_relaxed);
    idValidObjects_.store(0, std::memory_order_relaxed);
    idInvalidObjects_.store(0, std::memory_order_relaxed);
    idTargetGroupObjects_.store(0, std::memory_order_relaxed);
    idUnservedTargetGroupObjects_.store(0, std::memory_order_relaxed);
    for (auto& r : idReasonCounts_) {
        r.store(0, std::memory_order_relaxed);
    }
}

void ProcessingService::appendRealtimeMonitoringFrame(uint64_t index, uint64_t timestampNs,
                                                      const FilterResult& validation,
                                                      const cv::Mat& originalImage,
                                                      const cv::Mat& processedImage) {
    if (!monitoringActive_.load(std::memory_order_relaxed)) return; // gating: no-op when inactive
    if (originalImage.empty() || processedImage.empty()) {
        return;
    }

    ProcessedFrame monitoringFrame;
    monitoringFrame.index = index;
    monitoringFrame.timestampNs = timestampNs;
    monitoringFrame.validation = validation;
    monitoringFrame.originalImage = originalImage; // shallow refcount share (frozen-mats invariant)
    monitoringFrame.processedImage = processedImage; // shallow refcount share

    std::scoped_lock monitoringLk(monitoringFramesMutex_);
    if (validation.isValid) {
        monitoringValidAppended_.fetch_add(1, std::memory_order_relaxed);
        monitoringValidFrames_.push_back(std::move(monitoringFrame));
    } else {
        monitoringInvalidAppended_.fetch_add(1, std::memory_order_relaxed);
        monitoringInvalidFrames_.push_back(std::move(monitoringFrame));
    }
}

void ProcessingService::publishRealtimeBatchFrame(ProcessedFrame&& frame) {
    if (frame.originalImage.empty() || frame.processedImage.empty()) {
        return;
    }

    const uint64_t frameIndex = frame.index;
    const FilterResult validation = frame.validation;

    // Monitoring (gated — skip when no consumer is active; ROI clones outside lock)
    if (monitoringActive_.load(std::memory_order_relaxed)) {
        Roi roi = getRealtimeRoi();
        if (roi.w <= 0 || roi.h <= 0) {
            roi.x = 0;
            roi.y = 0;
            roi.w = frame.originalImage.cols;
            roi.h = frame.originalImage.rows;
        }
        roi.x = std::max(0, std::min(roi.x, frame.originalImage.cols - 1));
        roi.y = std::max(0, std::min(roi.y, frame.originalImage.rows - 1));
        roi.w = std::max(1, std::min(roi.w, frame.originalImage.cols - roi.x));
        roi.h = std::max(1, std::min(roi.h, frame.originalImage.rows - roi.y));
        const cv::Rect cvRoi(roi.x, roi.y, roi.w, roi.h);

        ProcessedFrame monitoringFrame;
        monitoringFrame.index = frameIndex;
        monitoringFrame.timestampNs = frame.timestampNs;
        monitoringFrame.validation = validation;
        monitoringFrame.originalImage = frame.originalImage(cvRoi).clone();
        monitoringFrame.processedImage = frame.processedImage(cvRoi).clone();

        std::scoped_lock monitoringLk(monitoringFramesMutex_);
        if (validation.isValid) {
            monitoringValidFrames_.push_back(std::move(monitoringFrame));
        } else {
            monitoringInvalidFrames_.push_back(std::move(monitoringFrame));
        }
    }

    // Publish snapshot: build outside lock, pointer-swap inside (no full-frame copy under mutex)
    {
        auto newSnap = std::make_shared<RealtimeSnapshot>();
        newSnap->index = frameIndex;
        newSnap->mask = frame.processedImage; // shallow refcount share (frozen-mats invariant)
        newSnap->contours = validation.allContours ? *validation.allContours
                                                   : std::vector<std::vector<cv::Point>>{};
        newSnap->validation = validation;
        std::scoped_lock snapshotLk(snapshotMutex_);
        latestSnapshot_ = std::move(newSnap); // O(1) pointer swap inside lock
    }

    uint64_t observed = rtLastProcessed_.load(std::memory_order_relaxed);
    while (frameIndex > observed && !rtLastProcessed_.compare_exchange_weak(
                                        observed, frameIndex, std::memory_order_relaxed)) {
    }

    // Persist iff this frame's admission was counted under the run (not
    // "iff active"): frames still in the batch pipeline when endExperiment()
    // runs are drained there and reach the buffer for the remainder flush.
    if (!experimentSettled_.load(std::memory_order_acquire) && experimentAccounting_.wasAdmitted(frameIndex)) {
        bool shouldSave = validation.isValid;
        if (!validation.isValid) {
            const size_t counter = invalidFrameCounter_.fetch_add(1, std::memory_order_relaxed);
            const size_t rate = invalidFrameSamplingRate_.load(std::memory_order_relaxed);
            shouldSave = rate > 0 && (counter % rate) == 0;
        }
        if (shouldSave) {
            appendExperimentFrame(std::move(frame), validation.isValid);
        }
    }
}

void ProcessingService::realtimeBatchLoop() {
    rtLastProcessed_.store(0);

    std::atomic<uint64_t> callbackValid{0};
    std::atomic<uint64_t> callbackInvalid{0};

    const BatchPipelineConfig initialConfig = makeRealtimeBatchPipelineConfig();
    rtBatchPipelineActive_.store(true, std::memory_order_release);
    const bool started = startBatchPipeline(initialConfig, [this, &callbackValid, &callbackInvalid](
                                                               std::vector<ProcessedFrame> batch) {
        std::vector<FilterResult> frameValidations;
        uint64_t lastFrameIndex = 0;
        uint64_t lastFrameTimestamp = 0;
        uint64_t lastFrameHostUs = 0;
        bool hasPendingFrame = false;

        // Async-batch mode has no per-frame algo stamps (batch timing is
        // aggregate), so the timing record carries frame identity only.
        const auto makeTiming = [](uint64_t frameIndex, uint64_t hostUs) {
            RealtimeFrameTiming timing;
            timing.present = true;
            timing.frameIndex = frameIndex;
            timing.grabUs = hostUs;
            return timing;
        };

        // Experiment accounting (issue #367): one outcome per admitted frame
        // index. A frame whose images came back empty failed in the core
        // (computeProcessedFrame) and is ProcessingFailed; every other frame
        // terminates through its validations (Processed / Rejected).
        const auto settleFrame = [this, &frameValidations, &lastFrameIndex] {
            if (!frameValidations.empty()) {
                noteRealtimeValidation(lastFrameIndex, frameValidations);
            }
        };
        for (auto& frame : batch) {
            if (frame.originalImage.empty() || frame.processedImage.empty()) {
                if (hasPendingFrame && frame.index != lastFrameIndex) {
                    publishRealtimeValidationCallbacks(frameValidations, lastFrameTimestamp,
                                                       makeTiming(lastFrameIndex, lastFrameHostUs));
                    settleFrame();
                    frameValidations.clear();
                    hasPendingFrame = false;
                }
                noteRealtimeOutcome(frame.index, backend::recording::FrameOutcome::ProcessingFailed);
                continue;
            }
            if (frame.validation.isValid) {
                callbackValid.fetch_add(1, std::memory_order_relaxed);
            } else {
                callbackInvalid.fetch_add(1, std::memory_order_relaxed);
            }
            if (!hasPendingFrame) {
                lastFrameIndex = frame.index;
                lastFrameTimestamp = frame.timestampNs;
                lastFrameHostUs = frame.hostTimestampUs;
                hasPendingFrame = true;
            } else if (frame.index != lastFrameIndex || frame.timestampNs != lastFrameTimestamp) {
                publishRealtimeValidationCallbacks(frameValidations, lastFrameTimestamp,
                                                   makeTiming(lastFrameIndex, lastFrameHostUs));
                settleFrame();
                frameValidations.clear();
                lastFrameIndex = frame.index;
                lastFrameTimestamp = frame.timestampNs;
                lastFrameHostUs = frame.hostTimestampUs;
            }

            frameValidations.push_back(frame.validation);
            publishRealtimeBatchFrame(std::move(frame));
        }

        if (hasPendingFrame && !frameValidations.empty()) {
            publishRealtimeValidationCallbacks(frameValidations, lastFrameTimestamp,
                                               makeTiming(lastFrameIndex, lastFrameHostUs));
            settleFrame();
        }
    });
    if (!started) {
        rtBatchPipelineActive_.store(false, std::memory_order_release);
        rtRunning_.store(false, std::memory_order_release);
        backend::diagnostics::CrashStateMirror::instance().processing.realtimeRunning.store(false);
        SPDLOG_ERROR("ProcessingService: async realtime batch mode could not start");
        return;
    }

    using clock = std::chrono::steady_clock;
    auto lastSummaryTs = clock::now();
    uint64_t queuedSinceSummary = 0;
    uint64_t skippedSinceSummary = 0;
    double enqueueMsSinceSummary = 0.0;
    uint64_t lastProcessedTotal = 0;
    uint64_t lastAlgoMicrosTotal = 0;

    SPDLOG_INFO("ProcessingService: realtime async batch loop started");
    if (initialConfig.processing.auto_background_enabled) {
        SPDLOG_WARN(
            "ProcessingService: async batch realtime mode does not run inline auto-background "
            "capture; use frame-by-frame mode when auto-background capture is required");
    }
    if (initialConfig.processing.multi_image_enabled &&
        initialConfig.processing.multi_image_count > 1) {
        SPDLOG_WARN("ProcessingService: async batch realtime mode records trigger frames only; use "
                    "frame-by-frame mode for multi-image series capture");
    }

    while (rtRunning_.load(std::memory_order_acquire) &&
           getRealtimeProcessingMode() == RealtimeProcessingMode::AsyncBatch) {
        if (!rtStore_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const uint64_t total = rtStore_->totalWritten();
        if (total == 0) {
            rtStore_->waitForFrame(0, std::chrono::milliseconds(5));
            continue;
        }

        const uint64_t earliest = rtStore_->earliestAvailableIndex();
        const uint64_t latest = rtStore_->latestAvailableIndex();
        uint64_t last = rtLastProcessed_.load(std::memory_order_relaxed);
        if (last > latest) {
            // FrameStore::resize() renumbers frames from 0; a cached pointer
            // from the old numbering would idle this loop forever.
            SPDLOG_WARN(
                "Async batch realtime pointer {} beyond latest {} (store resized?); resyncing",
                last, latest);
            last = latest;
            rtLastProcessed_.store(last, std::memory_order_relaxed);
        }
        if (last + 1 < earliest) {
            const uint64_t skipped = earliest - (last + 1);
            skippedSinceSummary += skipped;
            backend::diagnostics::PipelineTimingRecorder::instance().countSkipped(
                backend::diagnostics::PipelineSkipReason::RingBehind, skipped);
            noteRealtimeLost(skipped); // experiment accounting: StoreOverwritten
            last = earliest - 1;
            rtLastProcessed_.store(last, std::memory_order_relaxed);
            SPDLOG_DEBUG(
                "Async batch realtime fell behind, skipping {} frames (last={}, earliest={})",
                skipped, last, earliest);
        }

        if (last >= latest) {
            // Event-driven wake instead of sleep-polling (issue #282); see
            // the matching comment in realtimeInlineLoop.
            rtStore_->waitForFrame(total, std::chrono::milliseconds(2));
        } else {
            const bool dropFrames = rtDropFrames_.load(std::memory_order_relaxed) &&
                                    !experimentActive_.load(std::memory_order_relaxed);
            const uint64_t firstIdx = dropFrames ? latest : last + 1;
            // Count the dropped-to-latest range only once an iteration
            // actually advances rtLastProcessed_ past it (same rule as the
            // inline loop): a failed slot fetch leaves `last` unchanged and
            // retries, and counting up front would tally the range twice.
            const uint64_t droppedToLatest =
                (dropFrames && last + 1 < firstIdx) ? firstIdx - (last + 1) : 0;
            bool droppedCounted = false;
            auto countDroppedToLatest = [&] {
                if (droppedToLatest > 0 && !droppedCounted) {
                    droppedCounted = true;
                    skippedSinceSummary += droppedToLatest;
                    backend::diagnostics::PipelineTimingRecorder::instance().countSkipped(
                        backend::diagnostics::PipelineSkipReason::DroppedToLatest, droppedToLatest);
                }
            };

            for (uint64_t idx = firstIdx;
                 idx <= latest && rtRunning_.load(std::memory_order_acquire); ++idx) {
                const auto enqueueStart = clock::now();
                if (!rtEnabled_.load(std::memory_order_relaxed)) {
                    countDroppedToLatest();
                    rtLastProcessed_.store(idx, std::memory_order_relaxed);
                    continue;
                }

                backend::playback::Frame frame;
                if (!rtStore_->getByWriteIndex(idx, frame)) {
                    continue; // not counted — retried from the same `last`
                }
                countDroppedToLatest();
                // Experiment accounting (issue #367): a frame is admitted when
                // it is handed to the batch queue; a rejected frame is a
                // processing-side loss and terminates immediately. Outcomes
                // for accepted frames are counted when the batch callback
                // publishes them (same rule as the inline loop).
                noteRealtimeAdmitted(idx);
                const bool accepted = enqueueBatchFrame(frame, idx);
                if (accepted) {
                    ++queuedSinceSummary;
                } else {
                    ++skippedSinceSummary;
                    backend::diagnostics::PipelineTimingRecorder::instance().countSkipped(
                        backend::diagnostics::PipelineSkipReason::BatchQueueRejected);
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                }
                rtLastProcessed_.store(idx, std::memory_order_relaxed);

                const auto enqueueEnd = clock::now();
                enqueueMsSinceSummary +=
                    std::chrono::duration<double, std::milli>(enqueueEnd - enqueueStart).count();
            }
        }

        const auto now = clock::now();
        const double windowMs =
            std::chrono::duration<double, std::milli>(now - lastSummaryTs).count();
        if (windowMs >= 1000.0) {
            const uint64_t processedTotal = batchFramesProcessed_.load(std::memory_order_relaxed);
            const uint64_t processedSinceSummary = processedTotal - lastProcessedTotal;
            lastProcessedTotal = processedTotal;

            const uint64_t algoMicrosTotal = batchAlgoMicrosTotal_.load(std::memory_order_relaxed);
            const uint64_t algoMicrosSinceSummary = algoMicrosTotal - lastAlgoMicrosTotal;
            lastAlgoMicrosTotal = algoMicrosTotal;

            const uint64_t validSinceSummary = callbackValid.exchange(0, std::memory_order_relaxed);
            const uint64_t invalidSinceSummary =
                callbackInvalid.exchange(0, std::memory_order_relaxed);
            const double fps =
                windowMs > 0.0 ? (static_cast<double>(processedSinceSummary) * 1000.0 / windowMs)
                               : 0.0;
            const double vfps =
                windowMs > 0.0 ? (static_cast<double>(validSinceSummary) * 1000.0 / windowMs) : 0.0;
            const double ifps = windowMs > 0.0
                                    ? (static_cast<double>(invalidSinceSummary) * 1000.0 / windowMs)
                                    : 0.0;
            const double algoAvgUs = processedSinceSummary > 0
                                         ? static_cast<double>(algoMicrosSinceSummary) /
                                               static_cast<double>(processedSinceSummary)
                                         : 0.0;
            algoFps1s_.store(fps, std::memory_order_relaxed);
            validFps1s_.store(vfps, std::memory_order_relaxed);
            invalidFps1s_.store(ifps, std::memory_order_relaxed);
            algoAvgUs1s_.store(algoAvgUs, std::memory_order_relaxed);
            algoAvgUs1sUpdatedUs_.store(backend::Tools::getTimestamp(), std::memory_order_relaxed);

            const auto stats = getBatchPipelineStats();
            SPDLOG_DEBUG("Realtime async batch summary: queued={} processed={} skipped={} "
                         "dropped={} queue={} max_queue={} "
                         "window_ms={:.0f} enqueue_avg_ms={:.3f} algo_avg_us={:.1f} fps={:.1f}",
                         queuedSinceSummary, processedSinceSummary, skippedSinceSummary,
                         stats.framesDropped, stats.currentQueueDepth, stats.maxQueueDepth,
                         windowMs,
                         queuedSinceSummary > 0
                             ? enqueueMsSinceSummary / static_cast<double>(queuedSinceSummary)
                             : 0.0,
                         algoAvgUs, fps);

            lastSummaryTs = now;
            queuedSinceSummary = 0;
            skippedSinceSummary = 0;
            enqueueMsSinceSummary = 0.0;
        }
    }

    rtBatchPipelineActive_.store(false, std::memory_order_release);
    stopBatchPipeline();
    SPDLOG_INFO("ProcessingService: realtime async batch loop stopped");
}

void ProcessingService::realtimeLoop() {
    auto operation = acquireProcessingCoreOperation();
    // An exception escaping a std::thread entry function is std::terminate —
    // the whole process dies on one bad frame. Catch, log, and restart the
    // loop instead (same policy as CaptureService::run).
    while (rtRunning_.load(std::memory_order_acquire)) {
        try {
            if (getRealtimeProcessingMode() == RealtimeProcessingMode::AsyncBatch) {
                realtimeBatchLoop();
            } else {
                realtimeInlineLoop();
            }
            break; // normal exit (stopRealtime or mode switch)
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("ProcessingService: realtime loop exception: {} — restarting loop",
                         ex.what());
        } catch (...) {
            SPDLOG_ERROR("ProcessingService: realtime loop unknown exception — restarting loop");
        }
        // The batch loop may have thrown before its own cleanup ran.
        rtBatchPipelineActive_.store(false, std::memory_order_release);
        stopBatchPipeline();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// FROZEN-MATS INVARIANT: every cv::Mat published from this loop (gray, mask,
// grayROI, grayFull, fullMask) is freshly allocated per iteration and never
// written after publication. All consumers (Hdf5Service, monitoring rings,
// overlay readers) are read-only. Shallow refcount assigns are therefore safe.
// If in-place buffer reuse is ever added here this invariant must be revisited.
void ProcessingService::realtimeInlineLoop() {
    rtLastProcessed_.store(0);
    using clock = std::chrono::steady_clock;
    // Per-frame latency instrumentation sink (no-op unless enabled; see
    // knowledge_map/diagnostics/PipelineTimingRecorder.md).
    auto& rtTimingRecorder = backend::diagnostics::PipelineTimingRecorder::instance();
    auto lastSummaryTs = clock::now();
    uint64_t framesSinceSummary = 0;
    uint64_t framesSkippedSinceSummary = 0;
    double msSinceSummary = 0.0;
    double algoMsSinceSummary = 0.0;
    uint64_t validSinceSummary = 0;
    uint64_t invalidSinceSummary = 0;

    // Multi-image series state (persists across loop iterations, single-threaded access)
    ProcessedFrame pendingMultiImageFrame;
    size_t multiImageRemaining = 0; // frames still needed to complete current series
    bool multiImagePending = false;

    // Hoisted config/roi/bg: refresh only when configVersion_ changes (P7)
    uint64_t lastRtConfigVer = std::numeric_limits<uint64_t>::max();
    Roi rtCachedRoi{};
    std::shared_ptr<cv::Mat> rtCachedBg;
    ProcessingConfig rtCachedConfig;

    while (rtRunning_.load()) {
        // Refresh config/roi/background only when something changed
        const uint64_t curRtConfigVer = configVersion_.load(std::memory_order_acquire);
        if (curRtConfigVer != lastRtConfigVer) {
            {
                std::scoped_lock lk(rtMutex_);
                rtCachedRoi = rtRoi_;
                rtCachedBg = rtBgGray_;
            }
            {
                std::scoped_lock lk(configMutex_);
                rtCachedConfig = processingConfig_;
            }
            lastRtConfigVer = curRtConfigVer;
        }
        if (!rtStore_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const uint64_t total = rtStore_->totalWritten();
        if (total == 0) {
            rtStore_->waitForFrame(0, std::chrono::milliseconds(5));
            continue;
        }
        const uint64_t earliest = rtStore_->earliestAvailableIndex();
        const uint64_t latest = rtStore_->latestAvailableIndex();
        uint64_t last = rtLastProcessed_.load();
        if (last > latest) {
            // FrameStore::resize() renumbers frames from 0; a cached pointer
            // from the old numbering would idle this loop forever.
            SPDLOG_WARN("Realtime pointer {} beyond latest {} (store resized?); resyncing", last,
                        latest);
            last = latest;
            rtLastProcessed_.store(last);
        }
        if (last + 1 < earliest) {
            // Skip ahead if our pointer fell behind the ring window
            uint64_t skipped = earliest - (last + 1);
            framesSkippedSinceSummary += skipped;
            rtTimingRecorder.countSkipped(backend::diagnostics::PipelineSkipReason::RingBehind,
                                          skipped);
            noteRealtimeLost(skipped);
            last = earliest - 1;
            // Publish the advance immediately: if the frame fetch below fails
            // (slot mid-write / evicted) the loop retries with `last` already
            // past the counted range, so the count cannot repeat.
            rtLastProcessed_.store(last);
            SPDLOG_DEBUG("Processing fell behind, skipping {} frames (last={}, earliest={})",
                         skipped, last, earliest);
        }
        if (last >= latest) {
            // Caught up: block until the producer pushes the next frame
            // (event-driven, issue #282) instead of sleep-polling — the old
            // fixed 2 ms poll put a uniform 0-2 ms wait in front of every
            // frame and dominated end-to-end latency. The timeout preserves
            // stop responsiveness (rtRunning_ is rechecked each iteration).
            rtStore_->waitForFrame(total, std::chrono::milliseconds(2));
            continue;
        }

        // If enabled, prefer processing only the most recent frame to minimize latency (drop
        // intermediate frames). We intentionally ignore this mode during experiments to avoid
        // dropping frames that might be saved.
        const bool dropFrames =
            rtDropFrames_.load(std::memory_order_relaxed) && !experimentActive_.load();
        if (dropFrames) {
            const uint64_t idx = latest;
            // Frames jumped over to reach `idx`. Count them ONLY on paths
            // that advance rtLastProcessed_ past the range: a failed frame
            // fetch (slot mid-write — totalWritten_ increments before the
            // slot copy — or evicted) leaves `last` unchanged and retries,
            // so counting up front would tally the same range again next
            // iteration (seen as skip-accounting overshoot on slow CI
            // runners once the event-driven wake made the consumer hot).
            const uint64_t droppedToLatest = (last + 1 < idx) ? idx - (last + 1) : 0;
            bool droppedCounted = false;
            auto countDroppedToLatest = [&] {
                if (droppedToLatest > 0 && !droppedCounted) {
                    droppedCounted = true;
                    framesSkippedSinceSummary += droppedToLatest;
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::DroppedToLatest, droppedToLatest);
                }
            };
            const auto frameStart = clock::now();
            if (!rtEnabled_.load()) {
                countDroppedToLatest();
                rtLastProcessed_.store(idx);
                continue;
            }

            // Use hoisted config/roi/bg (refreshed at top of while loop when configVersion_
            // changed)
            Roi roi = rtCachedRoi;
            std::shared_ptr<cv::Mat> bgShared = rtCachedBg;
            ProcessingConfig config = rtCachedConfig;

            // Get frame - use ROI access if ROI is specified, otherwise full frame
            backend::playback::Frame f{};
            bool useROI = (roi.w > 0 && roi.h > 0);
            if (useROI) {
                // Clamp ROI to reasonable bounds first
                if (!rtStore_->getByWriteIndex(idx, f)) {
                    continue; // not counted — retried from the same `last`
                }
                if (f.width == 0 || f.height == 0 || f.data.empty()) {
                    continue; // not counted — retried from the same `last`
                }
                // Frame readable: every exit past this point advances
                // rtLastProcessed_, so the dropped range is counted exactly once.
                countDroppedToLatest();
                const int frameW = static_cast<int>(f.width);
                const int frameH = static_cast<int>(f.height);
                roi.x = std::max(0, std::min(roi.x, frameW - 1));
                roi.y = std::max(0, std::min(roi.y, frameH - 1));
                roi.w = std::max(1, std::min(roi.w, frameW - roi.x));
                roi.h = std::max(1, std::min(roi.h, frameH - roi.y));

                // Extract ROI directly from frame
                cv::Mat grayROI = makeGrayROI(f, roi.x, roi.y, roi.w, roi.h);
                if (grayROI.empty()) {
                    continue;
                }

                // Create ROI-sized mask (much smaller than full frame)
                cv::Mat mask(roi.h, roi.w, CV_8UC1, cv::Scalar(0));
                cv::Mat blurredCurr, blurredBg, thresh;
                const auto algoStart = clock::now();
                const uint64_t algoStartUsRec =
                    rtTimingRecorder.isEnabled() ? rtTimingRecorder.nowUs() : 0;
                auto toOdd = [](int v) -> int {
                    if (v < 1) v = 1;
                    if ((v % 2) == 0) v += 1;
                    return v;
                };
                const int blurK = toOdd(config.gaussian_blur_size);
                const int threshVal = std::max(0, config.bg_subtract_threshold);

                cv::GaussianBlur(grayROI, blurredCurr, cv::Size(blurK, blurK), 0);
                bool hasBackground = (bgShared && !bgShared->empty() &&
                                      bgShared->size() == cv::Size(static_cast<int>(f.width),
                                                                   static_cast<int>(f.height)) &&
                                      bgShared->type() == CV_8UC1);

                // For processing: use background subtraction if available
                cv::Mat diffForProcessing;
                if (hasBackground) {
                    cv::Rect bgRoi(roi.x, roi.y, roi.w, roi.h);
                    cv::Mat bgROI = (*bgShared)(bgRoi);
                    cv::GaussianBlur(bgROI, blurredBg, cv::Size(blurK, blurK), 0);
                    cv::subtract(blurredCurr, blurredBg, diffForProcessing);
                } else {
                    diffForProcessing = blurredCurr;
                }

                // For auto-capture detection: always use frame-to-frame difference when enabled
                cv::Mat diffForAutoCapture;
                cv::Mat autoCaptureBackground;
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    if (!previousFrameForAutoCapture_.empty() &&
                        previousFrameForAutoCapture_.size() == blurredCurr.size() &&
                        previousFrameForAutoCapture_.type() == blurredCurr.type()) {
                        autoCaptureBackground = previousFrameForAutoCapture_;
                        cv::absdiff(blurredCurr, previousFrameForAutoCapture_, diffForAutoCapture);
                    } else {
                        // First frame or size mismatch: store current frame and skip auto-capture
                        // check
                        previousFrameForAutoCapture_ =
                            blurredCurr; // share refcount; blurredCurr reallocs next iter
                        diffForAutoCapture =
                            blurredCurr; // Use current frame for thresholding (will not be empty)
                    }
                } else {
                    diffForAutoCapture = diffForProcessing; // Fallback to processing diff
                }

                // Use frame-to-frame diff for empty frame detection when auto-capture is enabled
                cv::Mat diff = (config.auto_background_enabled && !experimentActive_.load())
                                   ? diffForAutoCapture
                                   : diffForProcessing;
                cv::threshold(diff, thresh, threshVal, 255, cv::THRESH_BINARY);

                // Check for empty frame: count non-zero pixels after binary threshold
                int pixelCount = cv::countNonZero(thresh);
                const bool autoCaptureEmptyCheck =
                    config.auto_background_enabled && !experimentActive_.load();
                ProcessingConfig emptyConfig = config;
                if (autoCaptureEmptyCheck) emptyConfig.gaussian_blur_size = 1;
                bool emptyFrame = true;
                std::string emptyError;
                const cv::Mat emptyBackground =
                    autoCaptureEmptyCheck
                        ? autoCaptureBackground
                        : (hasBackground ? (*bgShared)(cv::Rect(roi.x, roi.y, roi.w, roi.h))
                                         : cv::Mat{});
                noteRealtimeAdmitted(idx);
                if (!isImageEmptyWithActiveKernel(
                        autoCaptureEmptyCheck ? blurredCurr : grayROI, emptyBackground, emptyConfig,
                        Roi{0, 0, roi.w, roi.h}, autoCaptureEmptyCheck, emptyFrame, &emptyError)) {
                    SPDLOG_ERROR("Realtime processing core empty check failed for frame {}: {}",
                                 idx, emptyError);
                    // A core failure is a ProcessingFailed outcome, never an empty
                    // frame (issue #367): skip the frame explicitly.
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }
                if (emptyFrame) {
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::Empty, &f);
                    SPDLOG_TRACE("Empty frame detected (idx={}, pixel_count={}, threshold={}), "
                                 "skipping further processing",
                                 idx, pixelCount, config.empty_frame_pixel_threshold);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::EmptyFrame);

                    // Auto-capture logic (only when experiment is NOT running)
                    if (config.auto_background_enabled && !experimentActive_.load()) {
                        uint64_t currentEmpty =
                            consecutiveEmptyFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
                        uint64_t lastCapture =
                            lastAutoBackgroundFrame_.load(std::memory_order_relaxed);
                        uint64_t framesSinceCapture = (idx > lastCapture) ? (idx - lastCapture) : 0;

                        // Check if we should capture: enough consecutive empty frames AND cooldown
                        // period passed
                        if (currentEmpty >=
                                static_cast<uint64_t>(config.auto_background_empty_frames) &&
                            framesSinceCapture >=
                                static_cast<uint64_t>(config.auto_background_cooldown_frames)) {

                            // Capture full frame as background (not just ROI)
                            cv::Mat fullGray = makeGrayCopy(f);
                            if (!fullGray.empty()) {
                                setRealtimeBackgroundGray(fullGray);
                                lastAutoBackgroundFrame_.store(idx, std::memory_order_relaxed);
                                consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);

                                // Update previous frame cache to current frame (for next
                                // frame-to-frame comparison)
                                {
                                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                                    previousFrameForAutoCapture_ =
                                        blurredCurr; // share refcount; blurredCurr reallocs next
                                                     // iter
                                }

                                // Notify via callback
                                {
                                    std::scoped_lock callbackLk(backgroundCaptureCallbackMutex_);
                                    if (backgroundCaptureCallback_) {
                                        backgroundCaptureCallback_(fullGray.clone(), idx);
                                    }
                                }

                                SPDLOG_INFO("Auto-captured background at frame {} ({} consecutive "
                                            "empty frames)",
                                            idx, currentEmpty);
                            }
                        }
                    } else {
                        // Reset counter if auto-capture disabled, experiment running, or movement
                        // detected
                        if (!config.auto_background_enabled || experimentActive_.load()) {
                            consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                        }
                    }

                    rtLastProcessed_.store(idx);
                    continue; // Skip morphology, contours, validation, and frame accumulation
                }

                // Reset counter on non-empty frames
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                }

                // Update previous frame for frame-to-frame comparison (always when auto-capture
                // enabled)
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    previousFrameForAutoCapture_ =
                        blurredCurr; // share refcount; blurredCurr reallocs next iter
                }

                // Use background subtraction diff for actual processing (morphology, contours,
                // etc.)
                cv::threshold(diffForProcessing, thresh, threshVal, 255, cv::THRESH_BINARY);
                const cv::Mat kernelBackground =
                    hasBackground ? (*bgShared)(cv::Rect(roi.x, roi.y, roi.w, roi.h)) : cv::Mat{};
                std::string kernelError;
                if (!processMaskWithActiveKernel(grayROI, kernelBackground, config,
                                                 Roi{0, 0, roi.w, roi.h}, mask, &kernelError)) {
                    SPDLOG_ERROR("Realtime processing core failed for frame {}: {}", idx,
                                 kernelError);
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }

                // Always run validation for monitoring (even without experiment)
                // mask is ROI-sized so contour coords are 0-based; use local roi for border check
                cv::Rect localRoi(0, 0, roi.w, roi.h);
                auto validations = filterProcessedObjects(mask, localRoi, config, grayROI);
                if (validations.empty()) {
                    validations.push_back(FilterResult{});
                }
                const FilterResult& validation = validations.front();
                noteRealtimeValidation(idx, validations);

                // Extract contours from validation result and adjust coordinates for full-frame
                // snapshot Contours from filterProcessedImage are in ROI coordinates, need to
                // adjust for full frame
                std::vector<std::vector<cv::Point>> contours =
                    validation.allContours ? *validation.allContours
                                           : std::vector<std::vector<cv::Point>>{};
                for (auto& contour : contours) {
                    for (auto& pt : contour) {
                        pt.x += roi.x;
                        pt.y += roi.y;
                    }
                }
                const auto algoEnd = clock::now();
                const uint64_t algoEndUsRec = algoStartUsRec != 0 ? rtTimingRecorder.nowUs() : 0;
                const double algoMs =
                    std::chrono::duration<double, std::milli>(algoEnd - algoStart).count();
                algoMsSinceSummary += algoMs;
                for (const auto& objectValidation : validations) {
                    if (objectValidation.isValid) {
                        ++validSinceSummary;
                    } else {
                        ++invalidSinceSummary;
                    }
                }

                publishRealtimeValidationCallbacks(
                    validations, f.timestamp,
                    {true, idx, f.hostTimestampUs, algoStartUsRec, algoEndUsRec});

                // Off the trigger-critical path: update the identification
                // funnel + invalid-reason histogram from this frame's objects.
                accumulateIdentificationCounters(
                    validations, config, pixelToMicronFactor_.load(std::memory_order_relaxed));

                // Always accumulate frames for monitoring (with size limit)
                for (const auto& objectValidation : validations) {
                    appendRealtimeMonitoringFrame(idx, f.timestamp, objectValidation, grayROI,
                                                  mask);
                }

                // Throttled DEBUG: accumulation sizes and process memory
                if ((idx % 5000ULL) == 0ULL) {
                    size_t vSz = 0;
                    size_t iSz = 0;
                    {
                        const auto bufferedCounts = experimentBuffer_.counts();
                        vSz = bufferedCounts.valid;
                        iSz = bufferedCounts.invalid;
                    }
                    SPDLOG_TRACE("Accumulated frames (idx={}): valid={}, invalid={}, "
                                 "flush_interval={}, since_last_flush={}, mem_mb={:.1f}",
                                 idx, vSz, iSz, flushInterval_.load(), framesSinceLastFlush_.load(),
                                 backend::Tools::getProcessMemoryMB());
                }

                // Throttled DEBUG: monitoring buffer sizes and process memory
                if ((idx % 5000ULL) == 0ULL) {
                    size_t monValidSz = 0;
                    size_t monInvalidSz = 0;
                    {
                        std::scoped_lock mLk(monitoringFramesMutex_);
                        monValidSz = monitoringValidFrames_.size();
                        monInvalidSz = monitoringInvalidFrames_.size();
                    }
                    SPDLOG_TRACE("Realtime monitoring sizes (idx={}): mon_valid={}, "
                                 "mon_invalid={}, mem_mb={:.1f}",
                                 idx, monValidSz, monInvalidSz,
                                 backend::Tools::getProcessMemoryMB());
                }

                // Create full frame copy outside algo timing, only when needed for
                // experiment/snapshot
                cv::Mat grayFull;

                // Also accumulate frames for the experiment iff this frame's
                // admission was counted under the run. Not "iff active": the
                // frame in flight across endExperiment() (which the drain wait
                // there lets finish) must still reach the buffer so the
                // remainder flush persists it (review, 2026-09-08).
                if (!experimentSettled_.load(std::memory_order_acquire) && experimentAccounting_.wasAdmitted(idx)) {
                    const bool multiImageMode =
                        config.multi_image_enabled && config.multi_image_count > 1;
                    const TargetGroupEvent targetOwner = selectTargetGroupTriggerOwner(validations);
                    const FilterResult* triggerAnchor = nullptr;
                    if (targetOwner.isTargetGroup) {
                        for (const auto& objectValidation : validations) {
                            if (objectValidation.isValid && objectValidation.isTargetGroup &&
                                objectValidation.objectId == targetOwner.objectId &&
                                objectValidation.trackId == targetOwner.trackId) {
                                triggerAnchor = &objectValidation;
                                break;
                            }
                        }
                    }
                    if (!triggerAnchor) {
                        const auto triggerFallbackIt =
                            std::find_if(validations.begin(), validations.end(),
                                         [](const FilterResult& result) { return result.isValid; });
                        if (triggerFallbackIt != validations.end()) {
                            triggerAnchor = &(*triggerFallbackIt);
                        }
                    }

                    // Helper: lazy-init full-frame gray; returns a shallow refcount copy.
                    // Frozen invariant: do not write through the returned Mat.
                    auto makeFullGray = [&]() -> cv::Mat {
                        if (grayFull.empty()) {
                            grayFull = makeGrayCopy(f);
                        }
                        return grayFull; // shallow refcount copy — caller must not modify
                    };

                    if (multiImagePending) {
                        // Collecting series images for pending multi-image trigger
                        pendingMultiImageFrame.seriesImages.push_back(makeFullGray());
                        --multiImageRemaining;
                        SPDLOG_TRACE(
                            "Multi-image series (ROI path): captured frame {} (remaining={})", idx,
                            multiImageRemaining);

                        if (multiImageRemaining == 0) {
                            multiImagePending = false;
                            SPDLOG_DEBUG("Multi-image series complete (ROI path): trigger_idx={}, "
                                         "series_size={}",
                                         pendingMultiImageFrame.index,
                                         pendingMultiImageFrame.seriesImages.size());
                            appendExperimentFrame(std::move(pendingMultiImageFrame), true);
                            pendingMultiImageFrame = ProcessedFrame{};
                        }
                    } else {
                        bool shouldSave = false;
                        if (triggerAnchor) {
                            if (multiImageMode) {
                                // Start new multi-image series
                                cv::Mat fullGray = makeFullGray();
                                cv::Mat fullMask(fullGray.rows, fullGray.cols, CV_8UC1,
                                                 cv::Scalar(0));
                                cv::Rect fullCvRoi(roi.x, roi.y, roi.w, roi.h);
                                mask.copyTo(fullMask(fullCvRoi));

                                pendingMultiImageFrame = ProcessedFrame{};
                                pendingMultiImageFrame.index = idx;
                                pendingMultiImageFrame.timestampNs = f.timestamp;
                                pendingMultiImageFrame.validation = *triggerAnchor;
                                pendingMultiImageFrame.originalImage =
                                    fullGray; // shallow refcount share
                                pendingMultiImageFrame.processedImage = std::move(fullMask);
                                pendingMultiImageFrame.seriesImages.push_back(std::move(fullGray));
                                multiImageRemaining =
                                    static_cast<size_t>(config.multi_image_count - 1);
                                multiImagePending = true;
                                SPDLOG_DEBUG("Multi-image series started (ROI path): "
                                             "trigger_idx={}, count={}",
                                             idx, config.multi_image_count);
                            } else {
                                shouldSave = true;
                            }
                        } else {
                            size_t counter =
                                invalidFrameCounter_.fetch_add(1, std::memory_order_relaxed);
                            size_t rate = invalidFrameSamplingRate_.load(std::memory_order_relaxed);
                            if (rate > 0 && (counter % rate) == 0) {
                                shouldSave = true;
                            }
                        }

                        if (shouldSave) {
                            ProcessedFrame frame;
                            frame.index = idx;
                            frame.timestampNs = f.timestamp;
                            frame.validation = triggerAnchor ? *triggerAnchor : validation;
                            cv::Mat fullGray = makeFullGray();
                            cv::Mat fullMask(fullGray.rows, fullGray.cols, CV_8UC1, cv::Scalar(0));
                            cv::Rect fullCvRoi(roi.x, roi.y, roi.w, roi.h);
                            mask.copyTo(fullMask(fullCvRoi));
                            frame.originalImage = std::move(fullGray);
                            frame.processedImage = std::move(fullMask);

                            appendExperimentFrame(std::move(frame), validation.isValid);
                        }
                    }
                } else if (multiImagePending) {
                    // Experiment ended while collecting series — save partial
                    SPDLOG_WARN("Multi-image series incomplete (ROI path, experiment ended): "
                                "trigger_idx={}, collected={}",
                                pendingMultiImageFrame.index,
                                pendingMultiImageFrame.seriesImages.size());
                    multiImagePending = false;
                    multiImageRemaining = 0;
                    appendExperimentFrame(std::move(pendingMultiImageFrame), true);
                    pendingMultiImageFrame = ProcessedFrame{};
                }

                // Publish snapshot: build outside lock, pointer-swap inside
                {
                    // Create full-size mask for snapshot display
                    cv::Mat fullMaskSnapshot;
                    if (useROI) {
                        // Reuse the frame we already fetched for this index. The
                        // experiment path may have built grayFull already; otherwise
                        // derive the snapshot from f directly instead of taking the
                        // contended FrameStore lock a second time for the same idx.
                        cv::Mat grayFullSnap;
                        if (!grayFull.empty()) {
                            grayFullSnap = grayFull;
                        } else if (!f.data.empty()) {
                            grayFullSnap = makeGrayCopy(f);
                        }
                        if (!grayFullSnap.empty()) {
                            fullMaskSnapshot = cv::Mat(grayFullSnap.rows, grayFullSnap.cols,
                                                       CV_8UC1, cv::Scalar(0));
                            cv::Rect fullCvRoiSnap(roi.x, roi.y, roi.w, roi.h);
                            mask.copyTo(fullMaskSnapshot(fullCvRoiSnap));
                        } else {
                            fullMaskSnapshot = mask; // shallow (mask not modified after this)
                        }
                    } else {
                        fullMaskSnapshot = mask; // shallow (mask not modified after this)
                    }
                    auto newSnap = std::make_shared<RealtimeSnapshot>();
                    newSnap->index = idx;
                    newSnap->mask = std::move(fullMaskSnapshot);
                    newSnap->contours = std::move(contours);
                    newSnap->validation = validation;
                    std::scoped_lock lk(snapshotMutex_);
                    latestSnapshot_ = std::move(newSnap); // O(1) pointer swap inside lock
                }

                rtLastProcessed_.store(idx);
            } else {
                // No ROI specified - process full frame (fallback to original behavior)
                backend::playback::Frame f{};
                if (!rtStore_->getByWriteIndex(idx, f)) {
                    continue; // not counted — retried from the same `last`
                }
                if (f.width == 0 || f.height == 0 || f.data.empty()) {
                    continue; // not counted — retried from the same `last`
                }
                // Frame readable: every exit past this point advances
                // rtLastProcessed_, so the dropped range is counted exactly once.
                countDroppedToLatest();
                cv::Mat gray = makeGrayCopy(f);
                if (gray.empty()) {
                    rtLastProcessed_.store(idx);
                    continue;
                }

                // Clamp ROI (will be full frame if not set)
                if (roi.w <= 0 || roi.h <= 0) {
                    roi.x = 0;
                    roi.y = 0;
                    roi.w = gray.cols;
                    roi.h = gray.rows;
                }
                roi.x = std::max(0, std::min(roi.x, gray.cols - 1));
                roi.y = std::max(0, std::min(roi.y, gray.rows - 1));
                roi.w = std::max(1, std::min(roi.w, gray.cols - roi.x));
                roi.h = std::max(1, std::min(roi.h, gray.rows - roi.y));

                cv::Rect cvRoi(roi.x, roi.y, roi.w, roi.h);

                // Build full-size mask
                cv::Mat mask(gray.rows, gray.cols, CV_8UC1, cv::Scalar(0));
                cv::Mat roiCurr = gray(cvRoi);
                cv::Mat blurredCurr, blurredBg, thresh;
                const auto algoStart = clock::now();
                const uint64_t algoStartUsRec =
                    rtTimingRecorder.isEnabled() ? rtTimingRecorder.nowUs() : 0;
                auto toOdd = [](int v) -> int {
                    if (v < 1) v = 1;
                    if ((v % 2) == 0) v += 1;
                    return v;
                };
                const int blurK = toOdd(config.gaussian_blur_size);
                const int threshVal = std::max(0, config.bg_subtract_threshold);

                cv::GaussianBlur(roiCurr, blurredCurr, cv::Size(blurK, blurK), 0);
                bool hasBackground =
                    (bgShared && !bgShared->empty() && bgShared->size() == gray.size() &&
                     bgShared->type() == CV_8UC1);

                // For processing: use background subtraction if available
                cv::Mat diffForProcessing;
                if (hasBackground) {
                    cv::GaussianBlur((*bgShared)(cvRoi), blurredBg, cv::Size(blurK, blurK), 0);
                    cv::subtract(blurredCurr, blurredBg, diffForProcessing);
                } else {
                    diffForProcessing = blurredCurr;
                }

                // For auto-capture detection: always use frame-to-frame difference when enabled
                cv::Mat diffForAutoCapture;
                cv::Mat autoCaptureBackground;
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    if (!previousFrameForAutoCapture_.empty() &&
                        previousFrameForAutoCapture_.size() == blurredCurr.size() &&
                        previousFrameForAutoCapture_.type() == blurredCurr.type()) {
                        autoCaptureBackground = previousFrameForAutoCapture_;
                        cv::absdiff(blurredCurr, previousFrameForAutoCapture_, diffForAutoCapture);
                    } else {
                        // First frame or size mismatch: store current frame and skip auto-capture
                        // check
                        previousFrameForAutoCapture_ =
                            blurredCurr; // share refcount; blurredCurr reallocs next iter
                        diffForAutoCapture =
                            blurredCurr; // Use current frame for thresholding (will not be empty)
                    }
                } else {
                    diffForAutoCapture = diffForProcessing; // Fallback to processing diff
                }

                // Use frame-to-frame diff for empty frame detection when auto-capture is enabled
                cv::Mat diff = (config.auto_background_enabled && !experimentActive_.load())
                                   ? diffForAutoCapture
                                   : diffForProcessing;
                cv::threshold(diff, thresh, threshVal, 255, cv::THRESH_BINARY);

                // Check for empty frame: count non-zero pixels after binary threshold
                int pixelCount = cv::countNonZero(thresh);
                const bool autoCaptureEmptyCheck =
                    config.auto_background_enabled && !experimentActive_.load();
                ProcessingConfig emptyConfig = config;
                if (autoCaptureEmptyCheck) emptyConfig.gaussian_blur_size = 1;
                bool emptyFrame = true;
                std::string emptyError;
                const cv::Mat emptyBackground =
                    autoCaptureEmptyCheck ? autoCaptureBackground
                                          : (hasBackground ? (*bgShared)(cvRoi) : cv::Mat{});
                noteRealtimeAdmitted(idx);
                if (!isImageEmptyWithActiveKernel(
                        autoCaptureEmptyCheck ? blurredCurr : roiCurr, emptyBackground, emptyConfig,
                        Roi{0, 0, roi.w, roi.h}, autoCaptureEmptyCheck, emptyFrame, &emptyError)) {
                    SPDLOG_ERROR("Realtime processing core empty check failed for frame {}: {}",
                                 idx, emptyError);
                    // A core failure is a ProcessingFailed outcome, never an empty
                    // frame (issue #367): skip the frame explicitly.
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }
                if (emptyFrame) {
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::Empty, &f);
                    SPDLOG_TRACE("Empty frame detected (idx={}, pixel_count={}, threshold={}), "
                                 "skipping further processing",
                                 idx, pixelCount, config.empty_frame_pixel_threshold);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::EmptyFrame);

                    // Auto-capture logic (only when experiment is NOT running)
                    if (config.auto_background_enabled && !experimentActive_.load()) {
                        uint64_t currentEmpty =
                            consecutiveEmptyFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
                        uint64_t lastCapture =
                            lastAutoBackgroundFrame_.load(std::memory_order_relaxed);
                        uint64_t framesSinceCapture = (idx > lastCapture) ? (idx - lastCapture) : 0;

                        // Check if we should capture: enough consecutive empty frames AND cooldown
                        // period passed
                        if (currentEmpty >=
                                static_cast<uint64_t>(config.auto_background_empty_frames) &&
                            framesSinceCapture >=
                                static_cast<uint64_t>(config.auto_background_cooldown_frames)) {

                            // Capture full frame as background (not just ROI)
                            cv::Mat fullGray = makeGrayCopy(f);
                            if (!fullGray.empty()) {
                                setRealtimeBackgroundGray(fullGray);
                                lastAutoBackgroundFrame_.store(idx, std::memory_order_relaxed);
                                consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);

                                // Update previous frame cache to current frame (for next
                                // frame-to-frame comparison)
                                {
                                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                                    previousFrameForAutoCapture_ =
                                        blurredCurr; // share refcount; blurredCurr reallocs next
                                                     // iter
                                }

                                // Notify via callback
                                {
                                    std::scoped_lock callbackLk(backgroundCaptureCallbackMutex_);
                                    if (backgroundCaptureCallback_) {
                                        backgroundCaptureCallback_(fullGray.clone(), idx);
                                    }
                                }

                                SPDLOG_INFO("Auto-captured background at frame {} ({} consecutive "
                                            "empty frames)",
                                            idx, currentEmpty);
                            }
                        }
                    } else {
                        // Reset counter if auto-capture disabled, experiment running, or movement
                        // detected
                        if (!config.auto_background_enabled || experimentActive_.load()) {
                            consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                        }
                    }

                    rtLastProcessed_.store(idx);
                    continue;
                }

                // Reset counter on non-empty frames
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                }

                // Update previous frame for frame-to-frame comparison (always when auto-capture
                // enabled)
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    previousFrameForAutoCapture_ =
                        blurredCurr; // share refcount; blurredCurr reallocs next iter
                }

                // Use background subtraction diff for actual processing (morphology, contours,
                // etc.)
                cv::threshold(diffForProcessing, thresh, threshVal, 255, cv::THRESH_BINARY);

                // Update previous frame for frame-to-frame comparison (when no background and
                // auto-capture enabled)
                if (!hasBackground && config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    previousFrameForAutoCapture_ =
                        blurredCurr; // share refcount; blurredCurr reallocs next iter
                }

                std::string kernelError;
                if (!processMaskWithActiveKernel(gray, hasBackground ? *bgShared : cv::Mat{},
                                                 config, roi, mask, &kernelError)) {
                    SPDLOG_ERROR("Realtime processing core failed for frame {}: {}", idx,
                                 kernelError);
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }

                auto validations = filterProcessedObjects(mask, cvRoi, config, gray);
                if (validations.empty()) {
                    validations.push_back(FilterResult{});
                }
                const FilterResult& validation = validations.front();
                noteRealtimeValidation(idx, validations);

                // Extract contours from validation result for snapshot
                std::vector<std::vector<cv::Point>> contours =
                    validation.allContours ? *validation.allContours
                                           : std::vector<std::vector<cv::Point>>{};
                const auto algoEnd = clock::now();
                const uint64_t algoEndUsRec = algoStartUsRec != 0 ? rtTimingRecorder.nowUs() : 0;
                const double algoMs =
                    std::chrono::duration<double, std::milli>(algoEnd - algoStart).count();
                algoMsSinceSummary += algoMs;
                for (const auto& objectValidation : validations) {
                    if (objectValidation.isValid) {
                        ++validSinceSummary;
                    } else {
                        ++invalidSinceSummary;
                    }
                }

                publishRealtimeValidationCallbacks(
                    validations, f.timestamp,
                    {true, idx, f.hostTimestampUs, algoStartUsRec, algoEndUsRec});

                // Off the trigger-critical path: update the identification
                // funnel + invalid-reason histogram from this frame's objects.
                accumulateIdentificationCounters(
                    validations, config, pixelToMicronFactor_.load(std::memory_order_relaxed));

                // Always accumulate frames for monitoring (with size limit)
                cv::Mat roiOriginal = gray(cvRoi);
                cv::Mat roiMask = mask(cvRoi);
                for (const auto& objectValidation : validations) {
                    appendRealtimeMonitoringFrame(idx, f.timestamp, objectValidation, roiOriginal,
                                                  roiMask);
                }

                // Throttled DEBUG: accumulation sizes and process memory
                if ((idx % 500ULL) == 0ULL) {
                    size_t vSz = 0;
                    size_t iSz = 0;
                    {
                        const auto bufferedCounts = experimentBuffer_.counts();
                        vSz = bufferedCounts.valid;
                        iSz = bufferedCounts.invalid;
                    }
                    SPDLOG_DEBUG("Accumulated frames (idx={}): valid={}, invalid={}, "
                                 "flush_interval={}, since_last_flush={}, mem_mb={:.1f}",
                                 idx, vSz, iSz, flushInterval_.load(), framesSinceLastFlush_.load(),
                                 backend::Tools::getProcessMemoryMB());
                }

                // Throttled DEBUG: monitoring buffer sizes and process memory
                if ((idx % 500ULL) == 0ULL) {
                    size_t monValidSz = 0;
                    size_t monInvalidSz = 0;
                    {
                        std::scoped_lock mLk(monitoringFramesMutex_);
                        monValidSz = monitoringValidFrames_.size();
                        monInvalidSz = monitoringInvalidFrames_.size();
                    }
                    SPDLOG_DEBUG("Realtime monitoring sizes (idx={}): mon_valid={}, "
                                 "mon_invalid={}, mem_mb={:.1f}",
                                 idx, monValidSz, monInvalidSz,
                                 backend::Tools::getProcessMemoryMB());
                }

                // Also accumulate frames for the experiment iff this frame's
                // admission was counted under the run. Not "iff active": the
                // frame in flight across endExperiment() (which the drain wait
                // there lets finish) must still reach the buffer so the
                // remainder flush persists it (review, 2026-09-08).
                if (!experimentSettled_.load(std::memory_order_acquire) && experimentAccounting_.wasAdmitted(idx)) {
                    // Determine if we should save this frame
                    bool shouldSave = false;
                    if (validation.isValid) {
                        shouldSave = true;
                    } else {
                        size_t counter =
                            invalidFrameCounter_.fetch_add(1, std::memory_order_relaxed);
                        size_t rate = invalidFrameSamplingRate_.load(std::memory_order_relaxed);
                        if (rate > 0 && (counter % rate) == 0) {
                            shouldSave = true;
                        }
                    }

                    if (shouldSave) {
                        ProcessedFrame frame;
                        frame.index = idx;
                        frame.timestampNs = f.timestamp;
                        frame.validation = validation;
                        frame.originalImage = gray;  // shallow refcount share
                        frame.processedImage = mask; // shallow (mask used for snapshot below)

                        appendExperimentFrame(std::move(frame), validation.isValid);
                    }
                }

                // Publish snapshot: build outside lock, pointer-swap inside
                {
                    auto newSnap = std::make_shared<RealtimeSnapshot>();
                    newSnap->index = idx;
                    newSnap->mask = mask; // shallow refcount share (mask not modified after this)
                    newSnap->contours = std::move(contours);
                    newSnap->validation = validation;
                    std::scoped_lock lk(snapshotMutex_);
                    latestSnapshot_ = std::move(newSnap); // O(1) pointer swap inside lock
                }

                rtLastProcessed_.store(idx);
            }

            // Per-frame timing
            const auto frameEnd = clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
            SPDLOG_TRACE("Realtime processing: idx={} time_ms={:.3f} roi={}x{}", idx, ms, roi.w,
                         roi.h);

            // Periodic summary
            framesSinceSummary += 1;
            msSinceSummary += ms;
            const auto now = frameEnd;
            const double windowMs =
                std::chrono::duration<double, std::milli>(now - lastSummaryTs).count();
            if (windowMs >= 1000.0) {
                const double avgMs =
                    framesSinceSummary > 0
                        ? (msSinceSummary / static_cast<double>(framesSinceSummary))
                        : 0.0;
                const double algoAvgMs =
                    framesSinceSummary > 0
                        ? (algoMsSinceSummary / static_cast<double>(framesSinceSummary))
                        : 0.0;
                const double fps =
                    windowMs > 0.0 ? (static_cast<double>(framesSinceSummary) * 1000.0 / windowMs)
                                   : 0.0;
                const double vfps =
                    windowMs > 0.0 ? (static_cast<double>(validSinceSummary) * 1000.0 / windowMs)
                                   : 0.0;
                const double ifps =
                    windowMs > 0.0 ? (static_cast<double>(invalidSinceSummary) * 1000.0 / windowMs)
                                   : 0.0;
                algoFps1s_.store(fps, std::memory_order_relaxed);
                validFps1s_.store(vfps, std::memory_order_relaxed);
                invalidFps1s_.store(ifps, std::memory_order_relaxed);
                const double algoAvgUs = algoAvgMs * 1000.0;
                algoAvgUs1s_.store(algoAvgUs, std::memory_order_relaxed);
                algoAvgUs1sUpdatedUs_.store(backend::Tools::getTimestamp(),
                                            std::memory_order_relaxed);
                SPDLOG_DEBUG("Realtime processing summary: processed={} skipped={} "
                             "window_ms={:.0f} avg_ms={:.3f} algo_avg_ms={:.3f} ~fps={:.1f}",
                             framesSinceSummary, framesSkippedSinceSummary, windowMs, avgMs,
                             algoAvgMs, fps);

                // Extended summary: buffers, ROI, background, and process memory
                size_t vSz = 0, iSz = 0, monValidSz = 0, monInvalidSz = 0;
                {
                    const auto bufferedCounts = experimentBuffer_.counts();
                    vSz = bufferedCounts.valid;
                    iSz = bufferedCounts.invalid;
                }
                {
                    std::scoped_lock mLk(monitoringFramesMutex_);
                    monValidSz = monitoringValidFrames_.size();
                    monInvalidSz = monitoringInvalidFrames_.size();
                }
                const bool hasBg = (rtCachedBg != nullptr && !rtCachedBg->empty());
                const size_t flushInt = flushInterval_.load();
                const size_t sinceFlush = framesSinceLastFlush_.load();
                const double memMB = backend::Tools::getProcessMemoryMB();
                const double peakMB = backend::Tools::getPeakProcessMemoryMB();
                SPDLOG_DEBUG("Realtime buffers: acc_valid={} acc_invalid={} mon_valid={} "
                             "mon_invalid={} flush_interval={} since_last_flush={} roi={}x{} bg={} "
                             "mem_mb={:.1f} peak_mb={:.1f}",
                             vSz, iSz, monValidSz, monInvalidSz, flushInt, sinceFlush,
                             rtCachedRoi.w, rtCachedRoi.h, hasBg ? 1 : 0, memMB, peakMB);
                lastSummaryTs = now;
                framesSinceSummary = 0;
                framesSkippedSinceSummary = 0;
                msSinceSummary = 0.0;
                algoMsSinceSummary = 0.0;
                validSinceSummary = 0;
                invalidSinceSummary = 0;
            }
        } else {
            for (uint64_t idx = last + 1; idx <= latest && rtRunning_.load(); ++idx) {
                const auto frameStart = clock::now();
                if (!rtEnabled_.load()) {
                    rtLastProcessed_.store(idx);
                    continue;
                }
                backend::playback::Frame f{};
                if (!rtStore_->getByWriteIndex(idx, f)) {
                    continue;
                }
                if (f.width == 0 || f.height == 0 || f.data.empty()) {
                    continue;
                }
                cv::Mat gray = makeGrayCopy(f);
                if (gray.empty()) {
                    rtLastProcessed_.store(idx);
                    continue;
                }

                // Use hoisted config/roi/bg (refreshed at top of while loop when configVersion_
                // changed)
                Roi roi = rtCachedRoi;
                std::shared_ptr<cv::Mat> bgShared = rtCachedBg;
                ProcessingConfig config = rtCachedConfig;

                // Clamp ROI
                if (roi.w <= 0 || roi.h <= 0) {
                    roi.x = 0;
                    roi.y = 0;
                    roi.w = gray.cols;
                    roi.h = gray.rows;
                }
                roi.x = std::max(0, std::min(roi.x, gray.cols - 1));
                roi.y = std::max(0, std::min(roi.y, gray.rows - 1));
                roi.w = std::max(1, std::min(roi.w, gray.cols - roi.x));
                roi.h = std::max(1, std::min(roi.h, gray.rows - roi.y));

                cv::Rect cvRoi(roi.x, roi.y, roi.w, roi.h);

                // Build full-size mask
                cv::Mat mask(gray.rows, gray.cols, CV_8UC1, cv::Scalar(0));
                cv::Mat roiCurr = gray(cvRoi);
                cv::Mat blurredCurr, blurredBg, thresh;
                const auto algoStart = clock::now();
                const uint64_t algoStartUsRec =
                    rtTimingRecorder.isEnabled() ? rtTimingRecorder.nowUs() : 0;
                auto toOdd = [](int v) -> int {
                    if (v < 1) v = 1;
                    if ((v % 2) == 0) v += 1;
                    return v;
                };
                const int blurK = toOdd(config.gaussian_blur_size);
                const int threshVal = std::max(0, config.bg_subtract_threshold);

                cv::GaussianBlur(roiCurr, blurredCurr, cv::Size(blurK, blurK), 0);
                bool hasBackground =
                    (bgShared && !bgShared->empty() && bgShared->size() == gray.size() &&
                     bgShared->type() == CV_8UC1);

                // For processing: use background subtraction if available
                cv::Mat diffForProcessing;
                if (hasBackground) {
                    cv::GaussianBlur((*bgShared)(cvRoi), blurredBg, cv::Size(blurK, blurK), 0);
                    cv::subtract(blurredCurr, blurredBg, diffForProcessing);
                } else {
                    diffForProcessing = blurredCurr;
                }

                // For auto-capture detection: always use frame-to-frame difference when enabled
                cv::Mat diffForAutoCapture;
                cv::Mat autoCaptureBackground;
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    if (!previousFrameForAutoCapture_.empty() &&
                        previousFrameForAutoCapture_.size() == blurredCurr.size() &&
                        previousFrameForAutoCapture_.type() == blurredCurr.type()) {
                        autoCaptureBackground = previousFrameForAutoCapture_;
                        cv::absdiff(blurredCurr, previousFrameForAutoCapture_, diffForAutoCapture);
                    } else {
                        // First frame or size mismatch: store current frame and skip auto-capture
                        // check
                        previousFrameForAutoCapture_ =
                            blurredCurr; // share refcount; blurredCurr reallocs next iter
                        diffForAutoCapture =
                            blurredCurr; // Use current frame for thresholding (will not be empty)
                    }
                } else {
                    diffForAutoCapture = diffForProcessing; // Fallback to processing diff
                }

                // Use frame-to-frame diff for empty frame detection when auto-capture is enabled
                cv::Mat diff = (config.auto_background_enabled && !experimentActive_.load())
                                   ? diffForAutoCapture
                                   : diffForProcessing;
                cv::threshold(diff, thresh, threshVal, 255, cv::THRESH_BINARY);

                // Check for empty frame: count non-zero pixels after binary threshold
                int pixelCount = cv::countNonZero(thresh);
                const bool autoCaptureEmptyCheck =
                    config.auto_background_enabled && !experimentActive_.load();
                ProcessingConfig emptyConfig = config;
                if (autoCaptureEmptyCheck) emptyConfig.gaussian_blur_size = 1;
                bool emptyFrame = true;
                std::string emptyError;
                const cv::Mat emptyBackground =
                    autoCaptureEmptyCheck ? autoCaptureBackground
                                          : (hasBackground ? (*bgShared)(cvRoi) : cv::Mat{});
                noteRealtimeAdmitted(idx);
                if (!isImageEmptyWithActiveKernel(
                        autoCaptureEmptyCheck ? blurredCurr : roiCurr, emptyBackground, emptyConfig,
                        Roi{0, 0, roi.w, roi.h}, autoCaptureEmptyCheck, emptyFrame, &emptyError)) {
                    SPDLOG_ERROR("Realtime processing core empty check failed for frame {}: {}",
                                 idx, emptyError);
                    // A core failure is a ProcessingFailed outcome, never an empty
                    // frame (issue #367): skip the frame explicitly.
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }
                if (emptyFrame) {
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::Empty, &f);
                    SPDLOG_TRACE("Empty frame detected (idx={}, pixel_count={}, threshold={}), "
                                 "skipping further processing",
                                 idx, pixelCount, config.empty_frame_pixel_threshold);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::EmptyFrame);

                    // Auto-capture logic (only when experiment is NOT running)
                    if (config.auto_background_enabled && !experimentActive_.load()) {
                        uint64_t currentEmpty =
                            consecutiveEmptyFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
                        uint64_t lastCapture =
                            lastAutoBackgroundFrame_.load(std::memory_order_relaxed);
                        uint64_t framesSinceCapture = (idx > lastCapture) ? (idx - lastCapture) : 0;

                        // Check if we should capture: enough consecutive empty frames AND cooldown
                        // period passed
                        if (currentEmpty >=
                                static_cast<uint64_t>(config.auto_background_empty_frames) &&
                            framesSinceCapture >=
                                static_cast<uint64_t>(config.auto_background_cooldown_frames)) {

                            // Capture full frame as background (not just ROI)
                            cv::Mat fullGray = makeGrayCopy(f);
                            if (!fullGray.empty()) {
                                setRealtimeBackgroundGray(fullGray);
                                lastAutoBackgroundFrame_.store(idx, std::memory_order_relaxed);
                                consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);

                                // Update previous frame cache to current frame (for next
                                // frame-to-frame comparison)
                                {
                                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                                    previousFrameForAutoCapture_ =
                                        blurredCurr; // share refcount; blurredCurr reallocs next
                                                     // iter
                                }

                                // Notify via callback
                                {
                                    std::scoped_lock callbackLk(backgroundCaptureCallbackMutex_);
                                    if (backgroundCaptureCallback_) {
                                        backgroundCaptureCallback_(fullGray.clone(), idx);
                                    }
                                }

                                SPDLOG_INFO("Auto-captured background at frame {} ({} consecutive "
                                            "empty frames)",
                                            idx, currentEmpty);
                            }
                        }
                    } else {
                        // Reset counter if auto-capture disabled, experiment running, or movement
                        // detected
                        if (!config.auto_background_enabled || experimentActive_.load()) {
                            consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                        }
                    }

                    // Even on empty frames, capture series images if multi-image collection is
                    // active
                    if (multiImagePending && experimentActive_.load()) {
                        pendingMultiImageFrame.seriesImages.push_back(
                            gray); // shallow refcount share (frozen-mats invariant)
                        --multiImageRemaining;
                        SPDLOG_TRACE("Multi-image series: captured empty frame {} (remaining={})",
                                     idx, multiImageRemaining);
                        if (multiImageRemaining == 0) {
                            multiImagePending = false;
                            SPDLOG_DEBUG("Multi-image series complete (with empty frames): "
                                         "trigger_idx={}, series_size={}",
                                         pendingMultiImageFrame.index,
                                         pendingMultiImageFrame.seriesImages.size());
                            appendExperimentFrame(std::move(pendingMultiImageFrame), true);
                            pendingMultiImageFrame = ProcessedFrame{};
                        }
                    }

                    rtLastProcessed_.store(idx);
                    continue; // Skip morphology, contours, validation, and frame accumulation
                }

                // Reset counter on non-empty frames
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    consecutiveEmptyFrames_.store(0, std::memory_order_relaxed);
                }

                // Update previous frame for frame-to-frame comparison (always when auto-capture
                // enabled)
                if (config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    previousFrameForAutoCapture_ =
                        blurredCurr; // share refcount; blurredCurr reallocs next iter
                }

                // Use background subtraction diff for actual processing (morphology, contours,
                // etc.)
                cv::threshold(diffForProcessing, thresh, threshVal, 255, cv::THRESH_BINARY);

                // Update previous frame for frame-to-frame comparison (when no background and
                // auto-capture enabled)
                if (!hasBackground && config.auto_background_enabled && !experimentActive_.load()) {
                    std::scoped_lock prevFrameLk(previousFrameMutex_);
                    previousFrameForAutoCapture_ =
                        blurredCurr; // share refcount; blurredCurr reallocs next iter
                }

                std::string kernelError;
                if (!processMaskWithActiveKernel(gray, hasBackground ? *bgShared : cv::Mat{},
                                                 config, roi, mask, &kernelError)) {
                    SPDLOG_ERROR("Realtime processing core failed for frame {}: {}", idx,
                                 kernelError);
                    noteRealtimeOutcome(idx, backend::recording::FrameOutcome::ProcessingFailed);
                    rtTimingRecorder.countSkipped(
                        backend::diagnostics::PipelineSkipReason::KernelError);
                    rtLastProcessed_.store(idx);
                    continue;
                }

                // Always run validation for monitoring (even without experiment)
                // Use ROI-only data for validation (avoids O(frame_size) findContours/brightness
                // scan) mask is ROI-sized so contour coords are 0-based; use local roi for border
                // check
                cv::Mat roiMaskForValidation = mask(cvRoi).clone();
                cv::Rect localRoi(0, 0, cvRoi.width, cvRoi.height);
                auto validations =
                    filterProcessedObjects(roiMaskForValidation, localRoi, config, roiCurr);
                if (validations.empty()) {
                    validations.push_back(FilterResult{});
                }
                const FilterResult& validation = validations.front();
                noteRealtimeValidation(idx, validations);

                // Extract contours from validation result for snapshot
                // Contours are in ROI-relative coordinates — adjust to full-frame for
                // snapshot/storage
                std::vector<std::vector<cv::Point>> contours =
                    validation.allContours ? *validation.allContours
                                           : std::vector<std::vector<cv::Point>>{};
                for (auto& contour : contours) {
                    for (auto& pt : contour) {
                        pt.x += roi.x;
                        pt.y += roi.y;
                    }
                }
                const auto algoEnd = clock::now();
                const uint64_t algoEndUsRec = algoStartUsRec != 0 ? rtTimingRecorder.nowUs() : 0;
                const double algoMs =
                    std::chrono::duration<double, std::milli>(algoEnd - algoStart).count();
                algoMsSinceSummary += algoMs;
                for (const auto& objectValidation : validations) {
                    if (objectValidation.isValid) {
                        ++validSinceSummary;
                    } else {
                        ++invalidSinceSummary;
                    }
                }

                publishRealtimeValidationCallbacks(
                    validations, f.timestamp,
                    {true, idx, f.hostTimestampUs, algoStartUsRec, algoEndUsRec});

                // Off the trigger-critical path: update the identification
                // funnel + invalid-reason histogram from this frame's objects.
                accumulateIdentificationCounters(
                    validations, config, pixelToMicronFactor_.load(std::memory_order_relaxed));

                // Always accumulate frames for monitoring (with size limit)
                cv::Mat roiOriginal = gray(cvRoi);
                cv::Mat roiMask = mask(cvRoi);
                for (const auto& objectValidation : validations) {
                    appendRealtimeMonitoringFrame(idx, f.timestamp, objectValidation, roiOriginal,
                                                  roiMask);
                }

                // Throttled TRACE: accumulation sizes and process memory
                if ((idx % 5000ULL) == 0ULL) {
                    size_t vSz = 0;
                    size_t iSz = 0;
                    {
                        const auto bufferedCounts = experimentBuffer_.counts();
                        vSz = bufferedCounts.valid;
                        iSz = bufferedCounts.invalid;
                    }
                    SPDLOG_TRACE("Accumulated frames (idx={}): valid={}, invalid={}, "
                                 "flush_interval={}, since_last_flush={}, mem_mb={:.1f}",
                                 idx, vSz, iSz, flushInterval_.load(), framesSinceLastFlush_.load(),
                                 backend::Tools::getProcessMemoryMB());
                }

                // Throttled TRACE: monitoring buffer sizes and process memory
                if ((idx % 5000ULL) == 0ULL) {
                    size_t monValidSz = 0;
                    size_t monInvalidSz = 0;
                    {
                        std::scoped_lock mLk(monitoringFramesMutex_);
                        monValidSz = monitoringValidFrames_.size();
                        monInvalidSz = monitoringInvalidFrames_.size();
                    }
                    SPDLOG_TRACE("Realtime monitoring sizes (idx={}): mon_valid={}, "
                                 "mon_invalid={}, mem_mb={:.1f}",
                                 idx, monValidSz, monInvalidSz,
                                 backend::Tools::getProcessMemoryMB());
                }

                // Also accumulate frames for the experiment iff this frame's
                // admission was counted under the run. Not "iff active": the
                // frame in flight across endExperiment() (which the drain wait
                // there lets finish) must still reach the buffer so the
                // remainder flush persists it (review, 2026-09-08).
                if (!experimentSettled_.load(std::memory_order_acquire) && experimentAccounting_.wasAdmitted(idx)) {
                    const bool multiImageMode =
                        config.multi_image_enabled && config.multi_image_count > 1;
                    const TargetGroupEvent targetOwner = selectTargetGroupTriggerOwner(validations);
                    const FilterResult* triggerAnchor = nullptr;
                    if (targetOwner.isTargetGroup) {
                        for (const auto& objectValidation : validations) {
                            if (objectValidation.isValid && objectValidation.isTargetGroup &&
                                objectValidation.objectId == targetOwner.objectId &&
                                objectValidation.trackId == targetOwner.trackId) {
                                triggerAnchor = &objectValidation;
                                break;
                            }
                        }
                    }
                    if (!triggerAnchor) {
                        const auto triggerFallbackIt =
                            std::find_if(validations.begin(), validations.end(),
                                         [](const FilterResult& result) { return result.isValid; });
                        if (triggerFallbackIt != validations.end()) {
                            triggerAnchor = &(*triggerFallbackIt);
                        }
                    }

                    if (multiImagePending) {
                        // We're collecting series images for a pending multi-image trigger frame
                        pendingMultiImageFrame.seriesImages.push_back(
                            gray); // shallow refcount share
                        --multiImageRemaining;
                        SPDLOG_TRACE(
                            "Multi-image series: captured frame {} for series (remaining={})", idx,
                            multiImageRemaining);

                        if (multiImageRemaining == 0) {
                            // Series complete — push to validFrames
                            multiImagePending = false;
                            SPDLOG_DEBUG(
                                "Multi-image series complete: trigger_idx={}, series_size={}",
                                pendingMultiImageFrame.index,
                                pendingMultiImageFrame.seriesImages.size());
                            appendExperimentFrame(std::move(pendingMultiImageFrame), true);
                            pendingMultiImageFrame = ProcessedFrame{}; // reset
                        }
                        // Skip normal valid/invalid save for this frame — it's part of the series
                    } else {
                        // Normal experiment accumulation (or start of new multi-image series)
                        if (multiImageMode) {
                            if (triggerAnchor) {
                                const size_t validObjectCount = static_cast<size_t>(std::count_if(
                                    validations.begin(), validations.end(),
                                    [](const FilterResult& result) { return result.isValid; }));
                                if (validObjectCount > 1) {
                                    SPDLOG_WARN("Multi-image realtime mode detected {} valid "
                                                "objects in frame {}; recording one trigger series",
                                                validObjectCount, idx);
                                }

                                pendingMultiImageFrame = ProcessedFrame{};
                                pendingMultiImageFrame.index = idx;
                                pendingMultiImageFrame.timestampNs = f.timestamp;
                                pendingMultiImageFrame.validation = *triggerAnchor;
                                pendingMultiImageFrame.originalImage =
                                    gray; // shallow refcount share
                                pendingMultiImageFrame.processedImage =
                                    mask; // shallow (mask used for snapshot below)
                                pendingMultiImageFrame.seriesImages.push_back(
                                    gray); // shallow refcount share
                                multiImageRemaining =
                                    static_cast<size_t>(config.multi_image_count - 1);
                                multiImagePending = true;
                                SPDLOG_DEBUG("Multi-image series started: trigger_idx={}, count={}",
                                             idx, config.multi_image_count);
                            }
                        } else {
                            for (const auto& objectValidation : validations) {
                                bool shouldSaveObject = objectValidation.isValid;
                                if (!objectValidation.isValid) {
                                    size_t counter = invalidFrameCounter_.fetch_add(
                                        1, std::memory_order_relaxed);
                                    size_t rate =
                                        invalidFrameSamplingRate_.load(std::memory_order_relaxed);
                                    shouldSaveObject = rate > 0 && (counter % rate) == 0;
                                }

                                if (shouldSaveObject) {
                                    ProcessedFrame frame;
                                    frame.index = idx;
                                    frame.timestampNs = f.timestamp;
                                    frame.validation = objectValidation;
                                    frame.originalImage =
                                        gray; // shallow (mask used for snapshot below)
                                    frame.processedImage =
                                        mask; // shallow (mask used for snapshot below)

                                    appendExperimentFrame(std::move(frame),
                                                          objectValidation.isValid);
                                }
                            }
                        }
                    }
                } else if (multiImagePending) {
                    // Experiment ended while collecting a multi-image series — save partial series
                    SPDLOG_WARN("Multi-image series incomplete (experiment ended): trigger_idx={}, "
                                "collected={}/{}",
                                pendingMultiImageFrame.index,
                                pendingMultiImageFrame.seriesImages.size(),
                                pendingMultiImageFrame.seriesImages.size() + multiImageRemaining);
                    multiImagePending = false;
                    multiImageRemaining = 0;
                    appendExperimentFrame(std::move(pendingMultiImageFrame), true);
                    pendingMultiImageFrame = ProcessedFrame{};
                }

                // Publish snapshot: build outside lock, pointer-swap inside
                {
                    auto newSnap = std::make_shared<RealtimeSnapshot>();
                    newSnap->index = idx;
                    newSnap->mask = mask; // shallow refcount share (mask not modified after this)
                    newSnap->contours = std::move(contours);
                    newSnap->validation = validation;
                    std::scoped_lock lk(snapshotMutex_);
                    latestSnapshot_ = std::move(newSnap); // O(1) pointer swap inside lock
                }

                rtLastProcessed_.store(idx);

                // Per-frame timing
                const auto frameEnd = clock::now();
                const double ms =
                    std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
                SPDLOG_TRACE("Realtime processing: idx={} time_ms={:.3f} roi={}x{}", idx, ms, roi.w,
                             roi.h);

                // Periodic summary
                framesSinceSummary += 1;
                msSinceSummary += ms;
                const auto now = frameEnd;
                const double windowMs =
                    std::chrono::duration<double, std::milli>(now - lastSummaryTs).count();
                if (windowMs >= 1000.0) {
                    const double avgMs =
                        framesSinceSummary > 0
                            ? (msSinceSummary / static_cast<double>(framesSinceSummary))
                            : 0.0;
                    const double algoAvgMs =
                        framesSinceSummary > 0
                            ? (algoMsSinceSummary / static_cast<double>(framesSinceSummary))
                            : 0.0;
                    const double fps =
                        windowMs > 0.0
                            ? (static_cast<double>(framesSinceSummary) * 1000.0 / windowMs)
                            : 0.0;
                    const double vfps =
                        windowMs > 0.0
                            ? (static_cast<double>(validSinceSummary) * 1000.0 / windowMs)
                            : 0.0;
                    const double ifps =
                        windowMs > 0.0
                            ? (static_cast<double>(invalidSinceSummary) * 1000.0 / windowMs)
                            : 0.0;
                    algoFps1s_.store(fps, std::memory_order_relaxed);
                    validFps1s_.store(vfps, std::memory_order_relaxed);
                    invalidFps1s_.store(ifps, std::memory_order_relaxed);
                    const double algoAvgUs = algoAvgMs * 1000.0;
                    algoAvgUs1s_.store(algoAvgUs, std::memory_order_relaxed);
                    algoAvgUs1sUpdatedUs_.store(backend::Tools::getTimestamp(),
                                                std::memory_order_relaxed);
                    SPDLOG_DEBUG("Realtime processing summary: processed={} skipped={} "
                                 "window_ms={:.0f} avg_ms={:.3f} algo_avg_ms={:.3f} ~fps={:.1f}",
                                 framesSinceSummary, framesSkippedSinceSummary, windowMs, avgMs,
                                 algoAvgMs, fps);

                    // Extended summary: buffers, ROI, background, and process memory
                    size_t vSz = 0, iSz = 0, monValidSz = 0, monInvalidSz = 0;
                    {
                        const auto bufferedCounts = experimentBuffer_.counts();
                        vSz = bufferedCounts.valid;
                        iSz = bufferedCounts.invalid;
                    }
                    {
                        std::scoped_lock mLk(monitoringFramesMutex_);
                        monValidSz = monitoringValidFrames_.size();
                        monInvalidSz = monitoringInvalidFrames_.size();
                    }
                    const bool hasBgDf = (rtCachedBg != nullptr && !rtCachedBg->empty());
                    const size_t flushInt = flushInterval_.load();
                    const size_t sinceFlush = framesSinceLastFlush_.load();
                    const double memMB = backend::Tools::getProcessMemoryMB();
                    const double peakMB = backend::Tools::getPeakProcessMemoryMB();
                    SPDLOG_DEBUG("Realtime buffers: acc_valid={} acc_invalid={} mon_valid={} "
                                 "mon_invalid={} flush_interval={} since_last_flush={} roi={}x{} "
                                 "bg={} mem_mb={:.1f} peak_mb={:.1f}",
                                 vSz, iSz, monValidSz, monInvalidSz, flushInt, sinceFlush,
                                 rtCachedRoi.w, rtCachedRoi.h, hasBgDf ? 1 : 0, memMB, peakMB);
                    lastSummaryTs = now;
                    framesSinceSummary = 0;
                    framesSkippedSinceSummary = 0;
                    msSinceSummary = 0.0;
                    algoMsSinceSummary = 0.0;
                    validSinceSummary = 0;
                    invalidSinceSummary = 0;
                }
            }
        }
    }
}

} // namespace backend::services
