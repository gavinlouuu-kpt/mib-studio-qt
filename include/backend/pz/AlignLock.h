#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace backend::pz {

// Align's whole-frame preview can start in a state where the ingress is stuck: P[13] bits 15:8 (the
// per-lane FIFO overflow flags, sticky, OR'd into every frame's bad flag) read non-zero, the
// bridge counts every frame as lost and no preview is ever published (#629). The flags clear only
// at a receiver reset. This is the policy that notices it and recovers; the hardware accesses are
// hooks so it can be tested without a board.
struct IngressStatus {
    uint32_t status{0};  // P[13]
    uint32_t errors{0};  // P[12]
    uint32_t resyncs{0}; // P[14]
    uint32_t laneOverflow() const { return (status >> 8) & 0xFFu; }
};

struct AlignLockHooks {
    // True once a new preview has been published, waiting at most the given time.
    std::function<bool(std::chrono::milliseconds)> waitPreview;
    std::function<bool(IngressStatus&)> readStatus;
    // One clear step for the sticky flags: a receiver reset held for the given time (board
    // measurements 2026-10-08: a ~100 ms hold cured a failing start; the 100 µs pulse and the rx
    // buffer clear (bit 5) do not reliably). It is this one function so the sequence can change.
    std::function<bool(std::chrono::milliseconds hold)> clearFlags;
    // Called for every attempt (count and log it).
    std::function<void(int attempt, const IngressStatus&)> onAttempt;
    std::function<void(std::chrono::milliseconds)> pause;
};

struct AlignLockPolicy {
    std::chrono::milliseconds firstPreviewWait{1000};
    std::chrono::milliseconds receiverResetHold{100}; // P[8] bit 6 held this long (tunable)
    std::chrono::milliseconds settleAfterClear{500};
    std::chrono::milliseconds previewWaitAfterClear{1500};
    std::chrono::milliseconds slowStartWait{11000}; // no overflow flags: the old 12 s wait in total
    int attempts{4}; // each reset is a fresh try at the lane deskew (tunable)
};

struct AlignLockResult {
    bool locked{false};
    bool recovered{false}; // needed at least one reset
    int clears{0};
    std::string error; // operator text when not locked
};

AlignLockResult awaitAlignLock(const AlignLockHooks& hooks, const AlignLockPolicy& policy = {});

} // namespace backend::pz
