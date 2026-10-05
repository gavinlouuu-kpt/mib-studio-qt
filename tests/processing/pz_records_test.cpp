// PZ7035 result records (#447 E3): the decoder against the ABI bundle's
// fixtures, encode/decode round trips, the result ring (wrap, overrun), frame
// assembly, and the unet_cells_v2 profile decoder on the PL conformance
// vectors. (processing.pz_unet_cells_host compares the decoded cells with the
// host Contract 3 science.)
//
// argv[1]: third_party/pz7035-abi (fixtures/fixtures.json + *.bin)
// argv[2]: scripts/conformance/unet-cells-v2-pl-vectors.json
#include "backend/pz/PzRecords.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace pz = backend::pz;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

nlohmann::json readJson(const std::string& path) {
    std::ifstream in(path);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + path);
    return nlohmann::json::parse(in);
}

const char* typeName(pz::RecordType t) {
    switch (t) {
    case pz::RecordType::Frame: return "FRAME";
    case pz::RecordType::Result: return "RESULT";
    case pz::RecordType::Event: return "EVENT";
    case pz::RecordType::Counters: return "COUNTERS";
    case pz::RecordType::Preview: return "PREVIEW";
    case pz::RecordType::Descriptor: return "DESCRIPTOR";
    case pz::RecordType::Image: return "IMAGE";
    }
    return "?";
}

const char* decisionName(uint8_t d) {
    static const char* names[] = {"NONE", "ISSUED", "LATE", "MISSED", "INHIBITED"};
    return d < 5 ? names[d] : "?";
}

const char* stateName(uint8_t s) {
    static const char* names[] = {"FREE", "DEVICE_OWNED", "READY", "CPU_OWNED"};
    return s < 4 ? names[s] : "?";
}

// Every key the fixtures' "expect" objects use.
void checkExpect(const std::string& name, const pz::Record& r, const nlohmann::json& expect) {
    for (const auto& [key, want] : expect.items()) {
        const std::string where = name + "." + key;
        if (key == "context") continue;
        if (key == "type") MIB_EXPECT(want == typeName(r.type), where);
        else if (key == "sequence") MIB_EXPECT(want.get<uint32_t>() == r.sequence, where);
        else if (key == "length") MIB_EXPECT(want.get<uint16_t>() == r.length, where);
        else if (const auto* f = std::get_if<pz::FrameRecord>(&r.body)) {
            if (key == "frame_id") MIB_EXPECT(want.get<uint64_t>() == f->frameId, where);
            else if (key == "epoch") MIB_EXPECT(want.get<uint32_t>() == f->epoch, where);
            else if (key == "result_count") MIB_EXPECT(want.get<uint16_t>() == f->resultCount, where);
            else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* res = std::get_if<pz::ResultRecord>(&r.body)) {
            if (key == "result_index") MIB_EXPECT(want.get<uint16_t>() == res->resultIndex, where);
            else if (key == "payload") MIB_EXPECT(want.get<std::vector<uint32_t>>() == res->payload, where);
            else if (key == "target") MIB_EXPECT(want.get<bool>() == res->target(), where);
            else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* e = std::get_if<pz::EventRecord>(&r.body)) {
            if (key == "decision") MIB_EXPECT(want == decisionName(e->decision), where);
            else if (key == "latency_ticks") MIB_EXPECT(want.get<uint32_t>() == e->latencyTicks, where);
            else if (key == "output_timestamp") {
                MIB_EXPECT(want.is_null() ? !e->outputTimestamp
                                          : e->outputTimestamp && *e->outputTimestamp == want.get<uint64_t>(),
                           where);
            } else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* c = std::get_if<pz::CountersRecord>(&r.body)) {
            if (key == "snapshot_seq") MIB_EXPECT(want.get<uint32_t>() == c->snapshotSeq, where);
            else if (key == "FRAMES_OBSERVED") MIB_EXPECT(want.get<uint32_t>() == c->counters[0], where);
            else if (key == "COMMITS_ACKED") MIB_EXPECT(want.get<uint32_t>() == c->counters[15], where);
            else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* p = std::get_if<pz::PreviewRecord>(&r.body)) {
            if (key == "buffer_index") MIB_EXPECT(want.get<uint16_t>() == p->bufferIndex, where);
            else if (key == "byte_length") MIB_EXPECT(want.get<uint32_t>() == p->byteLength, where);
            else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* d = std::get_if<pz::DescriptorRecord>(&r.body)) {
            if (key == "buffer_index") MIB_EXPECT(want.get<uint16_t>() == d->bufferIndex, where);
            else if (key == "state") MIB_EXPECT(want == stateName(d->state), where);
            else if (key == "generation") MIB_EXPECT(want.get<uint16_t>() == d->generation, where);
            else MIB_EXPECT(false, "unchecked key " + where);
        } else if (const auto* i = std::get_if<pz::ImageRecord>(&r.body)) {
            if (key == "byte_offset") MIB_EXPECT(want.get<uint32_t>() == i->byteOffset, where);
            else if (key == "byte_length") MIB_EXPECT(want.get<uint32_t>() == i->byteLength, where);
            else MIB_EXPECT(false, "unchecked key " + where);
        }
    }
}

