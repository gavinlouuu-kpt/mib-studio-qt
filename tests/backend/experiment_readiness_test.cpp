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
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"
#include "backend/services/RfGeneratorService.h"
#include "backend/camera/mock/MockCamera.h"

#include "support/assert.h"
#include "support/fake_ssg.h"
#include "support/frames.h"
#include "support/faultinject.h"
#include "support/fault_kernel.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
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
#ifdef _WIN32
    _putenv_s("MIB_DISABLED_SERVICES",
              "auto_update,autofocus,trigger,syringe_pump,pulse_generator");
#else
    setenv("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator",
           1);
#endif
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("experiment_readiness");
    const fs::path frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "write mock frames");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& coord = backend.experiment();
    coord.setApplicationIdentity("test-version", "test-build", "test-os");
    auto& proc = backend.processing();
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string kernelError;
    MIB_REQUIRE(proc.activateProcessingKernel(kernel, &kernelError),
                "fault kernel active: " + kernelError);
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
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::NotRequired, "sorting disabled -> RF generator not required");
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

    // ---- 1b. RF sort generator gate (SSG3021X over the LAN transport) --------
    // Sorting on: a missing or disabled link is not required; a
    // configured link that is down or mis-armed fails closed, an armed
    // instrument passes and its readback lands in the candidate snapshot.
    {
        wd.mark("rf gate");
        auto cfgSort = proc.getProcessingConfig();
        cfgSort.enable_target_group = true;
        proc.setProcessingConfig(cfgSort);
        auto r = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::NotRequired, "sorting on, no rf_generator block -> not required");
        MIB_EXPECT(!r.candidate.rfGeneratorConfigured, "candidate: not configured");

        std::ifstream defaults(fs::path(__FILE__).parent_path().parent_path().parent_path() / "resources/defaults/config.json");
        MIB_REQUIRE(defaults.good(), "bundled default config readable");
        backend.setLastConfigJson(std::string(std::istreambuf_iterator<char>(defaults), {}));
        r = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::NotRequired, "bundled disabled RF generator -> not required");
        MIB_EXPECT(!r.candidate.rfGeneratorConfigured, "bundled RF generator disabled");

        backend.setLastConfigJson("{\"rf_generator\":{\"enabled\":true,\"transport\":\"lan\",\"resource\":\"127.0.0.1:1\",\"timeout_ms\":200}}");
        r = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::Fail, "configured but unreachable -> fail");
        MIB_EXPECT(r.candidate.rfGeneratorConfigured && !r.candidate.rfGeneratorConnected &&
                       !r.candidate.rfGeneratorError.empty(),
                   "candidate carries the link error");
#ifndef _WIN32
        mib::test::FakeSsg ssg;
        mib::test::LoopbackSsgServer server(ssg);
        MIB_REQUIRE(server.start(), "loopback SSG up");
        backend.setLastConfigJson("{\"rf_generator\":{\"enabled\":true,\"transport\":\"lan\",\"resource\":\"127.0.0.1:" +
                                  std::to_string(server.port()) + "\",\"timeout_ms\":500}}");
        r = coord.evaluateReadiness(out1);
        dumpGates(r);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::Pass, "armed instrument -> pass");
        MIB_EXPECT(r.candidate.rfGeneratorConnected && r.candidate.rfGeneratorIdentity == ssg.idn &&
                       r.candidate.rfGeneratorTriggerMode == "EXTernal" &&
                       std::fabs(r.candidate.rfGeneratorPulseWidthS - 50e-6) < 1e-12,
                   "candidate carries identity, trigger mode and window");
        {
            std::lock_guard<std::mutex> lk(ssg.m);
            ssg.trigMode = "AUTO";
            ssg.rfOn = false;
        }
        r = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::Fail, "mis-armed instrument -> fail");
        MIB_EXPECT(r.candidate.rfGeneratorIssues.size() == 2, "both blocking issues listed");
        const auto* g = r.gate("rf.generator");
        MIB_EXPECT(g && g->reason.find("rf.triggerMode") != std::string::npos &&
                       g->reason.find("rf.output") != std::string::npos,
                   "gate reason names the issues");
        server.stop();
        backend.rfGenerator().disconnect();
