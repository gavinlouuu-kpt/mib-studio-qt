// Record to the SATA SSD through the experiment lifecycle (#667 S2): with MIB_PZREC / MIB_SSD_IMAGE set the readiness gate `storage.ssd` blocks until the SSD
// is READY; Start asks pzrec for the run id (snapshot `ssd_run_id`), arms the provider, then opens the run on the SSD; Stop closes it (graceful) before the
// run finalises; a missing or not-READY SSD refuses the Start with the SSD named. Needs the PL vectors (argument 1) and a built pz7035 `pzrec` (argument 2).
#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/SciencePlacement.h"
#include "backend/processing/IExecutionProvider.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/pz/PzExecutionProviders.h"
#include "backend/pz/PzRecords.h"
#include "backend/pz/SsdStore.h"
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
#include <mutex>
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

// A scripted pzrec for the failure paths: a READY SSD whose start or stop misbehaves.
std::string recorderStatus(const std::string& state, bool open, uint32_t openId, uint32_t next) {
    char buf[1400];
    std::snprintf(buf, sizeof buf,
                  R"({"state":"%s","state_code":3,"last_error":"OK","raw_sectors":417792,"head_lba":2048,"tail_lba":2048,"free_sectors":415744,"min_run_sectors":131072,"runs":%u,"next_run_id":%u,"table_entries":256,"open_run":%s,"open_run_id":%u,"open_start_unix_ms":1791530000000,"open_client_tag":7,"open_wall_source":1,"recovered_runs":0,"skipped_bad_entries":0,"recovered_ids":[0,0,0,0,0,0,0,0],"counters":{"seen":0,"empty_filtered":0,"invalid_not_sampled":0,"passed":0,"written":0,"dropped":0,"failed":0,"bytes_written":0,"drain_kbps":0,"first_frame_id":0,"last_frame_id":0}})",
                  state.c_str(), next - 1, next, open ? "true" : "false", openId);
    return buf;
}
struct ScriptedSsd final : bpz::ISsdDevice {
    std::mutex m;
    std::string state = "READY";
    bool open = false, startRefuses = false, stopAlwaysFails = false;
    uint32_t openId = 0, next = 1, startOpensId = 0;
    int stopCalls = 0, abortCalls = 0;
    bool status(std::string& json, std::string*) override { std::lock_guard<std::mutex> l(m); json = recorderStatus(state, open, openId, next); return true; }
    bool runs(std::string& json, std::string*) override { json = "[]"; return true; }
    bool start(const bpz::SsdStartArgs&, std::string& json, std::string* error) override {
        std::lock_guard<std::mutex> l(m);
        if (startRefuses) { if (error) *error = "pzrec start exited with 1: cannot read the disk"; return false; }
        open = true; openId = startOpensId ? startOpensId : next; next = openId + 1; state = "RECORDING";
        json = recorderStatus(state, open, openId, next);
        return true;
    }
    bool stop(bool abort, std::string& json, std::string* error) override {
        std::lock_guard<std::mutex> l(m);
        ++stopCalls;
        if (abort) ++abortCalls;
        if (stopAlwaysFails) { if (error) *error = "pzrec stop did not answer within 30000 ms"; return false; }
        open = false; state = "READY";
        json = recorderStatus(state, open, openId, next);
        return true;
    }
};

std::unique_ptr<bpz::SsdStore> scriptedStore(ScriptedSsd*& raw) {
    auto dev = std::make_unique<ScriptedSsd>();
    raw = dev.get();
    auto store = std::make_unique<bpz::SsdStore>(std::move(dev), std::chrono::milliseconds(1), false);
    store->setStopRetries(2, std::chrono::milliseconds(5));
    return store;
}

} // namespace

