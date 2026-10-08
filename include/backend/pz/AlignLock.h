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

// The PL receiver self-heal (results9, pz7035 docs/YOFO_HOST_INTERFACE.md): diagnostics window at
// 0x40100400 (P[256..]), ID 'RXH1' at index 0; reads 0 on an image without the block.
inline constexpr uint32_t kRxHealId = 0x52584831u;
inline constexpr unsigned kRxHealWindow = 256; // P[256] = 0x40100400
inline constexpr unsigned kRxFsFsSeen = 32, kRxFsNoFsFrames = 33, kRxFsNoFsLines = 34; // results12: frames start only after a FrameStart line
inline constexpr unsigned kRxHealV2Word = 24;  // non-zero on heal v2 builds (results11+), which back off on their own
// CTRL word 1 = [0] enable, [15:8] max tries, [23:16] persist ms, [31:24] clear ms. Default 0x64140801; the
// qualified standing value for v1 raises persist to 250 ms (0xFA): 2/14 PL gave-ups against 7/22 (2026-10-08).
inline constexpr uint32_t kRxHealCtrlPersist250 = 0x64FA0801u;
struct RxHealStatus {
    bool present{false};
    uint32_t control{0};    // [0] enable, [15:8] max tries, [23:16] persist ms, [31:24] clear ms
    uint32_t status{0};     // [7:0] tries this episode, [8] gave up, [9] flags set now, [10] settled, [23:16] lane flags
    uint32_t autoResets{0}; // auto-reset count since the PL reset
    uint32_t lastPulse{0};  // [7:0] flags at the pulse, [15:8] its try number
    // Heal v2 (results11+; CTRL2 at word 24 is non-zero): flag clears and episodes counted separately.
    bool v2{false};
    uint32_t flagClears{0};     // word 5: flag clears issued (a clear during Run means frames were corrupted before it)
    uint32_t episodes{0};       // word 6
    uint32_t failedEpisodes{0}; // word 7: episodes that reached persistent failure
    unsigned tries() const { return status & 0xFFu; }
    bool gaveUp() const { return (status & 0x100u) != 0; }
    bool flagsSet() const { return (status & 0x200u) != 0; }
    uint32_t laneFlags() const { return (status >> 16) & 0xFFu; }
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
    // Optional: the PL self-heal status (present = false on an image without it). When the block is
    // there the PL does the resets: the host waits for it and does not pulse the receiver itself.
    std::function<bool(RxHealStatus&)> readHeal;
    // Called once when the PL block gave up or did not finish (count and log it): its state, whether it
    // reported gave-up, and the milliseconds from the start of the wait.
    std::function<void(const RxHealStatus&, bool gaveUp, long long afterMs)> onPlGaveUp;
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
    std::chrono::milliseconds healWait{6000}; // the PL block makes up to 8 tries of ~0.1 s each
    std::chrono::milliseconds healPoll{250};
    int attempts{8}; // ~40-45 % per reset: eight leave about a 1-2 % miss (tunable)
};

struct AlignLockResult {
    bool locked{false};
    bool recovered{false}; // needed at least one reset
    bool healedByPl{false}; // the PL self-heal cleared the flags (no host reset)
    bool plGaveUp{false};   // the PL block gave up (or did not finish); the host recovery ran as the backstop
    long long plGaveUpAfterMs{0}; // from the start of the wait to that point
    int clears{0};
    std::string error; // operator text when not locked
};

AlignLockResult awaitAlignLock(const AlignLockHooks& hooks, const AlignLockPolicy& policy = {});

} // namespace backend::pz
