// method_gate_e2e_test (#398 M2): central method -> readiness -> HDF5, end to
// end through AppBackend with the mock camera and a fake Supabase.
//  - a materialized central revision applied as config.json is recognised:
//    method.revision Warn (not validated here), Start allowed;
//  - planMethodApply hands out the exact materialized bytes + changed keys and
//    refuses revoked/uncached revisions;
//  - "Mark validated" accepts only a test run whose /run_provenance names the
//    revision (a run of another revision, a local-method run or a missing file
//    is refused); accepting it bumps the readiness generation and passes the
//    gate; the stale preflight is refused;
//  - Start freezes the exact revision + validation into /run_provenance
//    (run snapshot schema v2 "method" block), readable after close;
//  - a revoked revision (applied directly, or revoked centrally after a
//    refresh) blocks Start with NotReady;
//  - a locally edited config is a local method (NotRequired);
//  - (M2c) r1 is applied through the backend config.json applier with its
//    exact bytes, a revoked revision is not applied, and Apply is refused
//    while a run is in flight.
#include "backend/app/AppBackend.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/MethodApply.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/processing/ProcessingCoreSha256.h"
#include "backend/processing/ProcessingService.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/fake_supabase.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

using backend::app::ExperimentStartOutcome;
using backend::app::ExperimentStartRequest;
using backend::app::GateStatus;
using backend::profiles::RegistryJobState;
using backend::services::ProcessingService;
using mib::test::FakeSupabase;
using mib::test::revisionJson;
using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const backend::app::ReadinessGate& methodGate(const backend::app::ExperimentReadinessSnapshot& r) {
    const auto* g = r.gate("method.revision");
    MIB_REQUIRE(g != nullptr, "method.revision gate present");
    return *g;
}

void dumpGates(const backend::app::ExperimentReadinessSnapshot& r) {
    for (const auto& g : r.gates)
        if (g.status != GateStatus::Pass)
            std::fprintf(stderr, "  %-26s %-12s %s\n", g.id.c_str(), backend::app::toString(g.status),
                         g.reason.c_str());
}

RegistryJobState run(backend::profiles::ProfileRegistryWorker& worker, std::uint64_t id) {
    MIB_REQUIRE(id != 0, "registry request refused");
    MIB_REQUIRE(worker.waitForJob(id, 10s), "registry job finished");
    return worker.job(id).state;
}

} // namespace

