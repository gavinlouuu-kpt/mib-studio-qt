// Explicit per-run frame accounting (issue #367).
//
// Every frame admitted into the host acquisition/recording boundary must end
// in exactly one named terminal category (or an explicit pending-at-stop
// term). Frame counts are kept separately from object counts; a processing
// failure, a FrameStore loss, or a persistence failure can never be folded
// into the ordinary "empty / filtered" count. Qt-free and header-only so the
// recording thread, ProcessingService, Hdf5Service, tests, and the UI all
// share one definition.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace backend::recording {

// Terminal outcome of one admitted frame.
enum class FrameOutcome {
    Empty,                      // classified empty by the active core (true empty)
    Processed,                  // non-empty; handed on (recorded / analysed)
    RejectedByScientificFilter, // non-empty but failed validation (experiment path)
    ProcessingFailed,           // core/kernel error or exception — NOT empty
    StoreOverwritten,           // FrameStore slot evicted before it could be read
    StoreNotCommitted,          // FrameStore slot reserved but not yet committed at read time
    StoreMalformed,             // FrameStore returned an unusable frame (bad geometry/size)
    PersistenceFailed,          // admitted to the writer but not committed
    PendingAtStop,              // still in flight (batch not submitted) when the run stopped
    CancelledByPolicy,          // dropped under a declared policy (e.g. buffer cap)
};

inline const char* toString(FrameOutcome o)
{
    switch (o) {
    case FrameOutcome::Empty: return "empty";
    case FrameOutcome::Processed: return "processed";
    case FrameOutcome::RejectedByScientificFilter: return "rejectedByScientificFilter";
    case FrameOutcome::ProcessingFailed: return "processingFailed";
    case FrameOutcome::StoreOverwritten: return "storeOverwritten";
    case FrameOutcome::StoreNotCommitted: return "storeNotCommitted";
    case FrameOutcome::StoreMalformed: return "storeMalformed";
    case FrameOutcome::PersistenceFailed: return "persistenceFailed";
    case FrameOutcome::PendingAtStop: return "pendingAtStop";
    case FrameOutcome::CancelledByPolicy: return "cancelledByPolicy";
    }
    return "unknown";
}

// Final state of a run/file.
enum class RunCompletionState {
    Complete,            // every admitted frame reconciled; no loss, no failure
    IntentionallyPartial,// declared policy dropped/cancelled frames (LatestFrame, buffer cap)
    IncompleteLoss,      // undeclared loss: store overwrite/not-committed/malformed or processing failure
    Failed,              // persistence failure, fatal error, or accounting does not reconcile
    Unknown,             // legacy file without accounting
};

inline const char* toString(RunCompletionState s)
{
    switch (s) {
    case RunCompletionState::Complete: return "complete";
    case RunCompletionState::IntentionallyPartial: return "intentionallyPartial";
    case RunCompletionState::IncompleteLoss: return "incompleteLoss";
    case RunCompletionState::Failed: return "failed";
    case RunCompletionState::Unknown: return "unknown";
    }
    return "unknown";
}

inline RunCompletionState runCompletionStateFromString(const std::string& s)
{
    if (s == "complete") return RunCompletionState::Complete;
    if (s == "intentionallyPartial") return RunCompletionState::IntentionallyPartial;
    if (s == "incompleteLoss") return RunCompletionState::IncompleteLoss;
    if (s == "failed") return RunCompletionState::Failed;
    return RunCompletionState::Unknown;
}

// Plain (copyable) snapshot. Schema version 1 is what Hdf5Service persists.
struct RecordingAccountingSnapshot {
    static constexpr uint64_t kSchemaVersion = 1;
    uint64_t schemaVersion{kSchemaVersion};

