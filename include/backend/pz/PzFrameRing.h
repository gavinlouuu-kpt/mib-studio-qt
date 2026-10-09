#pragma once

// The every-frame ring of the results13 image (#649): the newest N frames, each as a platform store record
// (record set + MONO8 block + MASK1 block) in PS DDR outside Linux's RAM. This is the reader: it validates the
// ring placement, programs the store registers before ARM, waits for the ring to freeze after STOP, and
// copies a frame under the board owner's reader rule (pz7035 docs/FRAME_RING.md):
//   lo = max(0, HEAD + 1 - N); readable records are lo <= seq < FINAL; after a copy re-read HEAD' and keep the
//   copy only if seq >= max(0, HEAD' + 1 - N); the FRAME header is compared before and after as a second check.
// Frozen means STATE = IDLE after STOP and FINAL = HEAD + 1 with no fault, never earlier. The sticky STOP_STUCK bit may be set on a frozen
// ring (the device reached IDLE after all): frozen with stopIncomplete is the state where playback is allowed and flagged.
// Limit: sequences are 32 bits and the arithmetic is unsigned; a ring-only run near 2^32 (about 9.9 days at 5 kHz) is refused
// ("sequence limit reached: re-arm"), there is no wrap support.

#include "backend/pz/PzRecords.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace backend::pz {

inline constexpr uint32_t kRingRegFinalSeq = 0x3BCu;      // STORE_FINAL_SEQ, ABI 1.4
// The device's STATE is the bridge STATE register (0x040: IDLE, ARMED, RUNNING, DRAINING, FAULT). STORE_STATE (0x3A0) is used only for its three
// sticky fault bits, which hold until the next ARM or a hardware reset (RESET_GENERATION does not clear them): in ring-only mode its state code never
// reads DRAINING (the drain is off), so it says nothing about a STOP. Final wording of the board owner (pz7035 docs/FRAME_RING.md):
inline constexpr uint32_t kRingStateStalled = 1u << 9;       // RING_STALLED: an unacknowledged record left the tracking window, FINAL is frozen below it
inline constexpr uint32_t kRingStateStopStuck = 1u << 10;    // STOP_STUCK: the tap or its clock is broken; the device stays DRAINING (records below FINAL are stable)
inline constexpr uint32_t kRingStateResetRefused = 1u << 11; // RESET_GENERATION was written outside IDLE and refused
// After STOP the device reaches IDLE by itself in about 2 ms (the tap closes an open frame at the next SOF or after 2 ms without data,
// the frame marked bad), so a wait much longer than that means a hardware fault: "restore needed".
inline constexpr int kRingFreezeWaitMs = 1000;
inline constexpr uint32_t kStoreModeContinuous = 1u;
inline constexpr uint32_t kStoreModeRingOnly = 1u << 9;   // no drain: overwriting is the purpose
inline constexpr uint64_t kRingCeiling = 0x3F000000ull;   // the PL's result ring and preview slots start here
inline constexpr uint64_t kRingFloor = 0x00100000ull;     // the PL does not enforce a DDR floor: Studio does
inline constexpr uint32_t kRingRecordBytes = 59392;       // 512 x 96: 4096 set + 49152 MONO8 + 6144 MASK1, rounded
inline constexpr uint32_t kRingSetBytes = 4096;
inline constexpr uint32_t kRingBaseAlign = 4096;
inline constexpr uint64_t kRingSeqLimit = 0xFFF00000ull;  // head at or beyond this: the 32-bit sequence is about to wrap

// Register and memory access of the bridge window and PL-owned DDR (a /dev/mem mapping on the board, a fake in tests).
class IRingIo {
public:
    virtual ~IRingIo() = default;
    virtual uint32_t reg(uint32_t offset) = 0;
    virtual void setReg(uint32_t offset, uint32_t value) = 0;
    virtual bool read(uint64_t physical, void* dst, size_t bytes) = 0;
};

// Where a ring of `frames` records goes: ending at the PL carve-out, above Linux's RAM and the floor.
struct RingPlan {
    bool ok{false};
    std::string why;            // the gate reason when !ok
    uint64_t base{0};
    uint32_t records{0};
    uint64_t bytes{0};
    uint64_t maxRecords{0};     // what fits above Linux's RAM
};
// `linuxRamEnd` is required: 0 or anything below the DDR floor means "unknown" and refuses the plan (a ring must never be placed
// without knowing where Linux's RAM ends).
RingPlan planRing(uint32_t frames, uint64_t linuxRamEnd, uint32_t recordBytes = kRingRecordBytes);
// End (exclusive) of the highest "System RAM" range in /proc/iomem text, i.e. where the `mem=` limit puts Linux. nullopt when there is
// none or when the ranges are all zero (an unprivileged read of /proc/iomem shows zeros).
std::optional<uint64_t> systemRamEnd(const std::string& iomemText);

