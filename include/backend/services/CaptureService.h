#pragma once

#include "backend/camera/common/ICamera.h"
#include "backend/services/CaptureLifecycle.h"
#include "backend/services/TelemetrySample.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace backend { namespace playback { class FrameStore; } }

namespace backend::services {

struct CaptureStats {
    std::atomic<uint64_t> framesProcessed{0};
    std::atomic<uint64_t> lastFrameRate{0};     // from StreamModule StatisticsFrameRate
    std::atomic<uint64_t> lastDataRateMBps{0};  // from StreamModule StatisticsDataRate

    // Delivery mode + acquisition-queue telemetry. Modes are stored as the
    // integer value of ::camera::common::FrameDeliveryMode so the struct stays
    // lock-free; deliveryModeConfirmed distinguishes the backend-confirmed
    // active mode from the merely requested one.
    std::atomic<int> requestedDeliveryMode{0};
    std::atomic<int> activeDeliveryMode{0};
    std::atomic<bool> deliveryModeConfirmed{false};
    std::atomic<uint64_t> intentionallyDiscardedFrames{0};
    std::atomic<uint64_t> transportLostFrames{0};
    std::atomic<uint64_t> bufferUnderruns{0};
    std::atomic<uint64_t> sdkCompletedQueueDepth{0};
    std::atomic<uint64_t> sdkInputBufferCount{0};
    // Aggregate compatibility flag: true when the backend returned ANY queue
    // telemetry. Prefer the per-metric validity below (issue #368): a metric
    // this backend cannot observe stays Unsupported/Unavailable even while
    // this flag is true, so "unknown" is never read as a measured zero.
    std::atomic<bool> queueStatsValid{false};
    // Age of the last grabbed frame (device stamp -> host dequeue), only
    // populated when the backend reports timestampsHostComparable.
    std::atomic<uint64_t> lastFrameAgeUs{0};
    std::atomic<bool> frameAgeValid{false};
    // Host dequeue -> post-publish (callback + FrameStore copy) duration.
    std::atomic<uint64_t> lastPublishLatencyUs{0};

    // Per-metric validity (MetricValidity as int) and sample host time
    // (Tools::getTimestamp µs; 0 = never sampled). Reset at every session
    // start so a reconnect cannot inherit the previous session's values.
    std::atomic<int> frameRateValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<uint64_t> frameRateSampledUs{0};
    std::atomic<int> queueDepthValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<int> inputBuffersValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<int> underrunsValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<int> transportLossValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<int> discardsValidity{static_cast<int>(MetricValidity::Unavailable)};
    std::atomic<uint64_t> queueStatsSampledUs{0};
    std::atomic<uint64_t> frameAgeSampledUs{0};
    std::atomic<uint64_t> publishLatencySampledUs{0};
    std::atomic<uint64_t> framesSampledUs{0};
};

class CaptureService {
public:
    using FrameCallback = std::function<void(const uint8_t* data,
                                             size_t size,
                                             uint64_t width,
                                             uint64_t height,
                                             uint64_t timestampNs)>;

    using CameraFactory = std::function<std::unique_ptr<::camera::common::ICamera>()>;

    struct Config {
        int bufferPartCount = 1;  // number of images per buffer
        int numBuffers = 20;        // ring size
        // Requested SDK-queue policy; backends confirm or reject at start().
        ::camera::common::FrameDeliveryMode deliveryMode =
            ::camera::common::FrameDeliveryMode::EveryFrame;
    };

    CaptureService();
    ~CaptureService();

    void setConfig(const Config& cfg);
    void setFrameCallback(FrameCallback cb);

    // Optional: store frames to a shared ring for playback/display
    void setFrameStore(std::shared_ptr<backend::playback::FrameStore> store);

    void setCameraFactory(CameraFactory factory);

    // Callback fired when camera starts (with pointer + session generation)
    // or stops (with nullptr). Fired on the capture thread and, for the
    // nullptr case, additionally on the thread calling stop() BEFORE the
    // camera is torn down — so a consumer (TriggerService) can release its
    // camera reference while the object is still valid.
    using CameraReadyCallback = std::function<void(::camera::common::ICamera*, uint64_t generation)>;
    void setCameraReadyCallback(CameraReadyCallback cb);