void testBundleFixtures(const std::string& abiDir) {
    const auto doc = readJson(abiDir + "/fixtures/fixtures.json");
    MIB_EXPECT(doc.at("abi_major").get<int>() == 1, "ABI major 1");
    int n = 0;
    for (const auto& f : doc.at("fixtures")) {
        const std::string name = f.at("name").get<std::string>();
        const auto bytes = readFile(abiDir + "/fixtures/" + f.at("file").get<std::string>());
        const auto& expect = f.contains("expect") ? f.at("expect") : nlohmann::json::object();
        pz::Decoded d;
        if (expect.contains("context")) {
            const auto& ctx = expect.at("context");
            pz::StreamDecoder stream(ctx.at("epoch").get<uint32_t>(), ctx.at("generation").get<uint32_t>());
            stream.setLastSequence(ctx.at("last_sequence").get<uint32_t>());
            d = stream.feed(bytes.data(), bytes.size());
        } else {
            d = pz::decodeRecord(bytes.data(), bytes.size());
        }
        const std::string want = f.at("error").get<std::string>();
        MIB_EXPECT(want == pz::decodeErrorName(d.error),
                   name + ": " + pz::decodeErrorName(d.error) + " (" + d.detail + "), want " + want);
        if (d.ok()) {
            MIB_REQUIRE(d.record.has_value(), name);
            checkExpect(name, *d.record, expect);
            // Re-encoding reproduces the PL bytes for FRAME / RESULT.
            if (const auto* fr = std::get_if<pz::FrameRecord>(&d.record->body); fr && d.record->length == 64) {
                MIB_EXPECT(pz::encodeFrameRecord(*fr, d.record->sequence) == bytes, name + " re-encodes");
            }
            if (const auto* rr = std::get_if<pz::ResultRecord>(&d.record->body)) {
                MIB_EXPECT(pz::encodeResultRecord(*rr, d.record->sequence) == bytes, name + " re-encodes");
            }
        }
        ++n;
    }
    std::printf("bundle fixtures: %d decoded with the expected outcome\n", n);
    MIB_EXPECT(n == 26, "all 26 fixtures");
}

class VectorRing final : public pz::RingMemory {
public:
    explicit VectorRing(size_t size) : mem(size, 0xEE) {}
    size_t sizeBytes() const override { return mem.size(); }
    uint32_t head() override { return headCounter; }
    uint32_t tail() override { return tailCounter; }
    void setTail(uint32_t t) override { tailCounter = t; }
    void read(size_t offset, uint8_t* dst, size_t n) override {
        MIB_REQUIRE(offset + n <= mem.size(), "read inside the ring");
        std::copy(mem.begin() + offset, mem.begin() + offset + n, dst);
    }
    // Device side: append bytes at head with wrap.
    void write(const std::vector<uint8_t>& bytes) {
        for (uint8_t b : bytes) mem[headCounter++ % mem.size()] = b;
    }
    std::vector<uint8_t> mem;
    uint32_t headCounter{0};
    uint32_t tailCounter{0};
};

pz::ResultRecord cellResult(uint64_t frameId, uint16_t index, uint16_t x) {
    pz::ResultRecord r;
    r.frameId = frameId;
    r.resultIndex = index;
    r.flags = pz::kResultValid;
    r.scienceProfile = pz::kScienceProfileUnetCells;
    r.profileVersion = pz::kUnetCellsProfileVersion;
    r.bboxX = x;
    r.bboxW = 30;
    r.bboxH = 20;
    r.payloadValidity = 0x7FFF;
    r.payload.assign(15, 0);
    r.payload[0] = index + 1u;
    return r;
}

