// YOFO S1 execution providers: a record stream built from the PL conformance
// vectors (FRAME + its RESULTs per frame, as the PZ7035 ring writer emits
// them) replays through ReplayExecutionProvider into ProviderFrames that carry
// the PL's cells, with the stream checks counting corrupted records,
// incomplete frames and gaps, and a paced replay holding its frame rate.
//
// argv[1]: scripts/conformance/unet-cells-v2-pl-vectors.json
#include "backend/processing/pz/PzExecutionProviders.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bpz = backend::pz;
using backend::processing::ProviderFrame;
using backend::processing::pz::ReplayExecutionProvider;

namespace {

struct Expected {
    uint64_t frameId;
    int cells;
    int blemishes;
    std::vector<nlohmann::json> results;
};

struct Stream {
    std::vector<uint8_t> bytes;
    std::vector<Expected> frames;
    std::vector<size_t> resultOffsets; // byte offset of every RESULT record
};

Stream buildStream(const std::string& vectorsPath, int repeat) {
    std::ifstream in(vectorsPath);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + vectorsPath);
    const auto doc = nlohmann::json::parse(in);
    Stream s;
    uint32_t seq = 1;
    uint64_t frameId = 1000;
    for (int r = 0; r < repeat; ++r) {
        for (const auto& kase : doc.at("cases")) {
            for (const auto& frame : kase.at("frames")) {
                const auto& results = frame.at("results");
                bpz::FrameRecord f;
                f.runId = 7;
                f.frameId = frameId;
                f.timestamp = frameId * 11880; // 200 us at 59.4 MHz
                f.resultCount = static_cast<uint16_t>(results.size());
                f.flags = frame.at("empty").get<bool>() ? bpz::kFrameEmpty : 0;
                f.scienceProfile = bpz::kScienceProfileUnetCells;
                f.profileVersion = bpz::kUnetCellsProfileVersion;
                f.width = 512;
                f.height = 96;
                const auto fb = bpz::encodeFrameRecord(f, seq++);
                s.bytes.insert(s.bytes.end(), fb.begin(), fb.end());
                Expected e{frameId, frame.at("cells").get<int>(), frame.at("blemishes").get<int>(), {}};
                uint16_t index = 0;
                for (const auto& res : results) {
                    bpz::ResultRecord rr;
                    rr.frameId = frameId;
                    rr.resultIndex = index++;
                    rr.flags = res.at("target").get<bool>() ? bpz::kResultTarget : 0;
                    rr.scienceProfile = bpz::kScienceProfileUnetCells;
                    rr.profileVersion = bpz::kUnetCellsProfileVersion;
                    rr.bboxX = res.at("bbox").at("x").get<uint16_t>();
                    rr.bboxY = res.at("bbox").at("y").get<uint16_t>();
                    rr.bboxW = res.at("bbox").at("w").get<uint16_t>();
                    rr.bboxH = res.at("bbox").at("h").get<uint16_t>();
                    rr.payloadValidity = res.at("validity").get<uint32_t>();
                    rr.payload = res.at("payload").get<std::vector<uint32_t>>();
                    s.resultOffsets.push_back(s.bytes.size());
                    const auto rb = bpz::encodeResultRecord(rr, seq++);
                    s.bytes.insert(s.bytes.end(), rb.begin(), rb.end());
                    e.results.push_back(res);
                }
                s.frames.push_back(std::move(e));
                ++frameId;
            }
        }
    }
    return s;
}

std::vector<ProviderFrame> replay(std::vector<uint8_t> bytes, double fps, backend::processing::ProviderStatus* status,
                                  double* seconds = nullptr) {
    ReplayExecutionProvider provider(std::move(bytes), fps);
    std::mutex m;
    std::vector<ProviderFrame> got;
    provider.setSink([&](ProviderFrame&& f) {
        std::scoped_lock lk(m);
        got.push_back(std::move(f));
    });
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    MIB_REQUIRE(provider.start(7, &error), "replay starts: " + error);
    MIB_REQUIRE(provider.waitFinished(std::chrono::seconds(20)), "replay finishes");
    if (seconds) *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    provider.stop();
    *status = provider.status();
    std::scoped_lock lk(m);
    return got;
}

