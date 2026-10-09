#pragma once

// The every-frame ring of the results13 image (#649): the newest N frames, each as a platform store record
// (record set + MONO8 block + MASK1 block) in PS DDR outside Linux's RAM. This is the reader: it validates the
// ring placement, programs the store registers before ARM, waits for the ring to freeze after STOP, and
// copies a frame under the board owner's reader rule (pz7035 docs/FRAME_RING.md):
//   lo = max(0, HEAD + 1 - N); readable records are lo <= seq < FINAL; after a copy re-read HEAD' and keep the
//   copy only if seq >= max(0, HEAD' + 1 - N); the FRAME header is compared before and after as a second check.
// Frozen means STATE = IDLE after STOP and FINAL = HEAD + 1, never earlier.

#include "backend/pz/PzRecords.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::pz {

inline constexpr uint32_t kRingRegFinalSeq = 0x3BCu;      // STORE_FINAL_SEQ, ABI 1.4
// STORE_STATE: bits 8:0 are the state code; two sticky bits (results13 fb1d858) hold until the next ARM.
inline constexpr uint32_t kRingStateCodeMask = 0x1FFu;
inline constexpr uint32_t kRingStateStalled = 1u << 9;    // RING_STALLED: an unacknowledged record left the 256-entry window, FINAL is frozen below it
inline constexpr uint32_t kRingStateStopStuck = 1u << 10; // STOP_STUCK: STOP did not complete in about 84 ms, the device stays DRAINING
inline constexpr uint32_t kStoreModeContinuous = 1u;
inline constexpr uint32_t kStoreModeRingOnly = 1u << 9;   // no drain: overwriting is the purpose
inline constexpr uint64_t kRingCeiling = 0x3F000000ull;   // the PL's result ring and preview slots start here
inline constexpr uint64_t kRingFloor = 0x00100000ull;     // the PL does not enforce a DDR floor: Studio does
inline constexpr uint32_t kRingRecordBytes = 59392;       // 512 x 96: 4096 set + 49152 MONO8 + 6144 MASK1, rounded
inline constexpr uint32_t kRingSetBytes = 4096;
inline constexpr uint32_t kRingBaseAlign = 4096;

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
RingPlan planRing(uint32_t frames, uint64_t linuxRamEnd, uint32_t recordBytes = kRingRecordBytes);
// End (exclusive) of the highest "System RAM" range in /proc/iomem text, i.e. where the `mem=` limit puts Linux.
std::optional<uint64_t> systemRamEnd(const std::string& iomemText);

struct RingStatus {
    bool valid{false};          // the registers describe a configured ring
    std::string why;            // what is wrong when !valid
    uint32_t state{0};          // STORE_STATE code (PZ_MIB_STORE_STATE_*), the sticky bits removed
    bool stalled{false};        // RING_STALLED
    bool stopStuck{false};      // STOP_STUCK
    bool fault{false};          // FAULT, RING_STALLED or STOP_STUCK: the ring is invalid, re-arm (stop, RESET_GENERATION, ARM)
    std::string invalidReason;  // why, when fault; playback is not offered
    int64_t head{-1};           // newest sequence started (-1: none)
    uint64_t final{0};          // every sequence below it is complete and acknowledged
    uint32_t records{0};        // N
    uint32_t recordBytes{0};
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
    bool maskPresent{false};    // false: the mask block was NO_RESULT (the U-Net dropped the frame)
    bool resultsTruncated{false};
    std::vector<uint8_t> gray;  // width x height
    std::vector<uint8_t> mask;  // 1 bit per pixel, LSB first, row-major
    std::vector<RingCell> cells;
};

enum class RingRead { Ok, Unavailable, OutOfRange, Overwritten, Malformed };
const char* ringReadName(RingRead r);

class PzFrameRing {
public:
    explicit PzFrameRing(IRingIo& io) : io_(io) {}
    // Before ARM: base, number of records and STORE_MODE = CONTINUOUS | RING_ONLY, read back.
    bool program(const RingPlan& plan, std::string* error);
    RingStatus status();
    // After STOP: poll until frozen, a fault, or the timeout (the watchdog bounds the open frame to about 84 ms).
    RingStatus awaitFrozen(std::chrono::milliseconds timeout);
    RingRead readFrame(uint64_t seq, RingFrame& out, std::string* why);

private:
    IRingIo& io_;
};

// 'MIBR' v1: one ring frame for the browser. 48-byte little-endian header
//   0 "MIBR", 4 u16 version 1, 6 u16 header bytes 48, 8 u64 seq, 16 u64 frame id, 24 u64 timestamp ticks,
//   32 u32 tick Hz, 36 u32 frame flags, 40 u16 width, 42 u16 height, 44 u16 cells, 46 u16 bit0 mask present
//   bit1 results truncated
// then width*height gray bytes, width*height/8 mask bytes, then per cell 19 u32 words: payload words 0-14,
// word 15 = x | y << 16, word 16 = width | height << 16, word 17 = cells << 24 | index << 16 | valid, as the 'MIBC' run
// preview, and word 18 = the payload validity mask (a word the mask leaves out is not available, not 0).
std::vector<uint8_t> buildRingPacket(const RingFrame& frame, uint32_t tickHz);

} // namespace backend::pz
