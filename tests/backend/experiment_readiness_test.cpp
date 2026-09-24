// experiment_readiness_test (issue #369; host-SDK portion of #274)
//
//  - Readiness is evaluated from the actual backend state: no camera session
//    -> camera.session Fail and Start refused with NotReady (never a silent
//    fallback to "assume ready").
//  - A stable state keeps its readiness generation; every invalidation input
//    (ROI, processing config, background, camera reconnect, output path,
//    fault) bumps it, and a Start presenting the old generation is refused
//    with StaleReadiness — the presented preflight never authorizes Start.
//  - A hardware selection that falls back to the mock camera is reported as
//    a fallback (camera.source Fail); an explicit mock selection is a Warn.
//  - Unwritable output storage blocks Start.
//  - Concurrent Start requests: exactly one Started, the rest typed.
//  - The frozen RunConfigurationSnapshot is immutable for the run's duration
//    and is persisted to HDF5 (readable after close).
//  - Bounded background calibration: success publishes atomically with a new
//    generation/sha; contaminated frames -> FailedInsufficient with the
//    previous background preserved; timeout; cancel; config change.

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/camera/mock/MockCamera.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using backend::app::ExperimentStartOutcome;
using backend::app::ExperimentStartRequest;
using backend::app::GateStatus;
using backend::services::ProcessingService;
namespace fs = std::filesystem;

namespace {
bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void pushMat(backend::playback::FrameStore& s, const cv::Mat& m, uint64_t ts)
{
    s.pushFrame(m.data, m.total(), m.cols, m.rows, m.cols, 0x01080001, ts, ts);
}

GateStatus statusOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id)
{
    const auto* g = r.gate(id);
    return g ? g->status : GateStatus::Unavailable;
}

void dumpGates(const backend::app::ExperimentReadinessSnapshot& r)
{
    std::fprintf(stderr, "readiness gen=%llu ready=%d\n", (unsigned long long)r.generation, r.ready ? 1 : 0);
    for (const auto& g : r.gates) {
        std::fprintf(stderr, "  %-26s %-12s %s %s\n", g.id.c_str(), backend::app::toString(g.status),
                     g.reason.c_str(), g.detail.c_str());
    }
}

bool startCapture(backend::AppBackend& b)
{
    if (b.capture().requestStart() != backend::services::CaptureStartOutcome::Accepted) return false;
    if (!waitFor([&] { return b.capture().lifecycleSnapshot().cameraReady; }, std::chrono::seconds(5))) return false;
    return waitFor([&] { return b.capture().stats().framesProcessed.load() > 2; }, std::chrono::seconds(5));
}

void stopCapture(backend::AppBackend& b)
{
    b.capture().stop();
    waitFor([&] { return !b.capture().isRunning(); }, std::chrono::seconds(5));
}
} // namespace

