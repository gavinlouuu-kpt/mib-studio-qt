// The MIBR packets Studio's own reader produces from REAL stored records (#693): tools/ring_vs_ssd/testdata holds four consecutive records of the export slot's run 19
// (records.bin, as `GET /ssd/runs/19/records` delivered them) and the packets `PzFrameRing::readFrame` + `buildRingPacket` made from them (seq-<n>.mibr). The Python tool
// compares such packets with the records by an independent mapping of the RESULT words; this test keeps the fixture honest: if the reader or the packet builder changes, the packets here
// no longer equal the golden ones and the fixture is regenerated on purpose (`ring_packet_golden_test <testdata dir> --write`).
#include "backend/pz/PzFrameRing.h"

#include "pz_mib_abi.h"
#include "support/assert.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace pz = backend::pz;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kN = 4;
constexpr uint32_t kRec = pz::kRingRecordBytes;
constexpr uint64_t kBase = 0x3EF00000ull;
constexpr uint64_t kLinuxEnd = 0x2D000000ull;

struct FakeIo final : pz::IRingIo {
    std::map<uint32_t, uint32_t> regs;
    std::vector<uint8_t> mem;
    uint32_t reg(uint32_t o) override { return regs[o]; }
    void setReg(uint32_t o, uint32_t v) override { regs[o] = v; }
    bool read(uint64_t phys, void* dst, size_t n) override {
        if (phys < kBase || phys + n > kBase + mem.size()) return false;
        std::memcpy(dst, mem.data() + (phys - kBase), n);
        return true;
    }
};

std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + p.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    MIB_REQUIRE(argc >= 2, "usage: ring_packet_golden_test <tools/ring_vs_ssd/testdata> [--write]");
    const fs::path dir = argv[1];
    const bool write = argc > 2 && std::string(argv[2]) == "--write";
    const auto records = slurp(dir / "records.bin");
    MIB_REQUIRE(records.size() == static_cast<size_t>(kN) * kRec, "records.bin holds four 59,392 B records");

    FakeIo io;
    io.mem = records; // record i is the ring slot i, as the PL stored it
    io.regs[PZ_MIB_REG_STORE_BASE_LO] = static_cast<uint32_t>(kBase);
    io.regs[PZ_MIB_REG_STORE_BASE_HI] = 0;
    io.regs[PZ_MIB_REG_STORE_RECORDS] = kN;
    io.regs[PZ_MIB_REG_STORE_RECORD_BYTES] = kRec;
    io.regs[PZ_MIB_REG_STORE_SET_BYTES] = pz::kRingSetBytes;
    io.regs[PZ_MIB_REG_EPOCH] = 7;
    io.regs[PZ_MIB_REG_GENERATION] = 1;
    io.regs[PZ_MIB_REG_STORE_HEAD_SEQ] = kN - 1;
    io.regs[pz::kRingRegFinalSeq] = kN;
    io.regs[PZ_MIB_REG_STATE] = PZ_MIB_STATE_IDLE;
    io.regs[PZ_MIB_REG_STORE_STATE] = 0;
    pz::PzFrameRing ring(io, kLinuxEnd);
    const auto st = ring.status();
    MIB_REQUIRE(st.valid && st.frozen && st.count() == kN && st.epoch == 7, "the fake ring is frozen with four frames");
    for (uint32_t seq = 0; seq < kN; ++seq) {
        pz::RingFrame frame;
        std::string why;
        MIB_REQUIRE(ring.readFrame(seq, frame, &why) == pz::RingRead::Ok, "readFrame " + std::to_string(seq) + ": " + why);
        MIB_EXPECT(frame.cells.size() == 2, "the real frames carry two cells");
        const auto packet = pz::buildRingPacket(frame, 100000000u);
        const fs::path golden = dir / ("seq-" + std::to_string(seq) + ".mibr");
        if (write) {
            std::ofstream out(golden, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
            std::printf("wrote %s (%zu bytes)\n", golden.string().c_str(), packet.size());
        } else {
            MIB_EXPECT(packet == slurp(golden), "the packet of sequence " + std::to_string(seq) + " equals the golden one (regenerate with --write if the reader changed on purpose)");
        }
    }
    return mib::test::exitCode();
}