void testReplayDeliversThePlCells(const std::string& vectors) {
    const Stream s = buildStream(vectors, 1);
    backend::processing::ProviderStatus st;
    const auto got = replay(s.bytes, 0, &st);
    MIB_REQUIRE(got.size() == s.frames.size(), "one ProviderFrame per FRAME record");
    int cells = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const auto& g = got[i];
        const auto& e = s.frames[i];
        const std::string where = "frame " + std::to_string(e.frameId);
        MIB_EXPECT(g.frameId == e.frameId && g.runId == 7 && !g.incomplete, where + " identity");
        MIB_EXPECT(g.timestampNs == e.frameId * 200000ULL, where + " timestamp ticks -> ns");
        MIB_EXPECT(g.empty() == (e.cells == 0), where + " EMPTY flag");
        MIB_REQUIRE(g.cells.size() == e.results.size() && g.objects.size() == e.results.size(), where + " cells");
        for (size_t k = 0; k < e.results.size(); ++k) {
            const auto& r = e.results[k];
            const bool valid = r.at("reason").get<std::string>() == "NONE";
            MIB_EXPECT(g.cells[k].objectId == r.at("object_id").get<int>() &&
                           g.objects[k].objectId == r.at("object_id").get<int>(),
                       where + " object id");
            MIB_EXPECT(g.objects[k].isValid == valid && g.objects[k].touchesBorder == r.at("cut_off").get<bool>() &&
                           g.objects[k].isTargetGroup == r.at("target").get<bool>(),
                       where + " validity / cut-off / target");
            MIB_EXPECT(g.objects[k].objectCount == e.cells && g.cells[k].blemishCount == e.blemishes,
                       where + " counts");
            MIB_EXPECT(std::isnan(g.objects[k].ringRatio), where + " no ring width");
            ++cells;
        }
    }
    MIB_EXPECT(st.frames == got.size() && st.results == static_cast<uint64_t>(cells), "status counts");
    MIB_EXPECT(st.decodeErrors == 0 && st.sequenceGaps == 0 && st.incompleteFrames == 0 &&
                   st.orphanResults == 0 && st.unknownProfileResults == 0,
               "clean stream: no errors");
    std::printf("replay: %zu frames, %d cells delivered as the PL listed them\n", got.size(), cells);
}

void testCorruptedResultIsCounted(const std::string& vectors) {
    Stream s = buildStream(vectors, 1);
    MIB_REQUIRE(!s.resultOffsets.empty(), "stream has results");
    s.bytes[s.resultOffsets.front() + 40] ^= 0xFF; // payload byte -> CRC mismatch
    backend::processing::ProviderStatus st;
    const auto got = replay(s.bytes, 0, &st);
    MIB_EXPECT(got.size() == s.frames.size(), "every frame still delivered");
    MIB_EXPECT(st.decodeErrors == 1, "the corrupted RESULT is rejected: " + std::to_string(st.decodeErrors));
    MIB_EXPECT(st.incompleteFrames == 1, "its frame is reported incomplete");
    MIB_EXPECT(st.sequenceGaps == 1, "the next record shows the sequence gap");
}

void testPacedReplayHoldsItsRate(const std::string& vectors) {
    const Stream s = buildStream(vectors, 20); // 200 frames
    backend::processing::ProviderStatus st;
    double seconds = 0;
    const auto got = replay(s.bytes, 1000.0, &st, &seconds);
    const double expected = static_cast<double>(s.frames.size() - 1) / 1000.0;
    MIB_EXPECT(got.size() == s.frames.size(), "paced replay delivers every frame");
    // Ratio gate (testing rules): not faster than the rate, and not 3x slower.
    MIB_EXPECT(seconds >= 0.95 * expected && seconds <= 3.0 * expected + 0.05,
               "paced replay holds ~1000 frames/s: " + std::to_string(seconds) + " s for " +
                   std::to_string(s.frames.size()) + " frames");
}

// Optional (MIB_PZ_RING_CAPTURE=<pzres capture file>): replay bytes the PZ7035
// PL wrote to the result ring on the board. Every record must decode, no
// sequence or frame-id gaps, every frame complete.
void testBoardCapture(const char* path) {
    std::ifstream in(path, std::ios::binary);
    MIB_REQUIRE(static_cast<bool>(in), std::string("cannot open ") + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    backend::processing::ProviderStatus st;
    const auto got = replay(std::move(bytes), 0, &st);
    uint64_t idGaps = 0, cells = 0, valid = 0, cut = 0, empty = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        if (i && got[i].frameId != got[i - 1].frameId + 1) ++idGaps;
        cells += got[i].cells.size();
        empty += got[i].empty() ? 1 : 0;
        for (const auto& c : got[i].cells) {
            valid += c.valid() ? 1 : 0;
            cut += c.cutOff ? 1 : 0;
        }
    }
    std::printf("board capture %s: %zu frames, %llu cells (%llu valid, %llu cut off), %llu empty frames; "
                "decode errors %llu, sequence gaps %llu, frame-id gaps %llu, incomplete %llu, unknown profile %llu\n",
                path, got.size(), (unsigned long long)cells, (unsigned long long)valid, (unsigned long long)cut,
                (unsigned long long)empty, (unsigned long long)st.decodeErrors, (unsigned long long)st.sequenceGaps,
                (unsigned long long)idGaps, (unsigned long long)st.incompleteFrames,
                (unsigned long long)st.unknownProfileResults);
    MIB_EXPECT(!got.empty() && st.decodeErrors == 0 && st.sequenceGaps == 0 && idGaps == 0 &&
                   st.incompleteFrames <= 1 && st.unknownProfileResults == 0,
               "board capture decodes cleanly");
}

} // namespace

int main(int argc, char** argv) {
    (void)spdlog::default_logger();
    mib::test::Watchdog watchdog(60);
    MIB_REQUIRE(argc > 1, "usage: pz_execution_provider_test <unet-cells-v2-pl-vectors.json>");
    testReplayDeliversThePlCells(argv[1]);
    watchdog.mark("replay");
    testCorruptedResultIsCounted(argv[1]);
    watchdog.mark("corrupted");
    testPacedReplayHoldsItsRate(argv[1]);
    if (const char* capture = std::getenv("MIB_PZ_RING_CAPTURE"); capture && *capture) {
        watchdog.mark("board capture");
        testBoardCapture(capture);
    }
    return mib::test::exitCode();
}
