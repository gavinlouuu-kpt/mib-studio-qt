#include "backend/services/TriggerService.h"
#include "backend/camera/common/ICamera.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include <spdlog/spdlog.h>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace backend::services {

namespace {

// The trigger thread's wake-up latency and busy-wait pulse timing are directly
// visible on the TTL line (LED/sort pulse onset + width). Elevate its
// scheduling priority so background load (e.g. the HDF5 writer flushing an
// experiment batch) cannot preempt a pending pulse. Best-effort: failure is
// logged and the thread runs at default priority.
void raiseTriggerThreadPriority() {
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        SPDLOG_WARN("TriggerService: failed to raise trigger thread priority (error={})",
                    GetLastError());
    } else {
        SPDLOG_INFO("TriggerService: trigger thread priority set to TIME_CRITICAL");
    }
#else
    sched_param sp{};
    sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        // Real-time scheduling normally needs elevated privileges on Linux;
        // dev/CI runs fall back to default priority.
        SPDLOG_DEBUG("TriggerService: real-time priority unavailable, using default");
    } else {
        SPDLOG_INFO("TriggerService: trigger thread scheduled SCHED_FIFO");
    }
#endif
}

} // namespace

TriggerService::TriggerService() = default;

TriggerService::~TriggerService() {
    stopPeriodicTest();
    stop();
}

void TriggerService::startPeriodicTest(int intervalMs) {
    if (intervalMs < 1) intervalMs = 1;
    periodicIntervalMs_.store(intervalMs, std::memory_order_relaxed);
    if (periodicRunning_.load()) return; // interval updated above; thread picks it up
    periodicRunning_.store(true);
    periodicThread_ = std::thread(&TriggerService::periodicLoop, this);
    SPDLOG_INFO("TriggerService periodic test started ({} ms)", intervalMs);
}

void TriggerService::stopPeriodicTest() {
    if (!periodicRunning_.load()) return;
    // Same lost-notify guard as stop(): flip the flag under the wait mutex.
    {
        std::lock_guard<std::mutex> lk(periodicMutex_);
        periodicRunning_.store(false);
    }
    periodicCv_.notify_all();
    if (periodicThread_.joinable()) periodicThread_.join();
    SPDLOG_INFO("TriggerService periodic test stopped");
}

void TriggerService::periodicLoop() {
    while (periodicRunning_.load()) {
        {
            std::unique_lock<std::mutex> lk(periodicMutex_);
            periodicCv_.wait_for(
                lk,
                std::chrono::milliseconds(periodicIntervalMs_.load(std::memory_order_relaxed)),
                [this] { return !periodicRunning_.load(); });
        }
        if (!periodicRunning_.load()) break;
        manualPulse();
    }
}

void TriggerService::start() {
    // start() runs on the capture thread (camera-ready callback) while stop()
    // may run on the GUI thread: thread_ itself needs a lock (issue #365).
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (running_.load()) return;
    if (thread_.joinable()) {
        // A previous loop that exited is reaped before a new one is assigned.
        thread_.join();
    }
    running_.store(true);
    backend::diagnostics::CrashStateMirror::instance().trigger.running.store(true);
    thread_ = std::thread(&TriggerService::triggerLoop, this);
    SPDLOG_INFO("TriggerService started");
}

void TriggerService::stop() {
    // The periodic test generator feeds this service; stop it first so no
    // synthetic pulses arrive during (or after) teardown.
    stopPeriodicTest();
    // The trigger loop never takes lifecycleMutex_, so joining under it is
    // deadlock-free; it serializes stop() against a concurrent start().
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (!running_.load()) return;
    // Clear running_ while holding triggerMutex_ (the mutex the trigger thread
    // holds when evaluating its wait predicate) before notifying. Storing it
    // lock-free races with the loop's wait(): if the thread has just been
    // started and checks the predicate (running_ == true) but has not yet
    // blocked, a lock-free store+notify here lands in that gap and is lost, so
    // the thread blocks forever and this join() deadlocks. Under rapid
    // start/stop (e.g. capture restart) that hangs capture shutdown. Taking the
    // lock closes the window.
    {
        std::lock_guard<std::mutex> lk(triggerMutex_);
        running_.store(false);
    }
    triggerCV_.notify_all();
    if (thread_.joinable()) thread_.join();
    backend::diagnostics::CrashStateMirror::instance().trigger.running.store(false);
    SPDLOG_INFO("TriggerService stopped");
}

