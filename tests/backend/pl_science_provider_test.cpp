// PL science end to end through the shared experiment lifecycle (YOFO S1,
// ADR 0008): with MIB_PL_SCIENCE=1 and MIB_EXECUTION_PROVIDER=replay:<file>,
// AppBackend creates the provider and routes its frames into
// ProcessingService; readiness passes the science.pl gate; Start arms the
// provider after the run's accounting began; Stop stops it before the final
// drain. The run's accounting admits every replayed PL frame once and
// reconciles. A provider that cannot start rolls the Start back (NotReady,
// no file left behind).
#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/SciencePlacement.h"
#include "backend/processing/IExecutionProvider.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/pz/PzExecutionProviders.h"
#include "backend/pz/PzRecords.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
// POSIX setenv is not available with MSVC.
void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
}  // namespace

namespace fs = std::filesystem;
namespace bpz = backend::pz;
using backend::app::ExperimentStartOutcome;
using backend::app::ExperimentStartRequest;

namespace {

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

struct Written {
    uint64_t frames{0};
    uint64_t validCells{0};
    uint64_t invalidCells{0};
};

// The PL vectors as a record stream (FRAME + RESULTs per frame), `repeat`
// times.
Written writeRecords(const std::string& vectorsPath, const fs::path& out, int repeat) {
    Written w;
    std::ifstream in(vectorsPath);
    MIB_REQUIRE(static_cast<bool>(in), "cannot open " + vectorsPath);
    const auto doc = nlohmann::json::parse(in);
    std::vector<uint8_t> bytes;
    auto append = [&](const std::vector<uint8_t>& r) { bytes.insert(bytes.end(), r.begin(), r.end()); };
    uint32_t seq = 1;
    uint64_t frameId = 1;
    for (int k = 0; k < repeat; ++k) {
        for (const auto& kase : doc.at("cases")) {
            for (const auto& frame : kase.at("frames")) {
                const auto& results = frame.at("results");
                bpz::FrameRecord f;
                f.frameId = frameId;
                f.timestamp = frameId * 11880;
                f.resultCount = static_cast<uint16_t>(results.size());
                f.flags = results.empty() ? bpz::kFrameEmpty : 0;
                f.scienceProfile = bpz::kScienceProfileUnetCells;
                f.profileVersion = bpz::kUnetCellsProfileVersion;
                append(bpz::encodeFrameRecord(f, seq++));
                uint16_t index = 0;
                for (const auto& res : results) {
                    bpz::ResultRecord r;
                    r.frameId = frameId;
                    r.resultIndex = index++;
                    r.scienceProfile = bpz::kScienceProfileUnetCells;
                    r.profileVersion = bpz::kUnetCellsProfileVersion;
                    r.bboxX = res.at("bbox").at("x").get<uint16_t>();
                    r.bboxY = res.at("bbox").at("y").get<uint16_t>();
                    r.bboxW = res.at("bbox").at("w").get<uint16_t>();
                    r.bboxH = res.at("bbox").at("h").get<uint16_t>();
                    r.payloadValidity = res.at("validity").get<uint32_t>();
                    r.payload = res.at("payload").get<std::vector<uint32_t>>();
                    append(bpz::encodeResultRecord(r, seq++));
                    (res.at("reason").get<std::string>() == "NONE" ? w.validCells : w.invalidCells) += 1;
                }
                ++frameId;
            }
        }
    }
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    w.frames = frameId - 1;
    return w;
}

} // namespace