void testRingAndAssembly() {
    // Free-running counters start near 2^32 so they wrap during the test.
    VectorRing ring(1024);
    ring.headCounter = ring.tailCounter = 0xFFFFFF00u;
    pz::ResultRingReader reader(ring);
    pz::StreamDecoder stream(0, 0);
    pz::FrameAssembler assembler;
    uint32_t seq = 1;
    uint64_t frames = 0;
    size_t results = 0;
    std::vector<uint8_t> drained;
    for (uint64_t frameId = 100; frameId < 160; ++frameId) {
        const uint16_t count = static_cast<uint16_t>(frameId % 3);
        pz::FrameRecord f;
        f.frameId = frameId;
        f.resultCount = count;
        f.scienceProfile = pz::kScienceProfileUnetCells;
        f.profileVersion = pz::kUnetCellsProfileVersion;
        f.flags = count ? 0 : pz::kFrameEmpty;
        ring.write(pz::encodeFrameRecord(f, seq++));
        for (uint16_t i = 0; i < count; ++i) ring.write(pz::encodeResultRecord(cellResult(frameId, i, 7 * i), seq++));
        if (frameId % 4 == 3) { // the consumer drains every few frames
            drained.clear();
            const auto d = reader.drain(drained);
            MIB_EXPECT(!d.overrun, "no overrun while keeping up");
            for (const auto& [off, len] : pz::splitRecords(drained.data(), drained.size())) {
                const auto r = stream.feed(drained.data() + off, len);
                MIB_REQUIRE(r.ok(), std::string("ring record decodes: ") + pz::decodeErrorName(r.error));
                for (const auto& done : assembler.add(*r.record)) {
                    MIB_EXPECT(!done.incomplete && done.results.size() == done.frame.resultCount,
                               "complete frame");
                    for (const auto& res : done.results) {
                        MIB_EXPECT(res.frameId == done.frame.frameId, "result belongs to its frame");
                        MIB_EXPECT(pz::decodeUnetCellsV2(res).has_value(), "profile payload decodes");
                    }
                    ++frames;
                    results += done.results.size();
                }
            }
        }
    }
    MIB_EXPECT(frames == 60 && results == 60, "all 60 frames and 60 results through the ring: " +
                                                 std::to_string(frames) + "/" + std::to_string(results));
    MIB_EXPECT(stream.gaps() == 0 && assembler.orphanResults() == 0, "no gaps, no orphans");
    MIB_EXPECT(ring.tailCounter == ring.headCounter, "tail released to head");

    // Consumer too slow: the device wrote more than the ring holds.
    for (int i = 0; i < 20; ++i) {
        pz::FrameRecord f;
        f.frameId = 1000 + i;
        ring.write(pz::encodeFrameRecord(f, seq++));
    }
    drained.clear();
    const auto over = reader.drain(drained);
    MIB_EXPECT(over.overrun && over.bytes == 0 && ring.tailCounter == ring.headCounter,
               "an overrun is reported and the reader resynchronises at head");

    // A FRAME that is not followed by all its RESULTs is reported incomplete.
    pz::FrameAssembler partial;
    pz::Record fr;
    pz::FrameRecord f2;
    f2.frameId = 5;
    f2.resultCount = 2;
    fr.body = f2;
    partial.add(fr);
    pz::Record rr;
    rr.type = pz::RecordType::Result;
    rr.body = cellResult(5, 0, 0);
    partial.add(rr);
    pz::Record next;
    pz::FrameRecord f3;
    f3.frameId = 6;
    next.body = f3;
    const auto out = partial.add(next);
    MIB_EXPECT(out.size() == 2 && out[0].incomplete && out[0].results.size() == 1 && !out[1].incomplete,
               "a missing RESULT marks the frame incomplete");
}

