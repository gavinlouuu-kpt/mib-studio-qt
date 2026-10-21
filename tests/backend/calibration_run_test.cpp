// calibration_run_test (#716 A4a, bridge ABI 42): the calibration run through the facade with the
// real coordinator, store and mock camera.
//
//  - No actuation: listing, reading and readiness polls never apply, invalidate or append anything.
//  - The `calibration.record` gate follows the method's policy and names the real Fix.
//  - A run of a calibration method derives its record from its own file when it closes (here the
//    mock frames hold no beads: an `insufficient` record, which can never be applied).
//  - calibration_apply: refused during a run, on a stale expected factor and for an insufficient
//    record; applied, it sets pixel-to-micron and replies previous vs new.
//  - Start identity: the snapshot records the applied record in effect; a declared id that differs
//    is refused with InvalidRequest and opens nothing.
//  - A factor changed after the apply takes the record out of effect (re-apply restores it); an ROI
//    identity change invalidates the applied record.
//  - An HDF5 round trip: a synthetic bead file written by Hdf5Service reads back as evidence.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/CalibrationRecord.h"
#include "backend/app/CalibrationStore.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RunProvenanceRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace app = backend::app;
namespace bridge = backend::bridge;
using json = nlohmann::json;
using app::GateStatus;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

app::ExperimentReadinessSnapshot readiness(bridge::BackendFacade& facade, const std::string& path)
{
    app::ExperimentReadinessSnapshot r;
    facade.fetchExperimentReadiness(r, path);
    return r;
}

GateStatus gateStatus(const app::ExperimentReadinessSnapshot& r, const char* id)
{
    const auto* g = r.gate(id);
    return g ? g->status : GateStatus::Unavailable;
}

bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path,
                                      const std::string& startRequest = {})
{
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    start.startRequestJson = startRequest;
    start.readinessGeneration = readiness(facade, path).generation;
    return facade.dispatch(start);
}

bool stopRun(bridge::BackendFacade& facade, backend::AppBackend& b)
{
    bridge::ExperimentCommand stop;
    stop.action = bridge::ExperimentCommandAction::Stop;
    if (!facade.dispatch(stop).ok) return false;
    return waitFor([&] { return b.experiment().status().terminal && b.experiment().state() == app::ExperimentRunState::Idle; },
                   std::chrono::seconds(20));
}

const char* kCalibrationMethod =
    R"({"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7.32,"diameter_tolerance_um":0.1}})";

// A proposed record of 300 synthetic beads of 15 px, derived under `identity`.
app::CalibrationRecord proposedRecord(const std::string& id, const app::CalibrationIdentity& identity, uint64_t nowMs)
{
    app::CalibrationEvidence e;
    for (int i = 0; i < 300; ++i) {
        app::CalibrationSample s;
        s.frameIndex = static_cast<uint64_t>(i);
        const double d = 15.0 + 0.3 * std::sin(static_cast<double>(i));
        s.areaPx = 3.14159265358979323846 * d * d / 4.0;
        e.objects.push_back(s);
    }
    app::CalibrationRecord r;
    r.recordId = id;
    r.createdAtUnixMs = r.statusAtUnixMs = nowMs;
    r.file = "/beads/" + id + ".h5";
    r.methodSource = "local";
    r.reference.nominalDiameterUm = 7.32;
    r.reference.diameterToleranceUm = 0.1;
    r.identity = identity;
    r.derivation = app::deriveCalibration(e, r.reference, 0.4886);
    r.status = app::CalibrationStatus::Proposed;
    return r;
}

app::CalibrationIdentity identityFromList(const json& list)
{
    app::CalibrationIdentity id;
    const auto& c = list["current_identity"];
    if (c["camera"].is_string()) id.camera = c["camera"].get<std::string>();
    if (c["roi"].is_string()) id.roi = c["roi"].get<std::string>();
    id.chip = c["chip"].get<std::string>();
    id.objective = c["objective"].get<std::string>();
    return id;
}

} // namespace