int main(int argc, char** argv) {
    mib::test::Watchdog wd(90);
    MIB_REQUIRE(argc > 1, "usage: pl_science_provider_test <unet-cells-v2-pl-vectors.json>");
    mib::test::TempDir td("pl_science_provider");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "write mock frames");
    const fs::path records = td.path() / "ring.bin";
    const Written written = writeRecords(argv[1], records, 30);
    const uint64_t frameCount = written.frames;

    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", ("replay:" + records.string() + "@2000").c_str());
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "science on the PL");

    {
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
        auto* provider = backend.executionProvider();
        MIB_REQUIRE(provider && provider->name() == "replay", "AppBackend created the replay provider");
        auto& coord = backend.experiment();
        coord.setApplicationIdentity("test-version", "test-build", "test-os");
        backend.processing().setPixelToMicronFactor(0.4886);
        backend.processing().setMonitoringActive(true);
        backend.processing().setInvalidFrameSamplingRate(1); // record every invalid cell

        MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted,
                    "capture starts (preview path)");
        MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 2; },
                            std::chrono::seconds(5)),
                    "mock frames arrive");

        const std::string out = (td.path() / "run.h5").string();
        const auto readiness = coord.evaluateReadiness(out, "pl");
        const auto* gate = readiness.gate("science.pl");
        MIB_EXPECT(gate && gate->status == backend::app::GateStatus::Pass &&
                       gate->detail.find("replay") != std::string::npos,
                   "science.pl passes with the provider named");
        if (!readiness.ready) {
            for (const auto& g : readiness.gates)
                std::fprintf(stderr, "  %-26s %-12s %s %s\n", g.id.c_str(), backend::app::toString(g.status),
                             g.reason.c_str(), g.detail.c_str());
        }
        MIB_REQUIRE(readiness.ready, "ready to start");
        const auto* compileGate = readiness.gate("processing.profileCompile");
        MIB_EXPECT(compileGate && compileGate->status == backend::app::GateStatus::Pass,
                   "the settings compile into the PL profile");
        {
            // A setting outside the profile's range fails the gate.
            auto cfg = backend.processing().getProcessingConfig();
            const auto saved = cfg;
            cfg.laplacian_kernel_size = 2;
            backend.processing().setProcessingConfig(cfg);
            const auto bad = coord.evaluateReadiness(out, "pl");
            const auto* g = bad.gate("processing.profileCompile");
            MIB_EXPECT(!bad.ready && g && g->status == backend::app::GateStatus::Fail &&
                           g->reason.find("laplacian_kernel_size") != std::string::npos,
                       "an uncompilable setting blocks Start with the reason");
            backend.processing().setProcessingConfig(saved);
        }
        const auto ready = coord.evaluateReadiness(out, "pl");
        MIB_REQUIRE(ready.ready, "ready again");

        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = ready.generation;
        req.profileId = "pl";
        const auto started = coord.start(req);
        MIB_REQUIRE(started.outcome == ExperimentStartOutcome::Started, "run starts: " + started.message);
        wd.mark("running");
        MIB_REQUIRE(waitFor([&] { return provider->status().frames >= frameCount; }, std::chrono::seconds(20)),
                    "every replayed frame reached the provider sink");
        MIB_EXPECT(!backend.processing().getMonitoringValidFrames().empty(), "monitoring rows from PL cells");
        coord.requestStop(false);
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal; }, std::chrono::seconds(20)), "run finalizes");
        wd.mark("stopped");

        const auto a = backend.processing().experimentAccountingSnapshot();
        std::fprintf(stderr, "accounting: admitted %llu empty %llu processed %llu rejected %llu malformed %llu\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.empty, (unsigned long long)a.processed,
                     (unsigned long long)a.scientificallyRejected, (unsigned long long)a.storeMalformed);
        MIB_EXPECT(a.admitted == frameCount, "every PL frame admitted once: " + std::to_string(a.admitted) +
                                                 " of " + std::to_string(frameCount));
        MIB_EXPECT(a.reconciled, "the run's accounting reconciles");
        MIB_EXPECT(!provider->status().running && provider->status().decodeErrors == 0, "provider stopped, clean");
        MIB_EXPECT(provider->status().profileCommitted, "the compiled profile was committed before arming");
        auto* replay = dynamic_cast<backend::processing::pz::ReplayExecutionProvider*>(provider);
        MIB_REQUIRE(replay, "replay provider");
        MIB_EXPECT(replay->lastProfile().page[2] == static_cast<uint32_t>(
                       backend.processing().getProcessingConfig().min_cell_area_px),
                   "the committed page carries the configured size gate");
        MIB_EXPECT(!backend.hdf5().isFileOpen() && fs::exists(out), "run file finalized");
        backend.capture().stop();
        backend.shutdown();

        // The recording (YOFO S3): one metadata row per cell with the PL's
        // values, no images, and the provider in the run provenance.
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out), "run file opens");
        std::vector<backend::services::ProcessedFrame> valid, invalid;
        MIB_REQUIRE(reader.readValidMetadata(valid) && reader.readInvalidMetadata(invalid), "metadata reads");
        MIB_EXPECT(valid.size() == written.validCells && invalid.size() == written.invalidCells,
                   "one row per cell: valid " + std::to_string(valid.size()) + "/" +
                       std::to_string(written.validCells) + ", invalid " + std::to_string(invalid.size()) + "/" +
                       std::to_string(written.invalidCells));
        bool values = !valid.empty();
        for (const auto& f : valid) {
            values = values && f.validation.isValid && std::isfinite(f.validation.brightnessMean) &&
                     std::isfinite(f.validation.laplacianVariance) && f.validation.pixelCount > 0 &&
                     f.validation.contourArea > 0 && std::isnan(f.validation.ringRatio);
        }
        MIB_EXPECT(values, "valid rows carry the PL cell values");
        std::vector<backend::services::ProcessedFrame> full;
        MIB_EXPECT(reader.readValidFrames(full) && full.size() == valid.size() && full.front().originalImage.empty(),
                   "a PL run reads without images");
        std::string snapshot;
        MIB_EXPECT(reader.readRunSnapshotJson(snapshot) &&
                       snapshot.find("\"science_placement\":\"pl\"") != std::string::npos &&
                       snapshot.find("\"execution_provider\":\"replay\"") != std::string::npos,
                   "the run snapshot records the PL and the provider");
        MIB_EXPECT(snapshot.find("\"pl_core\":{\"valid\":false") != std::string::npos &&
                       snapshot.find("\"weights_sha256_prefix\":\"\"") != std::string::npos,
                   "the run snapshot carries the PL core (none for a replay)");
        reader.closeFile();
    }

    // A provider that cannot start rolls the Start back.
    {
        const fs::path missing = td.path() / "missing.bin";
        setEnv("MIB_EXECUTION_PROVIDER", ("replay:" + missing.string()).c_str());
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data2").string()), "backend init");
        MIB_EXPECT(backend.executionProvider() == nullptr, "an unreadable replay file gives no provider");
        const auto readiness = backend.experiment().evaluateReadiness((td.path() / "x.h5").string());
        const auto* gate = readiness.gate("science.pl");
        MIB_EXPECT(gate && gate->status == backend::app::GateStatus::Warn, "science.pl warns without a provider");
        backend.shutdown();
    }
    return mib::test::exitCode();
}
