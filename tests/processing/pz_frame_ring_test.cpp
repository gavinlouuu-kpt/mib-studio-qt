// PzFrameRing (#649 v1): the results13 ring reader against a fake ring in memory. The reader rule is the board
// owner's final wording (pz7035 docs/FRAME_RING.md): lo = max(0, HEAD + 1 - N), readable lo <= seq < FINAL,
// re-read HEAD' after the copy and drop the copy when the ring moved past it; frozen = STATE IDLE and
// FINAL = HEAD + 1. On top of it: a re-ARM during a copy, hostile registers and hostile record headers.
#include "backend/pz/PzFrameRing.h"

#include "pz_mib_abi.h"
#include "support/assert.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace pz = backend::pz;

namespace {

constexpr uint32_t kN = 4;                 // ring records
constexpr uint32_t kRec = pz::kRingRecordBytes;
constexpr uint64_t kBase = 0x3EF00000ull;  // inside the window, 4 KiB aligned
constexpr uint64_t kLinuxEnd = 0x2D000000ull;  // mem=720M
constexpr uint16_t kW = 512, kH = 96;

struct FakeIo final : pz::IRingIo {
    std::map<uint32_t, uint32_t> regs;
    std::vector<uint8_t> mem = std::vector<uint8_t>(static_cast<size_t>(kN) * kRec, 0); // from kBase
    std::function<void(uint64_t, size_t)> onRead;
    int reads = 0;
    uint32_t reg(uint32_t o) override { return regs[o]; }
    void setReg(uint32_t o, uint32_t v) override { regs[o] = v; }
    bool read(uint64_t phys, void* dst, size_t n) override {
        ++reads;
        if (onRead) onRead(phys, n);
        if (phys < kBase || phys + n > kBase + mem.size()) return false;
        std::memcpy(dst, mem.data() + (phys - kBase), n);
        return true;
    }
    // `bridgeState`: the bridge STATE register (0x040); `sticky`: the sticky fault bits of STORE_STATE (0x3A0), the only part of it Studio reads.
    void configure(uint32_t head, uint32_t final_, uint32_t bridgeState, uint32_t sticky = 0) {
        regs[PZ_MIB_REG_STORE_BASE_LO] = static_cast<uint32_t>(kBase);
        regs[PZ_MIB_REG_STORE_BASE_HI] = 0;
        regs[PZ_MIB_REG_STORE_RECORDS] = kN;
        regs[PZ_MIB_REG_STORE_RECORD_BYTES] = kRec;
        regs[PZ_MIB_REG_STORE_SET_BYTES] = pz::kRingSetBytes;
        regs[PZ_MIB_REG_EPOCH] = 5;
        regs[PZ_MIB_REG_GENERATION] = 2;
        regs[PZ_MIB_REG_STORE_HEAD_SEQ] = head;
        regs[pz::kRingRegFinalSeq] = final_;
        regs[PZ_MIB_REG_STATE] = bridgeState;
        regs[PZ_MIB_REG_STORE_STATE] = sticky;
    }
};

struct RecOpts {
    bool maskNoResult = false;
    bool maskIncomplete = false;
    bool monoIncomplete = false;
    bool frameInvalid = false;
    uint64_t frameId = 0;       // 0: 1000 + seq
    uint64_t resultFrameId = 0; // 0: the frame's
};

// The record set of a frame: FRAME, two RESULTs, IMAGE MONO8, IMAGE MASK1 (offsets inside the record).
std::vector<uint8_t> makeSet(uint64_t seq, const RecOpts& o, pz::ImageRecord* monoOut = nullptr, pz::ImageRecord* maskOut = nullptr) {
    const uint64_t frameId = o.frameId ? o.frameId : 1000 + seq;
    std::vector<uint8_t> set;
    auto add = [&](const std::vector<uint8_t>& r) { set.insert(set.end(), r.begin(), r.end()); };
    pz::FrameRecord f;
    f.runId = 7;
    f.frameId = frameId;
    f.timestamp = 5000 * frameId;
    f.epoch = 3;
    f.flags = o.frameInvalid ? pz::kFrameInvalid : 0;
    f.resultCount = 2;
    f.resultLimit = 16;
    f.scienceProfile = 2;
    f.profileVersion = 3;
    f.width = kW;
    f.height = kH;
    f.pixelFormat = PZ_MIB_PIXEL_FORMAT_MONO8;
    add(pz::encodeFrameRecord(f, 0));
    for (uint16_t i = 0; i < 2; ++i) {
        pz::ResultRecord r;
        r.frameId = o.resultFrameId ? o.resultFrameId : frameId;
        r.resultIndex = i;
        r.flags = i == 0 ? pz::kResultValid : pz::kResultInvalid;
        r.scienceProfile = 2;
        r.profileVersion = 3;
        r.bboxX = 100 + 10 * i;
        r.bboxY = 20;
        r.bboxW = 30;
        r.bboxH = 40;
        r.payloadValidity = 0x7FFF;
        for (uint32_t k = 0; k < 15; ++k) r.payload.push_back(1000 * (i + 1) + k);
        add(pz::encodeResultRecord(r, 1 + i));
    }
    pz::ImageRecord mono;
    mono.frameId = frameId;
    mono.byteOffset = pz::kRingSetBytes;
    mono.byteLength = kW * kH;
    mono.pixelFormat = PZ_MIB_PIXEL_FORMAT_MONO8;
    mono.width = kW;
    mono.height = kH;
    mono.stride = kW;
    mono.epoch = 3;
    mono.flags = o.monoIncomplete ? PZ_MIB_IMAGE_FLAGS_INCOMPLETE : 0;
    pz::ImageRecord mask = mono;
    mask.byteOffset = pz::kRingSetBytes + kW * kH;
    mask.byteLength = kW * kH / 8;
    mask.pixelFormat = PZ_MIB_PIXEL_FORMAT_MASK1;
    mask.stride = kW / 8;
    mask.flags = static_cast<uint8_t>((o.maskNoResult ? PZ_MIB_IMAGE_FLAGS_NO_RESULT : 0) | (o.maskIncomplete ? PZ_MIB_IMAGE_FLAGS_INCOMPLETE : 0));
    if (monoOut) *monoOut = mono;
    if (maskOut) *maskOut = mask;
    add(pz::encodeImageRecord(mono, 3));
    add(pz::encodeImageRecord(mask, 4));
    return set;
}

void putSet(FakeIo& io, uint64_t seq, const std::vector<uint8_t>& set, uint8_t grayBase) {
    MIB_REQUIRE(set.size() <= pz::kRingSetBytes, "record set fits");
    uint8_t* slot = io.mem.data() + (seq % kN) * static_cast<size_t>(kRec);
    std::memset(slot, 0, kRec);
    std::memcpy(slot, set.data(), set.size());
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) slot[pz::kRingSetBytes + i] = static_cast<uint8_t>(grayBase + i % 251);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH / 8; ++i) slot[pz::kRingSetBytes + kW * kH + i] = static_cast<uint8_t>(i * 7 + seq);
}