void TriggerService::setCamera(camera::common::ICamera* camera, uint64_t generation) {
    // Wait for any in-flight pulse: after this returns the trigger thread
    // holds no reference to the previous camera (issue #365).
    std::lock_guard<std::mutex> pulseLock(pulseMutex_);
    // The previous camera is still alive here (contract: unbind before
    // destroy), so its loopback subscription can be released.
    if (auto* previous = camera_.load(std::memory_order_acquire); previous && previous != camera) {
        previous->setLineEventCallback({});
    }
    const uint64_t newGeneration =
        camera ? (generation != 0 ? generation
                                  : autoGeneration_.fetch_add(1, std::memory_order_relaxed) + 1)
               : 0;
    size_t cleared = 0;
    {
        std::lock_guard<std::mutex> lk(triggerMutex_);
        // Requests made under the previous session must not fire on the new
        // one (or on nothing): clear and count them.
        cleared = pendingRequests_.size();
        pendingRequests_.clear();
        camera_.store(camera, std::memory_order_release);
        boundGeneration_.store(newGeneration, std::memory_order_release);
    }
    if (cleared > 0) {
        droppedStaleRequests_.fetch_add(cleared, std::memory_order_relaxed);
        SPDLOG_INFO("TriggerService: cleared {} pending request(s) from previous camera session",
                    cleared);
    }
    if (camera) {
        camera->configureTriggerOutput("TTLIO12");
        // Loopback edges (when the backend stamps its inputs) pair with fired
        // pulses in the event log; a backend without that capability returns
        // false and lineEdge* stay 0 in every record.
        const bool stamped = camera->setLineEventCallback(
            [this](const ::camera::common::LineEvent& ev) { onLineEvent(ev); });
        SPDLOG_INFO("TriggerService: line-event loopback {}",
                    stamped ? "subscribed" : "unavailable on this camera");
    }
    {
        // A new session starts with no unpaired pulses; edges from the old
        // session must not attach to new pulses.
        std::lock_guard<std::mutex> ek(eventMutex_);
        unpairedFired_.clear();
        unpairedLineEdges_.fetch_add(pendingEdges_.size(), std::memory_order_relaxed);
        pendingEdges_.clear();
    }
}

