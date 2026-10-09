// PzFrameRing (#649 v1): the results13 ring reader against a fake ring in memory. The reader rule is the board
// owner's final wording (pz7035 docs/FRAME_RING.md): lo = max(0, HEAD + 1 - N), readable lo <= seq < FINAL,
// re-read HEAD' after the copy and drop the copy when the ring moved past it; frozen = STATE IDLE and
// FINAL = HEAD + 1.
#include "backend/pz/PzFrameRing.h"

#include "pz_mib_abi.h"
#include "support/assert.h"

#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace pz = backend::pz;

namespace {

constexpr uint32_t kN = 4;                 // ring records
constexpr uint32_t kRec = pz::kRingRecordBytes;
constexpr uint64_t kBase = 0x3EF00000ull; // inside the window, 4 KiB aligned
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
    void configure(uint32_t head, uint32_t final_, uint32_t state) {
        regs[PZ_MIB_REG_STORE_BASE_LO] = static_cast<uint32_t>(kBase);
        regs[PZ_MIB_REG_STORE_BASE_HI] = 0;
        regs[PZ_MIB_REG_STORE_RECORDS] = kN;
        regs[PZ_MIB_REG_STORE_RECORD_BYTES] = kRec;
        regs[PZ_MIB_REG_STORE_HEAD_SEQ] = head;
        regs[pz::kRingRegFinalSeq] = final_;
        regs[PZ_MIB_REG_STORE_STATE] = state;
    }
};

// A store record for sequence `seq` in its slot: FRAME, two RESULTs, IMAGE MONO8, IMAGE MASK1, then the blocks.
void putRecord(FakeIo& io, uint64_t seq, uint8_t grayBase, bool maskNoResult = false, uint64_t frameIdOverride = 0) {
    const uint64_t frameId = frameIdOverride ? frameIdOverride : 1000 + seq;
    std::vector<uint8_t> set;
    auto add = [&](const std::vector<uint8_t>& r) { set.insert(set.end(), r.begin(), r.end()); };
    pz::FrameRecord f;
    f.runId = 7;
    f.frameId = frameId;
    f.timestamp = 5000 * frameId;
    f.epoch = 3;
    f.flags = 0;
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
        r.frameId = frameId;
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
    add(pz::encodeImageRecord(mono, 3));
    pz::ImageRecord mask = mono;
    mask.byteOffset = pz::kRingSetBytes + kW * kH;
    mask.byteLength = kW * kH / 8;
    mask.pixelFormat = PZ_MIB_PIXEL_FORMAT_MASK1;
    mask.stride = kW / 8;
    mask.flags = maskNoResult ? PZ_MIB_IMAGE_FLAGS_NO_RESULT : 0;
    add(pz::encodeImageRecord(mask, 4));
    MIB_REQUIRE(set.size() <= pz::kRingSetBytes, "record set fits");
    uint8_t* slot = io.mem.data() + (seq % kN) * static_cast<size_t>(kRec);
    std::memset(slot, 0, kRec);
    std::memcpy(slot, set.data(), set.size());
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) slot[pz::kRingSetBytes + i] = static_cast<uint8_t>(grayBase + i % 251);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH / 8; ++i) slot[pz::kRingSetBytes + kW * kH + i] = static_cast<uint8_t>(i * 7 + seq);
}

} // namespace

