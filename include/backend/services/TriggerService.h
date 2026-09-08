#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace camera::common {
class ICamera;
}

namespace backend::services {

struct TargetGroupSignal {
    bool isTargetGroup{false};
    int objectId{-1};
    int trackId{-1};
    // Source-frame identity for end-to-end latency correlation (see
    // PipelineTimingRecorder): FrameStore write index and host monotonic
    // acquisition stamp (microseconds; 0 if unknown).
    uint64_t frameIndex{0};
    uint64_t hostTimestampUs{0};
};

class TriggerService {
public:
    TriggerService();
    ~TriggerService();

    void start();
    void stop();

    // Bind the camera used for trigger output (called when a camera session
    // becomes available) or unbind with nullptr when it goes away.
    //
    // Session contract (issue #365): every call waits for any in-flight pulse
    // to finish, so once it returns the trigger thread can never touch the
    // previously bound camera again. Pending requests are cleared (counted in
    // getDroppedStaleRequestCount) because they belong to the old session.
    // `generation` tags the new session; requests enqueued under one
    // generation are refused at fire time if a different session is bound.
    // A zero generation auto-assigns the next internal value.
    void setCamera(::camera::common::ICamera* camera, uint64_t generation = 0);

    // Currently bound session generation (0 when no camera is bound).
    uint64_t boundGeneration() const { return boundGeneration_.load(std::memory_order_acquire); }

    // Called by ProcessingService callback when a frame has target-group ownership.
    // Metadata carries trigger owner identity; one trigger request is expected
    // per owning source frame.
    void onTargetGroupResult(const TargetGroupSignal& signal);

    // Pulse duration (microseconds)
    void setPulseDurationUs(int us) { pulseDurationUs_.store(us, std::memory_order_relaxed); }
    int getPulseDurationUs() const { return pulseDurationUs_.load(std::memory_order_relaxed); }

    // Metrics
    uint64_t getTriggerCount() const { return triggerCount_.load(std::memory_order_relaxed); }
    double getLastOnsetUs() const { return lastOnsetUs_.load(std::memory_order_relaxed); }
    int getLastTriggerObjectId() const {
        return lastTriggerObjectId_.load(std::memory_order_relaxed);
    }
    int getLastTriggerTrackId() const {
        return lastTriggerTrackId_.load(std::memory_order_relaxed);
    }
    // Requests evicted from a full pending queue (oldest dropped first). A
    // non-zero value means target-group frames arrived faster than pulses
    // could be driven for longer than the queue could absorb (issue #283).
    uint64_t getDroppedRequestCount() const {
        return droppedRequests_.load(std::memory_order_relaxed);
    }
    // Pulses that a dequeued request could not drive because no camera was
    // bound at fire time. A non-zero value means a target was identified for
    // sorting but the actuation hardware was absent — a silent sort loss until
    // this counter was added.
    uint64_t getDroppedPulsesNoCameraCount() const {
        return droppedPulsesNoCamera_.load(std::memory_order_relaxed);
    }
    // Pulses lost because the camera's setTriggerOutput(true) call failed. Same
    // meaning as above: a selected target never got its TTL edge.
    uint64_t getDroppedPulsesSetFailedCount() const {
        return droppedPulsesSetFailed_.load(std::memory_order_relaxed);
    }
    // Requests that belonged to a camera session other than the one bound
    // when they were dequeued (cleared on rebind, or generation mismatch at
    // fire time). They are never executed against the new session.
    uint64_t getDroppedStaleRequestCount() const {
        return droppedStaleRequests_.load(std::memory_order_relaxed);
    }
    // Total pulses lost after a request was dequeued (no-camera + set-failed).
    uint64_t getDroppedPulseCount() const {
        return droppedPulsesNoCamera_.load(std::memory_order_relaxed) +
               droppedPulsesSetFailed_.load(std::memory_order_relaxed);
    }

    void resetMetrics() {
        triggerCount_.store(0, std::memory_order_relaxed);
        lastOnsetUs_.store(0.0, std::memory_order_relaxed);
        lastTriggerObjectId_.store(-1, std::memory_order_relaxed);
        lastTriggerTrackId_.store(-1, std::memory_order_relaxed);
        droppedRequests_.store(0, std::memory_order_relaxed);
        droppedPulsesNoCamera_.store(0, std::memory_order_relaxed);
        droppedPulsesSetFailed_.store(0, std::memory_order_relaxed);
        droppedStaleRequests_.store(0, std::memory_order_relaxed);
    }

    // Bound on the pending-request queue. Sized to absorb a realistic burst
    // of same-window target frames; beyond it the OLDEST request is dropped
    // (and counted) — a backlog of stale pulses is worse than a counted drop.
    static constexpr size_t kMaxPendingRequests = 8;

private:
    void triggerLoop();

    // Serializes start()/stop() (they run on different threads: capture
    // thread via the camera-ready callback vs. GUI/shutdown) and guards
    // thread_. The trigger loop never takes it.
    std::mutex lifecycleMutex_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Trigger request signaling
    std::mutex triggerMutex_;
    std::condition_variable triggerCV_;

    // Held by the trigger thread for the whole duration of one pulse (camera
    // load -> rising edge -> busy wait -> falling edge) and by setCamera()
    // while swapping the bound camera. Guarantees no camera access after
    // setCamera() returns. Lock order: pulseMutex_ before triggerMutex_;
    // the loop never holds both.
    std::mutex pulseMutex_;

    // Per-request metadata for pulses and latency instrumentation
    // (PipelineTimingRecorder). Every target-group request gets its own entry
    // — and therefore its own pulse, in arrival order — instead of the old
    // single-bool flag that silently coalesced requests arriving while the
    // trigger thread was mid-pulse (issue #283). Bounded by
    // kMaxPendingRequests with counted drop-oldest overflow. Accessed only
    // under triggerMutex_.
    struct PendingRequest {
        uint64_t frameIndex{0};
        uint64_t hostTimestampUs{0};
        uint64_t requestUs{0};
        uint64_t generation{0}; // camera session the request was made under
    };
    std::deque<PendingRequest> pendingRequests_;

    // Camera reference (non-owning) + the session generation it belongs to.
    std::atomic<::camera::common::ICamera*> camera_{nullptr};
    std::atomic<uint64_t> boundGeneration_{0};
    std::atomic<uint64_t> autoGeneration_{0};

    // Pulse duration
    std::atomic<int> pulseDurationUs_{1};
    std::atomic<int> lastTriggerObjectId_{-1};
    std::atomic<int> lastTriggerTrackId_{-1};

    // Metrics
    std::atomic<uint64_t> triggerCount_{0};
    std::atomic<double> lastOnsetUs_{0.0};
    std::atomic<uint64_t> droppedRequests_{0};
    // Pulses lost after a request was already dequeued — the target was
    // selected but no TTL edge was driven. Split by cause so a missing camera
    // (setup/teardown race) is distinguishable from a hardware set failure.
    std::atomic<uint64_t> droppedPulsesNoCamera_{0};
    std::atomic<uint64_t> droppedPulsesSetFailed_{0};
    std::atomic<uint64_t> droppedStaleRequests_{0};
};

} // namespace backend::services
