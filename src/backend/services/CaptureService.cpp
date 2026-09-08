#include "backend/services/CaptureService.h"
#include "backend/app/Tools.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/playback/FrameStore.h"
#include "backend/camera/egrabber/EGrabberCamera.h"
#include "backend/camera/common/ICamera.h"
#include "backend/camera/mock/MockCamera.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace backend::services {

CaptureService::CaptureService() {
#if MIB_HAS_EGRABBER
    cameraFactory_ = []() {
        return std::make_unique<::camera::common::EGrabberCamera>();
    };
#else
    cameraFactory_ = []() {
        camera::mock::MockCameraOptions options;
        options.folder = std::filesystem::path("data") / "mock_frames";
        return std::make_unique<camera::mock::MockCamera>(options);
    };
#endif
}

CaptureService::~CaptureService() {
    stop();
    // stop() joins the worker; nothing else can be joinable here, but a
    // defensive reap keeps the destructor from ever std::terminate-ing.
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    reapFinishedWorkerLocked();
}

void CaptureService::setConfig(const Config& cfg) { config_ = cfg; }

void CaptureService::setFrameCallback(FrameCallback cb) { callback_ = std::move(cb); }

void CaptureService::setFrameStore(std::shared_ptr<backend::playback::FrameStore> store) { frameStore_ = std::move(store); }

void CaptureService::setCameraFactory(CameraFactory factory) {
    cameraFactory_ = std::move(factory);
}

