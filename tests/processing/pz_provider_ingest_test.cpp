// PL science ingest (YOFO S1, ADR 0008): ProviderFrames from the replay
// provider (the PL conformance vectors as FRAME/RESULT records, plus an
// ingress-error frame and empty frames) drive ProcessingService exactly like
// the inline loop drives it after metrics:
// - run accounting reconciles: every frame admitted once and booked Empty,
//   Processed, RejectedByScientificFilter, or StoreMalformed (FRAME.INVALID);
// - the identification funnel and reason histogram follow the PL's reasons,
//   including Laplacian and Channel;
// - monitoring rows (no images) carry the cells and the configured p2m;
// - the target-group callback never fires (the PL owns the trigger).
//
// argv[1]: scripts/conformance/unet-cells-v2-pl-vectors.json
#include "backend/processing/IExecutionProvider.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/pz/PzExecutionProviders.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace bpz = backend::pz;
using backend::processing::ProviderFrame;
using backend::services::ProcessingService;

namespace {

struct Built {
    std::vector<uint8_t> bytes;
    uint64_t frames{0}, empty{0}, invalid{0}, withValid{0}, rejected{0}, validCells{0}, invalidCells{0};
    std::map<int, uint64_t> reasons; // PL reason code -> invalid cells
};

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& rec) { out.insert(out.end(), rec.begin(), rec.end()); }

