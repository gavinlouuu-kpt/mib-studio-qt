// Embedded E0 (#443, epic #441): deterministic no-UI experiment baseline.
//
// Drives the *existing* backend through the frontend-neutral facade with no
// Qt, no display server and no network, exactly the way the standalone
// instrument host (#446) will:
//
//   configure explicit mock source -> readiness (fails closed, then passes)
//   -> Start against the presented readiness generation -> admit at least N
//   frames -> Stop -> await real finalization -> reopen the HDF5 file and
//   verify provenance, accounting conservation and the terminal outcome.
//
// What this proves: the shared runtime authority already supports the
// appliance lifecycle without a UI. What it deliberately does not claim: an
// exact-count admission terminator (there is none in the backend today; the
// run is stopped by the client once >= N frames are admitted, and the
// exact-count gate is #446's follow-up, see the E0 execution plan).
//
// Rules from docs/howto/writing-tests.md: watchdog, no naked waits, conserved
// frame accounting, round-trip verification of what was persisted.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RecordingAccounting.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/wait.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace app = backend::app;
namespace bridge = backend::bridge;
using backend::services::ProcessingService;

constexpr int kFrameW = 96;
constexpr int kFrameH = 96;
constexpr int kFixtureFrames = 8;      // distinct mock images, looped
constexpr uint64_t kAdmitAtLeast = 40; // run length in admitted frames, not wall time
constexpr const char* kProfileId = "e0-smoke-profile"; // invalidation input: preflight and Start must agree

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

app::GateStatus statusOf(const app::ExperimentReadinessSnapshot& r, const std::string& id)
{
    const auto* g = r.gate(id);
    return g ? g->status : app::GateStatus::Unavailable;
}

void dumpGates(const app::ExperimentReadinessSnapshot& r)
{
    std::cerr << "readiness gen=" << r.generation << " ready=" << r.ready << "\n";
    for (const auto& g : r.gates) {
        std::cerr << "  " << g.id << " = " << app::toString(g.status);
        if (!g.reason.empty()) std::cerr << " (" << g.reason << ")";
        std::cerr << "\n";
    }
}