void CaptureService::setCameraReadyCallback(CameraReadyCallback cb) {
    cameraReadyCallback_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Lifecycle owner (issue #365)
// ---------------------------------------------------------------------------

void CaptureService::reapFinishedWorkerLocked() {
    // Only join a thread whose run() has returned: joining a live worker
    // here would block the lifecycle mutex on hardware (grabFrame timeouts).
    if (thread_.joinable() && workerExited_.load(std::memory_order_acquire)) {
        thread_.join();
    }
}

void CaptureService::transition(CaptureLifecycleState state, uint64_t generation) {
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        // A transition from an old generation must not overwrite a newer
        // session's state (e.g. an old worker finishing after a restart).
        if (generation < snapshot_.generation) {
            return;
        }
        snapshot_.state = state;
        snapshot_.transitionHostTimeUs = Tools::getTimestamp();
        if (state != CaptureLifecycleState::Running) {
            snapshot_.cameraReady = false;
        }
    }
    lifecycleCv_.notify_all();
}

void CaptureService::recordFailure(CaptureFailureKind kind, const std::string& message,
                                   uint64_t generation) {
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    if (generation < snapshot_.generation) {
        return;
    }
    snapshot_.lastFailure = kind;
    snapshot_.lastFailureMessage = message;
    snapshot_.lastFailureGeneration = generation;
}

CaptureStartOutcome CaptureService::requestStart() {
    std::unique_lock<std::mutex> lk(lifecycleMutex_);
    switch (snapshot_.state) {
    case CaptureLifecycleState::Starting:
    case CaptureLifecycleState::Running:
        return CaptureStartOutcome::AlreadyActive;
    case CaptureLifecycleState::Stopping:
        // Another thread is inside stop() (it holds no lifecycle lock while
        // joining, so we can observe this). Refuse rather than race it.
        return CaptureStartOutcome::RejectedStopping;
    case CaptureLifecycleState::Idle:
    case CaptureLifecycleState::Faulted:
        break;
    }
    if (!cameraFactory_) {
        return CaptureStartOutcome::RejectedNoFactory;
    }
    // A Faulted session (natural worker exit) leaves thread_ joinable. Reap
    // it before assigning a new thread — assigning over a joinable
    // std::thread calls std::terminate (the #365 restart hazard).
    reapFinishedWorkerLocked();
    if (thread_.joinable()) {
        // Worker has not signalled exit yet (Faulted state is published just
        // before the final return). Wait briefly for it under the CV.
        lifecycleCv_.wait_for(lk, std::chrono::seconds(5), [this] {
            return workerExited_.load(std::memory_order_acquire);
        });
        reapFinishedWorkerLocked();
        if (thread_.joinable()) {
            SPDLOG_ERROR("CaptureService: previous worker still joinable after fault; refusing restart");
            return CaptureStartOutcome::RejectedStopping;
        }
    }

    const uint64_t generation = snapshot_.generation + 1;
    snapshot_.generation = generation;
    // Telemetry belongs to the session: every metric is Unavailable until
    // the new camera produces it (issue #368). Done here, synchronously, so
    // a snapshot taken right after start() never shows the previous session.
    resetSessionTelemetryLocked();
    snapshot_.state = CaptureLifecycleState::Starting;
    snapshot_.cameraReady = false;
    snapshot_.transitionHostTimeUs = Tools::getTimestamp();
    running_.store(true, std::memory_order_release);
    workerExited_.store(false, std::memory_order_release);
    thread_ = std::thread(&CaptureService::run, this, generation);
    return CaptureStartOutcome::Accepted;
}

bool CaptureService::start() {
    const auto outcome = requestStart();
    if (outcome == CaptureStartOutcome::RejectedNoFactory) {
        SPDLOG_ERROR("CaptureService::start rejected: no camera factory configured");
    } else if (outcome == CaptureStartOutcome::RejectedStopping) {
        SPDLOG_WARN("CaptureService::start rejected: a stop is still in progress");
    }
    return outcome == CaptureStartOutcome::Accepted ||
           outcome == CaptureStartOutcome::AlreadyActive;
}

void CaptureService::stop() {
    uint64_t generation = 0;
    bool needsTeardown = false;
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        generation = snapshot_.generation;
        const bool active = snapshot_.state == CaptureLifecycleState::Starting ||
                            snapshot_.state == CaptureLifecycleState::Running;
        if (active) {
            snapshot_.state = CaptureLifecycleState::Stopping;
            snapshot_.cameraReady = false;
            snapshot_.transitionHostTimeUs = Tools::getTimestamp();
            needsTeardown = true;
        }
        running_.store(false, std::memory_order_release);
    }
    if (needsTeardown) {
        lifecycleCv_.notify_all();
        // Stop the trigger thread BEFORE tearing the camera down: it calls
        // ICamera::setTriggerOutput and must not race the grabber teardown.
        // releaseCamera() on the capture thread repeats this call later; both
        // the callback and TriggerService::stop() are idempotent.
        if (cameraReadyCallback_) {
            cameraReadyCallback_(nullptr, generation);
        }
        std::scoped_lock lk(cameraMutex_);
        if (activeCamera_) {
            activeCamera_->stop();
        }
    }
    // Join outside every lock: the worker takes lifecycleMutex_ for its own
    // transitions on the way out.
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        if (thread_.get_id() != std::this_thread::get_id()) {
            // Worker fully reaped: explicit stop always ends in Idle (the
            // failure record, if any, survives for diagnostics).
            snapshot_.state = CaptureLifecycleState::Idle;
            snapshot_.cameraReady = false;
            snapshot_.transitionHostTimeUs = Tools::getTimestamp();
        }
    }
    lifecycleCv_.notify_all();
}

bool CaptureService::isRunning() const {
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    return snapshot_.isActive();
}

CaptureLifecycleSnapshot CaptureService::lifecycleSnapshot() const {
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    return snapshot_;
}

CaptureLifecycleState CaptureService::waitForState(
    std::initializer_list<CaptureLifecycleState> states,
    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lk(lifecycleMutex_);
    auto matches = [&] {
        for (auto s : states) {
            if (snapshot_.state == s) return true;
        }
        return false;
    };
    lifecycleCv_.wait_for(lk, timeout, matches);
    return snapshot_.state;
}

bool CaptureService::softTriggerActiveCamera() {
    std::scoped_lock lk(cameraMutex_);
    return activeCamera_ && activeCamera_->softTrigger();
}

// ---------------------------------------------------------------------------
// Telemetry (issue #368)
// ---------------------------------------------------------------------------