int main()
{
    mib::test::Watchdog wd(150);
    mib::test::TempDir td("mib_calibration_run");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 128, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    // ---- an HDF5 round trip: synthetic beads written by the run writer read back as evidence ----
    {
        wd.mark("hdf5 round trip");
        const auto path = (td.path() / "beads.h5").string();
        backend::services::Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path) && writer.initializeDatasets(), "open a synthetic bead file");
        std::vector<backend::services::ProcessedFrame> valid;
        for (int i = 0; i < 250; ++i) {
            backend::services::ProcessedFrame f;
            f.index = static_cast<uint64_t>(i);
            f.timestampNs = static_cast<uint64_t>(i) * 1000;
            f.originalImage = cv::Mat(8, 8, CV_8UC1, cv::Scalar(0));
            f.processedImage = cv::Mat(8, 8, CV_8UC1, cv::Scalar(0));
            f.validation.isValid = true;
            f.validation.area = 3.14159265358979323846 * 16.0 * 16.0 / 4.0; // 16 px beads
            f.validation.deformability = 0.02;
            f.validation.laplacianVariance = 42.0;
            valid.push_back(f);
        }
        MIB_REQUIRE(writer.appendFrames(valid, {}), "append bead rows");
        writer.closeFile();
        app::CalibrationEvidence evidence;
        std::string why;
        MIB_REQUIRE(app::readCalibrationEvidence(path, evidence, &why), "read evidence: " + why);
        MIB_EXPECT(evidence.objects.size() == 250 && evidence.focusFieldRecorded && evidence.eFieldRecorded,
                   "every valid row, and the columns the file records");
        MIB_EXPECT(evidence.ticksPerSecond == 0, "no acquisition provenance: no declared clock, so no event rate");
        app::CalibrationReference ref;
        ref.nominalDiameterUm = 8.0;
        ref.diameterToleranceUm = 0.1;
        const auto d = app::deriveCalibration(evidence, ref, 0.5);
        MIB_EXPECT(d.sufficient() && d.results.medianDiameterPx && std::fabs(*d.results.medianDiameterPx - 16.0) < 1e-9 &&
                       std::fabs(*d.results.proposedPixelToMicron - 0.5) < 1e-12 &&
                       d.results.focusStatus == "recorded" && d.results.eventRateStatus == "not recorded",
                   "16 px beads of 8 um propose 0.5 um/px");
        MIB_EXPECT(!app::readCalibrationEvidence((td.path() / "missing.h5").string(), evidence, &why),
                   "a missing file is not evidence");
    }

    backend::AppBackend backendApp;
    bridge::BackendFacade facade(backendApp);
    MIB_EXPECT(json::parse(facade.calibrationRecordsListJson())["valid"] == false, "nothing before initialize");
    MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
    auto& proc = backendApp.processing();
    auto* store = backendApp.calibrationStore();
    MIB_REQUIRE(store && store->error().empty(), "the calibration store exists");
    {
        // The method document's calibration members are validated with the rest of config.json: a
        // malformed declaration refuses the whole document (nothing is applied).
        auto stageError = [&](const std::string& text) {
            try {
                (void)app::stageConfigDocument(backendApp, text);
                return std::string();
            } catch (const std::exception& e) {
                return std::string(e.what());
            }
        };
        MIB_EXPECT(stageError(kCalibrationMethod).empty(), "a calibration method stages");
        MIB_EXPECT(stageError(R"({"calibration_policy":"required","calibration_max_age_hours":12})").empty(),
                   "a measurement method with a policy stages");
        MIB_EXPECT(stageError(R"({"purpose":"calibration"})").find("calibration_reference") != std::string::npos,
                   "a calibration method without its reference is refused");
        MIB_EXPECT(stageError(R"({"calibration_policy":"sometimes"})").find("calibration_policy") != std::string::npos,
                   "an unknown policy is refused");
    }
    {
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = 5;
        configure.mockLoopFiles = true;
        MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
        bridge::CameraCommand startCapture;
        startCapture.action = bridge::CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
    }
    const auto probe = (td.path() / "probe.h5").string();
    MIB_REQUIRE(waitFor([&] { return readiness(facade, probe).ready; }, std::chrono::seconds(15)),
                "ready with a running mock camera");
    const double factor0 = proc.getPixelToMicronFactor();

    // ---- no actuation: listing, reading and polling change nothing -------------------------
    {
        wd.mark("no actuation");
        const auto list = json::parse(facade.calibrationRecordsListJson());
        MIB_EXPECT(list["valid"] == true && list["records"].empty() && list["applied_record_id"].is_null() &&
                       list["in_effect_record_id"].is_null() && list["current_identity"]["known"] == true &&
                       list["current_identity"]["chip"] == "not tracked" && list["store"]["error"].is_null(),
                   list.dump());
        const auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::NotRequired && !r.calibrationRecordId,
                   "a legacy method: calibration.record not required, nothing in effect");
        const auto rev = store->revision();
        for (int i = 0; i < 10; ++i) {
            (void)facade.calibrationRecordsListJson();
            (void)facade.calibrationRecordGetJson("cal-none");
            (void)readiness(facade, probe);
        }
        MIB_EXPECT(store->revision() == rev && proc.getPixelToMicronFactor() == factor0 && !store->applied(),
                   "navigation and listing never apply anything");
        const auto missing = json::parse(facade.calibrationRecordGetJson("cal-none"));
        MIB_EXPECT(missing["found"] == false && missing["record"].is_null(), missing.dump());
    }

    // ---- the gate follows the method's policy ----------------------------------------------
    {
        wd.mark("policy");
        backendApp.setLastConfigJson(R"({"calibration_policy":"required"})");
        auto r = readiness(facade, probe);
        const auto* g = r.gate("calibration.record");
        MIB_REQUIRE(g, "the gate is in the report");
        MIB_EXPECT(g->status == GateStatus::Fail && !r.ready && g->remediation == "run the calibration method",
                   "required without a record blocks; Fix: " + g->remediation);
        const auto refused = startRun(facade, (td.path() / "refused.h5").string());
        MIB_EXPECT(!refused.ok && !std::filesystem::exists(td.path() / "refused.h5"), "Start refused: " + refused.message);
        backendApp.setLastConfigJson(R"({"calibration_policy":"warn"})");
        r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::Warn && r.ready, "warn does not block");
    }

    // ---- a run of a calibration method derives its record ----------------------------------
    std::string derivedId;
    {
        wd.mark("calibration run");
        backendApp.setLastConfigJson(kCalibrationMethod);
        const auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::NotRequired && r.ready,
                   "a calibration method needs no applied record");
        const auto path = (td.path() / "calibration_run.h5").string();
        const auto started = startRun(facade, path);
        MIB_REQUIRE(started.ok, "the calibration run starts: " + started.message);
        const auto during = json::parse(facade.applyCalibrationJson("cal-anything", factor0));
        MIB_EXPECT(during["ok"] == false && during["outcome"] == static_cast<int>(app::CalibrationApplyOutcome::RunActive) &&
                       proc.getPixelToMicronFactor() == factor0,
                   "apply is refused while a run is active: " + during.dump());
        const auto run = backendApp.experiment().activeRun();
        MIB_REQUIRE(run && run->v3.runId, "the run has an id");
        MIB_REQUIRE(stopRun(facade, backendApp), "the run finalizes");
        const auto list = json::parse(facade.calibrationRecordsListJson());
        MIB_REQUIRE(list["records"].size() == 1, "one record derived: " + list.dump());
        const auto& rec = list["records"][0];
        derivedId = rec["record_id"].get<std::string>();
        MIB_EXPECT(rec["source"]["file"] == path && rec["source"]["run_id"] == *run->v3.runId &&
                       rec["source"]["file_sha256"].is_string() && rec["inputs"]["nominal_diameter_um"] == 7.32 &&
                       rec["method"]["source"] == "local",
                   "the record names its source run, file, hash and inputs: " + rec.dump());
        MIB_EXPECT(rec["status"] == "insufficient" && rec["applicable"] == false && rec["results"]["valid_objects"] == 0 &&
                       rec["results"]["focus"]["status"] == "not recorded",
                   "no beads in the mock frames: insufficient, nothing fabricated: " + rec.dump());
        const auto apply = json::parse(facade.applyCalibrationJson(derivedId, factor0));
        MIB_EXPECT(apply["outcome"] == static_cast<int>(app::CalibrationApplyOutcome::Insufficient) &&
                       proc.getPixelToMicronFactor() == factor0,
                   "an insufficient record can never be applied: " + apply.dump());
        const auto got = json::parse(facade.calibrationRecordGetJson(derivedId));
        MIB_EXPECT(got["found"] == true && got["history"].size() == 1 && got["record"]["record_id"] == derivedId,
                   got.dump());
    }

    // ---- apply: compare-and-set, previous vs new -------------------------------------------
    const std::string id = "cal-test-0001";
    double applied = 0.0;
    {
        wd.mark("apply");
        backendApp.setLastConfigJson(R"({"calibration_policy":"required"})");
        const auto identity = identityFromList(json::parse(facade.calibrationRecordsListJson()));
        MIB_REQUIRE(identity.known(), "the current identity is known");
        std::string why;
        MIB_REQUIRE(store->addDerived(proposedRecord(id, identity, app::WallClock::nowNs() / 1000000), &why),
                    "a proposed record: " + why);
        auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::Fail &&
                       r.gate("calibration.record")->remediation.find("apply proposed record " + id) == 0,
                   "the Fix names the proposed record");
        const auto stale = json::parse(facade.applyCalibrationJson(id, factor0 * 1.1));
        MIB_EXPECT(stale["outcome"] == static_cast<int>(app::CalibrationApplyOutcome::StaleFactor) &&
                       proc.getPixelToMicronFactor() == factor0 && !store->applied(),
                   "a stale expected factor applies nothing: " + stale.dump());
        const auto ok = json::parse(facade.applyCalibrationJson(id, factor0));
        MIB_REQUIRE(ok["ok"] == true, "applied: " + ok.dump());
        applied = ok["new_factor"].get<double>();
        MIB_EXPECT(ok["previous_factor"] == factor0 && std::fabs(applied - 7.32 / 15.0) < 0.02 &&
                       proc.getPixelToMicronFactor() == applied && ok["record"]["status"] == "applied" &&
                       ok["superseded_record_id"].is_null(),
                   "previous vs new; the factor is set: " + ok.dump());
        r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::Pass && r.calibrationRecordId == id && r.ready,
                   "the applied record is in effect");
        const auto list = json::parse(facade.calibrationRecordsListJson());
        MIB_EXPECT(list["applied_record_id"] == id && list["in_effect_record_id"] == id, list.dump());
    }

    // ---- Start identity --------------------------------------------------------------------
    {
        wd.mark("start identity");
        const auto mismatch = startRun(facade, (td.path() / "mismatch.h5").string(),
                                       R"({"calibration_record_id":"cal-other"})");
        MIB_EXPECT(!mismatch.ok && mismatch.message.find("mismatch") != std::string::npos &&
                       !std::filesystem::exists(td.path() / "mismatch.h5"),
                   "a declared id that differs is refused and opens nothing: " + mismatch.message);
        const auto matchPath = (td.path() / "declared.h5").string();
        const auto match = startRun(facade, matchPath, std::string(R"({"calibration_record_id":")") + id + "\"}");
        MIB_REQUIRE(match.ok, "the applied id may be declared: " + match.message);
        MIB_REQUIRE(stopRun(facade, backendApp), "finalizes");
        const auto plainPath = (td.path() / "undeclared.h5").string();
        const auto plain = startRun(facade, plainPath);
        MIB_REQUIRE(plain.ok, "undeclared is fine: " + plain.message);
        MIB_REQUIRE(stopRun(facade, backendApp), "finalizes");
        for (const auto& path : {matchPath, plainPath}) {
            backend::services::Hdf5Service file;
            backend::recording::RecordedRunProvenance recorded;
            MIB_REQUIRE(file.loadFile(path) && backend::recording::readRecordedRunProvenance(file, recorded), "read " + path);
            file.closeFile();
            MIB_EXPECT(recorded.v3.identity.calibrationRecordId == std::optional<std::string>(id),
                       "the snapshot records the backend's applied record: " + path);
        }
        MIB_EXPECT(store->latestRecords().size() == 2, "measurement runs derive no record");
    }

    // ---- a factor changed after the apply; an identity change ------------------------------
    {
        wd.mark("factor and identity");
        proc.setPixelToMicronFactor(0.6);
        auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r, "calibration.record") == GateStatus::Fail && !r.calibrationRecordId &&
                       r.gate("calibration.record")->remediation.find("re-apply record " + id) == 0,
                   "a changed factor takes the record out of effect");
        const auto declared = startRun(facade, (td.path() / "stale_declared.h5").string(),
                                       std::string(R"({"calibration_record_id":")") + id + "\"}");
        MIB_EXPECT(!declared.ok, "a record not in effect cannot be declared");
        const auto again = json::parse(facade.applyCalibrationJson(id, 0.6));
        MIB_EXPECT(again["ok"] == true && again["previous_factor"] == 0.6 && again["new_factor"] == applied &&
                       proc.getPixelToMicronFactor() == applied,
                   "an explicit re-apply restores it: " + again.dump());
        MIB_EXPECT(readiness(facade, probe).calibrationRecordId == id, "in effect again");

        proc.setRealtimeRoi(backend::services::ProcessingService::Roi{0, 0, 64, 64});
        MIB_REQUIRE(waitFor([&] { return !proc.realtimeRoiPending(); }, std::chrono::seconds(5)), "the ROI applies");
        r = readiness(facade, probe);
        const auto* g = r.gate("calibration.record");
        MIB_EXPECT(g && g->status == GateStatus::Fail && !r.calibrationRecordId &&
                       g->reason.find("invalidated") != std::string::npos,
                   "an ROI change invalidates the applied record: " + (g ? g->reason : std::string()));
        const auto list = json::parse(facade.calibrationRecordsListJson());
        MIB_EXPECT(list["applied_record_id"].is_null(), list.dump());
        const auto got = json::parse(facade.calibrationRecordGetJson(id));
        MIB_EXPECT(got["record"]["status"] == "invalidated" &&
                       got["record"]["status_reason"].get<std::string>().find("ROI") != std::string::npos,
                   got.dump());
        const auto rev = store->revision();
        (void)readiness(facade, probe);
        MIB_EXPECT(store->revision() == rev, "an invalidation is appended once");
        const auto reapply = json::parse(facade.applyCalibrationJson(id, proc.getPixelToMicronFactor()));
        MIB_EXPECT(reapply["ok"] == false, "an invalidated record is not applied again");
    }

    facade.shutdown();
    return mib::test::exitCode();
}