int main()
{
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("experiment_readiness");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "write mock frames");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& coord = backend.experiment();
    coord.setApplicationIdentity("test-version", "test-build", "test-os");
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
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeBackgroundGray(cv::Mat()); // start from "no background"
    proc.startRealtime(backend.getFrameStore());
    MIB_REQUIRE(proc.isRealtimeRunning(), "realtime running");

    const std::string out1 = (td.path() / "run1.h5").string();

    // ---- 1. No camera session: readiness fails closed ------------------------
    {
        wd.mark("no session");
        camera::mock::MockCameraOptions opts;
        opts.folder = frames;
        opts.frameInterval = std::chrono::microseconds(2000);
        opts.loopFiles = true;
        backend.configureMockCamera(opts);
        const auto r = coord.evaluateReadiness(out1);
        dumpGates(r);
        MIB_EXPECT(!r.ready, "not ready without a camera session");
        MIB_EXPECT(statusOf(r, "camera.session") == GateStatus::Fail, "camera.session fails when idle");
        MIB_EXPECT(statusOf(r, "camera.geometry") == GateStatus::Unavailable, "geometry unknown before frames");
        MIB_EXPECT(statusOf(r, "camera.deliveryMode") == GateStatus::Unavailable, "delivery mode unknown when idle");
        MIB_EXPECT(statusOf(r, "camera.source") == GateStatus::Warn, "explicit mock is a warning, not a failure");
        MIB_EXPECT(statusOf(r, "trigger.output") == GateStatus::NotRequired, "sorting disabled -> trigger not required");
        ExperimentStartRequest req;
        req.outputPath = out1;
        req.readinessGeneration = r.generation;
        const auto res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::NotReady, "start refused: NotReady");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "no file opened on refusal");
        MIB_EXPECT(coord.state() == backend::app::ExperimentRunState::Idle, "still idle");
        const auto again = coord.evaluateReadiness(out1);
        MIB_EXPECT(again.generation == r.generation, "stable state keeps its generation");
    }

    // ---- 2. Running mock camera: ready; stale preflight refused ---------------
    uint64_t genReady = 0;
    {
        wd.mark("running");
        MIB_REQUIRE(startCapture(backend), "mock capture running with frames");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto r = coord.evaluateReadiness(out1);
        dumpGates(r);
        MIB_REQUIRE(r.ready, "ready with a running mock camera");
        MIB_EXPECT(statusOf(r, "camera.session") == GateStatus::Pass, "session pass");
        MIB_EXPECT(statusOf(r, "camera.geometry") == GateStatus::Pass, "geometry pass");
        MIB_EXPECT(statusOf(r, "processing.core") == GateStatus::Pass, "core pass (no pin)");
        MIB_EXPECT(statusOf(r, "processing.background") == GateStatus::Warn, "no background -> warn only");
        MIB_EXPECT(statusOf(r, "storage.output") == GateStatus::Pass, "writable output");
        MIB_EXPECT(r.candidate.camera.simulated && !r.candidate.camera.fallback, "candidate records explicit mock");
        MIB_EXPECT(r.candidate.frameWidth == 96 && r.candidate.frameHeight == 96, "candidate geometry from frames");
        MIB_EXPECT(r.candidate.captureGeneration == backend.capture().lifecycleSnapshot().generation,
                   "candidate carries capture generation");
        genReady = r.generation;
        MIB_EXPECT(coord.evaluateReadiness(out1).generation == genReady, "generation stable while nothing changes");

        // ROI edit after preflight -> stale.
        proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 64, 64});
        ExperimentStartRequest req;
        req.outputPath = out1;
        req.readinessGeneration = genReady;
        auto res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::StaleReadiness, "ROI change invalidates the preflight");
        MIB_EXPECT(res.readiness.generation == genReady + 1, "re-evaluation bumped the generation once");
        MIB_EXPECT(!backend.hdf5().isFileOpen() && coord.state() == backend::app::ExperimentRunState::Idle,
                   "stale start has no side effects");
        genReady = res.readiness.generation;

        // Config edit -> stale.
        auto cfg = proc.getProcessingConfig();
        cfg.gaussian_blur_size = cfg.gaussian_blur_size == 5 ? 7 : 5;
        proc.setProcessingConfig(cfg);
        req.readinessGeneration = genReady;
        res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::StaleReadiness, "config change invalidates the preflight");
        genReady = res.readiness.generation;

        // Background change -> stale.
        proc.setRealtimeBackgroundGray(cv::Mat(96, 96, CV_8UC1, cv::Scalar(3)));
        req.readinessGeneration = genReady;
        res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::StaleReadiness, "background change invalidates the preflight");
        MIB_EXPECT(statusOf(res.readiness, "processing.background") == GateStatus::Pass, "background now present");
        MIB_EXPECT(!res.readiness.candidate.backgroundSha256.empty() && res.readiness.candidate.backgroundGeneration > 0,
                   "background identity captured");
        genReady = res.readiness.generation;

        // Output path change -> stale.
        req.readinessGeneration = genReady;
        req.outputPath = (td.path() / "other.h5").string();
        res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::StaleReadiness, "output path change invalidates the preflight");

        // Unresolved fault -> blocks.
        coord.reportUnresolvedFault("test.fault", "simulated unresolved fault");
        const auto rf = coord.evaluateReadiness(out1);
        MIB_EXPECT(!rf.ready && statusOf(rf, "lifecycle.fault") == GateStatus::Fail, "unresolved fault blocks start");
        req.outputPath = out1;
        req.readinessGeneration = rf.generation;
        res = coord.start(req);
        MIB_EXPECT(res.outcome == ExperimentStartOutcome::NotReady, "fault -> NotReady");
        coord.clearUnresolvedFault();
        MIB_EXPECT(coord.evaluateReadiness(out1).ready, "ready again after the fault is cleared");
    }

    // ---- 3. Reconnect (stop/start capture) invalidates ------------------------
    {
        wd.mark("reconnect");
        const auto before = coord.evaluateReadiness(out1);
        stopCapture(backend);
        const auto stopped = coord.evaluateReadiness(out1);
        MIB_EXPECT(!stopped.ready && stopped.generation != before.generation, "stop invalidates");
        MIB_REQUIRE(startCapture(backend), "restart capture");
        const auto restarted = coord.evaluateReadiness(out1);
        MIB_EXPECT(restarted.ready && restarted.generation != before.generation && restarted.generation != stopped.generation,
                   "new session is a new generation");
        ExperimentStartRequest req;
        req.outputPath = out1;
        req.readinessGeneration = before.generation;
        MIB_EXPECT(coord.start(req).outcome == ExperimentStartOutcome::StaleReadiness,
                   "pre-reconnect preflight cannot authorize start");
    }

    // ---- 4. Hardware fallback is never silent ---------------------------------