void CaptureService::resetSessionTelemetryLocked() {
    const int unavailable = static_cast<int>(MetricValidity::Unavailable);
    stats_.framesProcessed.store(0, std::memory_order_relaxed);
    stats_.lastFrameRate.store(0, std::memory_order_relaxed);
    stats_.lastDataRateMBps.store(0, std::memory_order_relaxed);
    stats_.intentionallyDiscardedFrames.store(0, std::memory_order_relaxed);
    stats_.transportLostFrames.store(0, std::memory_order_relaxed);
    stats_.bufferUnderruns.store(0, std::memory_order_relaxed);
    stats_.sdkCompletedQueueDepth.store(0, std::memory_order_relaxed);
    stats_.sdkInputBufferCount.store(0, std::memory_order_relaxed);
    stats_.queueStatsValid.store(false, std::memory_order_release);
    stats_.lastFrameAgeUs.store(0, std::memory_order_relaxed);
    stats_.frameAgeValid.store(false, std::memory_order_relaxed);
    stats_.lastPublishLatencyUs.store(0, std::memory_order_relaxed);
    for (auto* slot : {&stats_.frameRateValidity, &stats_.queueDepthValidity,
                       &stats_.inputBuffersValidity, &stats_.underrunsValidity,
                       &stats_.transportLossValidity, &stats_.discardsValidity}) {
        slot->store(unavailable, std::memory_order_relaxed);
    }
    for (auto* t : {&stats_.frameRateSampledUs, &stats_.queueStatsSampledUs,
                    &stats_.frameAgeSampledUs, &stats_.publishLatencySampledUs,
                    &stats_.framesSampledUs}) {
        t->store(0, std::memory_order_relaxed);
    }
    timestampDescriptor_ = {};
    timestampDescriptor_.validity = ::camera::common::TimestampValidity::Unavailable;
}

::camera::common::TimestampDescriptor CaptureService::timestampDescriptor() const {
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    return timestampDescriptor_;
}