void TriggerService::onTargetGroupResult(const TargetGroupSignal& signal) {
    if (!signal.isTargetGroup) {
        return;
    }

    SPDLOG_DEBUG("TriggerService target-group callback: objectId={}, trackId={}", signal.objectId,
                 signal.trackId);

    lastTriggerObjectId_.store(signal.objectId, std::memory_order_release);
    lastTriggerTrackId_.store(signal.trackId, std::memory_order_release);
    // Always stamped (one clock read): the event log needs request time even
    // without the diagnostics recorder.
    const uint64_t requestUs = backend::diagnostics::PipelineTimingRecorder::nowUs();
    // Enqueue while holding the same mutex the trigger thread uses for its
    // wait() predicate. Mutating the queue lock-free races with the consumer's
    // predicate check: if the entry lands after the consumer evaluates the
    // predicate but before it blocks, the notify is lost and the trigger is
    // delayed until the next request. Taking the lock closes that window.
    // Every request gets its own queue entry — and its own pulse, in arrival
    // order (issue #283); the old single-bool flag silently coalesced
    // requests arriving while the trigger thread was mid-pulse. Overflow
    // drops the OLDEST entry (a backlog of stale pulses is worse than a
    // counted drop) and is surfaced via getDroppedRequestCount().
    bool evicted = false;
    PendingRequest evictedRequest;
    {
        std::lock_guard<std::mutex> lk(triggerMutex_);
        if (pendingRequests_.size() >= kMaxPendingRequests) {
            evictedRequest = pendingRequests_.front();
            evicted = true;
            pendingRequests_.pop_front();
            const uint64_t dropped = droppedRequests_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dropped == 1 || (dropped % 100) == 0) {
                SPDLOG_WARN("TriggerService: pending-request queue full ({}), dropped oldest "
                            "request (total dropped: {})",
                            kMaxPendingRequests, dropped);
            }
        }
        pendingRequests_.push_back(PendingRequest{signal.frameIndex, signal.hostTimestampUs,
                                                  requestUs,
                                                  boundGeneration_.load(std::memory_order_acquire),
                                                  signal.objectId, signal.trackId});
    }
    triggerCV_.notify_one();
    if (evicted) {
        // The evicted target never reaches the loop: log it here so the
        // record set stays one-per-request.
        recordEvent(makeEvent(evictedRequest, backend::recording::TriggerOutcome::DroppedQueueFull,
                              0, 0, 0));
    }
}

backend::recording::TriggerEventRecord
TriggerService::makeEvent(const PendingRequest& req, backend::recording::TriggerOutcome outcome,
                          uint64_t wakeUs, uint64_t fireUs, uint64_t pulseDoneUs) {
    backend::recording::TriggerEventRecord r;
    r.frameIndex = req.frameIndex;
    r.grabUs = req.hostTimestampUs;
    r.objectId = req.objectId;
    r.trackId = req.trackId;
    r.generation = req.generation;
    r.requestUs = req.requestUs;
    r.wakeUs = wakeUs;
    r.fireUs = fireUs;
    r.pulseDoneUs = pulseDoneUs;
    r.outcome = static_cast<uint8_t>(outcome);
    return r;
}

void TriggerService::recordEvent(backend::recording::TriggerEventRecord record) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    record.sequence = nextSequence_++;
    if (events_.size() >= kMaxBufferedEvents) {
        events_.pop_front();
        const uint64_t dropped = droppedEvents_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dropped == 1 || (dropped % 1000) == 0) {
            SPDLOG_WARN("TriggerService: event log full ({}), dropped oldest record "
                        "(total dropped: {}) — is the experiment flush draining it?",
                        kMaxBufferedEvents, dropped);
        }
    }
    if (record.outcome == static_cast<uint8_t>(backend::recording::TriggerOutcome::Fired)) {
        // A held edge belongs to this pulse only if it was delivered after
        // the pulse woke (same host clock as wakeUs); an older one is a
        // leftover from a pulse whose record was drained first — count it
        // rather than attach it to the wrong pulse. An edge without a host
        // stamp cannot be dated and is attached (synchronous-loopback
        // assumption).
        while (!pendingEdges_.empty() && pendingEdges_.front().hostTimestampUs != 0 &&
               pendingEdges_.front().hostTimestampUs < record.wakeUs) {
            pendingEdges_.pop_front();
            unpairedLineEdges_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!pendingEdges_.empty()) {
            // Edge delivered before the record existed (synchronous loopback).
            record.lineEdgeTimestamp = pendingEdges_.front().timestamp;
            record.lineEdgeHostUs = pendingEdges_.front().hostTimestampUs;
            pendingEdges_.pop_front();
        } else {
            if (unpairedFired_.size() >= kMaxUnpairedFired) unpairedFired_.pop_front();
            unpairedFired_.push_back(record.sequence);
        }
    }
    events_.push_back(record);
}

