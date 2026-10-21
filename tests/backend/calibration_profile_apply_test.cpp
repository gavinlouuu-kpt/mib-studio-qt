// calibration_profile_apply_test (#716 A4a fix): the method's calibration declarations reach the
// `calibration.record` gate through the real apply paths, never stale ones.
//
// ProfileStore::apply builds the applied config text from the previous provenance plus the keys
// it applies. The calibration members (purpose, calibration_policy, calibration_max_age_hours,
// calibration_reference) always reflect exactly the method just applied:
//  - declared: copied verbatim (the strict parse accepted them), so `required` blocks Start and a
//    calibration method's run derives its record at close;
//  - not declared: removed, so a measurement method never inherits an earlier method's purpose
//    or policy (absent = measurement, policy none);
//  - applying processing settings only (ProcessingSettingsCommand) keeps the current method's
//    declarations unchanged;
//  - the central method path (applyConfigDocument) applies the method's full text verbatim.
//
// #776 (bridge ABI 43): the readiness report's method calibration (purpose, policy, max age,
// reference) follows each apply: a calibration method reports its own, a method without
// declarations (or no method at all) reports measurement / none with `declared` false, and
// switching between them leaves nothing stale.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/CalibrationRecord.h"
#include "backend/app/CalibrationStore.h"
#include "backend/app/ConfigDocumentApply.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
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

GateStatus gateStatus(const app::ExperimentReadinessSnapshot& r)
{
    const auto* g = r.gate("calibration.record");
    return g ? g->status : GateStatus::Unavailable;
}

bool capture(bridge::BackendFacade& facade, bool start)
{
    bridge::CameraCommand c;
    c.action = start ? bridge::CameraCommandAction::StartCapture : bridge::CameraCommandAction::StopCapture;
    return facade.dispatch(c).ok;
}

bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path)
{
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
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

// The calibration members of the applied config text (null for an absent member).
json declarations(backend::AppBackend& b)
{
    const auto text = b.getLastConfigJson();
    const auto root = json::parse(text.empty() ? "{}" : text);
    json out = json::object();
    for (const auto* key : app::kCalibrationDeclarationKeys) out[key] = root.contains(key) ? root.at(key) : json(nullptr);
    return out;
}

bool noDeclarations(backend::AppBackend& b)
{
    const auto text = b.getLastConfigJson();
    const auto root = json::parse(text.empty() ? "{}" : text);
    for (const auto* key : app::kCalibrationDeclarationKeys)
        if (root.contains(key)) return false;
    return true;
}

// The readiness report's method calibration as JSON (#776), so a failure prints it.
json methodCalibration(const app::ExperimentReadinessSnapshot& r)
{
    const auto& m = r.methodCalibration;
    json ref = nullptr;
    if (m.reference)
        ref = {{"nominal_diameter_um", m.reference->nominalDiameterUm},
               {"diameter_tolerance_um", m.reference->diameterToleranceUm},
               {"nominal_e_kpa", m.reference->nominalEKpa ? json(*m.reference->nominalEKpa) : json(nullptr)},
               {"max_cv_percent", m.reference->maxCvPercent}};
    return {{"declared", m.declared},
            {"purpose", app::toString(m.purpose)},
            {"calibration_policy", app::toString(m.policy)},
            {"calibration_max_age_hours", m.maxAgeHours},
            {"calibration_reference", ref},
            {"error", r.methodCalibrationError}};
}

const json kUndeclared = json::parse(R"({"declared":false,"purpose":"measurement","calibration_policy":"none",
    "calibration_max_age_hours":24.0,"calibration_reference":null,"error":""})");

const char* kGated = R"({"config_schema_version":1,"calibration_policy":"required","calibration_max_age_hours":12})";
const char* kCalibration =
    R"({"config_schema_version":1,"purpose":"calibration","calibration_reference":{"nominal_diameter_um":7.32,"diameter_tolerance_um":0.1,"max_cv_percent":4}})";
