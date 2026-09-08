// Bounded experiment accumulation buffer (issue #370, extracted from
// ProcessingService so the retention/backpressure policy has one owner and
// can be tested without the realtime pipeline).
//
// Holds the ProcessedFrames accepted for persistence since the last flush,
// bounded by BOTH a frame count and a byte budget. Eviction policy (unchanged
// from the in-service implementation): when a bound is exceeded, sampled
// invalid frames are evicted first (oldest first); valid frames are evicted
// only when the backlog is entirely valid and still over the bound. Every
// eviction is reported to the caller so it can be accounted for
// (persistenceCancelledByPolicy) — nothing is dropped silently.
//
// Byte accounting counts the image bytes each retained ProcessedFrame keeps
// alive (original + mask + series). Images shared by refcount with another
// owner (monitoring ring, snapshot) are counted here as well: the number is
// "what this buffer alone keeps alive", an upper bound.
#pragma once

#include "backend/diagnostics/MemoryBudget.h"
#include "backend/processing/ProcessingTypes.h"

#include <atomic>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace backend::services {

// Bytes a ProcessedFrame keeps alive (image payloads only).
uint64_t processedFrameBytes(const ProcessedFrame& frame);

class ExperimentFrameBuffer {
public:
    struct Policy {
        size_t maxFrames{1000};
        uint64_t maxBytes{0}; // 0 = no byte budget
    };
    struct AppendResult {
        bool stored{false};
        size_t droppedValid{0};
        size_t droppedInvalid{0};
        size_t bufferedAfter{0};
        uint64_t bytesAfter{0};
        size_t dropped() const { return droppedValid + droppedInvalid; }
    };

    ExperimentFrameBuffer() : ExperimentFrameBuffer(Policy{}) {}
    explicit ExperimentFrameBuffer(const Policy& policy);

    void setPolicy(const Policy& policy);
    Policy policy() const;

    // Admit one frame. Returns what was stored/evicted (the frame itself may
    // be the one refused when the buffer is full of valid frames).
    AppendResult append(ProcessedFrame&& frame, bool isValid);

    BufferedFrameCounts counts() const;
    uint64_t bytes() const { return accountant_.bytes(); }
    bool empty() const;

    // Move everything out (oldest first) for a flush; the buffer is empty
    // afterwards. cv::Mat moves are refcount transfers, not copies.
    void takeAll(std::vector<ProcessedFrame>& valid, std::vector<ProcessedFrame>& invalid);
    // Copies (for review/inspection paths).
    std::vector<ProcessedFrame> copyValid() const;
    std::vector<ProcessedFrame> copyInvalid() const;
    void clear();

    // Lifetime totals of frames evicted by the bound (reset by resetTotals()).
    uint64_t totalDroppedValid() const { return droppedValidTotal_.load(std::memory_order_relaxed); }
    uint64_t totalDroppedInvalid() const { return droppedInvalidTotal_.load(std::memory_order_relaxed); }
    void resetTotals();

    diagnostics::MemoryOwnerStats memoryStats() const;

private:
    struct Entry {
        ProcessedFrame frame;
        uint64_t bytes{0};
    };
    // Evict until both bounds hold. Caller holds mutex_.
    void trimLocked(size_t& droppedValid, size_t& droppedInvalid);
    bool overBoundLocked() const;

    mutable std::mutex mutex_;
    Policy policy_;
    std::deque<Entry> valid_;
    std::deque<Entry> invalid_;
    uint64_t bytes_{0}; // under mutex_; mirrored into accountant_
    diagnostics::ByteAccountant accountant_;
    std::atomic<uint64_t> droppedValidTotal_{0};
    std::atomic<uint64_t> droppedInvalidTotal_{0};
};

} // namespace backend::services