bool dispatchOk(bridge::BackendFacade& facade, const bridge::CameraCommand& c, const char* what)
{
    const auto res = facade.dispatch(c);
    if (!res.ok) std::cerr << what << " failed: " << res.message << "\n";
    return res.ok;
}

} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("headless_experiment_smoke");

    // Offline by construction: the LUT fetch must not touch the network and
    // no hardware discovery may run (MIB_CAMERA_MODE gates startup discovery).
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
    setEnv("MIB_CAMERA_MODE", "mock");

    const auto frames = td / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, kFixtureFrames, kFrameW, kFrameH), "write mock frames");
    const std::string outputPath = (td / "run.h5").string();

    backend::AppBackend backend;
    bridge::BackendFacade facade(backend);
    wd.mark("initialize");
    MIB_REQUIRE(facade.initialize((td / "data").string()), "facade initialize");
    backend.experiment().setApplicationIdentity("e0-smoke", "e0-build", "test-os");

    // Deterministic classification: lenient config, fixed ROI, inline
    // processing, no background (same recipe as experiment_readiness_test).
    auto& proc = backend.processing();
    {
        auto cfg = proc.getProcessingConfig();
        cfg.empty_frame_pixel_threshold = 1;
        cfg.bg_subtract_threshold = 100;
        cfg.enable_border_check = false;
        cfg.enable_area_range_check = false;
        cfg.enable_deformability_range_check = false;
        cfg.enable_ring_ratio_check = false;
        cfg.enable_area_ratio_check = false;
        cfg.require_single_inner_contour = false;
        cfg.auto_background_enabled = false;
        cfg.enable_target_group = false;
        proc.setProcessingConfig(cfg);
    }
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, kFrameW, kFrameH});
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeBackgroundGray(cv::Mat());
    proc.startRealtime(backend.getFrameStore());
    MIB_REQUIRE(proc.isRealtimeRunning(), "realtime processing running");

    // ---- 1. Explicit mock source, no session yet: readiness fails closed ----
    wd.mark("configure");
    {
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = frames.string();
        configure.mockFrameIntervalMs = 2;
        configure.mockLoopFiles = true;
        MIB_REQUIRE(dispatchOk(facade, configure, "configure mock camera"), "configure");

        app::ExperimentReadinessSnapshot r;
        MIB_REQUIRE(facade.fetchExperimentReadiness(r, outputPath), "fetch readiness (idle)");
        dumpGates(r);
        MIB_EXPECT(!r.ready, "not ready without a camera session");
        MIB_EXPECT(statusOf(r, "camera.session") == app::GateStatus::Fail, "camera.session fails when idle");
        MIB_EXPECT(statusOf(r, "camera.source") == app::GateStatus::Warn,
                   "explicit mock is a warning, never a silent fallback");
        MIB_EXPECT(r.candidate.camera.simulated && !r.candidate.camera.fallback,
                   "candidate records explicit mock, not a hardware fallback");

        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = outputPath;
        start.readinessGeneration = r.generation;
        const auto res = facade.dispatch(start);
        MIB_EXPECT(!res.ok, "start refused while not ready");
        MIB_EXPECT(res.experimentStartOutcome.has_value() &&
                       *res.experimentStartOutcome == app::ExperimentStartOutcome::NotReady,
                   "refusal is typed NotReady");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "no file opened on refusal");
    }

    // ---- 2. Session running: readiness passes; stale generation refused ----
    wd.mark("start capture");
    {
        bridge::CameraCommand startCapture;
        startCapture.action = bridge::CameraCommandAction::StartCapture;
        MIB_REQUIRE(dispatchOk(facade, startCapture, "start capture"), "start capture");
    }
    app::ExperimentReadinessSnapshot ready;
    MIB_REQUIRE(mib::test::waitFor([&] {
                    return facade.fetchExperimentReadiness(ready, outputPath, kProfileId) && ready.ready;
                }, 10000),
                "readiness settles once the mock camera streams");
    dumpGates(ready);
    MIB_EXPECT(statusOf(ready, "camera.session") == app::GateStatus::Pass, "session pass");
    MIB_EXPECT(statusOf(ready, "camera.geometry") == app::GateStatus::Pass, "geometry pass");
    MIB_EXPECT(statusOf(ready, "storage.output") == app::GateStatus::Pass, "output writable");
    MIB_EXPECT(ready.candidate.frameWidth == kFrameW && ready.candidate.frameHeight == kFrameH,
               "candidate geometry comes from delivered frames");

    {
        // A stale preflight (generation from before the session) is refused
        // with no side effects: the appliance UI cannot start on old truth.
        bridge::ExperimentCommand stale;
        stale.action = bridge::ExperimentCommandAction::Start;
        stale.outputPath = outputPath;
        stale.profileId = kProfileId;
        stale.readinessGeneration = ready.generation > 0 ? ready.generation - 1 : 0;
        const auto res = facade.dispatch(stale);
        MIB_EXPECT(!res.ok, "stale readiness generation is refused");
        MIB_EXPECT(res.experimentStartOutcome.has_value() &&
                       *res.experimentStartOutcome == app::ExperimentStartOutcome::StaleReadiness,
                   "refusal is typed StaleReadiness");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "stale start opened no file");
        // The refused start re-evaluated; fetch the current generation again.
        MIB_REQUIRE(facade.fetchExperimentReadiness(ready, outputPath, kProfileId) && ready.ready, "re-fetch readiness");
    }

    // ---- 3. Start on the current generation ------------------------------
    wd.mark("start experiment");
    app::RunConfigurationSnapshot frozen;
    {
        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = outputPath;
        start.readinessGeneration = ready.generation;
        start.profileId = kProfileId;
        const auto res = facade.dispatch(start);
        MIB_REQUIRE(res.ok, "start accepted: " + res.message);
        MIB_EXPECT(res.experimentStartOutcome.has_value() &&
                       *res.experimentStartOutcome == app::ExperimentStartOutcome::Started,
                   "outcome Started");
        const auto active = backend.experiment().activeRun();
        MIB_REQUIRE(active.has_value(), "active run snapshot frozen");
        frozen = *active;
        MIB_EXPECT(frozen.profileId == kProfileId, "profile identity frozen into the run");
        MIB_EXPECT(frozen.camera.simulated, "run snapshot records the mock source");

        bridge::ExperimentCommand again = start;
        const auto dup = facade.dispatch(again);
        MIB_EXPECT(!dup.ok, "duplicate start is refused while active");
    }

    // ---- 4. Fixed admitted-frame run length, then Stop --------------------
    wd.mark("admit frames");
    MIB_REQUIRE(mib::test::waitFor([&] {
                    return proc.experimentAccountingSnapshot().admitted >= kAdmitAtLeast;
                }, 30000),
                "at least kAdmitAtLeast frames admitted");
    {
        app::ExperimentStatus s;
        MIB_REQUIRE(facade.fetchExperimentStatus(s), "fetch status while active");
        MIB_EXPECT(s.state == app::ExperimentRunState::Active && !s.terminal, "status Active");
    }

    wd.mark("stop");
    {
        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        const auto res = facade.dispatch(stop);
        MIB_REQUIRE(res.ok, "stop accepted");
        MIB_EXPECT(res.experimentStopOutcome.has_value() &&
                       *res.experimentStopOutcome == app::ExperimentStopOutcome::Accepted,
                   "stop outcome Accepted");
    }
    app::ExperimentStatus terminal;
    MIB_REQUIRE(mib::test::waitFor([&] {
                    return facade.fetchExperimentStatus(terminal) && terminal.terminal;
                }, 30000),
                "finalization reaches a terminal status");
    MIB_EXPECT(terminal.state == app::ExperimentRunState::Idle, "terminal state Idle");
    MIB_EXPECT(terminal.finalizationOk, "every finalize step succeeded");
    MIB_EXPECT(!terminal.cancelled, "not cancelled");
    MIB_EXPECT(terminal.completion == backend::recording::RunCompletionState::Complete,
               std::string("terminal completion is Complete, got ") +
                   backend::recording::toString(terminal.completion) + " (" + terminal.completionReason + ")");
    MIB_EXPECT(!backend.hdf5().isFileOpen(), "file closed after finalization");
    {
        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        MIB_EXPECT(!facade.dispatch(stop).ok, "second stop fails safely");
    }
    const auto liveAccounting = proc.experimentAccountingSnapshot();

    // ---- 5. Reopen: provenance, accounting conservation, terminal outcome ---
    wd.mark("reopen");
    {
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(outputPath), "finalized file reopens");

        std::string runJson, readinessJson;
        MIB_EXPECT(reader.readRunSnapshotJson(runJson, &readinessJson), "run snapshot provenance present");
        MIB_EXPECT(runJson == app::runSnapshotToJson(frozen), "persisted run snapshot equals the frozen one");
        MIB_EXPECT(readinessJson.find("\"camera.session\"") != std::string::npos,
                   "readiness evaluation persisted with the run");

        backend::recording::RecordingAccountingSnapshot persisted;
        MIB_EXPECT(reader.readRunAccounting(persisted), "run accounting present");
        MIB_EXPECT(persisted.completion == backend::recording::RunCompletionState::Complete,
                   std::string("persisted completion Complete, got ") +
                       backend::recording::toString(persisted.completion) + " (" + persisted.completionReason + ")");
        MIB_EXPECT(persisted.reconciled, "persisted accounting reconciled");
        MIB_EXPECT(persisted.admitted >= kAdmitAtLeast, "persisted admitted count covers the requested run length");
        MIB_EXPECT(persisted.admitted == liveAccounting.admitted, "persisted admitted == live admitted");
        MIB_EXPECT(persisted.persistenceCommitted == liveAccounting.persistenceCommitted,
                   "persisted committed == live committed");
        MIB_EXPECT(persisted.persistenceFailed == 0 && persisted.persistencePendingAtStop == 0,
                   "nothing failed or left pending at stop");
        MIB_EXPECT(persisted.storeOverwritten == 0 && persisted.storeNotCommitted == 0,
                   "no undeclared loss");

        uint64_t startNs = 0, endNs = 0;
        size_t totalValid = 0, totalInvalid = 0;
        ProcessingService::Roi roi{};
        MIB_EXPECT(reader.readExperimentInfo(startNs, endNs, totalValid, totalInvalid, &roi), "experiment info present");
        MIB_EXPECT(startNs == frozen.startWallClockNs && endNs >= startNs, "start/end wall clock consistent");
        MIB_EXPECT(roi.w == kFrameW && roi.h == kFrameH, "ROI persisted");

        std::vector<backend::services::ProcessedFrame> valid, invalid;
        MIB_EXPECT(reader.readValidMetadata(valid), "valid metadata readable");
        // The invalid dataset only exists once an invalid frame was written;
        // a run with none reads back as "absent", which is not a failure.
        const bool invalidOk = reader.readInvalidMetadata(invalid);
        MIB_EXPECT(invalidOk || invalid.empty(), "invalid metadata readable or absent");
        const uint64_t persistedFrames = static_cast<uint64_t>(valid.size() + invalid.size());
        std::cerr << "admitted=" << persisted.admitted << " empty=" << persisted.empty
                  << " processed=" << persisted.processed << " rejected=" << persisted.scientificallyRejected
                  << " committed=" << persisted.persistenceCommitted << " valid=" << valid.size()
                  << " invalid=" << invalid.size() << "\n";
        // Conservation: every frame the writer confirmed is readable back,
        // and every admitted frame is accounted for by exactly one outcome.
        MIB_EXPECT(persistedFrames == persisted.persistenceCommitted, "readable frames == committed frames");
        // TD-16: /experiment_info total_valid/invalid_frames are written from the
        // frames still buffered at finalization (ExperimentCoordinator passes the
        // remainder counts to writeExperimentInfo), so they understate any run that
        // had a periodic flush. The accounting_* attributes above are the run truth.
        // Pin the invariant that must hold either way; tighten to equality when
        // TD-16 lands.
        MIB_EXPECT(totalValid + totalInvalid <= persistedFrames,
                   "experiment_info totals never exceed the readable frames (TD-16)");
        MIB_EXPECT(persisted.admitted == persisted.empty + persisted.processed + persisted.processingFailed +
                                             persisted.cancelledByPolicy + persisted.pendingAtStop,
                   "admitted frames are conserved across outcomes");
        MIB_EXPECT(persisted.processed + persisted.scientificallyRejected >= persistedFrames,
                   "persisted frames never exceed classified frames");

        backend::processing::ProcessingCoreIdentity core;
        MIB_EXPECT(reader.readProcessingCoreIdentity(core), "processing core identity persisted");
        MIB_EXPECT(core.version == frozen.processingCore.version, "core version matches the frozen run");
        reader.closeFile();
    }

    // ---- 6. Ordered shutdown with nothing active ---------------------------
    wd.mark("shutdown");
    {
        bridge::CameraCommand stopCapture;
        stopCapture.action = bridge::CameraCommandAction::StopCapture;
        facade.dispatch(stopCapture);
    }
    proc.stopRealtime();
    facade.shutdown();
    return mib::test::exitCode();
}