// unet_cells_v2: the PL vectors' RESULTs, encoded and decoded through the
// record path, decode to the cells the vectors list (ids, reasons, flags,
// bbox, counts).
void testProfileDecodesVectors(const std::string& vectorsPath) {
    static const std::map<std::string, pz::UnetCellReason> reasons = {
        {"NONE", pz::UnetCellReason::None},           {"NO_CONTOUR", pz::UnetCellReason::NoContour},
        {"BORDER", pz::UnetCellReason::Border},       {"AREA", pz::UnetCellReason::Area},
        {"DEFORM", pz::UnetCellReason::Deform},       {"AREA_RATIO", pz::UnetCellReason::AreaRatio},
        {"LAPLACIAN", pz::UnetCellReason::Laplacian}, {"CHANNEL", pz::UnetCellReason::Channel}};
    const auto doc = readJson(vectorsPath);
    int cells = 0;
    uint32_t seq = 1;
    for (const auto& kase : doc.at("cases")) {
        for (const auto& frame : kase.at("frames")) {
            for (const auto& e : frame.at("results")) {
                pz::ResultRecord r;
                r.frameId = 7;
                r.resultIndex = static_cast<uint16_t>(e.at("object_id").get<int>() - 1);
                r.flags = e.at("target").get<bool>() ? pz::kResultTarget : 0;
                r.scienceProfile = pz::kScienceProfileUnetCells;
                r.profileVersion = pz::kUnetCellsProfileVersion;
                r.bboxX = e.at("bbox").at("x").get<uint16_t>();
                r.bboxY = e.at("bbox").at("y").get<uint16_t>();
                r.bboxW = e.at("bbox").at("w").get<uint16_t>();
                r.bboxH = e.at("bbox").at("h").get<uint16_t>();
                r.payloadValidity = e.at("validity").get<uint32_t>();
                r.payload = e.at("payload").get<std::vector<uint32_t>>();
                const auto bytes = pz::encodeResultRecord(r, seq++);
                const auto d = pz::decodeRecord(bytes.data(), bytes.size());
                MIB_REQUIRE(d.ok(), "vector RESULT decodes");
                const auto c = pz::decodeUnetCellsV2(std::get<pz::ResultRecord>(d.record->body));
                MIB_REQUIRE(c.has_value(), "unet_cells_v2 payload decodes");
                const std::string where = kase.at("name").get<std::string>() + "/" +
                                          frame.at("name").get<std::string>();
                MIB_EXPECT(c->objectId == e.at("object_id").get<int>(), where + " object id");
                MIB_EXPECT(c->reason == reasons.at(e.at("reason").get<std::string>()), where + " reason");
                MIB_EXPECT(c->cutOff == e.at("cut_off").get<bool>() && c->target == e.at("target").get<bool>(),
                           where + " flags");
                MIB_EXPECT(c->bboxX == r.bboxX && c->bboxWidth == r.bboxW, where + " bbox");
                MIB_EXPECT(c->cellCount == frame.at("cells").get<int>() &&
                               c->blemishCount == frame.at("blemishes").get<int>(),
                           where + " frame counts");
                MIB_EXPECT(std::isfinite(c->brightnessMean) && std::isfinite(c->laplacianVariance) &&
                               std::isfinite(c->centroidX),
                           where + " always-present words");
                MIB_EXPECT(std::isfinite(c->deformability) == !(c->cutOff || c->degenerate()),
                           where + " metrics only for measured cells");
                ++cells;
            }
        }
    }
    std::printf("unet_cells_v2: %d PL results decoded through the record path\n", cells);
    MIB_EXPECT(cells > 0, "decoded cells");

    pz::ResultRecord other = cellResult(1, 0, 0);
    other.scienceProfile = 1;
    MIB_EXPECT(!pz::decodeUnetCellsV2(other), "another profile's payload is unavailable, not zero");
    other = cellResult(1, 0, 0);
    other.payload.resize(3);
    MIB_EXPECT(!pz::decodeUnetCellsV2(other), "a short payload is unavailable");
}

} // namespace

void testEmptyPayloadResult() {
    // A RESULT with no payload words round-trips without copying from an
    // empty vector (UBSan: null pointer passed to memcpy).
    pz::ResultRecord r = cellResult(7, 0, 12);
    r.payload.clear();
    r.payloadValidity = 0;
    const auto bytes = pz::encodeResultRecord(r, 3);
    const auto d = pz::decodeRecord(bytes.data(), bytes.size());
    MIB_REQUIRE(d.ok() && d.record.has_value(), "empty-payload RESULT decodes");
    const auto* rr = std::get_if<pz::ResultRecord>(&d.record->body);
    MIB_REQUIRE(rr != nullptr, "empty-payload record is a RESULT");
    MIB_EXPECT(rr->payload.empty() && rr->frameId == 7, "empty payload preserved");
    MIB_EXPECT(pz::encodeResultRecord(*rr, 3) == bytes, "empty-payload RESULT re-encodes");
}

int main(int argc, char** argv) {
    MIB_REQUIRE(argc > 2, "usage: pz_records_test <third_party/pz7035-abi> <unet-cells-v2-pl-vectors.json>");
    testBundleFixtures(argv[1]);
    testRingAndAssembly();
    testEmptyPayloadResult();
    testProfileDecodesVectors(argv[2]);
    return mib::test::exitCode();
}