void TriggerService::onLineEvent(const ::camera::common::LineEvent& event) {
    if (!event.rising) return;
    std::lock_guard<std::mutex> lk(eventMutex_);
    // FIFO pairing: the pulses were driven in order and the edges arrive in
    // order, so the oldest unpaired pulse owns the next edge. Time-based
    // matching would need the edge clock to be host-comparable, which it is
    // not on every backend.
    while (!unpairedFired_.empty()) {
        const uint64_t seq = unpairedFired_.front();
        unpairedFired_.pop_front();
        // Sequence numbers are contiguous, so the record (if still buffered)
        // sits at a computable offset from the front.
        if (events_.empty() || seq < events_.front().sequence) continue; // drained already
        const size_t offset = static_cast<size_t>(seq - events_.front().sequence);
        if (offset >= events_.size()) continue;
        auto& rec = events_[offset];
        if (rec.sequence != seq) continue;
        rec.lineEdgeTimestamp = event.timestamp;
        rec.lineEdgeHostUs = event.hostTimestampUs;
        return;
    }
    // No fired record yet: either the camera reported the edge from inside
    // setTriggerOutput (record follows immediately) or the log was drained
    // between the pulse and the edge. Hold it briefly for the former; a
    // stale hold is bounded and evicted, and counted as unpaired.
    if (pendingEdges_.size() >= kMaxPendingEdges) {
        pendingEdges_.pop_front();
        unpairedLineEdges_.fetch_add(1, std::memory_order_relaxed);
    }
    pendingEdges_.push_back(event);
}

std::vector<backend::recording::TriggerEventRecord> TriggerService::drainEvents() {
    std::vector<backend::recording::TriggerEventRecord> out;
    std::lock_guard<std::mutex> lk(eventMutex_);
    out.assign(events_.begin(), events_.end());
    events_.clear();
    return out;
}

size_t TriggerService::bufferedEventCount() const {
    std::lock_guard<std::mutex> lk(eventMutex_);
    return events_.size();
}