int main() {
    // ---- placement ---------------------------------------------------------------------------------------
    {
        const auto ok = pz::planRing(5000, 0x2D000000ull); // Linux below 720 MiB
        MIB_EXPECT(ok.ok && ok.records == 5000 && ok.bytes == 5000ull * kRec, "5000 frames fit above 720 MiB");
        MIB_EXPECT(ok.base % 4096 == 0 && ok.base >= 0x2D000000ull && 0x3F000000ull - (ok.base + ok.bytes) < 4096,
                   "base is aligned, above Linux, and the ring ends within one page of the PL carve-out");
        MIB_EXPECT(ok.base + ok.bytes <= pz::kRingCeiling, "the ring stays below the PL result ring");
        const auto full = pz::planRing(5000, 0x3F000000ull); // today's mem=1008M
        MIB_EXPECT(!full.ok && full.why.find("mem=") != std::string::npos && full.maxRecords == 0, "mem=1008M: refuse with the remedy");
        const auto big = pz::planRing(20000, 0x2D000000ull);
        MIB_EXPECT(!big.ok && big.maxRecords > 0 && big.maxRecords < 20000 && big.why.find("room for") != std::string::npos, "too many frames: say how many fit");
        MIB_EXPECT(!pz::planRing(0, 0x2D000000ull).ok, "0 frames refused");
        const auto low = pz::planRing(10, 0);  // Linux RAM unknown (0): the floor still holds
        MIB_EXPECT(low.ok && low.base >= pz::kRingFloor, "never below the DDR floor");
        const std::string iomem =
            "00000000-0fffffff : System RAM\n  00008000-007fffff : Kernel code\n10000000-2cffffff : System RAM\n"
            "40000000-4fffffff : something else\n";
        MIB_EXPECT(pz::systemRamEnd(iomem) == 0x2D000000ull, "System RAM end from /proc/iomem, nested lines ignored");
        MIB_EXPECT(!pz::systemRamEnd("40000000-4fffffff : uart\n").has_value(), "no System RAM line: unknown");
    }

    // ---- programming -------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        std::string err;
        const auto plan = pz::planRing(5000, 0x2D000000ull);
        MIB_EXPECT(ring.program(plan, &err), "program: " + err);
        MIB_EXPECT(io.regs[PZ_MIB_REG_STORE_RECORDS] == 5000 && io.regs[PZ_MIB_REG_STORE_BASE_LO] == static_cast<uint32_t>(plan.base) &&
                       (io.regs[PZ_MIB_REG_STORE_MODE] & pz::kStoreModeRingOnly) && (io.regs[PZ_MIB_REG_STORE_MODE] & pz::kStoreModeContinuous),
                   "base, records and CONTINUOUS|RING_ONLY are written");
        MIB_EXPECT(!ring.program(pz::planRing(5000, 0x3F000000ull), &err) && err.find("mem=") != std::string::npos, "an invalid plan is never written");
    }

    // ---- status ------------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        io.configure(0xFFFFFFFFu, 0, PZ_MIB_STORE_STATE_IDLE);
        auto s = ring.status();
        MIB_EXPECT(s.valid && s.head == -1 && s.count() == 0 && s.frozen, "no frame yet: empty and frozen");
        io.configure(9, 9, PZ_MIB_STORE_STATE_CAPTURING); // 10 started, 9 complete, ring of 4
        s = ring.status();
        MIB_EXPECT(s.lo == 6 && s.final == 9 && s.count() == 3 && !s.frozen, "lo = HEAD + 1 - N; readable 6..8; running is not frozen");
        io.configure(9, 10, PZ_MIB_STORE_STATE_DRAINING);
        MIB_EXPECT(!ring.status().frozen, "DRAINING is not frozen even with FINAL = HEAD + 1");
        io.configure(9, 9, PZ_MIB_STORE_STATE_IDLE);
        MIB_EXPECT(!ring.status().frozen, "IDLE with FINAL below HEAD + 1 is not frozen");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE);
        s = ring.status();
        MIB_EXPECT(s.frozen && s.lo == 6 && s.count() == 4, "IDLE and FINAL = HEAD + 1: frozen, the last 4 frames");
        io.configure(9, 10, PZ_MIB_STORE_STATE_FAULT);
        s = ring.status();
        MIB_EXPECT(s.fault && !s.frozen, "a ring fault is never frozen: re-arm needed");
        // The sticky bits hold until the next ARM and invalidate the ring whatever the state code says.
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateStalled);
        s = ring.status();
        MIB_EXPECT(s.valid && s.stalled && s.fault && !s.frozen && s.state == PZ_MIB_STORE_STATE_IDLE &&
                       s.invalidReason.find("RING_STALLED") != std::string::npos,
                   "RING_STALLED: IDLE and FINAL = HEAD + 1 but not frozen, with the reason");
        io.configure(9, 10, PZ_MIB_STORE_STATE_DRAINING | pz::kRingStateStopStuck);
        s = ring.status();
        MIB_EXPECT(s.valid && s.stopStuck && s.stopIncomplete && !s.fault && !s.frozen && s.lo == 6 && s.final == 10,
                   "STOP_STUCK: the device stays DRAINING, the ring is not frozen but not invalid: stop incomplete, the records below FINAL stay readable");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateResetRefused);
        s = ring.status();
        MIB_EXPECT(s.resetRefused && s.fault && !s.frozen && s.invalidReason.find("RESET_GENERATION") != std::string::npos,
                   "bit 11: RESET_GENERATION was refused: invalid");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateStalled | pz::kRingStateStopStuck);
        MIB_EXPECT(ring.status().fault && !ring.status().frozen, "stalled and stuck: invalid (the stall wins)");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE);
        MIB_EXPECT(ring.status().frozen && !ring.status().fault, "frozen means IDLE with neither bit set");
        io.regs[PZ_MIB_REG_STORE_RECORDS] = 0;
        MIB_EXPECT(!ring.status().valid, "no ring configured");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE);
        io.regs[PZ_MIB_REG_STORE_BASE_LO] = 0x1000; // below the DDR floor
        MIB_EXPECT(!ring.status().valid, "a base below the floor is not trusted");
    }

    // ---- an invalid ring offers no playback ----------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        for (uint64_t q = 6; q <= 9; ++q) putRecord(io, q, static_cast<uint8_t>(q));
        pz::RingFrame f;
        std::string why;
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE);
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Ok, "the same ring reads when frozen");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateStalled);
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Unavailable && why.find("RING_STALLED") != std::string::npos,
                   "RING_STALLED: no frame is offered, the reason names it");
        io.configure(9, 10, PZ_MIB_STORE_STATE_DRAINING | pz::kRingStateStopStuck);
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Ok && ring.readFrame(9, f, &why) == pz::RingRead::Ok,
                   "STOP_STUCK: the frozen part is still read, under the reader rule");
        MIB_EXPECT(ring.readFrame(10, f, &why) == pz::RingRead::OutOfRange, "STOP_STUCK: the open frame (at FINAL and beyond) is never offered");
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateResetRefused);
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Unavailable && why.find("RESET_GENERATION") != std::string::npos,
                   "bit 11: no frame is offered");
        io.configure(9, 10, PZ_MIB_STORE_STATE_FAULT);
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Unavailable, "FAULT: no frame is offered");
        // awaitFrozen returns at once on a ring that will never freeze (invalid, or stuck) rather than waiting out the timeout.
        for (const uint32_t state : {PZ_MIB_STORE_STATE_DRAINING | pz::kRingStateStopStuck, PZ_MIB_STORE_STATE_IDLE | pz::kRingStateStalled}) {
            io.configure(9, 10, state);
            const auto t0 = std::chrono::steady_clock::now();
            const auto st = ring.awaitFrozen(std::chrono::milliseconds(500));
            MIB_EXPECT(!st.frozen && (st.fault || st.stopIncomplete) && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200),
                       "a ring that will not freeze does not wait out the timeout");
        }
    }

    // ---- awaitFrozen -------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        io.configure(9, 9, PZ_MIB_STORE_STATE_DRAINING);
        int polls = 0;
        io.onRead = nullptr;
        // The state turns to IDLE after a few register polls (the open frame completes).
        struct Ticker final : pz::IRingIo {
            FakeIo& f; int n = 0;
            explicit Ticker(FakeIo& x) : f(x) {}
            uint32_t reg(uint32_t o) override {
                if (o == PZ_MIB_REG_STORE_STATE && ++n > 12) { f.regs[PZ_MIB_REG_STORE_STATE] = PZ_MIB_STORE_STATE_IDLE; f.regs[pz::kRingRegFinalSeq] = 10; }
                return f.reg(o);
            }
            void setReg(uint32_t o, uint32_t v) override { f.setReg(o, v); }
            bool read(uint64_t p, void* d, size_t b) override { return f.read(p, d, b); }
        } ticker(io);
        pz::PzFrameRing r2(ticker);
        const auto frozen = r2.awaitFrozen(std::chrono::milliseconds(500));
        MIB_EXPECT(frozen.frozen && frozen.final == 10, "frozen once the open frame completed");
        (void)polls;
        io.configure(9, 9, PZ_MIB_STORE_STATE_DRAINING);
        const auto stuck = ring.awaitFrozen(std::chrono::milliseconds(20));
        MIB_EXPECT(!stuck.frozen, "a ring that never freezes times out and is reported as not frozen (never papered over)");
    }

    // ---- reading -----------------------------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        for (uint64_t s = 6; s <= 9; ++s) putRecord(io, s, static_cast<uint8_t>(s * 3));
        io.configure(9, 10, PZ_MIB_STORE_STATE_IDLE);
        pz::RingFrame f;
        std::string why;
        MIB_EXPECT(ring.readFrame(7, f, &why) == pz::RingRead::Ok, "read 7: " + why);
        MIB_EXPECT(f.seq == 7 && f.frameId == 1007 && f.timestampTicks == 5000 * 1007ull && f.width == kW && f.height == kH, "frame fields");
        MIB_EXPECT(f.gray.size() == static_cast<size_t>(kW) * kH && f.gray[0] == 21 && f.gray[5] == static_cast<uint8_t>(21 + 5), "gray block is the one of sequence 7");
        MIB_EXPECT(f.maskPresent && f.mask.size() == static_cast<size_t>(kW) * kH / 8 && f.mask[1] == static_cast<uint8_t>(7 + 7), "mask block is packed, LSB first, of sequence 7");
        MIB_REQUIRE(f.cells.size() == 2, "two cells");
        MIB_EXPECT(f.cells[0].valid && !f.cells[1].valid && f.cells[0].x == 100 && f.cells[1].x == 110 && f.cells[0].width == 30 && f.cells[0].payload[3] == 1003 && f.cells[1].payload[14] == 2014,
                   "cells carry bbox, valid flag and the 15 payload words");
        MIB_EXPECT(ring.readFrame(6, f, &why) == pz::RingRead::Ok, "oldest readable record");
        MIB_EXPECT(ring.readFrame(9, f, &why) == pz::RingRead::Ok, "newest readable record");
        MIB_EXPECT(ring.readFrame(5, f, &why) == pz::RingRead::OutOfRange, "5 is gone (HEAD - N)");
        MIB_EXPECT(ring.readFrame(10, f, &why) == pz::RingRead::OutOfRange, "10 does not exist yet");

        // The packet the browser decodes.
        ring.readFrame(7, f, &why);
        const auto packet = pz::buildRingPacket(f, 100000000u);
        MIB_EXPECT(packet.size() == 48 + f.gray.size() + f.mask.size() + 2 * 18 * 4, "packet size");
        MIB_EXPECT(std::memcmp(packet.data(), "MIBR", 4) == 0 && packet[4] == 1 && packet[6] == 48 && packet[8] == 7 && packet[16] == (1007 & 0xFF) && packet[44] == 2 && (packet[46] & 1),
                   "packet header: magic, version, seq, frame id, cells, mask present");
        const size_t cellAt = 48 + f.gray.size() + f.mask.size();
        uint32_t w15 = 0, w17 = 0;
        std::memcpy(&w15, packet.data() + cellAt + 4 * 15, 4);
        std::memcpy(&w17, packet.data() + cellAt + 18 * 4 + 4 * 17, 4);
        MIB_EXPECT(w15 == (100u | 20u << 16) && w17 == 0, "cell words 15 (x|y) and 17 (valid) follow the run-preview layout");
    }

    // ---- the reader rule: a record overwritten while it was copied ---------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        for (uint64_t s = 6; s <= 9; ++s) putRecord(io, s, static_cast<uint8_t>(s));
        io.configure(10, 10, PZ_MIB_STORE_STATE_CAPTURING); // running: 11 started, 10 complete, readable 7..9
        pz::RingFrame f;
        std::string why;
        // HEAD moves on while sequence 7 is being copied: slot of 7 is reused by 11 (7 mod 4 = 11 mod 4).
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == kBase + (7 % kN) * static_cast<uint64_t>(kRec) + pz::kRingSetBytes) {
                io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 11; // its pixel writes started: lo = 8, so 7 is gone
                io.onRead = nullptr;
            }
        };
        MIB_EXPECT(ring.readFrame(7, f, &why) == pz::RingRead::Overwritten, "HEAD' past the record: the copy is dropped (pixels are overwritten before the header)");
        MIB_EXPECT(why.find("moved past") != std::string::npos, "the reason says why");
        // Record 9 is not in danger when HEAD advances by one.
        io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 10;
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n == 49152 && phys == kBase + (9 % kN) * static_cast<uint64_t>(kRec) + pz::kRingSetBytes) {
                io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 11;
                io.onRead = nullptr;
            }
        };
        MIB_EXPECT(ring.readFrame(9, f, &why) == pz::RingRead::Ok, "a newer record survives HEAD advancing by one");
        // The header changes between the two reads of it: dropped.
        io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = 10;
        bool second = false;
        io.onRead = [&](uint64_t phys, size_t n) {
            if (n < pz::kRingSetBytes && phys == kBase + (8 % kN) * static_cast<uint64_t>(kRec)) {
                if (!second) { second = true; return; }
            }
            if (n == 49152 && phys == kBase + (8 % kN) * static_cast<uint64_t>(kRec) + pz::kRingSetBytes)
                io.mem[(8 % kN) * static_cast<size_t>(kRec) + 40] ^= 0xFF; // a header byte changes under the reader
        };
        MIB_EXPECT(ring.readFrame(8, f, &why) == pz::RingRead::Overwritten, "a header that differs between the two reads is dropped");
    }

    // ---- malformed and degraded records ------------------------------------------------------------------
    {
        FakeIo io;
        pz::PzFrameRing ring(io);
        for (uint64_t s = 0; s < 3; ++s) putRecord(io, s, 1, s == 1);
        io.configure(2, 3, PZ_MIB_STORE_STATE_IDLE);
        pz::RingFrame f;
        std::string why;
        MIB_EXPECT(ring.readFrame(1, f, &why) == pz::RingRead::Ok && !f.maskPresent && f.mask.size() == static_cast<size_t>(kW) * kH / 8 &&
                       std::all_of(f.mask.begin(), f.mask.end(), [](uint8_t b) { return b == 0; }),
                   "a NO_RESULT mask: frame readable, mask absent (all zero)");
        std::memset(io.mem.data() + 2 * static_cast<size_t>(kRec), 0, 64); // a zeroed header
        MIB_EXPECT(ring.readFrame(2, f, &why) == pz::RingRead::Malformed, "a record without a FRAME is malformed");
        putRecord(io, 0, 1, false, 5); // RESULTs of frame 5 inside a record whose FRAME says 5: consistent
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Ok && f.frameId == 5, "frame id comes from the record");
        // A RESULT of another frame inside the set (a stale record from an older lap).
        {
            putRecord(io, 0, 1);
            uint8_t* slot = io.mem.data();
            // The first RESULT starts after the FRAME record (16 + FRAME payload rounded): flip its frame id.
            const auto frameLen = pz::encodeFrameRecord(pz::FrameRecord{}, 0).size();
            slot[frameLen + 16] ^= 0x01;
            // The CRC no longer matches: the record is rejected as malformed either way.
            MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Malformed, "a corrupted RESULT fails its CRC: malformed");
        }
        io.regs[PZ_MIB_REG_STORE_RECORDS] = 0;
        MIB_EXPECT(ring.readFrame(0, f, &why) == pz::RingRead::Unavailable, "no ring: unavailable");
    }

    return mib::test::exitCode();
}
