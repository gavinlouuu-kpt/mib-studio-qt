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
    // One clear step for the sticky flags: a receiver reset (P[8] bit 6, bit 4 kept) held for the
    // given time. Board measurements 2026-10-08: each reset is an independent ~40-45 % chance to
    // clear the flags whatever its length (100 us to 100 ms); once clear they stay clear. The rx
    // buffer clear (bit 5) and a timing rewrite + reset are not cures. It is this one function so
    // the sequence can change.
    std::function<bool(std::chrono::milliseconds hold)> clearFlags;
    // Called for every attempt (count and log it).
    std::function<void(int attempt, const IngressStatus&)> onAttempt;
    std::function<void(std::chrono::milliseconds)> pause;
};

struct AlignLockPolicy {
    std::chrono::milliseconds firstPreviewWait{1000};
    std::chrono::milliseconds receiverResetHold{100}; // P[8] bit 6 held this long (tunable)
    std::chrono::milliseconds settleAfterClear{400}; // the receiver settles ~35 ms after the reset
    std::chrono::milliseconds previewWaitAfterClear{1500}; // only once the flags read clear
    std::chrono::milliseconds slowStartWait{11000}; // no overflow flags: the old 12 s wait in total
    int attempts{8}; // ~40-45 % per reset: eight leave about a 1-2 % miss (tunable)
};

struct AlignLockResult {
    bool locked{false};
    bool recovered{false}; // needed at least one reset
    int clears{0};
    std::string error; // operator text when not locked
};

AlignLockResult awaitAlignLock(const AlignLockHooks& hooks, const AlignLockPolicy& policy = {});

} // namespace backend::pz