#endif
        // Restore: link disabled, sorting off, so the remaining sections see
        // the same backend as before.
        backend.setLastConfigJson("{}");
        cfgSort.enable_target_group = false;
        proc.setProcessingConfig(cfgSort);
        r = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(r, "rf.generator") == GateStatus::NotRequired, "restored: not required");
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
        MIB_EXPECT(statusOf(r, "storage.roundtrip") == GateStatus::Pass,
                   "destination HDF5 roundtrip verified");
        MIB_EXPECT(r.candidate.camera.simulated && !r.candidate.camera.fallback, "candidate records explicit mock");
        MIB_EXPECT(r.candidate.frameWidth == 96 && r.candidate.frameHeight == 96, "candidate geometry from frames");
        MIB_EXPECT(r.candidate.captureGeneration == backend.capture().lifecycleSnapshot().generation,
                   "candidate carries capture generation");
        const auto originalBudget = proc.getMaxBufferedBytes();
        proc.setMaxBufferedBytes(1);
        const auto impossible = coord.evaluateReadiness(out1);
        MIB_EXPECT(!impossible.ready && statusOf(impossible, "storage.buffer") == GateStatus::Fail,
                   "oversized payload blocks readiness");
        MIB_EXPECT(impossible.generation != r.generation, "buffer budget invalidates readiness");
        const auto beforeSeries = proc.getProcessingConfig();
        auto series = beforeSeries;
        series.multi_image_enabled = true;
        series.multi_image_count = 10;
        proc.setProcessingConfig(series);
        proc.setMaxBufferedBytes(4 * 96 * 96);
        const auto oversizedSeries = coord.evaluateReadiness(out1);
        MIB_EXPECT(!oversizedSeries.ready &&
                       statusOf(oversizedSeries, "storage.buffer") == GateStatus::Fail,
                   "oversized series blocks readiness");
        proc.setProcessingConfig(beforeSeries);
        proc.setMaxBufferedBytes(originalBudget);
        genReady = coord.evaluateReadiness(out1).generation;
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
        // Geometry can change independently of the capture lifecycle (SDK ROI /
        // format renegotiation). Its payload gate must invalidate prior preflight.
        const auto budget = proc.getMaxBufferedBytes();
        proc.setMaxBufferedBytes(2 * 96 * 96);
        auto store = backend.getFrameStore();
        pushMat(*store, cv::Mat(96, 96, CV_8UC1, cv::Scalar(0)), 9000);
        const auto small = coord.evaluateReadiness(out1);
        pushMat(*store, cv::Mat(192, 96, CV_8UC1, cv::Scalar(0)), 9001);
        const auto large = coord.evaluateReadiness(out1);
        MIB_EXPECT(statusOf(small, "storage.buffer") != GateStatus::Fail &&
                       statusOf(large, "storage.buffer") == GateStatus::Fail,
                   "changed geometry changes payload feasibility");
        MIB_EXPECT(large.generation != small.generation,
                   "frame geometry invalidates readiness without a lifecycle change");
        MIB_EXPECT(coord.evaluateReadiness(out1).generation == large.generation,
                   "unchanged frame geometry keeps readiness stable");
        proc.setMaxBufferedBytes(budget);
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
        MIB_EXPECT(frozen.camera.effective == "mock" && frozen.camera.simulated,
                   "camera source frozen");

        // Join the consumer before editing the live recipe so pipeline progress
        // cannot end the run between the active snapshot and duplicate Start.
        proc.stopRealtime();
        const auto backgroundBefore = proc.backgroundGeneration();
        // Later edits must be refused while the run owns the pipeline.
        proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 32, 32});
        proc.setRealtimeBackgroundGray(cv::Mat(96, 96, CV_8UC1, cv::Scalar(9)));
        MIB_EXPECT(proc.getRealtimeRoi().w == 64, "active run refuses direct ROI changes");
        MIB_EXPECT(proc.backgroundGeneration() == backgroundBefore,
                   "active run refuses direct background changes");
        auto backgroundConfig = proc.getProcessingConfig();
        backgroundConfig.auto_background_enabled = !backgroundConfig.auto_background_enabled;
        const auto configBefore = proc.getConfigVersion();
        proc.setProcessingConfig(backgroundConfig);
        MIB_EXPECT(proc.getConfigVersion() == configBefore,
                   "active run refuses background configuration");
        std::string calibrationError;
        MIB_EXPECT(!proc.startBackgroundCalibration({}, &calibrationError) &&
                       calibrationError.find("not idle") != std::string::npos,
                   "active run refuses calibration before touching its state");
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
                       runJson.find("\"schema_version\":2") != std::string::npos &&
                       runJson.find("\"method\":{") != std::string::npos,
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
            throw std::runtime_error("injected observer failure");
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
        proc.stopRealtime();
        auto input = std::make_shared<backend::playback::FrameStore>();
        auto cfg = proc.getProcessingConfig();
        cfg.require_single_inner_contour = true;
        proc.setProcessingConfig(cfg);
        proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
        proc.setRealtimeBackgroundGray(cv::Mat());
        proc.setInvalidFrameSamplingRate(1);
        pushMat(*input, cv::Mat(96, 96, CV_8UC1, cv::Scalar(0)), 999);
        proc.startRealtime(input);
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
        // A ring passes the inner-contour rule; a solid disk fails it.
        for (uint64_t i = 0; i < 10; ++i) {
            cv::Mat frame = mib::test::ringFrame(96, 96, 0);
            if (i >= 4) cv::circle(frame, cv::Point(32, 48), 19, cv::Scalar(220), -1);
            pushMat(*input, frame, 1000 + i);
            MIB_REQUIRE(waitFor(
                            [&] {
                                const auto a = proc.experimentAccountingSnapshot();
                                return a.processed + a.scientificallyRejected == i + 1;
                            },
                            std::chrono::seconds(5)),
                        "known frame classified before the next input");
        }
        // Flush before Stop so the remainder is empty but the run totals are not.
        proc.flushBufferedFrames(backend.hdf5());
        MIB_REQUIRE(proc.finishFlush(), "known mixed batch committed before Stop");
        const auto saved = coordinator.status();
        MIB_EXPECT(saved.validSaved == 4 && saved.invalidSaved == 6,
                   "status separates committed frame classes");
        MIB_EXPECT(saved.droppedValid == 0 && saved.droppedInvalid == 0,
                   "pending frames are not policy drops");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted,
                   "stop accepted");
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
        std::vector<backend::services::ProcessedFrame> validFrames, invalidFrames;
        MIB_REQUIRE(reader.readValidFrames(validFrames), "read saved valid frames");
        MIB_REQUIRE(reader.readInvalidFrames(invalidFrames), "read saved invalid frames");
        MIB_EXPECT(v == validFrames.size() && iv == invalidFrames.size(),
                   "experiment totals equal all saved datasets, not the stop remainder");
        MIB_EXPECT(v + iv == back.persistenceCommitted,
                   "header totals reconcile with committed accounting");
        MIB_EXPECT(v == 4 && iv == 6, "known mixed run stores four valid and six invalid totals");
        MIB_EXPECT(terminal->validSaved == v && terminal->invalidSaved == iv,
                   "terminal saved counters match file totals");
        reader.closeFile();
        coordinator.setStatusCallback({});
        proc.stopRealtime();
        proc.startRealtime(backend.getFrameStore());
    }
    {
        wd.mark("fatal save error");
        auto& coordinator = backend.experiment();
        const auto out = (td.path() / "fatal_run.h5").string();
        proc.setFlushInterval(1000000);

        auto r = coordinator.evaluateReadiness(out);
        MIB_REQUIRE(r.ready, "ready for fatal test");
        ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = r.generation;
        MIB_REQUIRE(coordinator.start(req).started(), "start");
        coordinator.reportUnresolvedFault("test.active", "active run fault");
        const auto activeFault = coordinator.status();
        std::string activeAckError;
        MIB_EXPECT(!coordinator.acknowledgeFault(activeFault.startGeneration, activeFault.faultRevision,
                       activeFault.faultCode, activeFault.faultMessage, activeAckError),
                   "active experiment fault cannot be acknowledged before finalization");
        MIB_REQUIRE(waitFor([&] { return proc.getBufferedFrameCounts().total() > 0; },
                            std::chrono::seconds(20)),
                    "frames buffered before save failure");
        MIB_REQUIRE(proc.flushBufferedFrames(backend.hdf5()) > 0, "first batch submitted");
        MIB_REQUIRE(proc.finishFlush(), "first batch persisted");
        MIB_REQUIRE(backend.hdf5().flush(), "flush before injecting HDF5 fault");
        MIB_REQUIRE(mib::test::blockHdf5ImageAppends(out), "inject HDF5 append failure");
        MIB_REQUIRE(waitFor([&] { return proc.getBufferedFrameCounts().total() > 0; },
                            std::chrono::seconds(20)),
                    "second batch buffered");
        proc.flushBufferedFrames(backend.hdf5());
        MIB_REQUIRE(waitFor([&] { return coordinator.status().terminal; }, std::chrono::seconds(20)),
                    "fatal error finalizes");
        const auto s = coordinator.status();
        MIB_EXPECT(s.state == backend::app::ExperimentRunState::Failed, "Failed after a fatal save error");
        MIB_EXPECT(!s.finalizationOk, "finalization not ok");
        MIB_EXPECT(s.completion == backend::recording::RunCompletionState::Failed, "completion Failed");
        MIB_EXPECT(s.completionReason.find("HDF5 write failed") != std::string::npos,
                   "completion reason is the fault message");
        MIB_EXPECT(s.persistenceCommitted > 0 && s.persistenceFailed > 0,
                   "successful and failed batches counted separately");
        MIB_EXPECT(s.faultCode == "experiment.saveFailed", "fault code reported in status");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), "file closed after failure");
        MIB_EXPECT(coordinator.hasUnresolvedFault(), "fault latched for the next preflight");
        MIB_EXPECT(!coordinator.evaluateReadiness(out).ready, "readiness blocked while the fault is latched");
        MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::NotActive, "NotActive when Failed");
        std::string acknowledgmentError;
        MIB_EXPECT(!coordinator.acknowledgeFault(s.startGeneration + 1, s.faultRevision,
                                                 s.faultCode, s.faultMessage, acknowledgmentError),
                   "stale run cannot acknowledge current fault");
        MIB_EXPECT(!coordinator.acknowledgeFault(s.startGeneration, s.faultRevision, s.faultCode,
                                                 "stale message", acknowledgmentError),
                   "changed fault requires review again");
        MIB_EXPECT(coordinator.hasUnresolvedFault(),
                   "rejected acknowledgment preserves readiness blocker");
        coordinator.reportUnresolvedFault(s.faultCode, s.faultMessage);
        MIB_EXPECT(!coordinator.acknowledgeFault(s.startGeneration, s.faultRevision, s.faultCode,
                                                 s.faultMessage, acknowledgmentError),
                   "same text repeated fault cannot be cleared by stale acknowledgment");
        const auto repeated = coordinator.status();
        MIB_REQUIRE(coordinator.acknowledgeFault(repeated.startGeneration, repeated.faultRevision,
                                                 repeated.faultCode, repeated.faultMessage,
                                                 acknowledgmentError),
                    "explicit matching new fault acknowledged");
        const auto acknowledged = coordinator.status();
        MIB_EXPECT(acknowledged.state == backend::app::ExperimentRunState::Idle,
                   "Idle once fault is acknowledged");
        MIB_EXPECT(acknowledged.outputPath == out && !acknowledged.finalizationOk &&
                       acknowledged.completion == backend::recording::RunCompletionState::Failed,
                   "acknowledgment preserves failed file outcome");
        MIB_EXPECT(!coordinator.acknowledgeFault(s.startGeneration, s.faultRevision, s.faultCode,
                                                 s.faultMessage, acknowledgmentError),
                   "duplicate acknowledgment rejected");
        MIB_REQUIRE(mib::test::restoreHdf5ImageAppends(out), "restore image datasets after fault");
        backend::services::Hdf5Service reader;
        MIB_EXPECT(reader.loadFile(out), "failed run's file is readable");
        backend::recording::RecordingAccountingSnapshot saved;
        MIB_REQUIRE(reader.readRunAccounting(saved), "failed run accounting persisted");
        MIB_EXPECT(saved.completion == backend::recording::RunCompletionState::Failed &&
                       saved.persistenceFailed > 0 && saved.reconciled,
                   "persisted write failure is Failed and reconciles");
        MIB_EXPECT(saved.fatalMessage == s.completionReason,
                   "persisted fatal reason matches terminal outcome");
        backend::recording::RecordingAccountingSnapshot cached;
        uint64_t generation = 0;
        MIB_REQUIRE(coordinator.lastRunAccounting(cached, generation), "cached last accounting");
        MIB_EXPECT(cached.completion == saved.completion &&
                       cached.persistenceFailed == saved.persistenceFailed,
                   "cached and persisted accounting agree after acknowledgement");
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
    proc.stopRealtime(); // join any captured frame still in flight
    auto store = std::make_shared<backend::playback::FrameStore>();
    proc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    const cv::Mat emptyFrame(96, 96, CV_8UC1, cv::Scalar(7));
    uint64_t ts = 10'000;
    // The realtime cursor starts at index zero; seed it before calibration.
    pushMat(*store, emptyFrame, ++ts);
    auto& timing = backend::diagnostics::PipelineTimingRecorder::instance();
    const bool timingWasEnabled = timing.isEnabled();
    timing.setEnabled(true);
    proc.startRealtime(store);
    auto pushAndWait = [&](const cv::Mat& frame) {
        const auto index = store->committedCount();
        const auto emptyBefore =
            timing.skippedCount(backend::diagnostics::PipelineSkipReason::EmptyFrame);
        pushMat(*store, frame, ++ts);
        const auto processed = [&] {
            ProcessingService::RealtimeSnapshot snapshot;
            return (proc.getLatestSnapshot(snapshot) && snapshot.index >= index) ||
                   timing.skippedCount(backend::diagnostics::PipelineSkipReason::EmptyFrame) >
                       emptyBefore;
        };
        MIB_REQUIRE(waitFor(processed, std::chrono::seconds(5)),
                    "calibration frame processed before the next input");
    };
    auto pushEmpty = [&] { pushAndWait(emptyFrame); };
    auto pushRing = [&](int i) { pushAndWait(mib::test::ringFrame(96, 96, i)); };
    using BgState = ProcessingService::BackgroundCalibrationState;
    auto waitFinished = [&] {
        return waitFor([&] { return proc.backgroundCalibrationStatus().finished(); },
                       std::chrono::seconds(10));
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
    {
        wd.mark("bg publication idle gate");
        proc.stopRealtime();
        std::atomic<bool> allowStart{true};
        proc.setBackgroundPublicationTransaction([&](const std::function<void()>& apply) {
            if (!allowStart.exchange(false)) return false;
            apply();
            return true;
        });
        store = std::make_shared<backend::playback::FrameStore>();
        pushMat(*store, emptyFrame, ++ts);
        proc.startRealtime(store);
        ProcessingService::BackgroundCalibrationRequest req;
        req.requiredAccepted = 1;
        req.maxAttempts = 10;
        req.timeoutMs = 5000;
        const auto generation = proc.backgroundGeneration();
        MIB_REQUIRE(proc.startBackgroundCalibration(req), "start calibration before idle gate closes");
        pushEmpty();
        MIB_REQUIRE(waitFinished(), "publication refusal finishes calibration");
        MIB_EXPECT(proc.backgroundCalibrationStatus().state == BgState::Cancelled,
                   "closed idle transaction refuses calibration apply");
        MIB_EXPECT(proc.backgroundGeneration() == generation, "refused apply preserves background");
        proc.stopRealtime();
        proc.setBackgroundPublicationTransaction([&](const std::function<void()>& apply) {
            return backend.experiment().withIdleConfiguration(apply, false);
        });
        store = std::make_shared<backend::playback::FrameStore>();
        pushMat(*store, emptyFrame, ++ts);
        proc.startRealtime(store);
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
        MIB_REQUIRE(waitFinished(), "timeout calibration finishes without frames");
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
        MIB_EXPECT(statusOf(coord.evaluateReadiness(out1), "processing.backgroundCalibration") == GateStatus::Fail,
                   "pending calibration must block a frozen experiment start");
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
        timing.setEnabled(timingWasEnabled);
        std::string err;
        MIB_EXPECT(!proc.startBackgroundCalibration(ProcessingService::BackgroundCalibrationRequest{}, &err) && !err.empty(),
                   "calibration refused when realtime is not running");
    }

    backend.shutdown();
    return mib::test::exitCode();
}