    // Backwards-compatible request API. `start()` returns true when a session
    // is scheduled or already active — it is NOT hardware readiness; consult
    // lifecycleSnapshot().cameraReady / state for that.
    bool start();
    void stop();
    bool isRunning() const;

    // Structured request API (issue #365). Serialized and idempotent: a
    // duplicate start while Starting/Running is AlreadyActive; a naturally
    // failed (Faulted) session is reaped and restarted.
    CaptureStartOutcome requestStart();
    // Authoritative lifecycle truth for UI/experiment gating.
    CaptureLifecycleSnapshot lifecycleSnapshot() const;
    // Blocks until the worker for the current generation has left
    // Starting/Running, or the timeout elapses. Returns the state reached.
    // Never called on the capture thread itself.
    CaptureLifecycleState waitForState(std::initializer_list<CaptureLifecycleState> states,
                                       std::chrono::milliseconds timeout) const;

    // Fire one software acquisition trigger on the live camera. Thread-safe;
    // returns false when no camera is active or it does not support it.
    // (Acquisition trigger — not the sort-output pulse.)
    bool softTriggerActiveCamera();

    const CaptureStats& stats() const { return stats_; }

    // Consistent per-metric telemetry view (issue #368): every metric carries
    // its own validity (Valid / Unavailable / Unsupported / Error / Stale),
    // sample time, age, and session generation. `freshnessWindowUs` marks a
    // Valid sample older than that as Stale. Between sessions every metric
    // is Unavailable (never a stale zero from the previous camera).
    AcquisitionTelemetrySnapshot telemetrySnapshot(uint64_t freshnessWindowUs = 3'000'000ULL) const;

    // Describes Frame::timestamp for the current/last session (from
    // ICamera::timestampDescriptor, tagged with the session generation).
    ::camera::common::TimestampDescriptor timestampDescriptor() const;

    // Backend-confirmed active mode (falls back to the requested mode until a
    // camera has confirmed one). Prefer this over Config::deliveryMode when
    // displaying state to the user.
    ::camera::common::FrameDeliveryMode activeDeliveryMode() const {
        return stats_.deliveryModeConfirmed.load(std::memory_order_acquire)
                   ? static_cast<::camera::common::FrameDeliveryMode>(
                         stats_.activeDeliveryMode.load(std::memory_order_acquire))
                   : config_.deliveryMode;
    }

private:
    void run(uint64_t generation);
    // Lifecycle transitions; lifecycleMutex_ must NOT be held by the caller.
    void transition(CaptureLifecycleState state, uint64_t generation);
    void recordFailure(CaptureFailureKind kind, const std::string& message, uint64_t generation);
    // Join a worker that has already exited (Faulted/Idle with a joinable
    // thread). lifecycleMutex_ must be held. Never joins a live worker.
    void reapFinishedWorkerLocked();
    // Set every telemetry metric to Unavailable and clear the descriptor at
    // the start of a session (issue #368). lifecycleMutex_ must be held.
    void resetSessionTelemetryLocked();

    Config config_{};
    FrameCallback callback_{};
    std::shared_ptr<backend::playback::FrameStore> frameStore_{};

    CameraFactory cameraFactory_{};
    CameraReadyCallback cameraReadyCallback_;
    ::camera::common::ICamera* activeCamera_{nullptr};
    std::mutex cameraMutex_;

    // Serializes start()/stop() and guards thread_/snapshot_. The worker
    // takes it only for short transitions, never while blocked in grabFrame.
    mutable std::mutex lifecycleMutex_;
    mutable std::condition_variable lifecycleCv_;
    std::thread thread_;
    // Worker's "keep looping" flag (cleared by stop() and by the worker on
    // natural exit). Distinct from the lifecycle state.
    std::atomic<bool> running_{false};
    // Set by the worker when it has fully exited run(); tells the lifecycle
    // owner that thread_ can be joined without blocking on hardware.
    std::atomic<bool> workerExited_{true};
    CaptureLifecycleSnapshot snapshot_{};

    CaptureStats stats_{};
    // Session timestamp descriptor (set when the camera opens, cleared on
    // release). Guarded by lifecycleMutex_.
    ::camera::common::TimestampDescriptor timestampDescriptor_{};
};

} // namespace backend::services