Built build(const std::string& vectorsPath) {
    std::ifstream in(vectorsPath);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + vectorsPath);
    const auto doc = nlohmann::json::parse(in);
    static const std::map<std::string, int> codes = {{"NONE", 0},   {"NO_CONTOUR", 1}, {"BORDER", 2},
                                                     {"AREA", 3},   {"DEFORM", 5},     {"AREA_RATIO", 6},
                                                     {"LAPLACIAN", 7}, {"CHANNEL", 8}};
    Built b;
    uint32_t seq = 1;
    uint64_t frameId = 500;
    auto frameRecord = [&](uint16_t results, uint32_t flags) {
        bpz::FrameRecord f;
        f.frameId = frameId;
        f.timestamp = frameId * 11880;
        f.resultCount = results;
        f.flags = flags;
        f.scienceProfile = bpz::kScienceProfileUnetCells;
        f.profileVersion = bpz::kUnetCellsProfileVersion;
        append(b.bytes, bpz::encodeFrameRecord(f, seq++));
    };
    for (const auto& kase : doc.at("cases")) {
        for (const auto& frame : kase.at("frames")) {
            const auto& results = frame.at("results");
            frameRecord(static_cast<uint16_t>(results.size()), results.empty() ? bpz::kFrameEmpty : 0);
            bool anyValid = false;
            uint16_t index = 0;
            for (const auto& res : results) {
                bpz::ResultRecord r;
                r.frameId = frameId;
                r.resultIndex = index++;
                r.flags = res.at("target").get<bool>() ? bpz::kResultTarget : 0;
                r.scienceProfile = bpz::kScienceProfileUnetCells;
                r.profileVersion = bpz::kUnetCellsProfileVersion;
                r.bboxX = res.at("bbox").at("x").get<uint16_t>();
                r.bboxY = res.at("bbox").at("y").get<uint16_t>();
                r.bboxW = res.at("bbox").at("w").get<uint16_t>();
                r.bboxH = res.at("bbox").at("h").get<uint16_t>();
                r.payloadValidity = res.at("validity").get<uint32_t>();
                r.payload = res.at("payload").get<std::vector<uint32_t>>();
                append(b.bytes, bpz::encodeResultRecord(r, seq++));
                const int code = codes.at(res.at("reason").get<std::string>());
                if (code == 0) {
                    anyValid = true;
                    ++b.validCells;
                } else {
                    ++b.invalidCells;
                    ++b.reasons[code];
                }
            }
            ++b.frames;
            if (results.empty()) ++b.empty;
            else if (anyValid) ++b.withValid;
            else ++b.rejected;
            ++frameId;
        }
    }
    // An ingress-error frame (no RESULTs, ever) and two more empty frames.
    frameRecord(0, bpz::kFrameInvalid);
    ++b.frames;
    ++b.invalid;
    ++frameId;
    for (int i = 0; i < 2; ++i) {
        frameRecord(0, bpz::kFrameEmpty);
        ++b.frames;
        ++b.empty;
        ++frameId;
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    (void)spdlog::default_logger();
    mib::test::Watchdog watchdog(60);
    MIB_REQUIRE(argc > 1, "usage: pz_provider_ingest_test <unet-cells-v2-pl-vectors.json>");
    const Built b = build(argv[1]);

    ProcessingService svc;
    svc.setPixelToMicronFactor(0.4886);
    std::atomic<int> triggers{0};
    svc.setTargetGroupCallback([&](const backend::services::TargetGroupEvent&) { ++triggers; });
    svc.setMonitoringActive(true);
    svc.resetIdentificationCounters();
    svc.startExperiment();

    backend::processing::pz::ReplayExecutionProvider provider(b.bytes, 0);
    provider.setSink([&](ProviderFrame&& f) { svc.ingestProviderFrame(f); });
    std::string error;
    MIB_REQUIRE(provider.start(42, &error), "replay starts: " + error);
    MIB_REQUIRE(provider.waitFinished(std::chrono::seconds(20)), "replay finishes");
    provider.stop();
    svc.endExperiment();

    const auto a = svc.experimentAccountingSnapshot();
    MIB_EXPECT(a.admitted == b.frames, "every PL frame admitted: " + std::to_string(a.admitted));
    MIB_EXPECT(a.empty == b.empty, "EMPTY frames booked Empty: " + std::to_string(a.empty));
    MIB_EXPECT(a.processed == b.withValid, "frames with a valid cell booked Processed");
    MIB_EXPECT(a.scientificallyRejected == b.rejected, "frames with only invalid cells booked Rejected");
    MIB_EXPECT(a.storeMalformed == b.invalid && a.processingFailed == 0,
               "an ingress-error frame is an unusable input, not a processing failure");
    MIB_EXPECT(a.objectsDetected == b.validCells, "objects detected = valid cells");
    MIB_EXPECT(a.sequenceGaps == 0, "no gaps in the admitted frame ids");

    const auto id = svc.getIdentificationCounters();
    MIB_EXPECT(id.validObjects == b.validCells && id.invalidObjects == b.invalidCells, "funnel objects");
    namespace science = backend::processing::science;
    for (const auto& [code, n] : b.reasons) {
        MIB_EXPECT(id.reasonCounts[code - 1] == n,
                   "reason " + std::to_string(code) + ": " + std::to_string(id.reasonCounts[code - 1]) +
                       " vs " + std::to_string(n));
    }
    MIB_EXPECT(id.reasonCounts[static_cast<int>(science::InvalidReasonCode::Ring)] == 0, "no ring reasons");

    const auto valid = svc.getMonitoringValidFrames();
    const auto invalid = svc.getMonitoringInvalidFrames();
    MIB_EXPECT(valid.size() == b.validCells && invalid.size() == b.invalidCells, "one monitoring row per cell");
    for (const auto& row : valid) {
        MIB_EXPECT(row.originalImage.empty() && row.validation.analysisPixelToMicronFactor == 0.4886 &&
                       std::isnan(row.validation.ringRatio),
                   "imageless rows with the configured p2m");
    }
    MIB_EXPECT(triggers.load() == 0, "a PL target never fires a PS trigger");
    std::printf("ingest: %llu frames (%llu empty, %llu with valid cells, %llu rejected, %llu ingress errors), "
                "%llu valid + %llu invalid cells; accounting %s\n",
                (unsigned long long)b.frames, (unsigned long long)b.empty, (unsigned long long)b.withValid,
                (unsigned long long)b.rejected, (unsigned long long)b.invalid, (unsigned long long)b.validCells,
                (unsigned long long)b.invalidCells, a.reconciled ? "reconciles" : "DOES NOT reconcile");
    MIB_EXPECT(a.reconciled, "accounting reconciles");
    return mib::test::exitCode();
}