    // --- frame terms --------------------------------------------------------
    uint64_t admitted{0};            // frames the run claimed (attempted to read)
    uint64_t empty{0};
    uint64_t processed{0};
    uint64_t scientificallyRejected{0};
    uint64_t processingFailed{0};
    uint64_t storeOverwritten{0};
    uint64_t storeNotCommitted{0};
    uint64_t storeMalformed{0};
    uint64_t cancelledByPolicy{0};
    uint64_t pendingAtStop{0};       // classified but never handed to the writer
    // --- persistence terms --------------------------------------------------
    uint64_t persistenceAdmitted{0}; // frames handed to the writer queue
    uint64_t persistenceCommitted{0};// frames the writer confirmed written
    uint64_t persistenceFailed{0};   // frames in a batch whose write failed/overflowed
    uint64_t persistencePendingAtStop{0}; // queued but never written when the run ended
    uint64_t persistenceCancelledByPolicy{0}; // evicted from a bounded buffer under declared policy
    // --- object terms (kept separate from frames) ---------------------------
    uint64_t objectsDetected{0};
    // --- sequence -------------------------------------------------------------
    bool hasIndexRange{false};
    uint64_t firstFrameIndex{0};
    uint64_t lastFrameIndex{0};
    uint64_t sequenceGaps{0};        // count of gaps in the admitted index sequence
    uint64_t sequenceGapFrames{0};   // total frames those gaps span
    uint64_t sessionGeneration{0};   // capture session generation (0 = unknown)
    // --- declared policy ------------------------------------------------------
    bool policyAllowsDrops{false};   // e.g. LatestFrame delivery mode selected
    // --- finalization ---------------------------------------------------------
    bool fatalError{false};
    std::string fatalMessage;
    RunCompletionState completion{RunCompletionState::Unknown};
    std::string completionReason;
    bool reconciled{false};

    // Reconciliation equations. Every admitted frame must be in exactly one
    // frame term; every writer admission must be committed, failed, or
    // pending. Unknown/unobservable terms are never assumed zero — they are
    // simply not part of the admitted count.
    uint64_t frameTermsSum() const
    {
        return empty + processed + scientificallyRejected + processingFailed + storeOverwritten +
               storeNotCommitted + storeMalformed + cancelledByPolicy + pendingAtStop;
    }
    uint64_t persistenceTermsSum() const
    {
        return persistenceCommitted + persistenceFailed + persistencePendingAtStop +
               persistenceCancelledByPolicy;
    }
    bool framesReconcile() const { return frameTermsSum() == admitted; }
    bool persistenceReconciles() const { return persistenceTermsSum() == persistenceAdmitted; }
};

// Derive the final completion state. Pure; safe to call repeatedly.
inline RecordingAccountingSnapshot reconcile(RecordingAccountingSnapshot s)
{
    s.reconciled = s.framesReconcile() && s.persistenceReconciles();
    std::string reason;
    if (!s.reconciled) {
        s.completion = RunCompletionState::Failed;
        reason = "accounting does not reconcile (admitted=" + std::to_string(s.admitted) +
                 " frameTerms=" + std::to_string(s.frameTermsSum()) +
                 ", persistenceAdmitted=" + std::to_string(s.persistenceAdmitted) +
                 " persistenceTerms=" + std::to_string(s.persistenceTermsSum()) + ")";
    } else if (s.fatalError || s.persistenceFailed > 0) {
        s.completion = RunCompletionState::Failed;
        reason = s.fatalError ? s.fatalMessage : "persistence failures";
    } else if (s.storeOverwritten > 0 || s.storeNotCommitted > 0 || s.storeMalformed > 0 ||
               s.processingFailed > 0 || s.sequenceGaps > 0) {
        s.completion = RunCompletionState::IncompleteLoss;
        reason = "undeclared loss: storeOverwritten=" + std::to_string(s.storeOverwritten) +
                 " storeNotCommitted=" + std::to_string(s.storeNotCommitted) +
                 " storeMalformed=" + std::to_string(s.storeMalformed) +
                 " processingFailed=" + std::to_string(s.processingFailed) +
                 " sequenceGaps=" + std::to_string(s.sequenceGaps);
    } else if (s.cancelledByPolicy > 0 || s.pendingAtStop > 0 || s.persistencePendingAtStop > 0 ||
               s.persistenceCancelledByPolicy > 0 || s.policyAllowsDrops) {
        s.completion = RunCompletionState::IntentionallyPartial;
        reason = "declared policy: cancelledByPolicy=" + std::to_string(s.cancelledByPolicy) +
                 " pendingAtStop=" + std::to_string(s.pendingAtStop) +
                 " persistencePendingAtStop=" + std::to_string(s.persistencePendingAtStop) +
                 " persistenceCancelledByPolicy=" + std::to_string(s.persistenceCancelledByPolicy) +
                 (s.policyAllowsDrops ? " (delivery policy allows drops)" : "");
    } else {
        s.completion = RunCompletionState::Complete;
        reason = "all admitted frames reconciled";
    }
    s.completionReason = reason;
    return s;
}