const char* kCalibrationWithE =
    R"({"config_schema_version":1,"purpose":"calibration","calibration_max_age_hours":6,"calibration_reference":{"nominal_diameter_um":6,"diameter_tolerance_um":0.2,"nominal_e_kpa":1.5}})";
const char* kMeasurement = R"({"config_schema_version":1,"buffer_threshold":64})";

} // namespace

int main()
{
    mib::test::Watchdog wd(150);
    mib::test::TempDir td("mib_calibration_profile_apply");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 128, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    backend::AppBackend backendApp;
    bridge::BackendFacade facade(backendApp);
    MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
    auto* store = backendApp.calibrationStore();
    MIB_REQUIRE(store && store->error().empty(), "the calibration store exists");
    {
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = 5;
        configure.mockLoopFiles = true;
        MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
    }

    // A real profile directory, written and applied through the facade's profile command.
    const auto base = (td.path() / "profiles").string();
    auto command = [&](const json& request) { return json::parse(facade.profileCommand(base, request.dump())); };
    for (const auto& [name, document] : {std::pair<const char*, const char*>{"gated", kGated},
                                         {"calibration", kCalibration},
                                         {"measurement", kMeasurement}}) {
        const auto created = command({{"operation", "create"}, {"name", name}, {"document_json", document}});
        MIB_REQUIRE(created["ok"] == true && created["saved"] == true, std::string("create ") + name + ": " + created.dump());
    }
    auto applyProfile = [&](const char* name) {
        const auto read = command({{"operation", "read"}, {"name", name}});
        MIB_REQUIRE(read["ok"] == true, read.dump());
        const auto applied =
            command({{"operation", "apply"}, {"name", name}, {"baseline", read["profile"]["revision"]}});
        MIB_REQUIRE(applied["ok"] == true && applied["applied"] == true, std::string("apply ") + name + ": " + applied.dump());
    };
    const auto probe = (td.path() / "probe.h5").string();
    // Profiles apply with capture stopped (configApplyBlocker); the readiness and Start checks
    // need frames. Wait until nothing but the calibration gate could block.
    auto applyThenCapture = [&](const char* name) {
        MIB_REQUIRE(capture(facade, false), "capture stop");
        // ProcessingSettingsCommand starts realtime processing on its own when it is enabled;
        // a capture stop does not end that, and configApplyBlocker refuses while it runs.
        if (backendApp.processing().isRealtimeRunning()) backendApp.processing().stopRealtime();
        applyProfile(name);
        MIB_REQUIRE(capture(facade, true), "capture start");
        MIB_REQUIRE(waitFor(
                        [&] {
                            const auto ids = readiness(facade, probe).blockingGateIds();
                            return ids.empty() || ids == std::vector<std::string>{"calibration.record"};
                        },
                        std::chrono::seconds(15)),
                    std::string("only the calibration gate may block after applying ") + name);
    };

    // ---- no method applied: measurement / none, declared false (#776) ----------------------
    {
        wd.mark("no method");
        MIB_EXPECT(backendApp.getLastConfigJson().empty(), "nothing applied yet");
        const auto r = readiness(facade, probe);
        MIB_EXPECT(methodCalibration(r) == kUndeclared, "no method: " + methodCalibration(r).dump());
    }

    // ---- a measurement method that requires a record: the gate follows its policy ------------
    {
        wd.mark("required");
        applyThenCapture("gated");
        MIB_EXPECT(methodCalibration(readiness(facade, probe)) ==
                       json::parse(R"({"declared":true,"purpose":"measurement","calibration_policy":"required",
                           "calibration_max_age_hours":12.0,"calibration_reference":null,"error":""})"),
                   "#776: the report names the policy: " + methodCalibration(readiness(facade, probe)).dump());
        MIB_EXPECT(declarations(backendApp) == json::parse(R"({"purpose":null,"calibration_policy":"required",
                       "calibration_max_age_hours":12,"calibration_reference":null})"),
                   "the declarations reach the applied text verbatim: " + declarations(backendApp).dump());
        const auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r) == GateStatus::Fail && !r.ready &&
                       r.gate("calibration.record")->remediation == "run the calibration method",
                   "required without a record blocks");
        const auto refused = startRun(facade, (td.path() / "refused.h5").string());
        MIB_EXPECT(!refused.ok && !std::filesystem::exists(td.path() / "refused.h5"), "Start refused: " + refused.message);
    }

    // ---- processing settings keep the current method's declarations ------------------------
    {
        wd.mark("processing settings");
        const auto before = declarations(backendApp);
        MIB_REQUIRE(capture(facade, false), "capture stop");
        bridge::ProcessingSettingsCommand settings;
        settings.flushInterval = 77;
        const auto result = facade.dispatch(settings);
        MIB_REQUIRE(result.ok, "processing settings apply: " + result.message);
        MIB_EXPECT(backendApp.processing().getFlushInterval() == 77 &&
                       json::parse(backendApp.getLastConfigJson()).value("buffer_threshold", 0) == 77,
                   "the settings applied");
        MIB_EXPECT(declarations(backendApp) == before, "the declarations are unchanged: " + declarations(backendApp).dump());
        MIB_REQUIRE(capture(facade, true), "capture start");
        MIB_EXPECT(waitFor([&] { return gateStatus(readiness(facade, probe)) == GateStatus::Fail; }, std::chrono::seconds(5)),
                   "still required");
    }

    // ---- a calibration method: its own declarations, nothing carried over; derives at close --
    {
        wd.mark("calibration method");
        applyThenCapture("calibration");
        MIB_EXPECT(declarations(backendApp) ==
                       json::parse(R"({"purpose":"calibration","calibration_policy":null,"calibration_max_age_hours":null,
                           "calibration_reference":{"nominal_diameter_um":7.32,"diameter_tolerance_um":0.1,"max_cv_percent":4}})"),
                   "the calibration method's members, and no policy carried over from 'gated': " +
                       declarations(backendApp).dump());
        const auto r = readiness(facade, probe);
        MIB_EXPECT(gateStatus(r) == GateStatus::NotRequired && r.ready, "a calibration method needs no applied record");
        MIB_EXPECT(methodCalibration(r) ==
                       json::parse(R"({"declared":true,"purpose":"calibration","calibration_policy":"none",
                           "calibration_max_age_hours":24.0,"error":"","calibration_reference":{"nominal_diameter_um":7.32,
                           "diameter_tolerance_um":0.1,"nominal_e_kpa":null,"max_cv_percent":4.0}})"),
                   "#776: the calibration method's purpose and reference, no policy from 'gated': " +
                       methodCalibration(r).dump());
        const auto path = (td.path() / "calibration_run.h5").string();
        const auto started = startRun(facade, path);
        MIB_REQUIRE(started.ok, "the calibration run starts: " + started.message);
        MIB_REQUIRE(stopRun(facade, backendApp), "the run finalizes");
        const auto list = json::parse(facade.calibrationRecordsListJson());
        MIB_REQUIRE(list["records"].size() == 1, "one record derived: " + list.dump());
        const auto& rec = list["records"][0];
        MIB_EXPECT(rec["source"]["file"] == path && rec["inputs"]["nominal_diameter_um"] == 7.32 &&
                       rec["method"]["source"] == "local",
                   "the record is derived from the applied method's reference: " + rec.dump());
    }

    // ---- a measurement method without declarations: none carried over ----------------------
    {
        wd.mark("measurement method");
        applyThenCapture("measurement");
        MIB_EXPECT(noDeclarations(backendApp),
                   "no purpose, policy, age or reference survives from 'calibration': " + backendApp.getLastConfigJson());
        MIB_EXPECT(json::parse(backendApp.getLastConfigJson()).value("buffer_threshold", 0) == 64,
                   "the measurement profile applied");
        const auto r = readiness(facade, probe);
        const auto* g = r.gate("calibration.record");
        MIB_EXPECT(g && g->status == GateStatus::NotRequired && g->reason == "the method's calibration policy is none" && r.ready,
                   "measurement, policy none: " + (g ? g->reason : std::string()));
        MIB_EXPECT(methodCalibration(r) == kUndeclared,
                   "#776: declared false, nothing stale from 'calibration': " + methodCalibration(r).dump());
        const auto path = (td.path() / "measurement_run.h5").string();
        const auto started = startRun(facade, path);
        MIB_REQUIRE(started.ok, "the measurement run starts: " + started.message);
        MIB_REQUIRE(stopRun(facade, backendApp), "the run finalizes");
        MIB_EXPECT(store->latestRecords().size() == 1, "a measurement run derives no record");
    }

    // ---- 'gated' after 'calibration': the purpose does not carry over ----------------------
    {
        wd.mark("gated after calibration");
        applyThenCapture("calibration");
        applyThenCapture("gated");
        const auto d = declarations(backendApp);
        MIB_EXPECT(d["purpose"].is_null() && d["calibration_reference"].is_null() && d["calibration_policy"] == "required",
                   "the gated method is a measurement method again: " + d.dump());
        MIB_EXPECT(gateStatus(readiness(facade, probe)) == GateStatus::Fail, "and its policy blocks");
        const auto m = methodCalibration(readiness(facade, probe));
        MIB_EXPECT(m["purpose"] == "measurement" && m["calibration_reference"].is_null() &&
                       m["calibration_policy"] == "required" && m["declared"] == true,
                   "#776: no purpose or reference carried over: " + m.dump());
    }

    // ---- the central method path applies the method's full text verbatim -------------------
    {
        wd.mark("central method text");
        MIB_REQUIRE(capture(facade, false), "capture stop");
        if (backendApp.processing().isRealtimeRunning()) backendApp.processing().stopRealtime();
        const auto report = app::applyConfigDocument(backendApp, kMeasurement);
        MIB_REQUIRE(report.ok, "apply: " + report.error);
        MIB_EXPECT(backendApp.getLastConfigJson() == kMeasurement && noDeclarations(backendApp),
                   "the applied text is the method's own: " + backendApp.getLastConfigJson());
        MIB_EXPECT(methodCalibration(readiness(facade, probe)) == kUndeclared,
                   "#776: the central measurement text declares nothing: " + methodCalibration(readiness(facade, probe)).dump());
        const auto calibration = app::applyConfigDocument(backendApp, kCalibrationWithE);
        MIB_REQUIRE(calibration.ok, "apply: " + calibration.error);
        MIB_EXPECT(backendApp.getLastConfigJson() == kCalibrationWithE, "verbatim");
        const auto m = methodCalibration(readiness(facade, probe));
        MIB_EXPECT(m["purpose"] == "calibration" && m["calibration_max_age_hours"] == 6.0 &&
                       m["calibration_reference"] == json::parse(R"({"nominal_diameter_um":6.0,"diameter_tolerance_um":0.2,
                           "nominal_e_kpa":1.5,"max_cv_percent":10.0})"),
                   "#776: the central method's reference, the CV default filled in: " + m.dump());
        const auto malformed = app::applyConfigDocument(backendApp, R"({"purpose":"calibration"})");
        MIB_EXPECT(!malformed.ok && backendApp.getLastConfigJson() == kCalibrationWithE,
                   "a declaration the strict parse refuses applies nothing: " + malformed.error);
        MIB_EXPECT(methodCalibration(readiness(facade, probe)) == m, "#776: and the report is unchanged");
    }

    facade.shutdown();
    return mib::test::exitCode();
}