AcquisitionTelemetrySnapshot CaptureService::telemetrySnapshot(uint64_t freshnessWindowUs) const {
    AcquisitionTelemetrySnapshot t;
    const uint64_t now = Tools::getTimestamp();
    t.snapshotHostTimeUs = now;
    t.freshnessWindowUs = freshnessWindowUs;
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        t.sessionGeneration = snapshot_.generation;
        t.sessionActive = snapshot_.isActive();
        t.timestampDescriptor = timestampDescriptor_;
    }
    auto sample = [&](uint64_t value, int validityInt, uint64_t sampledUs) {
        MetricSample m;
        m.value = value;
        m.validity = static_cast<MetricValidity>(validityInt);
        m.sampleHostTimeUs = sampledUs;
        m.sessionGeneration = t.sessionGeneration;
        m.ageUs = (sampledUs > 0 && now >= sampledUs) ? now - sampledUs : 0;
        if (m.validity == MetricValidity::Valid && sampledUs > 0 && freshnessWindowUs > 0 &&
            m.ageUs > freshnessWindowUs) {
            m.validity = MetricValidity::Stale;
        }
        return m;
    };
    const int valid = static_cast<int>(MetricValidity::Valid);
    const int unavailable = static_cast<int>(MetricValidity::Unavailable);
    const uint64_t framesAt = stats_.framesSampledUs.load(std::memory_order_relaxed);
    t.framesDelivered = sample(stats_.framesProcessed.load(std::memory_order_relaxed),
                               framesAt > 0 ? valid : unavailable, framesAt);
    t.captureFrameRate = sample(stats_.lastFrameRate.load(std::memory_order_relaxed),
                                stats_.frameRateValidity.load(std::memory_order_relaxed),
                                stats_.frameRateSampledUs.load(std::memory_order_relaxed));
    t.captureDataRateMBps = sample(stats_.lastDataRateMBps.load(std::memory_order_relaxed),
                                   stats_.frameRateValidity.load(std::memory_order_relaxed),
                                   stats_.frameRateSampledUs.load(std::memory_order_relaxed));
    const uint64_t qAt = stats_.queueStatsSampledUs.load(std::memory_order_relaxed);
    t.sdkCompletedQueueDepth = sample(stats_.sdkCompletedQueueDepth.load(std::memory_order_relaxed),
                                      stats_.queueDepthValidity.load(std::memory_order_relaxed), qAt);
    t.sdkInputBufferCount = sample(stats_.sdkInputBufferCount.load(std::memory_order_relaxed),
                                   stats_.inputBuffersValidity.load(std::memory_order_relaxed), qAt);
    t.bufferUnderruns = sample(stats_.bufferUnderruns.load(std::memory_order_relaxed),
                               stats_.underrunsValidity.load(std::memory_order_relaxed), qAt);
    t.transportLostFrames = sample(stats_.transportLostFrames.load(std::memory_order_relaxed),
                                   stats_.transportLossValidity.load(std::memory_order_relaxed), qAt);
    t.intentionallyDiscardedFrames = sample(stats_.intentionallyDiscardedFrames.load(std::memory_order_relaxed),
                                            stats_.discardsValidity.load(std::memory_order_relaxed), qAt);
    const uint64_t ageAt = stats_.frameAgeSampledUs.load(std::memory_order_relaxed);
    t.frameAgeUs = sample(stats_.lastFrameAgeUs.load(std::memory_order_relaxed),
                          stats_.frameAgeValid.load(std::memory_order_relaxed)
                              ? valid
                              : (t.timestampDescriptor.domain == ::camera::common::ClockDomain::HostMonotonicUs
                                     ? unavailable
                                     : static_cast<int>(MetricValidity::Unsupported)),
                          ageAt);
    const uint64_t pubAt = stats_.publishLatencySampledUs.load(std::memory_order_relaxed);
    t.publishLatencyUs = sample(stats_.lastPublishLatencyUs.load(std::memory_order_relaxed),
                                pubAt > 0 ? valid : unavailable, pubAt);
    return t;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void CaptureService::run(uint64_t generation) {
    std::unique_ptr<::camera::common::ICamera> camera;
    bool cameraStarted = false;

    auto releaseCamera = [&]() {
        // Without a running camera there is no confirmed mode; consumers
        // (status-bar badge) fall back to the requested config mode, so a mode
        // changed between runs shows up immediately as "(requested)".
        stats_.deliveryModeConfirmed.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            if (generation == snapshot_.generation) {
                snapshot_.cameraReady = false;
            }
            timestampDescriptor_.validity = ::camera::common::TimestampValidity::Unavailable;
        }
        // Consumers release their camera reference while it is still valid.
        if (cameraReadyCallback_) {
            cameraReadyCallback_(nullptr, generation);
        }
        if (camera) {
            camera->stop();
        }
        {
            std::scoped_lock lk(cameraMutex_);
            activeCamera_ = nullptr;
        }
        camera.reset();
    };

    // Terminal bookkeeping shared by every exit path. `faulted` marks a
    // natural (unrequested) exit; an explicit stop() owns the Idle transition.
    auto finish = [&](bool faulted) {
        releaseCamera();
        const bool stopRequested = !running_.exchange(false, std::memory_order_acq_rel);
        backend::diagnostics::CrashStateMirror::instance().capture.running.store(false);
        if (faulted && !stopRequested) {
            transition(CaptureLifecycleState::Faulted, generation);
        } else if (!stopRequested) {
            // Clean natural end (camera closed its own stream) without a stop
            // request is still a fault for the lifecycle: the session ended
            // without its owner asking.
            transition(CaptureLifecycleState::Faulted, generation);
        }
        // else: stop() is in flight and will publish Idle after the join.
        workerExited_.store(true, std::memory_order_release);
        lifecycleCv_.notify_all();
    };

    try {
        SPDLOG_INFO("CaptureService starting: gen={}, parts={}, buffers={}, mode={}",
                    generation, config_.bufferPartCount, config_.numBuffers,
                    ::camera::common::toString(config_.deliveryMode));
        stats_.requestedDeliveryMode.store(static_cast<int>(config_.deliveryMode),
                                           std::memory_order_relaxed);
        stats_.deliveryModeConfirmed.store(false, std::memory_order_release);
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().capture;
            m.running.store(true, std::memory_order_relaxed);
            m.bufferPartCount.store(config_.bufferPartCount, std::memory_order_relaxed);
            m.numBuffers.store(config_.numBuffers, std::memory_order_relaxed);
        }

        if (!cameraFactory_) {
            recordFailure(CaptureFailureKind::NoCameraFactory,
                          "CaptureService has no camera factory configured", generation);
            throw std::runtime_error("CaptureService has no camera factory configured");
        }

        camera = cameraFactory_();
        if (!camera) {
            recordFailure(CaptureFailureKind::CameraFactoryReturnedNull,
                          "CaptureService camera factory returned null", generation);
            throw std::runtime_error("CaptureService camera factory returned null");
        }

        const auto deliveryCaps = camera->deliveryCapabilities();
        if (config_.deliveryMode == ::camera::common::FrameDeliveryMode::LatestFrame &&
            !deliveryCaps.supportsLatestFrame) {
            recordFailure(CaptureFailureKind::UnsupportedDeliveryMode,
                          "This camera backend does not support Latest Frame delivery",
                          generation);
            throw std::runtime_error(
                "This camera backend does not support Latest Frame delivery; "
                "select Every Frame or use a backend with newest-frame support");
        }
        if (config_.deliveryMode == ::camera::common::FrameDeliveryMode::EveryFrame &&
            !deliveryCaps.supportsEveryFrame) {
            recordFailure(CaptureFailureKind::UnsupportedDeliveryMode,
                          "This camera backend does not support Every Frame delivery",
                          generation);
            throw std::runtime_error(
                "This camera backend does not support Every Frame delivery; "
                "select Latest Frame");
        }

        ::camera::common::CameraConfig camCfg;
        camCfg.bufferPartCount = config_.bufferPartCount;
        camCfg.numBuffers = config_.numBuffers;
        camCfg.deliveryMode = config_.deliveryMode;
        camera->applyConfig(camCfg);

        {
            std::scoped_lock lk(cameraMutex_);
            activeCamera_ = camera.get();
        }

        // A stop() that landed while we were opening must win: never confirm
        // readiness for a session whose owner already asked it to end.
        if (!running_.load(std::memory_order_acquire)) {
            finish(false);
            return;
        }

        if (!camera->start()) {
            const auto fault = camera->lastFailure();
            recordFailure(CaptureFailureKind::CameraStartFailed,
                          fault.message.empty() ? "CaptureService camera failed to start"
                                                : fault.message,
                          generation);
            throw std::runtime_error("CaptureService camera failed to start");
        }
        cameraStarted = true;

        stats_.activeDeliveryMode.store(static_cast<int>(camera->activeDeliveryMode()),
                                        std::memory_order_relaxed);
        stats_.deliveryModeConfirmed.store(true, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            timestampDescriptor_ = camera->timestampDescriptor();
            timestampDescriptor_.sessionGeneration = generation;
            if (generation == snapshot_.generation &&
                snapshot_.state == CaptureLifecycleState::Starting) {
                snapshot_.state = CaptureLifecycleState::Running;
                snapshot_.cameraReady = true;
                snapshot_.lastFailure = CaptureFailureKind::None;
                snapshot_.lastFailureMessage.clear();
                snapshot_.transitionHostTimeUs = Tools::getTimestamp();
            }
        }
        lifecycleCv_.notify_all();

        if (cameraReadyCallback_ && running_.load(std::memory_order_acquire)) {
            cameraReadyCallback_(camera.get(), generation);
        }

        constexpr uint64_t kStatsInterval = 1'000'000ULL;
        constexpr uint64_t kHealthCheckInterval = 5'000'000ULL;  // 5 seconds
        uint64_t nextStatsPoll = Tools::getTimestamp() + kStatsInterval;
        uint64_t nextHealthCheck = Tools::getTimestamp() + kHealthCheckInterval;

        // Largest frame size reserved in the FIFO so far this session. Drives a
        // one-time (per geometry) pre-reservation so the high-speed push hot path
        // never allocates.
        size_t reservedFrameBytes = 0;

        // Declared outside the loop so grabFrame's data.assign reuses the
        // vector's capacity frame-to-frame (pushFrame copies out of it) —
        // at triggered rates of 5000 fps a per-iteration Frame would malloc
        // the pixel buffer every 200 µs.
        ::camera::common::Frame frame;
        bool faulted = false;

        while (running_.load()) {
            // Periodic health check
            const uint64_t now = Tools::getTimestamp();
            if (now >= nextHealthCheck) {
                if (!camera->checkDeviceHealth()) {
                    SPDLOG_WARN("CaptureService: Device health check failed, stopping capture gracefully");
                    recordFailure(CaptureFailureKind::DeviceHealthLost,
                                  "Device health check failed", generation);
                    faulted = true;
                    break;
                }
                nextHealthCheck = now + kHealthCheckInterval;
            }

            const bool grabbed = camera->grabFrame(frame);
            // Host monotonic acquisition stamp: the anchor every downstream
            // latency measurement (algo start/end, trigger fire) compares
            // against. The frame's own `timestamp` is a device tick and lives
            // on a different clock — unless the backend has verified the
            // domains match (timestampsHostComparable), in which case the
            // difference is the frame's age in the SDK queues.
            frame.hostTimestampUs = Tools::getTimestamp();
            if (grabbed && deliveryCaps.timestampsHostComparable &&
                frame.timestamp <= frame.hostTimestampUs) {
                stats_.lastFrameAgeUs.store(frame.hostTimestampUs - frame.timestamp,
                                            std::memory_order_relaxed);
                stats_.frameAgeValid.store(true, std::memory_order_relaxed);
                stats_.frameAgeSampledUs.store(frame.hostTimestampUs, std::memory_order_relaxed);
            }
            if (!grabbed) {
                if (!running_.load()) {
                    break;
                }
                if (!camera->isRunning()) {
                    const auto fault = camera->lastFailure();
                    SPDLOG_INFO("CaptureService camera stopped streaming ({})",
                                fault.message.empty() ? "no detail" : fault.message);
                    recordFailure(CaptureFailureKind::StreamEnded,
                                  fault.message.empty() ? "Camera stopped streaming"
                                                        : fault.message,
                                  generation);
                    faulted = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (callback_) {
                callback_(frame.data.data(),
                          frame.data.size(),
                          frame.width,
                          frame.height,
                          frame.timestamp);
            }
            if (frameStore_) {
                // Pre-reserve FIFO slots to the live frame size so the
                // high-speed hot path never allocates. First reservation happens
                // on frame 1; re-reserve only if the geometry grows.
                const size_t frameBytes = frame.data.size();
                if (frameBytes > reservedFrameBytes) {
                    frameStore_->reserveFrameBytes(frameBytes);
                    reservedFrameBytes = frameBytes;
                }
                frameStore_->pushFrame(frame.data.data(),
                                       frame.data.size(),
                                       frame.width,
                                       frame.height,
                                       frame.linePitch,
                                       frame.pixelFormat,
                                       frame.timestamp,
                                       frame.hostTimestampUs);
            }
            {
                const uint64_t published = Tools::getTimestamp();
                stats_.lastPublishLatencyUs.store(published - frame.hostTimestampUs,
                                                  std::memory_order_relaxed);
                stats_.publishLatencySampledUs.store(published, std::memory_order_relaxed);
                stats_.framesSampledUs.store(published, std::memory_order_relaxed);
            }
            stats_.framesProcessed.fetch_add(1, std::memory_order_relaxed);
            backend::diagnostics::CrashStateMirror::instance().capture.framesProcessed
                .fetch_add(1, std::memory_order_relaxed);

            if (now >= nextStatsPoll) {
                ::camera::common::CameraStats cameraStats{};
                if (camera->pollStats(cameraStats)) {
                    stats_.lastFrameRate.store(cameraStats.frameRate, std::memory_order_relaxed);
                    stats_.lastDataRateMBps.store(cameraStats.dataRateMBps, std::memory_order_relaxed);
                    stats_.frameRateValidity.store(static_cast<int>(MetricValidity::Valid),
                                                   std::memory_order_relaxed);
                    stats_.frameRateSampledUs.store(now, std::memory_order_relaxed);
                    auto& m = backend::diagnostics::CrashStateMirror::instance().capture;
                    m.lastFrameRate.store(cameraStats.frameRate, std::memory_order_relaxed);
                    m.lastDataRateMBps.store(cameraStats.dataRateMBps, std::memory_order_relaxed);
                    SPDLOG_DEBUG("Capture stats: {} fps, {} MB/s", cameraStats.frameRate, cameraStats.dataRateMBps);
                } else {
                    stats_.frameRateValidity.store(static_cast<int>(MetricValidity::Error),
                                                   std::memory_order_relaxed);
                }
                ::camera::common::AcquisitionQueueStats queueStats{};
                if (camera->pollAcquisitionQueueStats(queueStats)) {
                    // Per-metric validity (issue #368): a field the backend
                    // cannot observe is Unsupported, never a measured zero.
                    auto setV = [&](std::atomic<int>& slot, bool valid) {
                        slot.store(static_cast<int>(valid ? MetricValidity::Valid
                                                          : MetricValidity::Unsupported),
                                   std::memory_order_relaxed);
                    };
                    setV(stats_.queueDepthValidity, queueStats.completedQueueDepthValid);
                    setV(stats_.inputBuffersValidity, queueStats.inputBufferCountValid);
                    setV(stats_.underrunsValidity, queueStats.underrunsValid);
                    setV(stats_.transportLossValidity, queueStats.transportLossValid);
                    // Intentional discards are always observable on a backend
                    // that returns queue stats (exact on drain paths).
                    setV(stats_.discardsValidity, true);
                    stats_.queueStatsSampledUs.store(now, std::memory_order_relaxed);
                    stats_.intentionallyDiscardedFrames.store(
                        queueStats.intentionallyDiscardedFrames, std::memory_order_relaxed);
                    stats_.transportLostFrames.store(queueStats.transportLostFrames,
                                                     std::memory_order_relaxed);
                    stats_.bufferUnderruns.store(queueStats.bufferUnderruns,
                                                 std::memory_order_relaxed);
                    stats_.sdkCompletedQueueDepth.store(queueStats.sdkCompletedQueueDepth,
                                                        std::memory_order_relaxed);
                    stats_.sdkInputBufferCount.store(queueStats.sdkInputBufferCount,
                                                     std::memory_order_relaxed);
                    stats_.queueStatsValid.store(true, std::memory_order_release);
                    // In EveryFrame mode a growing completed-buffer backlog is
                    // hidden latency; surface it (rate-limited to this 1 s
                    // stats interval).
                    if (config_.deliveryMode == ::camera::common::FrameDeliveryMode::EveryFrame &&
                        queueStats.completedQueueDepthValid &&
                        queueStats.sdkCompletedQueueDepth > 0) {
                        SPDLOG_WARN("CaptureService: {} completed SDK buffers backlogged in "
                                    "EveryFrame mode (latency is growing)",
                                    queueStats.sdkCompletedQueueDepth);
                    }
                } else {
                    for (auto* slot : {&stats_.queueDepthValidity, &stats_.inputBuffersValidity,
                                       &stats_.underrunsValidity, &stats_.transportLossValidity,
                                       &stats_.discardsValidity}) {
                        slot->store(static_cast<int>(MetricValidity::Unsupported),
                                    std::memory_order_relaxed);
                    }
                }
                nextStatsPoll = now + kStatsInterval;
            }
        }

        if (camera) {
            ::camera::common::CameraStats cameraStats{};
            if (camera->pollStats(cameraStats)) {
                stats_.lastFrameRate.store(cameraStats.frameRate, std::memory_order_relaxed);
                stats_.lastDataRateMBps.store(cameraStats.dataRateMBps, std::memory_order_relaxed);
            }
        }

        finish(faulted);
        SPDLOG_INFO("CaptureService stopped (gen={}, {})", generation,
                    faulted ? "faulted" : "requested");
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("CaptureService exception: {}", ex.what());
        if (!cameraStarted) {
            // recordFailure already ran for the typed pre-start failures;
            // anything else is an unexpected exception.
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            if (snapshot_.lastFailureGeneration != generation) {
                snapshot_.lastFailure = CaptureFailureKind::Exception;
                snapshot_.lastFailureMessage = ex.what();
                snapshot_.lastFailureGeneration = generation;
            }
        } else {
            recordFailure(CaptureFailureKind::Exception, ex.what(), generation);
        }
        finish(true);
    } catch (...) {
        SPDLOG_ERROR("CaptureService unknown exception");
        recordFailure(CaptureFailureKind::Exception, "unknown exception", generation);
        finish(true);
    }
}

} // namespace backend::services