// Lock-free accumulator used by the recording / realtime threads. Each
// counter is a relaxed atomic; snapshot() is a consistent-enough read for
// reporting (exact after the producer thread has stopped).
class RecordingAccounting {
public:
    void reset(uint64_t sessionGeneration, bool policyAllowsDrops)
    {
        admitted = 0; empty = 0; processed = 0; scientificallyRejected = 0; processingFailed = 0;
        storeOverwritten = 0; storeNotCommitted = 0; storeMalformed = 0; cancelledByPolicy = 0;
        pendingAtStop = 0; persistenceAdmitted = 0; persistenceCommitted = 0;
        persistenceFailed = 0; persistencePendingAtStop = 0; persistenceCancelledByPolicy = 0;
        objectsDetected = 0;
        hasIndexRange_.store(false); firstIndex_.store(0); lastIndex_.store(0); sequenceGaps = 0;
        sequenceGapFrames = 0; sessionGeneration_ = sessionGeneration;
        policyAllowsDrops_ = policyAllowsDrops; fatalError_ = false;
        fatalMessage_.clear();
    }

    // Record that index `idx` was admitted; tracks first/last and gaps.
    // Producer-thread only (the range fields are atomics so wasAdmitted()
    // may be read from any thread).
    void admit(uint64_t idx)
    {
        admitted.fetch_add(1, std::memory_order_relaxed);
        if (!hasIndexRange_.load(std::memory_order_relaxed)) {
            firstIndex_.store(idx, std::memory_order_relaxed);
            lastIndex_.store(idx, std::memory_order_relaxed);
            hasIndexRange_.store(true, std::memory_order_release);
            return;
        }
        const uint64_t last = lastIndex_.load(std::memory_order_relaxed);
        if (idx > last + 1) {
            sequenceGaps.fetch_add(1, std::memory_order_relaxed);
            sequenceGapFrames.fetch_add(idx - last - 1, std::memory_order_relaxed);
        }
        if (idx > last) lastIndex_.store(idx, std::memory_order_release);
    }

    // True when `idx` was admitted under this run. Outcomes and persistence
    // admissions are gated on this rather than on "experiment active", so a
    // frame in flight across startExperiment()/endExperiment() is counted
    // exactly when its admission was (start/stop boundary skew, 2026-09-08).
    bool wasAdmitted(uint64_t idx) const
    {
        if (!hasIndexRange_.load(std::memory_order_acquire)) return false;
        return idx >= firstIndex_.load(std::memory_order_relaxed) &&
               idx <= lastIndex_.load(std::memory_order_acquire);
    }
    uint64_t lastAdmittedIndex() const { return lastIndex_.load(std::memory_order_acquire); }
    bool hasAdmitted() const { return hasIndexRange_.load(std::memory_order_acquire); }

    // Frames the run never got to read (e.g. evicted from the ring before the
    // consumer reached them): admitted and terminated in one step, without
    // sequence tracking (their indices are known only as a range).
    void admitLost(uint64_t n, FrameOutcome o)
    {
        admitted.fetch_add(n, std::memory_order_relaxed);
        count(o, n);
    }

