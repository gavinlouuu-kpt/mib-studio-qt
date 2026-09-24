// Byte-budgeted ownership reporting (issue #370).
//
// Every major host-path owner of frame memory (camera SDK buffers, the
// FrameStore ring, processing queues, experiment/monitoring retention, the
// persistence queue, presentation snapshots) reports the same small record:
// current and peak bytes, current and peak item counts, its declared
// capacity (bytes and/or count; 0 = no declared bound) and how well the
// number is known. Unknown vendor-buffer memory is reported as Unknown —
// never as a measured zero. Qt-free, OpenCV-free, header-only so the backend,
// tests and the UI share one definition.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace backend::diagnostics {

enum class MemoryKnowledge {
    Measured,  // bytes are counted from the owner's own allocations
    Estimated, // derived (e.g. count × frame size) — an upper bound, not a measurement
    Unknown,   // the owner cannot observe it (vendor SDK buffers)
};

inline const char* toString(MemoryKnowledge k)
{
    switch (k) {
    case MemoryKnowledge::Measured: return "measured";
    case MemoryKnowledge::Estimated: return "estimated";
    case MemoryKnowledge::Unknown: return "unknown";
    }
    return "unknown";
}

struct MemoryOwnerStats {
    std::string name;                                   // stable id, e.g. "processing.experimentBuffer"
    MemoryKnowledge knowledge{MemoryKnowledge::Unknown};
    uint64_t currentBytes{0};
    uint64_t peakBytes{0};
    uint64_t currentCount{0};   // items (frames / batches / objects) retained now
    uint64_t peakCount{0};
    uint64_t capacityBytes{0};  // declared byte budget (0 = none declared)
    uint64_t capacityCount{0};  // declared item cap (0 = none declared)
    uint64_t evictedByBudget{0}; // items dropped/replaced to honour the budget (owner-specific)
    std::string note;           // what the numbers mean for this owner

    bool hasByteBudget() const { return capacityBytes > 0; }
    bool overBudget() const { return capacityBytes > 0 && currentBytes > capacityBytes; }
};

// Relaxed-atomic accountant for owners that add/remove items on hot paths.
// One add()/remove() pair per item; peak tracking is best-effort (a lost
// race can under-report a peak by one item, never over-report).
class ByteAccountant {
public:
    void add(uint64_t bytes, uint64_t items = 1) noexcept
    {
        const uint64_t b = bytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        const uint64_t c = count_.fetch_add(items, std::memory_order_relaxed) + items;
        raiseMax(peakBytes_, b);
        raiseMax(peakCount_, c);
    }
    void remove(uint64_t bytes, uint64_t items = 1) noexcept
    {
        bytes_.fetch_sub(std::min(bytes, bytes_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        count_.fetch_sub(std::min(items, count_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    // Replace the current values (owners that recount under their own lock).
    void set(uint64_t bytes, uint64_t items) noexcept
    {
        bytes_.store(bytes, std::memory_order_relaxed);
        count_.store(items, std::memory_order_relaxed);
        raiseMax(peakBytes_, bytes);
        raiseMax(peakCount_, items);
    }
    void reset() noexcept
    {
        bytes_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        peakBytes_.store(0, std::memory_order_relaxed);
        peakCount_.store(0, std::memory_order_relaxed);
    }
    void noteEvicted(uint64_t n = 1) noexcept { evicted_.fetch_add(n, std::memory_order_relaxed); }
    void resetEvicted() noexcept { evicted_.store(0, std::memory_order_relaxed); }

    uint64_t bytes() const noexcept { return bytes_.load(std::memory_order_relaxed); }
    uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    uint64_t peakBytes() const noexcept { return peakBytes_.load(std::memory_order_relaxed); }
    uint64_t peakCount() const noexcept { return peakCount_.load(std::memory_order_relaxed); }
    uint64_t evicted() const noexcept { return evicted_.load(std::memory_order_relaxed); }

    MemoryOwnerStats snapshot(std::string name, MemoryKnowledge knowledge, uint64_t capacityBytes,
                              uint64_t capacityCount, std::string note = {}) const
    {
        MemoryOwnerStats s;
        s.name = std::move(name);
        s.knowledge = knowledge;
        s.currentBytes = bytes();
        s.peakBytes = peakBytes();
        s.currentCount = count();
        s.peakCount = peakCount();
        s.capacityBytes = capacityBytes;
        s.capacityCount = capacityCount;
        s.evictedByBudget = evicted();
        s.note = std::move(note);
        return s;
    }

private:
    static void raiseMax(std::atomic<uint64_t>& slot, uint64_t v) noexcept
    {
        uint64_t cur = slot.load(std::memory_order_relaxed);
        while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> peakBytes_{0};
    std::atomic<uint64_t> peakCount_{0};
    std::atomic<uint64_t> evicted_{0};
};

// Whole-process view assembled by AppBackend::memoryBudgetSnapshot().
struct HostMemoryBudgetSnapshot {
    uint64_t sampledAtUs{0};
    double processRssMB{0.0};      // 0 = unavailable on this platform
    double processPeakRssMB{0.0};  // 0 = unavailable
    std::vector<MemoryOwnerStats> owners;

    // Sum of measured + estimated owner bytes. Shared allocations that two
    // owners both retain (refcounted images) are counted once per owner, so
    // this is an upper bound on what those owners keep alive together.
    uint64_t accountedBytes() const
    {
        uint64_t total = 0;
        for (const auto& o : owners)
            if (o.knowledge != MemoryKnowledge::Unknown) total += o.currentBytes;
        return total;
    }
    bool hasUnknownOwner() const
    {
        for (const auto& o : owners)
            if (o.knowledge == MemoryKnowledge::Unknown) return true;
        return false;
    }
    const MemoryOwnerStats* find(const std::string& name) const
    {
        for (const auto& o : owners)
            if (o.name == name) return &o;
        return nullptr;
    }
};

} // namespace backend::diagnostics