int main(int argc, char** argv) {
    mib::test::Watchdog wd(120);
    MIB_REQUIRE(argc > 2, "usage: ssd_experiment_test <unet-cells-v2-pl-vectors.json> <pzrec>");
    const std::string pzrec = argv[2];
    mib::test::TempDir td("ssd_experiment");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "write mock frames");
    const fs::path records = td.path() / "ring.bin";
    const Written written = writeRecords(argv[1], records, 30);
    const std::string img = (td.path() / "ssd.img").string();
    MIB_REQUIRE(std::system((pzrec + " format " + img + " --size-mib 512 --min-run-mib 64 > /dev/null").c_str()) == 0, "pzrec format");

    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", ("replay:" + records.string() + "@2000").c_str());
    setEnv("MIB_PZREC", pzrec.c_str());
    setEnv("MIB_SSD_IMAGE", img.c_str());
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "science on the PL");

    {
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
        auto* provider = backend.executionProvider();
        MIB_REQUIRE(provider && provider->name() == "replay", "replay provider");
        auto& coord = backend.experiment();
        coord.setApplicationIdentity("test-version", "test-build", "test-os");
        backend.processing().setPixelToMicronFactor(0.4886);
        backend.processing().setMonitoringActive(true);
        backend.processing().setInvalidFrameSamplingRate(1);
        MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted, "capture starts");
        MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 2; }, std::chrono::seconds(5)), "mock frames arrive");
        backend.ssdStore().refresh(true);

        const std::string out = (td.path() / "run.h5").string();
        auto ready = coord.evaluateReadiness(out, "pl");
        const auto* gate = ready.gate("storage.ssd");
        MIB_EXPECT(gate && gate->status == backend::app::GateStatus::Pass, "storage.ssd passes with a READY SSD");
        if (!ready.ready) {
            for (const auto& g : ready.gates)
                std::fprintf(stderr, "  %-26s %-12s %s %s\n", g.id.c_str(), backend::app::toString(g.status), g.reason.c_str(), g.detail.c_str());
        }
        MIB_REQUIRE(ready.ready, "ready to start");

        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = ready.generation;
        req.profileId = "pl";
        const auto started = coord.start(req);
        MIB_REQUIRE(started.outcome == ExperimentStartOutcome::Started, "run starts: " + started.message);
        MIB_EXPECT(started.run.ssdRunId == 1, "the run snapshot names the SSD run id");
        backend.ssdStore().refresh(true);
        const auto during = backend.ssdStore().status();
        MIB_EXPECT(during.state == bpz::SsdState::Recording && during.openRunId == 1, "the SSD is RECORDING run 1 while the experiment runs");
        MIB_EXPECT(backend.ssdStore().openedRunId() == 1, "Studio knows the run it opened");
        MIB_REQUIRE(waitFor([&] { return provider->status().frames >= written.frames; }, std::chrono::seconds(20)), "every replayed frame reached the sink");
        coord.requestStop(false);
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal; }, std::chrono::seconds(30)), "run finalizes");
        MIB_EXPECT(coord.status().finalizationOk, "the run finalised cleanly");
        backend.ssdStore().refresh(true);
        MIB_EXPECT(backend.ssdStore().status().state == bpz::SsdState::Ready && backend.ssdStore().openedRunId() == 0, "the SSD run is closed");
        std::vector<bpz::SsdRun> runs;
        std::string why;
        MIB_REQUIRE(backend.ssdStore().runs(runs, &why) && runs.size() == 1, why);
        MIB_EXPECT(runs[0].id == 1 && !runs[0].open && runs[0].reason == 0 && runs[0].clientTag == started.run.startGeneration, "the table holds the closed run with the experiment's generation as tag");
        MIB_EXPECT(runs[0].samplerN == 1, "the invalid sampler is the processing service's rate (the test set 1), as for the HDF5 file");
        MIB_EXPECT(runs[0].startUnixMs == started.run.startWallClockNs / 1000000ull, "start_unix_ms is the run's wall-clock start");

        // the filter follows the store's setting (MIB_SSD_FILTER): the default is valid, `all` is another filter in the table
        backend.ssdStore().setFilterFromEnv("all");
        const auto ready2 = coord.evaluateReadiness(out + "2.h5", "pl");
        MIB_REQUIRE(ready2.ready, "ready again");
        ExperimentStartRequest req2;
        req2.outputPath = out + "2.h5";
        req2.readinessGeneration = ready2.generation;
        req2.profileId = "pl";
        const auto started2 = coord.start(req2);
        MIB_REQUIRE(started2.outcome == ExperimentStartOutcome::Started, "second run starts: " + started2.message);
        MIB_EXPECT(started2.run.ssdRunId == 2, "ids are monotone");
        std::vector<bpz::SsdRun> runs2;
        coord.requestStop(false);
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal && backend.ssdStore().openedRunId() == 0; }, std::chrono::seconds(30)), "second run finalizes");
        backend.ssdStore().refresh(true);
        MIB_REQUIRE(backend.ssdStore().runs(runs2, &why) && runs2.size() == 2, why);
        MIB_EXPECT(runs2[0].filter != runs2[1].filter, "run 2 used the `all` filter, run 1 the default");

        backend.capture().stop();
        backend.shutdown();
    }
    // the run file records the SSD run id in its snapshot
    {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile((td.path() / "run.h5").string()), "run file opens");
        std::string snapshot;
        MIB_EXPECT(reader.readRunSnapshotJson(snapshot) && snapshot.find("\"ssd_run_id\":1") != std::string::npos, "the snapshot carries ssd_run_id");
        reader.closeFile();
    }

    // an SSD that is not READY refuses the Start with the SSD named
    {
        setEnv("MIB_SSD_IMAGE", (td.path() / "no-such.img").string().c_str());
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data2").string()), "backend init");
        MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted, "capture starts");
        MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 2; }, std::chrono::seconds(5)), "mock frames arrive");
        backend.ssdStore().refresh(true);
        const auto r = backend.experiment().evaluateReadiness((td.path() / "x.h5").string(), "pl");
        const auto* gate = r.gate("storage.ssd");
        MIB_EXPECT(!r.ready && gate && gate->status == backend::app::GateStatus::Fail, "storage.ssd blocks a Start when the SSD is not READY");
        backend.capture().stop();
        backend.shutdown();
    }
    // a failed `pzrec start` after the ARM rolls the Start back: the provider is stopped, the file removed, the SSD named
    {
        setEnv("MIB_PZREC", pzrec.c_str());
        setEnv("MIB_SSD_IMAGE", img.c_str());
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data4").string()), "backend init");
        auto* provider = backend.executionProvider();
        MIB_REQUIRE(provider, "provider");
        ScriptedSsd* dev = nullptr;
        backend.setSsdStoreForTesting(scriptedStore(dev));
        backend.processing().setPixelToMicronFactor(0.4886);
        MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted, "capture starts");
        MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 2; }, std::chrono::seconds(5)), "mock frames arrive");
        const std::string out = (td.path() / "refused.h5").string();

        dev->startRefuses = true;
        auto ready = backend.experiment().evaluateReadiness(out, "pl");
        MIB_REQUIRE(ready.ready, "ready (the SSD is READY)");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = ready.generation;
        req.profileId = "pl";
        auto r = backend.experiment().start(req);
        MIB_EXPECT(r.outcome == ExperimentStartOutcome::NotReady && r.message.find("SSD:") != std::string::npos && r.message.find("pzrec start") != std::string::npos,
                   "a refused pzrec start fails the Start with the SSD named: " + r.message);
        MIB_EXPECT(!provider->status().running && !fs::exists(out) && backend.ssdStore().openedRunId() == 0, "provider stopped, no file, nothing to stop");
        MIB_EXPECT(dev->stopCalls == 0, "nothing was opened: nothing is stopped");

        // a different id: fail closed and abort the run pzrec opened
        dev->startRefuses = false;
        dev->startOpensId = 7;
        ready = backend.experiment().evaluateReadiness(out, "pl");
        req.readinessGeneration = ready.generation;
        r = backend.experiment().start(req);
        MIB_EXPECT(r.outcome == ExperimentStartOutcome::NotReady && r.message.find("mismatch") != std::string::npos, "an id mismatch fails closed: " + r.message);
        MIB_EXPECT(dev->abortCalls == 1 && !provider->status().running && backend.ssdStore().openedRunId() == 0, "the unexpected run was aborted");

        // a stop that never works: the run finalises with an SSD fault, the UI is not blocked
        dev->startOpensId = 0;
        dev->stopCalls = dev->abortCalls = 0;
        ready = backend.experiment().evaluateReadiness(out, "pl");
        req.readinessGeneration = ready.generation;
        r = backend.experiment().start(req);
        MIB_REQUIRE(r.outcome == ExperimentStartOutcome::Started, "starts: " + r.message);
        dev->stopAlwaysFails = true;
        backend.experiment().requestStop(false);
        MIB_REQUIRE(waitFor([&] { return backend.experiment().status().terminal; }, std::chrono::seconds(90)), "the run finalizes even when pzrec stop fails");
        const auto st = backend.experiment().status();
        MIB_EXPECT(!st.finalizationOk && st.faultCode == "experiment.ssdStopFailed" && st.faultMessage.find("SSD run") != std::string::npos, "the failed SSD stop is a named fault: " + st.faultCode + " " + st.faultMessage);
        MIB_EXPECT(dev->stopCalls == 2, "the store retried");
        backend.capture().stop();
        backend.shutdown();
    }
    // without an SSD configured nothing changes
    {
        unsetenv("MIB_PZREC");
        unsetenv("MIB_SSD_IMAGE");
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((td.path() / "data3").string()), "backend init");
        const auto r = backend.experiment().evaluateReadiness((td.path() / "y.h5").string(), "pl");
        const auto* gate = r.gate("storage.ssd");
        MIB_EXPECT(gate && gate->status == backend::app::GateStatus::NotRequired, "no SSD configured: the gate is not required");
        backend.shutdown();
    }
    return mib::test::exitCode();
}
