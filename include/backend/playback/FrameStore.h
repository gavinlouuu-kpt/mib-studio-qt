#pragma once

#include "backend/diagnostics/MemoryBudget.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <functional>

namespace backend::playback {

struct Frame {
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t pixelFormat = 0; // PFNC code from Euresys
    size_t linePitch = 0;     // bytes per line
    uint64_t timestamp = 0;   // raw unit from source
    // Host monotonic microseconds (Tools::getTimestamp clock) stamped at
    // acquisition by CaptureService; 0 for frames from other producers.
    // Comparable across pipeline stages (see PipelineTimingRecorder).
    uint64_t hostTimestampUs = 0;
    std::vector<uint8_t> data;
};

// Typed result of an indexed read (issue #367). `Available` is the only
// success; every other value names why the frame could not be delivered so
// the recording loop can account for it instead of silently continuing.
enum class FrameReadOutcome {
    Available,       // the slot holds exactly this write index, fully committed
    NotYetCommitted, // index reserved (or beyond the reservation) but not yet published
    Overwritten,     // index evicted: a newer frame occupies the slot
    Malformed,       // slot committed but unusable (zero geometry / short buffer)
    OutOfRange,      // store empty / zero capacity
};

inline const char* toString(FrameReadOutcome o)
{
    switch (o) {
    case FrameReadOutcome::Available: return "available";
    case FrameReadOutcome::NotYetCommitted: return "notYetCommitted";
    case FrameReadOutcome::Overwritten: return "overwritten";
    case FrameReadOutcome::Malformed: return "malformed";
    case FrameReadOutcome::OutOfRange: return "outOfRange";
    }
    return "unknown";
}

struct IndexRange {
    uint64_t start = 0;
    uint64_t end = 0;
};

struct TimestampRange {
    uint64_t start = 0;
    uint64_t end = 0;
};

class FrameStore {
public:
    explicit FrameStore(size_t capacity = 512);

    // Copy frame data into the ring buffer. hostTimestampUs is the host
    // monotonic acquisition stamp (0 = unknown/not applicable).
    void pushFrame(const uint8_t* src, size_t size, uint64_t width, uint64_t height,
                   size_t linePitch, uint64_t pixelFormat, uint64_t timestamp,
                   uint64_t hostTimestampUs = 0);

    // Retrieve a copy of the latest COMMITTED frame; returns false if empty.
    // Uses the same committed-identity contract as indexed reads: never
    // exposes a slot whose identity/data copy has not completed.
    bool getLatest(Frame& out) const;

    // Retrieve a copy by absolute write index; returns false if out-of-range
    // (compatibility wrapper over readByWriteIndex).
    bool getByWriteIndex(uint64_t writeIndex, Frame& out) const;

    // Typed indexed read (issue #367): distinguishes not-yet-committed,
    // overwritten (evicted), and malformed slots from success so consumers
    // can account for every frame they claim.
    FrameReadOutcome readByWriteIndex(uint64_t writeIndex, Frame& out) const;

    // Publication boundary (issue #367). pushFrame first RESERVES the next
    // write index (totalWritten() advances) and only after the image +
    // metadata copy completes COMMITS it (committedCount() advances). With
    // the single capture producer, commit order equals reservation order, so
    // every index below committedCount() is readable unless evicted.
    uint64_t committedCount() const { return committed_.load(std::memory_order_acquire); }
    // Latest committed absolute index. Only valid when committedCount() > 0.
    uint64_t latestCommittedIndex() const { return committed_.load(std::memory_order_acquire) - 1; }

    // Test seam: invoked between reservation and commit of every push so a
    // deterministic test can observe the NotYetCommitted window. Not for
    // production use.
    void setCommitHookForTests(std::function<void(uint64_t writeIndex)> hook);

    // Retrieve ROI region from frame by absolute write index without full frame copy
    // Returns false if out-of-range or invalid ROI
    // The ROI is extracted directly from the frame data, avoiding full frame copy
    bool getByWriteIndexROI(uint64_t writeIndex, int roiX, int roiY, int roiW, int roiH,
                            Frame& out) const;