    void count(FrameOutcome o, uint64_t n = 1)
    {
        switch (o) {
        case FrameOutcome::Empty: empty.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::Processed: processed.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::RejectedByScientificFilter:
            scientificallyRejected.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::ProcessingFailed: processingFailed.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::StoreOverwritten: storeOverwritten.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::StoreNotCommitted: storeNotCommitted.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::StoreMalformed: storeMalformed.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::PersistenceFailed: persistenceFailed.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::PendingAtStop: pendingAtStop.fetch_add(n, std::memory_order_relaxed); break;
        case FrameOutcome::CancelledByPolicy: cancelledByPolicy.fetch_add(n, std::memory_order_relaxed); break;
        }
    }

    void setFatal(const std::string& message)
    {
        fatalError_ = true;
        fatalMessage_ = message;
    }

    RecordingAccountingSnapshot snapshot() const
    {
        RecordingAccountingSnapshot s;
        s.admitted = admitted.load(std::memory_order_relaxed);
        s.empty = empty.load(std::memory_order_relaxed);
        s.processed = processed.load(std::memory_order_relaxed);
        s.scientificallyRejected = scientificallyRejected.load(std::memory_order_relaxed);
        s.processingFailed = processingFailed.load(std::memory_order_relaxed);
        s.storeOverwritten = storeOverwritten.load(std::memory_order_relaxed);
        s.storeNotCommitted = storeNotCommitted.load(std::memory_order_relaxed);
        s.storeMalformed = storeMalformed.load(std::memory_order_relaxed);
        s.cancelledByPolicy = cancelledByPolicy.load(std::memory_order_relaxed);
        s.pendingAtStop = pendingAtStop.load(std::memory_order_relaxed);
        s.persistenceAdmitted = persistenceAdmitted.load(std::memory_order_relaxed);
        s.persistenceCommitted = persistenceCommitted.load(std::memory_order_relaxed);
        s.persistenceFailed = persistenceFailed.load(std::memory_order_relaxed);
        s.persistencePendingAtStop = persistencePendingAtStop.load(std::memory_order_relaxed);
        s.persistenceCancelledByPolicy = persistenceCancelledByPolicy.load(std::memory_order_relaxed);
        s.objectsDetected = objectsDetected.load(std::memory_order_relaxed);
        s.hasIndexRange = hasIndexRange_.load(std::memory_order_acquire);
        s.firstFrameIndex = firstIndex_.load(std::memory_order_relaxed);
        s.lastFrameIndex = lastIndex_.load(std::memory_order_relaxed);
        s.sequenceGaps = sequenceGaps.load(std::memory_order_relaxed);
        s.sequenceGapFrames = sequenceGapFrames.load(std::memory_order_relaxed);
        s.sessionGeneration = sessionGeneration_;
        s.policyAllowsDrops = policyAllowsDrops_;
        s.fatalError = fatalError_;
        s.fatalMessage = fatalMessage_;
        return s;
    }

    // Public relaxed counters (hot-path increments from the owning thread;
    // the writer thread touches only the persistence terms).
    std::atomic<uint64_t> admitted{0};
    std::atomic<uint64_t> empty{0};
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> scientificallyRejected{0};
    std::atomic<uint64_t> processingFailed{0};
    std::atomic<uint64_t> storeOverwritten{0};
    std::atomic<uint64_t> storeNotCommitted{0};
    std::atomic<uint64_t> storeMalformed{0};
    std::atomic<uint64_t> cancelledByPolicy{0};
    std::atomic<uint64_t> pendingAtStop{0};
    std::atomic<uint64_t> persistenceAdmitted{0};
    std::atomic<uint64_t> persistenceCommitted{0};
    std::atomic<uint64_t> persistenceFailed{0};
    std::atomic<uint64_t> persistencePendingAtStop{0};
    std::atomic<uint64_t> persistenceCancelledByPolicy{0};
    std::atomic<uint64_t> objectsDetected{0};
    std::atomic<uint64_t> sequenceGaps{0};
    std::atomic<uint64_t> sequenceGapFrames{0};

private:
    std::atomic<bool> hasIndexRange_{false};
    std::atomic<uint64_t> firstIndex_{0};
    std::atomic<uint64_t> lastIndex_{0};
    uint64_t sessionGeneration_{0};
    bool policyAllowsDrops_{false};
    bool fatalError_{false};
    std::string fatalMessage_;
};

} // namespace backend::recording