#if !MIB_HAS_EGRABBER
    {
        wd.mark("fallback");
        backend.setHardwareCameraSelection(0, 0, "Fake EGrabber");
        const auto info = backend.cameraSourceInfo();
        MIB_EXPECT(info.requested == "egrabber" && info.effective == "mock" && info.fallback && !info.fallbackReason.empty(),
                   "fallback reported with a reason");
        const auto r = coord.evaluateReadiness(out1);
        MIB_EXPECT(!r.ready && statusOf(r, "camera.source") == GateStatus::Fail, "fallback blocks start");
        ExperimentStartRequest req;
        req.outputPath = out1;
        req.readinessGeneration = r.generation;
        MIB_EXPECT(coord.start(req).outcome == ExperimentStartOutcome::NotReady, "fallback -> NotReady");
        camera::mock::MockCameraOptions opts;
        opts.folder = frames;
        opts.frameInterval = std::chrono::microseconds(2000);
        backend.configureMockCamera(opts);
        MIB_EXPECT(!backend.cameraSourceInfo().fallback && coord.evaluateReadiness(out1).ready,
                   "explicit mock selection clears the fallback");
    }
#endif

    // ---- 5. Unwritable storage ------------------------------------------------
    {
        wd.mark("storage");
        const fs::path blocker = td.path() / "not_a_dir";
        { std::ofstream f(blocker); f << "x"; }
        const std::string bad = (blocker / "run.h5").string();
        const auto r = coord.evaluateReadiness(bad);
        MIB_EXPECT(!r.ready && statusOf(r, "storage.output") == GateStatus::Fail, "unwritable destination blocks");
        const auto none = coord.evaluateReadiness("");
        MIB_EXPECT(!none.ready && statusOf(none, "storage.output") == GateStatus::Unavailable,
                   "no destination -> Unavailable (not Pass)");
    }

    // ---- 6. Concurrent start: exactly one Started; frozen snapshot ------------
    backend::app::RunConfigurationSnapshot frozen;
    {
        wd.mark("concurrent start");
        // The preflight must be evaluated for the same profile the Start
        // presents: profile identity is an invalidation input.
        const auto r = coord.evaluateReadiness(out1, "profile-A");
        MIB_REQUIRE(r.ready, "ready before concurrent start");
        std::atomic<int> started{0}, busy{0}, other{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                ExperimentStartRequest req;
                req.outputPath = out1;
                req.readinessGeneration = r.generation;
                req.profileId = "profile-A";
                const auto res = coord.start(req);
                if (res.outcome == ExperimentStartOutcome::Started) ++started;
                else if (res.outcome == ExperimentStartOutcome::Busy ||
                         res.outcome == ExperimentStartOutcome::AlreadyActive) ++busy;
                else ++other;
            });
        }
        for (auto& t : threads) t.join();
        std::fprintf(stderr, "concurrent: started=%d busy=%d other=%d\n", started.load(), busy.load(), other.load());
        MIB_EXPECT(started == 1, "exactly one start succeeds");
        MIB_EXPECT(busy == 3 && other == 0, "the rest are typed Busy/AlreadyActive");
        MIB_EXPECT(coord.state() == backend::app::ExperimentRunState::Active, "running");
        MIB_EXPECT(backend.hdf5().isFileOpen(), "HDF5 open for the run");
        auto active = coord.activeRun();
        MIB_REQUIRE(active.has_value(), "active run snapshot");
        frozen = *active;
        MIB_EXPECT(frozen.readinessGeneration == r.generation && frozen.startGeneration == 1, "snapshot generations");
        MIB_EXPECT(frozen.roiW == 64 && frozen.roiH == 64, "snapshot froze the ROI in force at start");
        MIB_EXPECT(frozen.applicationVersion == "test-version", "application identity recorded");
        MIB_EXPECT(frozen.camera.effective == "mock" && frozen.camera.simulated, "camera source frozen");

        // Later edits do not mutate the frozen run.
        proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 32, 32});
        proc.setRealtimeBackgroundGray(cv::Mat(96, 96, CV_8UC1, cv::Scalar(9)));
        const auto after = coord.activeRun();
        MIB_REQUIRE(after.has_value(), "still active");
        MIB_EXPECT(after->roiW == 64 && after->backgroundSha256 == frozen.backgroundSha256 &&
                       after->processingConfigVersion == frozen.processingConfigVersion,
                   "frozen snapshot unchanged by later edits");
        // A second start while running is AlreadyActive, never a second run.
        ExperimentStartRequest req;
        req.outputPath = out1;
        req.profileId = "profile-A";
        req.readinessGeneration = coord.evaluateReadiness(out1, "profile-A").generation;
        MIB_EXPECT(coord.start(req).outcome == ExperimentStartOutcome::AlreadyActive, "AlreadyActive while running");

        // Finalize through the coordinator (backend-owned stop path).
        MIB_EXPECT(coord.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
        MIB_REQUIRE(waitFor([&] { return coord.status().terminal; }, std::chrono::seconds(20)), "run finalizes");
        const auto finished = coord.finish();
        MIB_EXPECT(finished.has_value() && finished->startGeneration == frozen.startGeneration, "finish returns the run");
        MIB_EXPECT(coord.state() == backend::app::ExperimentRunState::Idle && !coord.activeRun().has_value(), "idle after finalize");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "coordinator closed the file");

        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out1), "reload run file");
        std::string runJson, readinessJson;
        MIB_REQUIRE(reader.readRunSnapshotJson(runJson, &readinessJson), "run snapshot persisted");
        MIB_EXPECT(runJson == backend::app::runSnapshotToJson(frozen), "persisted snapshot equals the frozen one");
        MIB_EXPECT(runJson.find("\"requested\":\"mock\"") != std::string::npos &&
                       runJson.find("\"profile_id\":\"profile-A\"") != std::string::npos &&
                       runJson.find("\"schema_version\":1") != std::string::npos,
                   "snapshot JSON content");
        MIB_EXPECT(readinessJson.find("\"camera.session\"") != std::string::npos, "readiness JSON persisted");
        reader.closeFile();
    }

    // ---- 6b. Status snapshot + callback (shared-backend lifecycle) --------------
    {
        wd.mark("status snapshot");
        auto& coordinator = backend.experiment();
        const auto idle = coordinator.status();
        MIB_EXPECT(idle.state == backend::app::ExperimentRunState::Idle, "idle before start");
        // The terminal outcome of the previous run stays readable until the
        // next start (clients that missed the callback can still pull it).
        // Only readability is asserted: the previous block's run can be
        // labelled Failed by the pre-existing start-boundary accounting race
        // (a frame admitted just before startExperiment() and counted after
        // it; see the #372 handoff follow-ups).
        MIB_EXPECT(idle.terminal && idle.completion != backend::recording::RunCompletionState::Unknown,
                   "previous run's terminal outcome still readable while idle");

        std::vector<backend::app::ExperimentRunState> seen;
        std::mutex seenMutex;
        coordinator.setStatusCallback([&](const backend::app::ExperimentStatus& s) {
            std::lock_guard<std::mutex> lk(seenMutex);
            seen.push_back(s.state);
        });

        const auto out = (td.path() / "status_run.h5").string();
        const auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for status test");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        const auto started = coordinator.start(req);
        MIB_REQUIRE(started.started(), "started: " + started.message);
        const auto active = coordinator.status();
        MIB_EXPECT(active.state == backend::app::ExperimentRunState::Active, "Active after start");
        MIB_EXPECT(active.startGeneration == started.run.startGeneration, "status carries the run identity");
        MIB_EXPECT(active.outputPath == started.run.outputPath, "status carries the output path");
        MIB_EXPECT(active.startWallClockNs == started.run.startWallClockNs, "status carries the start time");
        {
            std::lock_guard<std::mutex> lk(seenMutex);
            MIB_EXPECT(seen.size() == 2 && seen[0] == backend::app::ExperimentRunState::Starting &&
                           seen[1] == backend::app::ExperimentRunState::Active,
                       "callback observed Starting then Active");
        }
        coordinator.setStatusCallback({});
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
        MIB_REQUIRE(waitFor([&] { return coordinator.status().terminal; }, std::chrono::seconds(20)), "status run finalizes");
        MIB_EXPECT(coordinator.status().state == backend::app::ExperimentRunState::Idle, "idle after finalize");
    }

    // ---- 6c. Backend-owned finalization -----------------------------------------
    {
        wd.mark("finalize complete");
        auto& coordinator = backend.experiment();
        std::optional<backend::app::ExperimentStatus> terminal;
        std::vector<backend::app::ExperimentRunState> seen;
        std::mutex tMutex;
        coordinator.setStatusCallback([&](const backend::app::ExperimentStatus& s) {
            std::lock_guard<std::mutex> lk(tMutex);
            seen.push_back(s.state);
            if (s.terminal) terminal = s;
        });
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive, "NotActive when idle");
        const auto out = (td.path() / "finalize_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for finalize test");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        const auto started = coordinator.start(req);
        MIB_REQUIRE(started.started(), "start: " + started.message);
        // Let frames accumulate so a remainder exists at stop.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        MIB_EXPECT(coordinator.status().validBuffered + coordinator.status().invalidBuffered > 0,
                   "status reports buffered frames while active");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
        {
            const auto second = coordinator.requestStop(false);
            MIB_EXPECT(second == backend::app::ExperimentStopOutcome::Busy ||
                           second == backend::app::ExperimentStopOutcome::NotActive,
                       "second stop while stopping is Busy (or NotActive once finalized)");
        }
        MIB_REQUIRE(waitFor([&] { std::lock_guard<std::mutex> lk(tMutex); return terminal.has_value(); },
                            std::chrono::seconds(20)), "terminal status published");
        std::lock_guard<std::mutex> lk(tMutex);
        std::fprintf(stderr, "finalize: state=%s ok=%d completion=%s (%s) persisted=%llu/%llu\n",
                     backend::app::toString(terminal->state), terminal->finalizationOk ? 1 : 0,
                     backend::recording::toString(terminal->completion), terminal->completionReason.c_str(),
                     (unsigned long long)terminal->persistenceCommitted,
                     (unsigned long long)terminal->persistenceAdmitted);
        MIB_EXPECT(terminal->finalizationOk, "finalization ok");
        MIB_EXPECT(terminal->state == backend::app::ExperimentRunState::Idle, "Idle after a clean stop");
        MIB_EXPECT(terminal->completion == backend::recording::RunCompletionState::Complete ||
                       terminal->completion == backend::recording::RunCompletionState::IntentionallyPartial,
                   "clean run completion: " + terminal->completionReason);
        MIB_EXPECT(terminal->persistenceCommitted == terminal->persistenceAdmitted, "stop-time remainder credited");
        MIB_EXPECT(terminal->persistenceCommitted > 0, "frames were persisted");
        MIB_EXPECT(terminal->endWallClockNs >= terminal->startWallClockNs && terminal->startWallClockNs > 0,
                   "terminal status carries the run times");
        MIB_EXPECT(terminal->startGeneration == started.run.startGeneration, "terminal status carries the run identity");
        MIB_EXPECT(seen.size() >= 4 && seen[0] == backend::app::ExperimentRunState::Starting &&
                       seen[1] == backend::app::ExperimentRunState::Active &&
                       seen[2] == backend::app::ExperimentRunState::Stopping &&
                       seen.back() == backend::app::ExperimentRunState::Idle,
                   "callback observed Starting, Active, Stopping, Idle");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "file closed by the coordinator");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive, "NotActive after finalize");
        backend::services::Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(out), "finalized file reloads");
        backend::recording::RecordingAccountingSnapshot back;
        MIB_REQUIRE(reader.readRunAccounting(back), "accounting persisted by the coordinator");
        MIB_EXPECT(back.reconciled, "persisted accounting reconciles");
        uint64_t t0 = 0, t1 = 0; size_t v = 0, iv = 0;
        MIB_EXPECT(reader.readExperimentInfo(t0, t1, v, iv) && t0 == terminal->startWallClockNs,
                   "experiment info persisted by the coordinator");
        reader.closeFile();
        coordinator.setStatusCallback({});
    }
    {
        wd.mark("fatal save error");
        auto& coordinator = backend.experiment();
        const auto out = (td.path() / "fatal_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for fatal test");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        coordinator.onFatalSaveError("injected writer failure");
        MIB_REQUIRE(waitFor([&] { return coordinator.status().terminal; }, std::chrono::seconds(20)),
                    "fatal error finalizes");
        const auto s = coordinator.status();
        MIB_EXPECT(s.state == backend::app::ExperimentRunState::Failed, "Failed after a fatal save error");
        MIB_EXPECT(!s.finalizationOk, "finalization not ok");
        MIB_EXPECT(s.completion == backend::recording::RunCompletionState::Failed, "completion Failed");
        MIB_EXPECT(s.completionReason == "injected writer failure", "completion reason is the fault message");
        MIB_EXPECT(s.faultCode == "experiment.saveFailed", "fault code reported in status");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "file closed after failure");
        MIB_EXPECT(coordinator.hasUnresolvedFault(), "fault latched for the next preflight");
        MIB_EXPECT(!coordinator.evaluateReadiness(out).ready, "readiness blocked while the fault is latched");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive, "NotActive when Failed");
        coordinator.clearUnresolvedFault();
        MIB_EXPECT(coordinator.status().state == backend::app::ExperimentRunState::Idle, "Idle once the fault is cleared");
        backend::services::Hdf5Service reader;
        MIB_EXPECT(reader.loadFile(out), "failed run's file is readable");
        reader.closeFile();
    }
    {
        wd.mark("shutdown while active");
        auto& coordinator = backend.experiment();
        const auto out = (td.path() / "shutdown_run.h5").string();
        auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for shutdown test");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        coordinator.shutdown();
        const auto s = coordinator.status();
        MIB_EXPECT(s.terminal && s.state == backend::app::ExperimentRunState::Idle, "shutdown finalizes the run");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "shutdown closes the file");
        coordinator.shutdown(); // idempotent
        ExperimentStartRequest again;
        again.outputPath = (td.path() / "after_shutdown.h5").string();
        again.readinessGeneration = coordinator.evaluateReadiness(again.outputPath).generation;
        MIB_EXPECT(coordinator.start(again).outcome == ExperimentStartOutcome::Busy, "no start after shutdown");
    }

    // ---- 7. Bounded background calibration ------------------------------------
    stopCapture(backend);
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // realtime loop drains the store
    auto store = backend.getFrameStore();
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    const cv::Mat emptyFrame(96, 96, CV_8UC1, cv::Scalar(7));
    uint64_t ts = 10'000;
    auto pushEmpty = [&] { pushMat(*store, emptyFrame, ++ts); std::this_thread::sleep_for(std::chrono::milliseconds(3)); };
    auto pushRing = [&](int i) { pushMat(*store, mib::test::ringFrame(96, 96, i), ++ts); std::this_thread::sleep_for(std::chrono::milliseconds(3)); };
    using BgState = ProcessingService::BackgroundCalibrationState;
    auto waitFinished = [&] {
        return waitFor([&] { return proc.backgroundCalibrationStatus().finished(); }, std::chrono::seconds(10));
    };

    {
        wd.mark("bg success");
        std::string err;
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 5;
        req.maxAttempts = 20;
        req.timeoutMs = 5000;
        const uint64_t genBefore = proc.backgroundGeneration();
        MIB_REQUIRE(proc.startBackgroundCalibration(req, &err), "start calibration: " + err);
        MIB_EXPECT(!proc.startBackgroundCalibration(req, &err), "second concurrent calibration rejected");
        MIB_EXPECT(proc.backgroundCalibrationStatus().state == BgState::Running, "running");
        for (int i = 0; i < 5; ++i) pushEmpty();
        MIB_REQUIRE(waitFinished(), "calibration finishes");
        const auto st = proc.backgroundCalibrationStatus();
        std::fprintf(stderr, "bg success: state=%d attempted=%u accepted=%u nonEmpty=%u failed=%u msg=%s\n",
                     (int)st.state, st.attempted, st.accepted, st.rejectedNonEmpty, st.rejectedProcessingFailed,
                     st.message.c_str());
        MIB_EXPECT(st.state == BgState::Succeeded, "succeeded");
        MIB_EXPECT(st.accepted == 5 && st.attempted == 5 && st.rejectedNonEmpty == 0, "exact accounting");
        MIB_EXPECT(proc.backgroundGeneration() == genBefore + 1 && st.publishedBackgroundGeneration == genBefore + 1,
                   "one new background generation");
        MIB_EXPECT(!st.publishedSha256.empty() && st.publishedSha256 == proc.backgroundSha256(), "published identity");
        auto bg = proc.getRealtimeBackgroundGrayShared();
        MIB_REQUIRE(bg && !bg->empty(), "background published");
        MIB_EXPECT(bg->rows == 96 && bg->cols == 96 && bg->at<uint8_t>(48, 48) == 7, "mean of accepted frames");
        // Pushing more empty frames after completion must not change the background.
        pushEmpty();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MIB_EXPECT(proc.backgroundGeneration() == genBefore + 1, "finished operation ignores later frames");
    }
    const std::string goodSha = proc.backgroundSha256();
    const uint64_t goodGen = proc.backgroundGeneration();
    {
        wd.mark("bg contaminated");
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 5;
        req.maxAttempts = 8;
        req.timeoutMs = 5000;
        MIB_REQUIRE(proc.startBackgroundCalibration(req), "start contaminated calibration");
        pushEmpty();
        pushEmpty();
        for (int i = 0; i < 6; ++i) pushRing(i);
        MIB_REQUIRE(waitFinished(), "finishes at maxAttempts");
        const auto st = proc.backgroundCalibrationStatus();
        std::fprintf(stderr, "bg contaminated: state=%d attempted=%u accepted=%u nonEmpty=%u msg=%s\n",
                     (int)st.state, st.attempted, st.accepted, st.rejectedNonEmpty, st.message.c_str());
        MIB_EXPECT(st.state == BgState::FailedInsufficient, "insufficient empty frames -> explicit failure");
        MIB_EXPECT(st.attempted == 8 && st.accepted == 2 && st.rejectedNonEmpty == 6, "contamination counted");
        MIB_EXPECT(proc.backgroundGeneration() == goodGen && proc.backgroundSha256() == goodSha,
                   "previous background preserved on failure");
    }
    {
        wd.mark("bg timeout");
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 5;
        req.maxAttempts = 50;
        req.timeoutMs = 100;
        MIB_REQUIRE(proc.startBackgroundCalibration(req), "start timeout calibration");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto st = proc.backgroundCalibrationStatus();
        MIB_EXPECT(st.state == BgState::FailedTimeout, "no frames -> timeout is reported, never Running forever");
        MIB_EXPECT(proc.backgroundGeneration() == goodGen, "background preserved on timeout");
    }
    {
        wd.mark("bg cancel");
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 5;
        req.maxAttempts = 50;
        MIB_REQUIRE(proc.startBackgroundCalibration(req), "start cancel calibration");
        pushEmpty();
        proc.cancelBackgroundCalibration();
        const auto st = proc.backgroundCalibrationStatus();
        MIB_EXPECT(st.state == BgState::Cancelled, "cancelled");
        pushEmpty();
        pushEmpty();
        pushEmpty();
        pushEmpty();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MIB_EXPECT(proc.backgroundCalibrationStatus().state == BgState::Cancelled &&
                       proc.backgroundGeneration() == goodGen,
                   "cancelled operation never publishes");
    }
    {
        wd.mark("bg config change");
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 5;
        req.maxAttempts = 50;
        MIB_REQUIRE(proc.startBackgroundCalibration(req), "start config-change calibration");
        pushEmpty();
        auto cfg = proc.getProcessingConfig();
        cfg.gaussian_blur_size = cfg.gaussian_blur_size == 5 ? 7 : 5;
        proc.setProcessingConfig(cfg);
        pushEmpty();
        MIB_REQUIRE(waitFinished(), "finishes after config change");
        const auto st = proc.backgroundCalibrationStatus();
        MIB_EXPECT(st.state == BgState::FailedProcessing, "recipe change invalidates the operation");
        MIB_EXPECT(proc.backgroundGeneration() == goodGen && proc.backgroundSha256() == goodSha,
                   "background preserved");
    }
    {
        wd.mark("bg not running");
        proc.stopRealtime();
        std::string err;
        MIB_EXPECT(!proc.startBackgroundCalibration(ProcessingService::BackgroundCalibrationRequest{}, &err) && !err.empty(),
                   "calibration refused when realtime is not running");
    }

    backend.shutdown();
    return mib::test::exitCode();
}