    // Absolute index helpers (monotonic sequence since start)
    // Earliest absolute index currently retained in the ring
    uint64_t earliestAvailableIndex() const;

    // Latest absolute index written (w-1). Only valid when totalWritten() > 0
    uint64_t latestAvailableIndex() const;

    // Number of frames currently retained in the ring (<= capacity)
    size_t availableCount() const;

    // Current number of frames written since start (monotonic)
    uint64_t totalWritten() const { return totalWritten_.load(); }

    // Block until totalWritten() exceeds lastSeenTotal, a new frame is
    // pushed, or `timeout` elapses; returns the current totalWritten().
    // Event-driven replacement for consumer sleep-polling (issue #282):
    // pushFrame wakes waiters as soon as the new frame is readable, so a
    // caught-up realtime consumer no longer pays a fixed poll interval of
    // acquisition->processing latency. The producer pays a single relaxed
    // atomic load per push while nobody waits. The timeout keeps stop
    // paths (rtRunning_ checks) as responsive as the old sleep.
    uint64_t waitForFrame(uint64_t lastSeenTotal, std::chrono::microseconds timeout);

    // Ring capacity
    size_t capacity() const { return capacity_.load(std::memory_order_acquire); }

    // Save frames to disk as TIFF images
    // Saves all available frames if range not specified
    // filterFn: optional function that returns true if frame should be skipped (empty frames)
    bool saveFramesToDisk(const std::string& outputDir,
                          std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Save frames by index range (startIndex inclusive, endIndex inclusive)
    // filterFn: optional function that returns true if frame should be skipped (empty frames)
    bool saveFramesToDisk(const std::string& outputDir, uint64_t startIndex, uint64_t endIndex,
                          std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Save frames by timestamp range (startTimestamp inclusive, endTimestamp inclusive)
    // filterFn: optional function that returns true if frame should be skipped (empty frames)
    bool saveFramesToDisk(const std::string& outputDir, uint64_t startTimestamp,
                          uint64_t endTimestamp, bool useTimestamps,
                          std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Save frames to a single uncompressed AVI file.
    // Tries Y800 (grayscale) FourCC first, falls back to uncompressed BGR ("DIB ")
    // if the Y800 writer fails to open. Per-frame timestamps are NOT preserved.
    // filterFn: optional function that returns true if frame should be skipped.
    // fps: playback frame rate embedded in the AVI header (does not affect content).
    bool saveFramesToAvi(const std::string& outputPath, double fps = 30.0,
                         std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Save frames to AVI by index range (inclusive).
    bool saveFramesToAvi(const std::string& outputPath, uint64_t startIndex, uint64_t endIndex,
                         double fps = 30.0,
                         std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Save frames to AVI by timestamp range (inclusive).
    bool saveFramesToAvi(const std::string& outputPath, uint64_t startTimestamp,
                         uint64_t endTimestamp, bool useTimestamps, double fps = 30.0,
                         std::function<bool(const Frame&)> filterFn = nullptr) const;

    // Resize buffer capacity safely
    // Preserves existing frames when possible (if new size >= current available frames)
    // Clears buffer if new size < current available frames
    // Returns true on success, false on failure
    bool resize(size_t newCapacity);

    // Pre-reserve each ring slot's data buffer to at least frameBytes so the
    // per-frame pushFrame hot path performs no heap allocation for frames of
    // that size or smaller. Call at capture start once the frame geometry is
    // known. Idempotent: only grows slots whose capacity is below frameBytes.
    void reserveFrameBytes(size_t frameBytes);

    // Get available index range
    IndexRange getAvailableRange() const;

    // Get available timestamp range
    // Returns false if no frames available
    bool getAvailableTimestampRange(TimestampRange& out) const;

    // Estimate memory in bytes needed for a given capacity
    // Uses average frame size from existing frames if available, otherwise uses conservative
    // default
    size_t estimateMemoryBytesForCapacity(size_t capacity) const;

    // Measured retained memory (issue #370): bytes allocated by the ring
    // slots (capacity, not just size — that is what stays resident), the
    // peak, frames retained, and the declared bound (capacity × reserved
    // frame bytes when reserveFrameBytes was called). Lock-free.
    backend::diagnostics::MemoryOwnerStats memoryStats() const;

private:
    // Per-slot allocation size (parallel to ring_; written under the slot
    // lock or the exclusive structural lock) and its running sum.
    std::vector<size_t> slotBytes_;
    std::atomic<uint64_t> retainedBytes_{0};
    std::atomic<uint64_t> peakRetainedBytes_{0};
    std::atomic<size_t> reservedFrameBytes_{0};
    void noteSlotBytes(size_t idx, size_t bytes);
    // Atomic so the lock-free guard reads (earliest/latest/availableCount and
    // the top-of-function `capacity_ == 0` checks) cannot data-race with
    // resize()'s write. resize() holds structureMutex_ exclusively and also
    // calls those readers, so they cannot take the lock — atomicity is the fix.
    std::atomic<size_t> capacity_;
    // Structural lock: guards the identity of ring_ / slotMutexes_ and
    // capacity_. Held in SHARED mode by the per-frame hot path (pushFrame /
    // getByWriteIndex / getLatest) so producer and consumers do not serialize
    // against each other, and in EXCLUSIVE mode only by rare whole-ring
    // operations (resize / save / estimate) that replace the ring or touch
    // many slots at once.
    mutable std::shared_mutex structureMutex_;
    // Per-slot locks (parallel to ring_): a single slot lock is held only
    // while copying one frame in/out, so the large memcpy no longer happens
    // under a global mutex. The producer writing slot A never blocks a
    // consumer reading slot B.
    mutable std::vector<std::mutex> slotMutexes_;
    std::vector<Frame> ring_;
    // Parallel to ring_: the write index whose frame each slot currently
    // holds (kSlotEmpty when never written). Written/read only under the
    // slot's lock. Readers use it to verify identity after locking the
    // slot: totalWritten_ is incremented before the slot data is copied,
    // so both "slot overwritten by a wrapping producer" and "slot not yet
    // written for this index" races are caught by one comparison.
    static constexpr uint64_t kSlotEmpty = ~0ULL;
    std::vector<uint64_t> slotWriteIndices_;
    std::atomic<uint64_t> totalWritten_{0}; // reservation count
    std::atomic<uint64_t> committed_{0};    // publication count (<= totalWritten_)
    std::function<void(uint64_t)> commitHookForTests_;

    // Consumer wake-up (waitForFrame/pushFrame). waitWaiters_ is a
    // Dekker-style guard: the producer only takes waitMutex_ + notifies
    // when a waiter is registered, and both sides use seq_cst ordering on
    // (totalWritten_, waitWaiters_) so either the producer sees the waiter
    // or the waiter's predicate sees the new frame — never a lost wakeup.
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
    std::atomic<uint32_t> waitWaiters_{0};

    // Internal helper to save a single frame as TIFF
    bool saveFrameAsTiff(const Frame& frame, const std::string& filepath) const;

    // Internal helper: write already-collected frames to an AVI file.
    // Opens cv::VideoWriter once, writes sequentially, closes on return.
    // Must be called WITHOUT holding mutex_.
    bool writeFramesAsAvi(const std::vector<std::pair<uint64_t, Frame>>& frames,
                          const std::string& outputPath, double fps,
                          std::function<bool(const Frame&)> filterFn) const;

    // Internal helper to find frame indices by timestamp range
    bool findIndicesByTimestampRange(uint64_t startTimestamp, uint64_t endTimestamp,
                                     uint64_t& startIndex, uint64_t& endIndex) const;
};

} // namespace backend::playback