struct RingStatus {
    bool valid{false};          // the registers describe a configured ring
    std::string why;            // what is wrong when !valid
    uint32_t state{0};          // the bridge STATE (PZ_MIB_STATE_*)
    bool stalled{false};        // RING_STALLED
    bool stopStuck{false};      // STOP_STUCK
    bool resetRefused{false};   // RESET_GENERATION was refused
    bool fault{false};          // bridge FAULT, RING_STALLED or RESET_GENERATION refused: the ring is invalid, re-arm (stop, RESET_GENERATION in IDLE, ARM)
    std::string invalidReason;  // why, when fault; playback is not offered
    // STOP_STUCK: the records below FINAL stay stable and readable under the reader rule, flagged "stop incomplete"; the ring is not frozen.
    bool stopIncomplete{false};
    // Set by the provider when the wait for IDLE ran out without a sticky bit: a hardware fault, the PL needs a restore.
    bool restoreNeeded{false};
    int64_t head{-1};           // newest sequence started (-1: none)
    uint64_t final{0};          // every sequence below it is complete and acknowledged
    uint32_t records{0};        // N
    uint32_t recordBytes{0};
    uint32_t setBytes{0};       // STORE_SET_BYTES: the record set area at the start of every record
    uint32_t epoch{0};          // the bridge epoch and generation: a re-ARM or a reset changes them
    uint32_t generation{0};
    uint64_t base{0};
    uint64_t lo{0};             // oldest readable sequence
    bool frozen{false};         // STATE = IDLE and FINAL = HEAD + 1
    uint64_t count() const { return final > lo ? final - lo : 0; }
};

struct RingCell {
    uint16_t x{0}, y{0}, width{0}, height{0};
    uint16_t flags{0};
    bool valid{false};
    uint16_t index{0};          // result index within the frame
    uint32_t payloadValidity{0}; // which payload words carry a value (the others are 'not available', never 0)
    std::array<uint32_t, 15> payload{};  // unet_cells_v2 words 0-14
};

struct RingFrame {
    uint64_t seq{0};
    uint64_t frameId{0};
    uint64_t timestampTicks{0};
    uint32_t frameFlags{0};
    uint16_t width{0}, height{0};
    bool maskPresent{false};    // false: the mask block was NO_RESULT or INCOMPLETE (the U-Net dropped or cut the frame): never shown as present
    bool resultsTruncated{false};
    bool frameInvalid{false};   // FRAME flags INVALID or PARTIAL: an ingress-error frame, nothing was measured
    bool cut{false};            // the MONO8 block is INCOMPLETE: the tap closed the frame early (an open frame at STOP, or a lost line)
    bool maskIncomplete{false}; // the MASK1 block is INCOMPLETE
    std::vector<uint8_t> gray;  // width x height
    std::vector<uint8_t> mask;  // 1 bit per pixel, LSB first, row-major
    std::vector<RingCell> cells;
};

// The outcome of leaving a previous run or of arming a new ring: ok, or why not (and whether only a restore of the PL helps).
struct RingArmOutcome {
    bool ok{false};
    bool restoreNeeded{false};
    std::string why;
};

enum class RingRead { Ok, Unavailable, OutOfRange, Overwritten, Malformed };
const char* ringReadName(RingRead r);

class PzFrameRing {
public:
    // `linuxRamEnd`: where Linux's RAM ends (the `mem=` limit, from /proc/iomem). Required: with 0 (unknown) the ring is never valid, so a
    // wrong register can never make a read expose Linux RAM.
    PzFrameRing(IRingIo& io, uint64_t linuxRamEnd) : io_(io), linuxRamEnd_(linuxRamEnd) {}
    // Before ARM: base, number of records and STORE_MODE = CONTINUOUS | RING_ONLY, read back.
    bool program(const RingPlan& plan, std::string* error);
    RingStatus status();
    // Before a new ring is programmed: leave the previous run. The bridge STATE must be IDLE (the RTL ignores ARM and refuses RESET_GENERATION
    // in any other state): a leftover ARMED or RUNNING is stopped first, DRAINING is waited out (bounded), a FAULT is cleared; a FAULT register
    // that stays set refuses (ARM would be ignored). Then RESET_GENERATION, in IDLE, when a sticky fault bit was left, verified by the generation.
    // `onMutate` runs once before the first register write (the caller drops its "ring armed" claim there, never earlier).
    RingArmOutcome quiesce(const std::function<void()>& onMutate, std::chrono::milliseconds bound = std::chrono::milliseconds(kRingFreezeWaitMs));
    // After ARM: the bridge STATE must read ARMED (or RUNNING), otherwise the ARM was ignored.
    RingArmOutcome awaitArmed(std::chrono::milliseconds bound = std::chrono::milliseconds(200));
    // After STOP: poll until frozen, a fault, STOP_STUCK, or the bound (the device reaches IDLE by itself in about 2 ms). Past the bound without
    // a sticky bit the status says `restoreNeeded`: the tap or its clock is broken.
    RingStatus awaitFrozen(std::chrono::milliseconds timeout = std::chrono::milliseconds(kRingFreezeWaitMs));
    RingRead readFrame(uint64_t seq, RingFrame& out, std::string* why);

private:
    IRingIo& io_;
    uint64_t linuxRamEnd_;
};

// 'MIBR' v1: one ring frame for the browser. 48-byte little-endian header
//   0 "MIBR", 4 u16 version 1, 6 u16 header bytes 48, 8 u64 seq, 16 u64 frame id, 24 u64 timestamp ticks,
//   32 u32 tick Hz, 36 u32 frame flags, 40 u16 width, 42 u16 height, 44 u16 cells, 46 u16 flags: bit0 mask present,
//   bit1 results truncated, bit2 frame invalid, bit3 frame cut (MONO8 incomplete), bit4 mask incomplete
// then width*height gray bytes, width*height/8 mask bytes, then per cell 19 u32 words: payload words 0-14,
// word 15 = x | y << 16, word 16 = width | height << 16, word 17 = cells << 24 | index << 16 | valid, as the 'MIBC' run
// preview, and word 18 = the payload validity mask (a word the mask leaves out is not available, not 0).
std::vector<uint8_t> buildRingPacket(const RingFrame& frame, uint32_t tickHz);

} // namespace backend::pz