void putRecord(FakeIo& io, uint64_t seq, uint8_t grayBase, const RecOpts& o = {}) { putSet(io, seq, makeSet(seq, o), grayBase); }

uint64_t slotAddr(uint64_t seq) { return kBase + (seq % kN) * static_cast<uint64_t>(kRec); }

} // namespace

int main() {
    // ---- placement ---------------------------------------------------------------------------------------
    {
        const auto ok = pz::planRing(5000, kLinuxEnd); // Linux below 720 MiB
        MIB_EXPECT(ok.ok && ok.records == 5000 && ok.bytes == 5000ull * kRec, "5000 frames fit above 720 MiB");
        MIB_EXPECT(ok.base % 4096 == 0 && ok.base >= kLinuxEnd && 0x3F000000ull - (ok.base + ok.bytes) < 4096,
                   "base is aligned, above Linux, and the ring ends within one page of the PL carve-out");
        const auto full = pz::planRing(5000, 0x3F000000ull); // today's mem=1008M
        MIB_EXPECT(!full.ok && full.why.find("mem=") != std::string::npos && full.maxRecords == 0, "mem=1008M: refuse with the remedy");
        const auto big = pz::planRing(20000, kLinuxEnd);
        MIB_EXPECT(!big.ok && big.maxRecords > 0 && big.maxRecords < 20000 && big.why.find("room for") != std::string::npos, "too many frames: say how many fit");
        MIB_EXPECT(!pz::planRing(0, kLinuxEnd).ok, "0 frames refused");
        // The Linux RAM end is required: unknown (0, or anything below the floor) refuses the plan, so a ring is never placed blind.
        MIB_EXPECT(!pz::planRing(10, 0).ok && pz::planRing(10, 0).why.find("unknown") != std::string::npos, "unknown Linux RAM end (0): refused");
        MIB_EXPECT(!pz::planRing(10, 1).ok, "a Linux RAM end of 1 byte (a zeroed /proc/iomem) is unknown: refused");
        const std::string iomem =
            "00000000-0fffffff : System RAM\n  00008000-007fffff : Kernel code\n10000000-2cffffff : System RAM\n"
            "40000000-4fffffff : something else\n";
        MIB_EXPECT(pz::systemRamEnd(iomem) == kLinuxEnd, "System RAM end from /proc/iomem, nested lines ignored");
        MIB_EXPECT(!pz::systemRamEnd("40000000-4fffffff : uart\n").has_value(), "no System RAM line: unknown");
        MIB_EXPECT(!pz::systemRamEnd("00000000-00000000 : System RAM\n").has_value(), "an unprivileged read shows zero ranges: unknown, not 'RAM ends at 1'");
    }

    // ---- programming -------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        std::string err;
        const auto plan = pz::planRing(5000, kLinuxEnd);
        MIB_EXPECT(ring.program(plan, &err), "program: " + err);
        MIB_EXPECT(io.regs[PZ_MIB_REG_STORE_RECORDS] == 5000 && io.regs[PZ_MIB_REG_STORE_BASE_LO] == static_cast<uint32_t>(plan.base) &&
                       (io.regs[PZ_MIB_REG_STORE_MODE] & pz::kStoreModeRingOnly) && (io.regs[PZ_MIB_REG_STORE_MODE] & pz::kStoreModeContinuous),
                   "base, records and CONTINUOUS|RING_ONLY are written");
        MIB_EXPECT(!ring.program(pz::planRing(5000, 0x3F000000ull), &err) && err.find("mem=") != std::string::npos, "an invalid plan is never written");
        MIB_EXPECT((io.regs[PZ_MIB_REG_STORE_MODE] & pz::kStoreModeDrain) == 0, "a ring-only run does not set the SSD drain bit");
        // an SSD run (#667 S2): CONTINUOUS | RING_ONLY | DRAIN = 0x601
        FakeIo io2;
        pz::PzFrameRing ring2(io2, kLinuxEnd);
        MIB_EXPECT(ring2.program(plan, &err, /*ssdDrain=*/true), "program with the drain bit: " + err);
        MIB_EXPECT(io2.regs[PZ_MIB_REG_STORE_MODE] == 0x601u && pz::kStoreModeDrain == (1u << 10) && pz::kCapabilitySsdRecorder == (1u << 17),
                   "an SSD run arms the store in CONTINUOUS|RING_ONLY|DRAIN (0x601); the recorder capability is bit 17");
    }

    // ---- status ------------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        io.configure(0xFFFFFFFFu, 0, PZ_MIB_STATE_IDLE);
        auto s = ring.status();
        MIB_EXPECT(s.valid && s.head == -1 && s.count() == 0 && s.frozen, "no frame yet: empty and frozen");
        io.configure(9, 9, PZ_MIB_STATE_RUNNING); // 10 started, 9 complete, ring of 4
        s = ring.status();
        MIB_EXPECT(s.lo == 6 && s.final == 9 && s.count() == 3 && !s.frozen, "lo = HEAD + 1 - N; readable 6..8; running is not frozen");
        io.configure(9, 10, PZ_MIB_STATE_DRAINING);
        MIB_EXPECT(!ring.status().frozen, "DRAINING is not frozen even with FINAL = HEAD + 1");
        io.configure(9, 9, PZ_MIB_STATE_IDLE);
        MIB_EXPECT(!ring.status().frozen, "IDLE with FINAL below HEAD + 1 is not frozen");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        s = ring.status();
        MIB_EXPECT(s.frozen && s.lo == 6 && s.count() == 4 && !s.stopIncomplete, "IDLE and FINAL = HEAD + 1: frozen, the last 4 frames");
        io.configure(9, 10, PZ_MIB_STATE_FAULT);
        s = ring.status();
        MIB_EXPECT(s.fault && !s.frozen, "a ring fault is never frozen: re-arm needed");
        // The sticky bits hold until the next ARM and invalidate the ring whatever the state code says (RING_STALLED, bit 11).
        io.configure(9, 10, PZ_MIB_STATE_IDLE, pz::kRingStateStalled);
        s = ring.status();
        MIB_EXPECT(s.valid && s.stalled && s.fault && !s.frozen && s.state == PZ_MIB_STATE_IDLE &&
                       s.invalidReason.find("RING_STALLED") != std::string::npos,
                   "RING_STALLED: IDLE and FINAL = HEAD + 1 but not frozen, with the reason");
        io.configure(9, 10, PZ_MIB_STATE_DRAINING, pz::kRingStateStopStuck);
        s = ring.status();
        MIB_EXPECT(s.valid && s.stopStuck && s.stopIncomplete && !s.fault && !s.frozen && s.lo == 6 && s.final == 10,
                   "STOP_STUCK while DRAINING: not frozen, not invalid: stop incomplete, the records below FINAL stay readable");
        io.configure(9, 10, PZ_MIB_STATE_IDLE, pz::kRingStateStopStuck);
        s = ring.status();
        MIB_EXPECT(s.frozen && s.stopIncomplete && !s.fault && s.invalidReason.empty(),
                   "STOP_STUCK on a ring that reached IDLE after all: frozen AND stop incomplete = playback allowed and flagged");
        io.configure(9, 10, PZ_MIB_STATE_IDLE, pz::kRingStateResetRefused);
        s = ring.status();
        MIB_EXPECT(s.resetRefused && s.fault && !s.frozen && s.invalidReason.find("RESET_GENERATION") != std::string::npos,
                   "bit 11: RESET_GENERATION was refused: invalid");
        io.configure(9, 10, PZ_MIB_STATE_IDLE, pz::kRingStateStalled | pz::kRingStateStopStuck);
        MIB_EXPECT(ring.status().fault && !ring.status().frozen, "stalled and stuck: invalid (the stall wins)");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        MIB_EXPECT(ring.status().frozen && !ring.status().fault, "frozen means IDLE with no fault bit set");
        // Hostile or wrong registers never make a read reach Linux's RAM or wrap the arithmetic.
        io.regs[PZ_MIB_REG_STORE_RECORDS] = 0;
        MIB_EXPECT(!ring.status().valid, "no ring configured");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_LO] = 0x1000;
        MIB_EXPECT(!ring.status().valid, "a base below the floor is not trusted");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_HI] = 1;
        MIB_EXPECT(!ring.status().valid, "BASE_HI != 0 is not trusted (the PL area is below 4 GiB)");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_LO] = 0xFFFFF000u;
        io.regs[PZ_MIB_REG_STORE_RECORDS] = 0xFFFFFFFFu;
        MIB_EXPECT(!ring.status().valid, "a base near 4 GiB with a huge record count cannot wrap the sum into 'valid'");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_LO] = static_cast<uint32_t>(kLinuxEnd - 0x100000);
        MIB_EXPECT(!ring.status().valid && ring.status().why.find("Linux") != std::string::npos, "a base inside Linux's RAM is refused");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_LO] = static_cast<uint32_t>(0x3F000000ull - 2ull * kRec);
        MIB_EXPECT(!ring.status().valid, "records reaching past the PL area are refused");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_SET_BYTES] = 0;
        MIB_EXPECT(!ring.status().valid && ring.status().why.find("STORE_SET_BYTES") != std::string::npos, "an unset record-set size is not trusted");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_SET_BYTES] = kRec + 8;
        MIB_EXPECT(!ring.status().valid, "a record-set size larger than the record is not trusted");
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        pz::PzFrameRing blind(io, 0);
        MIB_EXPECT(!blind.status().valid && blind.status().why.find("unknown") != std::string::npos,
                   "without a known end of Linux's RAM the ring is never valid");
        // 32-bit sequences: no wrap support; near 2^32 the run is refused with a reason.
        io.configure(0xFFF00001u, 0xFFF00002u, PZ_MIB_STATE_RUNNING);
        s = ring.status();
        MIB_EXPECT(s.valid && s.fault && s.invalidReason.find("sequence limit") != std::string::npos, "the 32-bit sequence limit is a refusal with a reason, not a wrap");
    }

    // ---- awaitFrozen -------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        io.configure(9, 9, PZ_MIB_STATE_DRAINING);
        // The state turns to IDLE after a few register polls (the open frame completes).
        struct Ticker final : pz::IRingIo {
            FakeIo& f; int n = 0;
            explicit Ticker(FakeIo& x) : f(x) {}
            uint32_t reg(uint32_t o) override {
                if (o == PZ_MIB_REG_STATE && ++n > 12) { f.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_IDLE; f.regs[pz::kRingRegFinalSeq] = 10; }
                return f.reg(o);
            }
            void setReg(uint32_t o, uint32_t v) override { f.setReg(o, v); }
            bool read(uint64_t p, void* d, size_t b) override { return f.read(p, d, b); }
        } ticker(io);
        pz::PzFrameRing r2(ticker, kLinuxEnd);
        const auto frozen = r2.awaitFrozen(std::chrono::milliseconds(500));
        MIB_EXPECT(frozen.frozen && frozen.final == 10 && !frozen.restoreNeeded, "frozen once the open frame completed");
        io.configure(9, 9, PZ_MIB_STATE_DRAINING);
        const auto stuck = ring.awaitFrozen(std::chrono::milliseconds(20));
        MIB_EXPECT(!stuck.frozen && stuck.restoreNeeded,
                   "a ring that never freezes within the bound and shows no sticky bit needs a restore (the bound is owned here, not by callers)");
        MIB_EXPECT(pz::kRingFreezeWaitMs == 1000, "the default bound is 1 s");
        // Prompt returns: a ring that will not freeze by waiting is not waited on.
        struct Case { uint32_t bridge, sticky; };
        for (const Case c : {Case{PZ_MIB_STATE_DRAINING, pz::kRingStateStopStuck}, Case{PZ_MIB_STATE_IDLE, pz::kRingStateStalled},
                             Case{PZ_MIB_STATE_IDLE, pz::kRingStateResetRefused}, Case{PZ_MIB_STATE_FAULT, 0}}) {
            io.configure(9, 10, c.bridge, c.sticky);
            const auto t0 = std::chrono::steady_clock::now();
            const auto st = ring.awaitFrozen(std::chrono::milliseconds(500));
            MIB_EXPECT(!st.frozen && !st.restoreNeeded && (st.fault || st.stopIncomplete) && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200),
                       "a ring with a sticky bit or a fault returns at once, and is not 'restore needed'");
        }
    }

    // ---- reading -----------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        for (uint64_t s = 6; s <= 9; ++s) putRecord(io, s, static_cast<uint8_t>(s * 3));
        io.configure(9, 10, PZ_MIB_STATE_IDLE);
        pz::RingFrame f;
        std::string why;
        MIB_EXPECT(ring.readFrame(7, f, &why) == pz::RingRead::Ok, "read 7: " + why);
        MIB_EXPECT(f.seq == 7 && f.frameId == 1007 && f.timestampTicks == 5000 * 1007ull && f.width == kW && f.height == kH, "frame fields");
        MIB_EXPECT(f.gray.size() == static_cast<size_t>(kW) * kH && f.gray[0] == 21 && f.gray[5] == static_cast<uint8_t>(21 + 5), "gray block is the one of sequence 7");
        MIB_EXPECT(f.maskPresent && f.mask.size() == static_cast<size_t>(kW) * kH / 8 && f.mask[1] == static_cast<uint8_t>(7 + 7), "mask block is packed, LSB first, of sequence 7");
        MIB_REQUIRE(f.cells.size() == 2, "two cells");
        MIB_EXPECT(f.cells[0].valid && !f.cells[1].valid && f.cells[0].x == 100 && f.cells[1].x == 110 && f.cells[0].width == 30 && f.cells[0].payload[3] == 1003 && f.cells[1].payload[14] == 2014,
                   "cells carry bbox, valid flag and the 15 payload words");
        MIB_EXPECT(!f.frameInvalid && !f.cut && !f.maskIncomplete, "a good record carries no degraded flag");
        MIB_EXPECT(ring.readFrame(6, f, &why) == pz::RingRead::Ok, "oldest readable record");
        MIB_EXPECT(ring.readFrame(9, f, &why) == pz::RingRead::Ok, "newest readable record");
        MIB_EXPECT(ring.readFrame(5, f, &why) == pz::RingRead::OutOfRange, "5 is gone (HEAD - N)");
        MIB_EXPECT(ring.readFrame(10, f, &why) == pz::RingRead::OutOfRange, "10 does not exist yet");

        // The packet the browser decodes.
        ring.readFrame(7, f, &why);
        const auto packet = pz::buildRingPacket(f, 100000000u);
        MIB_EXPECT(packet.size() == 48 + f.gray.size() + f.mask.size() + 2 * 19 * 4, "packet size");
        MIB_EXPECT(std::memcmp(packet.data(), "MIBR", 4) == 0 && packet[4] == 1 && packet[6] == 48 && packet[8] == 7 && packet[16] == (1007 & 0xFF) && packet[44] == 2 && packet[46] == 1,
                   "packet header: magic, version, seq, frame id, cells, mask present and no other flag");
        const size_t cellAt = 48 + f.gray.size() + f.mask.size();
        uint32_t w15 = 0, w17 = 0, w17b = 0, w18 = 0;
        std::memcpy(&w15, packet.data() + cellAt + 4 * 15, 4);
        std::memcpy(&w17, packet.data() + cellAt + 4 * 17, 4);
        std::memcpy(&w18, packet.data() + cellAt + 4 * 18, 4);
        std::memcpy(&w17b, packet.data() + cellAt + 19 * 4 + 4 * 17, 4);
        MIB_EXPECT(w15 == (100u | 20u << 16) && w17 == (2u << 24 | 0u << 16 | 1u) && w17b == (2u << 24 | 1u << 16 | 0u) && w18 == 0x7FFF,
                   "cell words 15 (x|y), 17 (count|index|valid) follow the run-preview layout; word 18 is the payload validity");
        // An empty ring has nothing to read.
        io.configure(0xFFFFFFFFu, 0, PZ_MIB_STATE_IDLE);
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::OutOfRange, "head = -1: nothing to read, out of range");
    }

    // ---- the reader rule: a record overwritten while it was copied, and a ring re-armed while it was copied ----
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        for (uint64_t s = 6; s <= 9; ++s) putRecord(io, s, static_cast<uint8_t>(s));
        pz::RingFrame f;
        std::string why;
        const auto blockAddr = [&](uint64_t seq) { return slotAddr(seq) + pz::kRingSetBytes; };
        // HEAD moves on while sequence 7 is being copied: slot of 7 is reused by 11 (7 mod 4 = 11 mod 4); its pixels are rewritten too.
        io.configure(10, 10, PZ_MIB_STATE_RUNNING); // running: 11 started, 10 complete, readable 7..9
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == blockAddr(7)) {
                std::fill(io.mem.begin() + (7 % kN) * static_cast<size_t>(kRec) + pz::kRingSetBytes, io.mem.begin() + (7 % kN) * static_cast<size_t>(kRec) + pz::kRingSetBytes + 49152, 0xEE);
                io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 11; // its pixel writes started: lo = 8, so 7 is gone
                io.onRead = nullptr;
            }
        };
        MIB_EXPECT(ring.readFrame(7, f, &why) == pz::RingRead::Overwritten, "pixels rewritten and HEAD' past the record: the copy is dropped (the old header still matches)");
        MIB_EXPECT(why.find("moved past") != std::string::npos, "the reason says why");
        // Record 9 is not in danger when HEAD advances by one.
        io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 10;
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == blockAddr(9)) { io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 11; io.onRead = nullptr; }
        };
        MIB_EXPECT(ring.readFrame(9, f, &why) == pz::RingRead::Ok, "a newer record survives HEAD advancing by one");
        // The header changes between the two reads of it: dropped.
        io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 10;
        bool second = false;
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n < pz::kRingSetBytes && phys == slotAddr(8)) {
                if (!second) { second = true; return; }
            }
            if (n == 49152 && phys == blockAddr(8)) io.mem[(8 % kN) * static_cast<size_t>(kRec) + 40] ^= 0xFF;
        };
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten, "a header that differs between the two reads is dropped");
        io.onRead = nullptr;

        // Small sequences: HEAD 2 -> 4 with N = 4 drops sequence 0 (lo goes 0 -> 1); no unsigned underflow anywhere.
        FakeIo small;
        pz::PzFrameRing smallRing(small, kLinuxEnd);
        for (uint64_t s = 0; s < 3; ++s) putRecord(small, s, 1);
        small.configure(2, 2, PZ_MIB_STATE_RUNNING);
        MIB_EXPECT(smallRing.status().lo == 0, "HEAD 2 with N = 4: lo is 0");
        small.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == slotAddr(0) + pz::kRingSetBytes) { small.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 4; small.onRead = nullptr; }
        };
        MIB_EXPECT(smallRing.readFrame(0, f, &why) == pz::RingRead::Overwritten, "HEAD 2 -> 4 with N = 4: sequence 0 is gone, dropped");
        small.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 2;
        small.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == slotAddr(1) + pz::kRingSetBytes) { small.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 4; small.onRead = nullptr; }
        };
        MIB_EXPECT(smallRing.readFrame(1, f, &why) == pz::RingRead::Ok, "HEAD 2 -> 4 with N = 4: sequence 1 survives");

        for (uint64_t q = 6; q <= 9; ++q) putRecord(io, q, static_cast<uint8_t>(q)); // the header test above damaged record 8
        // A re-ARM while a frame is being copied (Resume while the browser fetches): the head restarts, the old header still matches,
        // and the old pixels are mixed with new ones. HEAD' = 1 would pass the window rule (lo2 = 0), so the registers must catch it.
        io.configure(10, 10, PZ_MIB_STATE_RUNNING);
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == blockAddr(8)) { io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 1; io.onRead = nullptr; }
        };
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten && why.find("restarted") != std::string::npos,
                   "a re-ARM mid-read (HEAD restarts below the snapshot): dropped, even though HEAD' passes the window rule");
        io.configure(10, 10, PZ_MIB_STATE_RUNNING);
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == blockAddr(8)) { io.regs[PZ_MIB_REG_EPOCH] = 6; io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 12; io.onRead = nullptr; }
        };
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten && why.find("re-armed") != std::string::npos,
                   "a new epoch mid-read (a re-ARM that already got past the old head): dropped");
        for (const uint32_t reg : {static_cast<uint32_t>(PZ_MIB_REG_GENERATION), static_cast<uint32_t>(PZ_MIB_REG_STORE_RECORDS), static_cast<uint32_t>(PZ_MIB_REG_STORE_BASE_LO)}) {
            io.configure(10, 10, PZ_MIB_STATE_RUNNING);
            io.onRead = [&](uint64_t phys, size_t n) {
                if (n == 49152 && phys == blockAddr(8)) { io.regs[reg] += reg == PZ_MIB_REG_STORE_BASE_LO ? 0x1000u : 1u; io.onRead = nullptr; }
            };
            MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten, "a changed generation, record count or base mid-read: dropped");
        }
        io.configure(10, 10, PZ_MIB_STATE_RUNNING);
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == blockAddr(8)) { io.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_DRAINING; io.onRead = nullptr; }
        };
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten && why.find("state") != std::string::npos, "a state change mid-read (STOP, a re-ARM): dropped");
        io.onRead = nullptr;
    }

    // ---- degraded records --------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        io.configure(2, 3, PZ_MIB_STATE_IDLE);
        pz::RingFrame f;
        std::string why;
        RecOpts noResult; noResult.maskNoResult = true;
        putRecord(io, 1, 1, noResult);
        MIB_EXPECT(ring.readFrame(1, f, &why) == pz::RingRead::Ok && !f.maskPresent && !f.maskIncomplete && f.mask.size() == static_cast<size_t>(kW) * kH / 8 &&
                       std::all_of(f.mask.begin(), f.mask.end(), [](uint8_t b) { return b == 0; }),
                   "a NO_RESULT mask: frame readable, mask absent (all zero)");
        RecOpts incompleteMask; incompleteMask.maskIncomplete = true;
        putRecord(io, 1, 1, incompleteMask);
        MIB_EXPECT(ring.readFrame(1, f, &why) == pz::RingRead::Ok && !f.maskPresent && f.maskIncomplete &&
                       std::all_of(f.mask.begin(), f.mask.end(), [](uint8_t b) { return b == 0; }),
                   "an INCOMPLETE mask is never shown as present: excluded and flagged");
        RecOpts cutFrame; cutFrame.monoIncomplete = true; cutFrame.frameInvalid = true;
        putRecord(io, 1, 1, cutFrame);
        MIB_EXPECT(ring.readFrame(1, f, &why) == pz::RingRead::Ok && f.cut && f.frameInvalid, "a cut frame (MONO8 INCOMPLETE) and an invalid FRAME are flagged");
        const auto packet = pz::buildRingPacket(f, 100000000u);
        MIB_EXPECT((packet[46] & 4) && (packet[46] & 8) && (packet[46] & 1), "the packet carries frame invalid and cut, and the mask (present in this record)");
        // A record with RESULTs of another frame, with valid CRCs (a stale record of an older lap): malformed.
        RecOpts stale; stale.resultFrameId = 4242;
        putRecord(io, 1, 1, stale);
        MIB_EXPECT(ring.readFrame(1, f, &why) == pz::RingRead::Malformed && why.find("another frame") != std::string::npos, "a RESULT of another frame (valid CRC) reaches the frame-id check: malformed");
        std::memset(io.mem.data() + 2 * static_cast<size_t>(kRec), 0, 64); // a zeroed header
        MIB_EXPECT(ring.readFrame(2, f, &why) == pz::RingRead::Malformed, "a record without a FRAME is malformed");
        // The set area is STORE_SET_BYTES, not a constant: a smaller area still parses what fits.
        putRecord(io, 0, 1);
        io.regs[PZ_MIB_REG_STORE_SET_BYTES] = 256;
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed, "records beyond a 256-byte set area are not read past it (the IMAGE records fall outside it)");
        io.regs[PZ_MIB_REG_STORE_SET_BYTES] = pz::kRingSetBytes;
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Ok, "the full set area parses");
        io.regs[PZ_MIB_REG_STORE_RECORDS] = 0;
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Unavailable, "no ring: unavailable");
    }

    // ---- hostile record headers --------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io, kLinuxEnd);
        io.configure(2, 3, PZ_MIB_STATE_IDLE);
        pz::RingFrame f;
        std::string why;
        pz::ImageRecord mono, mask;
        const auto base = makeSet(0, {}, &mono, &mask);
        auto frameAndResults = [&] {
            std::vector<uint8_t> out;
            // everything before the two IMAGE records: FRAME + 2 RESULT
            const size_t images = pz::encodeImageRecord(mono, 3).size() + pz::encodeImageRecord(mask, 4).size();
            out.assign(base.begin(), base.end() - static_cast<std::ptrdiff_t>(images));
            return out;
        };
        auto with = [&](pz::ImageRecord m, pz::ImageRecord k) {
            auto set = frameAndResults();
            const auto a = pz::encodeImageRecord(m, 3), b = pz::encodeImageRecord(k, 4);
            set.insert(set.end(), a.begin(), a.end());
            set.insert(set.end(), b.begin(), b.end());
            putSet(io, 0, set, 1);
            return ring.readFrame(0, f, &why);
        };
        MIB_EXPECT(with(mono, mask) == pz::RingRead::Ok, "the rebuilt record reads");
        auto bad = mono; bad.byteOffset = 100; // into the record set area
        MIB_EXPECT(with(bad, mask) == pz::RingRead::Malformed, "a MONO8 block inside the set area is malformed");
        bad = mono; bad.byteOffset = kRec - 100; // beyond the record
        MIB_EXPECT(with(bad, mask) == pz::RingRead::Malformed, "a MONO8 block beyond the record is malformed");
        bad = mono; bad.byteLength = kW * kH - 8;
        MIB_EXPECT(with(bad, mask) == pz::RingRead::Malformed, "a MONO8 length that is not width x height is malformed");
        bad = mono; bad.width = 510; bad.stride = 510; bad.byteLength = 510 * kH;
        MIB_EXPECT(with(bad, mask) == pz::RingRead::Malformed, "a width that is not a multiple of 8 is malformed");
        bad = mono; bad.width = 0; bad.height = 0;
        pz::ImageRecord badMask = mask;
        MIB_EXPECT(with(bad, badMask) == pz::RingRead::Ok, "a MONO8 record without geometry takes it from the FRAME");
        badMask = mask; badMask.byteLength = 100;
        MIB_EXPECT(with(mono, badMask) == pz::RingRead::Malformed, "a MASK1 length that is not width x height / 8 is malformed");
        badMask = mask; badMask.byteOffset = kRec - 10;
        MIB_EXPECT(with(mono, badMask) == pz::RingRead::Malformed, "a MASK1 block beyond the record is malformed");
        // Duplicates and ordering.
        {
            auto set = frameAndResults();
            const auto a = pz::encodeImageRecord(mono, 3);
            set.insert(set.end(), a.begin(), a.end());
            set.insert(set.end(), a.begin(), a.end());
            putSet(io, 0, set, 1);
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed && why.find("two MONO8") != std::string::npos, "two MONO8 records are malformed");
            set = frameAndResults();
            const auto k = pz::encodeImageRecord(mask, 4);
            const auto m = pz::encodeImageRecord(mono, 3);
            set.insert(set.end(), m.begin(), m.end());
            set.insert(set.end(), k.begin(), k.end());
            set.insert(set.end(), k.begin(), k.end());
            putSet(io, 0, set, 1);
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed && why.find("two MASK1") != std::string::npos, "two MASK1 records are malformed");
            // The first record must be the FRAME: a RESULT first is malformed.
            std::vector<uint8_t> swapped;
            pz::ResultRecord r;
            r.frameId = 1000;
            r.scienceProfile = 2;
            r.profileVersion = 3;
            const auto res = pz::encodeResultRecord(r, 0);
            swapped.insert(swapped.end(), res.begin(), res.end());
            swapped.insert(swapped.end(), base.begin(), base.end());
            putSet(io, 0, swapped, 1);
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed && why.find("FRAME") != std::string::npos, "a record that does not start with FRAME is malformed");
            // A second FRAME.
            auto twoFrames = base;
            const auto fr = std::vector<uint8_t>(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(pz::encodeFrameRecord(pz::FrameRecord{}, 0).size()));
            twoFrames.insert(twoFrames.end() - 1, fr.begin(), fr.end());
            (void)twoFrames;
        }
        // A record length beyond the record maximum, in a header whose magic is right.
        {
            putRecord(io, 0, 1);
            uint16_t length = 5000;
            std::memcpy(io.mem.data() + 6, &length, 2); // the FRAME's length field
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed && why.find("length") != std::string::npos, "a record length above 4096 is malformed");
            length = 8;
            std::memcpy(io.mem.data() + 6, &length, 2);
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed, "a record length below the header size is malformed");
        }
    }

    // ---- leaving a run and arming a new ring (the bridge STATE as the RTL has it) ---------------------------
    {
        // A model of pz_mib_abi_regs.v: ARM only in IDLE with no fault; STOP only in ARMED/RUNNING (-> DRAINING, IDLE after the frames end);
        // RESET_GENERATION only in IDLE, refused (bit 11, sticky) elsewhere; FAULT_CLEAR returns FAULT to IDLE. STORE_STATE never reads
        // DRAINING in ring-only mode: it holds only the sticky bits, cleared by ARM.
        struct Bridge final : pz::IRingIo {
            std::map<uint32_t, uint32_t> regs;
            std::vector<std::string> log;
            int drainPolls = 2, drainLeft = 0;
            bool neverDrains = false, resetIgnored = false;
            uint32_t faultSources = 0;
            Bridge(uint32_t state = PZ_MIB_STATE_IDLE) { regs[PZ_MIB_REG_STATE] = state; regs[PZ_MIB_REG_GENERATION] = 7; }
            uint32_t reg(uint32_t o) override {
                if (o == PZ_MIB_REG_STATE && regs[PZ_MIB_REG_STATE] == PZ_MIB_STATE_DRAINING && !neverDrains && drainLeft > 0 && --drainLeft == 0)
                    regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_IDLE;
                return regs[o];
            }
            void setReg(uint32_t o, uint32_t v) override {
                if (o != PZ_MIB_REG_CONTROL) {
                    log.push_back("write " + std::to_string(o));
                    regs[o] = v;
                    return;
                }
                uint32_t& state = regs[PZ_MIB_REG_STATE];
                if (v & PZ_MIB_CONTROL_FAULT_CLEAR) { log.push_back("FAULT_CLEAR"); regs[PZ_MIB_REG_FAULT] = faultSources; if (state == PZ_MIB_STATE_FAULT) state = PZ_MIB_STATE_IDLE; }
                if (v & PZ_MIB_CONTROL_ARM) {
                    log.push_back("ARM");
                    if (state == PZ_MIB_STATE_IDLE && regs[PZ_MIB_REG_FAULT] == 0) {
                        state = PZ_MIB_STATE_ARMED;
                        regs[PZ_MIB_REG_STORE_STATE] &= ~(pz::kRingStateStalled | pz::kRingStateStopStuck | pz::kRingStateResetRefused);
                    }
                }
                if (v & PZ_MIB_CONTROL_STOP) {
                    log.push_back("STOP");
                    if (state == PZ_MIB_STATE_ARMED || state == PZ_MIB_STATE_RUNNING) { state = PZ_MIB_STATE_DRAINING; drainLeft = drainPolls; }
                }
                if (v & PZ_MIB_CONTROL_RESET_GENERATION) {
                    log.push_back("RESET_GENERATION");
                    if (state == PZ_MIB_STATE_IDLE) { if (!resetIgnored) regs[PZ_MIB_REG_GENERATION] += 1; }
                    else regs[PZ_MIB_REG_STORE_STATE] |= pz::kRingStateResetRefused;
                }
            }
            bool read(uint64_t, void*, size_t) override { return false; }
        };
        constexpr auto kShort = std::chrono::milliseconds(30);
        // 1. A clean IDLE bridge: nothing to leave, no register is written, the caller's claim is not dropped.
        {
            Bridge b;
            pz::PzFrameRing ring(b, kLinuxEnd);
            int mutated = 0;
            const auto q = ring.quiesce([&] { ++mutated; }, kShort);
            MIB_EXPECT(q.ok && b.log.empty() && mutated == 0, "an IDLE bridge with no sticky bit: quiesce writes nothing and the claim stays");
        }
        // 2. A leftover RUNNING bridge is stopped first, then waited out, and only then does anything else happen.
        {
            Bridge b(PZ_MIB_STATE_RUNNING);
            b.regs[PZ_MIB_REG_STORE_STATE] = pz::kRingStateStopStuck;
            pz::PzFrameRing ring(b, kLinuxEnd);
            int mutated = 0;
            const auto q = ring.quiesce([&] { ++mutated; }, std::chrono::milliseconds(200));
            MIB_EXPECT(q.ok && mutated == 1 && b.log.size() == 2 && b.log[0] == "STOP" && b.log[1] == "RESET_GENERATION" && b.regs[PZ_MIB_REG_STATE] == PZ_MIB_STATE_IDLE,
                       "RUNNING: STOP, wait for IDLE, then RESET_GENERATION in IDLE (STOP_STUCK left); the claim is dropped once, before the first write");
            MIB_EXPECT(b.regs[PZ_MIB_REG_GENERATION] == 8, "the generation moved");
        }
        // 3. DRAINING that never ends: no register is written after the bound, restore needed (the old code polled a state that never reads DRAINING).
        {
            Bridge b(PZ_MIB_STATE_DRAINING);
            b.neverDrains = true;
            pz::PzFrameRing ring(b, kLinuxEnd);
            const auto q = ring.quiesce(nullptr, kShort);
            MIB_EXPECT(!q.ok && q.restoreNeeded && b.log.empty() && q.why.find("IDLE") != std::string::npos,
                       "a bridge stuck in DRAINING: restore needed, no RESET_GENERATION is written while DRAINING");
        }
        // 4. Each sticky bit alone, and bit 11: RESET_GENERATION in IDLE, verified by the generation; one that does not take fails.
        for (const uint32_t bit : {pz::kRingStateStalled, pz::kRingStateStopStuck, pz::kRingStateResetRefused}) {
            Bridge b;
            b.regs[PZ_MIB_REG_STORE_STATE] = bit;
            pz::PzFrameRing ring(b, kLinuxEnd);
            const auto q = ring.quiesce(nullptr, kShort);
            MIB_EXPECT(q.ok && b.log == std::vector<std::string>{"RESET_GENERATION"} && b.regs[PZ_MIB_REG_GENERATION] == 8, "a sticky fault bit: RESET_GENERATION in IDLE, generation moved");
        }
        {
            Bridge b;
            b.regs[PZ_MIB_REG_STORE_STATE] = pz::kRingStateStalled;
            b.resetIgnored = true;
            pz::PzFrameRing ring(b, kLinuxEnd);
            const auto q = ring.quiesce(nullptr, kShort);
            MIB_EXPECT(!q.ok && q.restoreNeeded && q.why.find("RESET_GENERATION did not take") != std::string::npos, "a RESET_GENERATION that does not move the generation fails");
        }
        // 5. A FAULT bridge is cleared; a fault that stays set would make the PL ignore ARM.
        {
            Bridge b(PZ_MIB_STATE_FAULT);
            b.regs[PZ_MIB_REG_FAULT] = 0x100;
            pz::PzFrameRing ring(b, kLinuxEnd);
            const auto q = ring.quiesce(nullptr, kShort);
            MIB_EXPECT(q.ok && b.log == std::vector<std::string>{"FAULT_CLEAR"} && b.regs[PZ_MIB_REG_STATE] == PZ_MIB_STATE_IDLE, "FAULT: FAULT_CLEAR returns the bridge to IDLE");
            MIB_EXPECT(q.faultCleared == 0x100 && q.faultState == PZ_MIB_STATE_FAULT, "the cleared FAULT value and the STATE are reported, not hidden");
            Bridge zeroFault(PZ_MIB_STATE_FAULT);
            pz::PzFrameRing ring4(zeroFault, kLinuxEnd);
            const auto q4 = ring4.quiesce(nullptr, kShort);
            MIB_EXPECT(q4.ok && q4.faultCleared == 0 && q4.faultState == PZ_MIB_STATE_FAULT, "STATE FAULT with the FAULT register at 0 is still reported (never a silent 0)");
            Bridge idleFault;
            idleFault.regs[PZ_MIB_REG_FAULT] = 0x20;
            pz::PzFrameRing ring3(idleFault, kLinuxEnd);
            const auto q3 = ring3.quiesce(nullptr, kShort);
            MIB_EXPECT(q3.ok && q3.faultCleared == 0x20, "a fault bit left in an IDLE bridge is cleared and reported too");
            Bridge stuck;
            stuck.regs[PZ_MIB_REG_FAULT] = 0x100;
            stuck.faultSources = 0x100;
            pz::PzFrameRing ring2(stuck, kLinuxEnd);
            const auto q2 = ring2.quiesce(nullptr, kShort);
            MIB_EXPECT(!q2.ok && q2.why.find("fault") != std::string::npos, "a fault register that stays set refuses (ARM would be ignored)");
        }
        // 6. The whole order: quiesce, program, ARM, verified; an ARM the PL ignored is reported, not reported as armed.
        {
            Bridge b(PZ_MIB_STATE_RUNNING);
            b.regs[PZ_MIB_REG_STORE_STATE] = pz::kRingStateStopStuck;
            pz::PzFrameRing ring(b, kLinuxEnd);
            const auto plan = pz::planRing(5000, kLinuxEnd);
            MIB_REQUIRE(ring.quiesce(nullptr, std::chrono::milliseconds(200)).ok, "quiesce");
            std::string why;
            MIB_REQUIRE(ring.program(plan, &why), "program: " + why);
            b.setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_ARM);
            MIB_EXPECT(ring.awaitArmed(kShort).ok && b.regs[PZ_MIB_REG_STATE] == PZ_MIB_STATE_ARMED, "after ARM the bridge reads ARMED");
            MIB_EXPECT(b.regs[PZ_MIB_REG_STORE_STATE] == 0, "the ARM cleared the sticky bits");
            const auto at = [&](const std::string& what) { return std::find(b.log.begin(), b.log.end(), what) - b.log.begin(); };
            MIB_EXPECT(at("STOP") < at("RESET_GENERATION") && at("RESET_GENERATION") < at("write " + std::to_string(PZ_MIB_REG_STORE_BASE_LO)) &&
                           at("write " + std::to_string(PZ_MIB_REG_STORE_MODE)) < at("ARM"),
                       "the order is STOP, RESET_GENERATION, base/records/mode, ARM");
            Bridge ignored;                         // ARM is ignored while a fault is set
            ignored.regs[PZ_MIB_REG_FAULT] = 0x4;
            pz::PzFrameRing ring2(ignored, kLinuxEnd);
            ignored.setReg(PZ_MIB_REG_CONTROL, PZ_MIB_CONTROL_ARM);
            const auto a = ring2.awaitArmed(kShort);
            MIB_EXPECT(!a.ok && a.why.find("ignored") != std::string::npos, "an ignored ARM is reported, never 'armed'");
        }
    }

    return mib::test::exitCode();
}