int main() {
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("method_gate_e2e");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "write mock frames");
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");
    setEnv("MIB_PROFILE_REGISTRY_URL", mib::test::kOrigin);
    setEnv("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY", mib::test::kKey);
    setEnv("MIB_INSTRUMENT_NAME", "MIB-01");

    FakeSupabase fake;
    fake.users["bob@lab"] = {"user-bob", "pw-bob", {"p1"}};
    fake.revisions["p1"] = {revisionJson("p1", "r1", 1, "published"),
                            revisionJson("p1", "r2", 2, "revoked")};

    backend::AppBackend backend;
    backend.setProfileRegistryTransport(mib::test::transportFor(fake));
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    MIB_REQUIRE(backend::profiles::isInstrumentUuid(backend.instrumentIdentity().id), "instrument id");
    auto& coord = backend.experiment();
    auto& registry = backend.profileRegistry();
    auto& proc = backend.processing();
    {
        auto cfg = proc.getProcessingConfig();
        cfg.enable_target_group = false;
        cfg.auto_background_enabled = false;
        proc.setProcessingConfig(cfg);
    }
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeBackgroundGray(cv::Mat(96, 96, CV_8UC1, cv::Scalar(5)));
    proc.startRealtime(backend.getFrameStore());
    camera::mock::MockCameraOptions opts;
    opts.folder = frames;
    opts.frameInterval = std::chrono::microseconds(2000);
    opts.loopFiles = true;
    backend.configureMockCamera(opts);
    MIB_REQUIRE(backend.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted,
                "capture start");
    MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 2; }, 5s),
                "frames flowing");

    const std::string out = (td.path() / "run-central.h5").string();

    wd.mark("sign in, refresh, materialize");
    MIB_REQUIRE(run(registry, registry.requestSignIn("bob@lab", "pw-bob")) == RegistryJobState::Succeeded,
                "sign in");
    MIB_REQUIRE(run(registry, registry.requestRefresh()) == RegistryJobState::Succeeded, "refresh");
    MIB_REQUIRE(run(registry, registry.requestMaterialize("r1")) == RegistryJobState::Succeeded,
                "materialize r1");
    MIB_REQUIRE(run(registry, registry.requestMaterialize("r2")) == RegistryJobState::Succeeded,
                "materialize r2 (revoked revisions stay readable)");
    const auto methods = td.path() / "data" / "methods";
    const auto r1Config = readText(methods / "r1" / "config.json");
    const auto r2Config = readText(methods / "r2" / "config.json");
    MIB_REQUIRE(!r1Config.empty() && !r2Config.empty(), "materialized configs");

    wd.mark("local config is NotRequired");
    backend.setLastConfigJson(R"({"config_schema_version":1,"gain":42})");
    auto r = coord.evaluateReadiness(out);
    MIB_EXPECT(methodGate(r).status == GateStatus::NotRequired && r.candidate.method.source == "local",
               "unmatched config: local method");

    wd.mark("applied central revision, not validated: Warn");
    // Apply through the backend config.json applier (the React/Tauri path,
    // #398 M2c); the Qt AppConfigWatcher records the same exact bytes.
    {
        const auto applied = backend::app::applyCentralMethod(backend, "r1");
        MIB_REQUIRE(applied.ok, applied.error);
        MIB_EXPECT(backend.getLastConfigJson() == r1Config, "applier recorded r1's exact bytes");
        const auto revoked = backend::app::applyCentralMethod(backend, "r2");
        MIB_EXPECT(!revoked.ok && revoked.error.find("revoked") != std::string::npos &&
                       backend.getLastConfigJson() == r1Config,
                   "a revoked revision is not applied");
    }
    r = coord.evaluateReadiness(out);
    dumpGates(r);
    MIB_EXPECT(r.candidate.method.source == "central" && r.candidate.method.revisionId == "r1",
               "applied config recognised as r1");
    MIB_EXPECT(methodGate(r).status == GateStatus::Warn, "not validated here: Warn");
    MIB_EXPECT(r.ready, "Warn does not block Start");
    const auto unvalidatedGeneration = r.generation;
    MIB_EXPECT(coord.evaluateReadiness(out).generation == unvalidatedGeneration,
               "stable method state keeps the generation");

    // Start + finalize one run; returns its persisted run_snapshot_json.
    const auto recordRun = [&](const std::string& path) {
        const auto ready = coord.evaluateReadiness(path);
        MIB_REQUIRE(ready.ready, "ready");
        ExperimentStartRequest req;
        req.outputPath = path;
        req.readinessGeneration = ready.generation;
        MIB_REQUIRE(coord.start(req).outcome == ExperimentStartOutcome::Started, "Started");
        const auto midRun = backend::app::applyCentralMethod(backend, "r1");
        MIB_EXPECT(!midRun.ok && midRun.error.find("in progress") != std::string::npos,
                   "Apply is refused while a run is in flight");
        MIB_EXPECT(coord.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop");
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal; }, 20s), "run finalizes");
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reload run file");
        std::string runJson;
        MIB_REQUIRE(reader.readRunSnapshotJson(runJson), "run snapshot persisted");
        reader.closeFile();
        return Json::parse(runJson);
    };

    wd.mark("plan apply");
    {
        auto plan = backend::app::planMethodApply(registry.snapshot(), "r1",
                                                  R"({"config_schema_version":1,"gain":42})");
        MIB_EXPECT(plan.ok && plan.configText == r1Config, "plan carries the exact materialized bytes");
        MIB_EXPECT(plan.changedKeys == std::vector<std::string>{"gain"}, "plan lists the changed key");
        MIB_EXPECT(plan.cameraScriptPath == (methods / "r1" / "egrabberConfig.js").string(),
                   "camera script path offered");
        plan = backend::app::planMethodApply(registry.snapshot(), "r2", r1Config);
        MIB_EXPECT(!plan.ok && plan.error.find("revoked") != std::string::npos, "revoked: not applicable");
        plan = backend::app::planMethodApply(registry.snapshot(), "nope", r1Config);
        MIB_EXPECT(!plan.ok, "uncached: not applicable");
    }

    wd.mark("unvalidated test run, then mark validated with it");
    const std::string testRun = (td.path() / "test-run.h5").string();
    const auto testJson = recordRun(testRun);
    MIB_EXPECT(testJson.at("method").at("validation") == "none" &&
                   testJson.at("method").at("revision_id") == "r1",
               "test run recorded the unvalidated revision");
    {
        const auto missing = backend.requestMethodValidation("r1", (td.path() / "nope.h5").string(), true);
        MIB_EXPECT(missing.jobId == 0 && missing.error.find("Cannot open") != std::string::npos,
                   "missing evidence refused");
        const auto wrongRevision = backend.requestMethodValidation("r2", testRun, true);
        MIB_EXPECT(wrongRevision.jobId == 0 &&
                       wrongRevision.error.find("not recorded with this revision") != std::string::npos,
                   "a run of r1 cannot validate r2");
        MIB_EXPECT(registry.snapshot().validations.empty(), "nothing recorded by refused requests");
    }
    r = coord.evaluateReadiness(out);
    const auto beforeValidation = r.generation;
    const auto accepted = backend.requestMethodValidation("r1", testRun, true);
    MIB_REQUIRE(accepted.jobId != 0, accepted.error);
    MIB_REQUIRE(run(registry, accepted.jobId) == RegistryJobState::Succeeded, "validation recorded");
    r = coord.evaluateReadiness(out);
    MIB_EXPECT(methodGate(r).status == GateStatus::Pass, "validated here: Pass");
    MIB_EXPECT(r.generation != beforeValidation, "validation bumps the readiness generation");
    {
        ExperimentStartRequest stale;
        stale.outputPath = out;
        stale.readinessGeneration = beforeValidation;
        MIB_EXPECT(coord.start(stale).outcome == ExperimentStartOutcome::StaleReadiness,
                   "pre-validation preflight cannot authorize Start");
    }

    wd.mark("start freezes the revision into HDF5");
    {
        const auto j = recordRun(out);
        const auto& m = j.at("method");
        MIB_EXPECT(j.at("schema_version") == 2, "run snapshot schema v2");
        MIB_EXPECT(m.at("source") == "central" && m.at("revision_id") == "r1" &&
                       m.at("central_state") == "published" && m.at("validation") == "passed" &&
                       m.at("validator_id") == "user-bob" &&
                       m.at("instrument_id") == backend.instrumentIdentity().id &&
                       m.at("instrument_name") == "MIB-01" &&
                       m.at("registry_origin") == mib::test::kOrigin,
                   "method block in /run_provenance");
        std::string hashError;
        MIB_EXPECT(m.at("validation_evidence_sha256") ==
                       backend::processing::fileSha256(testRun, &hashError, {}),
                   "evidence hash is the test run's");
        MIB_EXPECT(m.at("content_hash") == testJson.at("method").at("content_hash") &&
                       m.at("content_hash").get<std::string>().size() == 64,
                   "exact content hash frozen");
    }

    wd.mark("a local-method run is not evidence");
    {
        backend.setLastConfigJson(R"({"config_schema_version":1,"gain":42})");
        const std::string localRun = (td.path() / "local-run.h5").string();
        MIB_EXPECT(recordRun(localRun).at("method").at("source") == "local", "local run recorded");
        const auto refused = backend.requestMethodValidation("r1", localRun, true);
        MIB_EXPECT(refused.jobId == 0 && !refused.error.empty(), "local run refused as evidence");
    }

    wd.mark("revoked revision blocks Start");
    {
        backend.setLastConfigJson(r2Config);
        r = coord.evaluateReadiness(out);
        MIB_EXPECT(r.candidate.method.revisionId == "r2" && methodGate(r).status == GateStatus::Fail,
                   "revoked: Fail");
        MIB_EXPECT(!r.ready, "not ready");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        MIB_EXPECT(coord.start(req).outcome == ExperimentStartOutcome::NotReady, "NotReady");
    }

    wd.mark("central revocation after refresh");
    {
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            auto& revisions = fake.revisions["p1"];
            revisions[0]["state"] = "revoked";
            revisions[0]["metadata_version"] = 2;
        }
        backend.setLastConfigJson(r1Config);
        const auto beforeRefresh = coord.evaluateReadiness(out);
        MIB_EXPECT(methodGate(beforeRefresh).status == GateStatus::Pass,
                   "unknown to this instrument until a refresh");
        MIB_REQUIRE(run(registry, registry.requestRefresh()) == RegistryJobState::Succeeded, "refresh");
        r = coord.evaluateReadiness(out);
        MIB_EXPECT(methodGate(r).status == GateStatus::Fail && r.candidate.method.centralState == "revoked",
                   "revocation reaches the gate even though validated earlier");
        MIB_EXPECT(r.generation != beforeRefresh.generation, "revocation bumps the generation");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        MIB_EXPECT(coord.start(req).outcome == ExperimentStartOutcome::NotReady, "NotReady after revocation");
    }

    backend.capture().stop();
    backend.shutdown();
    return mib::test::exitCode();
}