void TriggerService::triggerLoop() {
    raiseTriggerThreadPriority();
    auto& timingRecorder = backend::diagnostics::PipelineTimingRecorder::instance();
    while (running_.load()) {
        PendingRequest pending;
        {
            std::unique_lock<std::mutex> lk(triggerMutex_);
            triggerCV_.wait(lk, [this] { return !running_.load() || !pendingRequests_.empty(); });
            if (!running_.load()) break;
            pending = pendingRequests_.front();
            pendingRequests_.pop_front();
        }

        // Own the camera for the whole pulse: setCamera() blocks on this
        // mutex, so the pointer loaded below stays valid until we release it.
        std::lock_guard<std::mutex> pulseLock(pulseMutex_);
        const uint64_t wakeUs = backend::diagnostics::PipelineTimingRecorder::nowUs();
        auto* cam = camera_.load(std::memory_order_acquire);
        if (cam && pending.generation != boundGeneration_.load(std::memory_order_acquire)) {
            // Request from an earlier camera session: never execute it
            // against the currently bound camera.
            const uint64_t stale =
                droppedStaleRequests_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (stale == 1 || (stale % 100) == 0) {
                SPDLOG_WARN("TriggerService: dropped stale request from session {} "
                            "(bound session {}, total stale drops: {})",
                            pending.generation, boundGeneration_.load(), stale);
            }
            recordEvent(makeEvent(pending, backend::recording::TriggerOutcome::DroppedStale,
                                  wakeUs, 0, 0));
            continue;
        }
        if (!cam) {
            // The request was dequeued but there is no camera to drive the
            // pulse: a selected target is lost. Count it instead of dropping
            // silently (previously a bare `continue`).
            const uint64_t lost =
                droppedPulsesNoCamera_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (lost == 1 || (lost % 100) == 0) {
                SPDLOG_WARN("TriggerService: no camera bound, dropped pulse for frame {} "
                            "(total no-camera drops: {})",
                            pending.frameIndex, lost);
            }
            recordEvent(makeEvent(pending, backend::recording::TriggerOutcome::DroppedNoCamera,
                                  wakeUs, 0, 0));
            continue;
        }

        const bool recordTiming = timingRecorder.isEnabled();

        // Fire trigger pulse: High -> busy-wait ~1us -> Low
        // Mirrors processTrigger() in MIB-Studio/src/mib_grabber/mib_grabber.cpp
        auto start = std::chrono::high_resolution_clock::now();
        if (!cam->setTriggerOutput(true)) {
            // The hardware refused the rising edge: the selected target is not
            // sorted. Count it instead of dropping silently (previously a bare
            // `continue`).
            const uint64_t lost =
                droppedPulsesSetFailed_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (lost == 1 || (lost % 100) == 0) {
                SPDLOG_WARN("TriggerService: setTriggerOutput(true) failed for frame {} "
                            "(total set-failed drops: {})",
                            pending.frameIndex, lost);
            }
            recordEvent(makeEvent(pending, backend::recording::TriggerOutcome::DroppedSetFailed,
                                  wakeUs, 0, 0));
            continue;
        }
        auto onset = std::chrono::high_resolution_clock::now();
        const uint64_t fireUs = backend::diagnostics::PipelineTimingRecorder::nowUs();

        // Busy-wait for the configured pulse duration
        auto pulseUs = std::chrono::microseconds(pulseDurationUs_.load(std::memory_order_relaxed));
        while (running_.load() && std::chrono::high_resolution_clock::now() - onset < pulseUs) {
            // Busy-wait
        }

        cam->setTriggerOutput(false);
        const uint64_t pulseDoneUs = backend::diagnostics::PipelineTimingRecorder::nowUs();

        // Canonical pulse record (always on). A loopback edge that arrived
        // before this record existed (MockCamera delivers it synchronously
        // from setTriggerOutput) is waiting in pendingEdges_ and is attached
        // by recordEvent; one that arrives later pairs via onLineEvent.
        recordEvent(makeEvent(pending, backend::recording::TriggerOutcome::Fired, wakeUs, fireUs,
                              pulseDoneUs));

        // Record metrics
        auto onsetUs = std::chrono::duration<double, std::micro>(onset - start).count();
        lastOnsetUs_.store(onsetUs, std::memory_order_relaxed);
        triggerCount_.fetch_add(1, std::memory_order_relaxed);

        // Always-on live end-to-end target latency: acquisition (host grab
        // stamp of the source frame) -> pulse onset. Independent of the
        // detailed timing recorder so it is visible without MIB_PIPELINE_TIMING.
        if (pending.hostTimestampUs != 0) {
            const uint64_t nowUs = backend::diagnostics::PipelineTimingRecorder::nowUs();
            if (nowUs >= pending.hostTimestampUs) {
                timingRecorder.noteTargetLatency(nowUs - pending.hostTimestampUs);
            }
        }

        // Mirror trigger/sort state so crash reports carry live values.
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().trigger;
            m.triggerCount.store(triggerCount_.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
            m.lastOnsetUs.store(static_cast<uint64_t>(onsetUs), std::memory_order_relaxed);
            m.droppedRequests.store(droppedRequests_.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
            m.droppedPulses.store(getDroppedPulseCount(), std::memory_order_relaxed);
            m.targetLatencyUs.store(
                static_cast<uint64_t>(timingRecorder.avgTargetLatencyUs()),
                std::memory_order_relaxed);
        }

        if (recordTiming) {
            backend::diagnostics::TriggerTimingRecord record;
            record.frameIndex = pending.frameIndex;
            record.grabUs = pending.hostTimestampUs;
            record.requestUs = pending.requestUs;
            record.wakeUs = wakeUs;
            record.fireUs = fireUs;
            record.pulseDoneUs = pulseDoneUs;
            // With the per-request queue nothing coalesces; overflow shows up
            // in getDroppedRequestCount() instead.
            record.coalesced = 0;
            timingRecorder.recordTrigger(record);
        }
    }
}

} // namespace backend::services
