// e2e_supervisor_shadow_test — shadow-mode supervisor beside a real
// AppBackend + mock camera (issue #422, phase D acceptance 6/7/10).
//
//   1. The snapshot builder reads the authoritative backend: idle backend ->
//      unknown acquisition metrics (never zero), capture state idle; with the
//      mock camera running -> frames delivered grow, camera ready, config
//      copied from ProcessingService.
//   2. With a provider that hangs for its whole timeout, capture keeps
//      delivering frames at the same rate (frame accounting conserved,
//      no supervisor-induced loss), backend status calls stay responsive, and
//      AppBackend::shutdown() completes promptly (cancel honoured) with the
//      capture stopped cleanly.
//   3. Boot-time env wiring: MIB_SUPERVISOR_MODE=shadow starts the rule
//      provider; MIB_SUPERVISOR_PROVIDER=jev without endpoint/transport fails
//      closed to NotConfigured while the backend operates normally.
//   4. Structural: the supervisor has no actuation authority — every record
//      says executed=false and the camera/trigger state is unchanged by a
//      recommendation.

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"
#include "backend/supervisor/ExperimentSnapshotBuilder.h"
#include "backend/supervisor/SupervisorService.h"
#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

using namespace backend::supervisor;
using Clock = std::chrono::steady_clock;

namespace {

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

bool waitFor(const std::function<bool()>& pred, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

struct HangingProvider final : DecisionProvider {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> cancelled{false};
    std::atomic<int> calls{0};
    std::string name() const override { return "hanging"; }
    std::string version() const override { return "hanging/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy& policy) override
    {
        ++calls;
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(policy.providerTimeoutMs), [&] { return cancelled.load(); });
        DecisionResult r;
        r.providerName = name();
        r.error = {cancelled.load() ? ProviderErrorKind::Cancelled : ProviderErrorKind::Timeout, "hung"};
        return r;
    }
    void cancel() override { cancelled.store(true); cv.notify_all(); }
};

uint64_t framesDelivered(backend::AppBackend& b)
{
    return b.capture().telemetrySnapshot().framesDelivered.value;
}

void testBuilderAndIsolation(const std::filesystem::path& frames, const std::filesystem::path& dataDir)
{
    mib::test::Watchdog wd(40);
    unsetEnv("MIB_SUPERVISOR_MODE");
    unsetEnv("MIB_SUPERVISOR_PROVIDER");
    backend::AppBackend b;
    MIB_REQUIRE(b.initialize(dataDir.string()), "backend initialize");
    MIB_EXPECT(b.supervisor().status().mode == SupervisorMode::Off, "supervisor off by default");
    MIB_EXPECT(!b.supervisor().isRunning(), "no worker by default");

    // 1a. Idle backend: unknown stays unknown.
    SnapshotBuildContext ctx;
    ctx.sequence = 1;
    ctx.objective = "e2e";
    const ExperimentSnapshot idle = buildExperimentSnapshot(b, ctx);
    MIB_EXPECT(idle.experimentState == "idle", "experiment idle");
    MIB_EXPECT(idle.acquisition.captureState == "idle" && !idle.acquisition.cameraReady, "capture idle");
    MIB_EXPECT(!idle.acquisition.framesDelivered.known, "frames delivered unknown before a session (not 0)");
    MIB_EXPECT(!idle.acquisition.transportLostFrames.known, "transport loss unknown before a session");
    MIB_EXPECT(!idle.configuration.exposureUs.known, "exposure not guessed");
    MIB_EXPECT(idle.configuration.detectionThreshold == b.processing().getProcessingConfig().bg_subtract_threshold,
               "detection threshold copied from ProcessingService");
    MIB_EXPECT(idle.detection.validObjects.known && idle.detection.validObjects.value == 0.0, "processing counters known (0)");
    MIB_EXPECT(!idle.trigger.lastOnsetUs.known, "no trigger onset before any trigger");

    // Policy on an idle backend: not active -> HUMAN_REVIEW by policy, no provider call.
    const auto rec0 = decideOnce(idle, DecisionPolicy{}, nullptr, SupervisorMode::Shadow);
    MIB_EXPECT(rec0.decidedBy == "policy" && rec0.policy.ruleId == "policy.not_active", "idle -> policy.not_active");

    // 1b. Mock camera running.
    camera::mock::MockCameraOptions opts;
    opts.folder = frames;
    opts.frameInterval = std::chrono::microseconds(2000); // ~500 fps
    opts.loopFiles = true;
    b.configureMockCamera(opts);
    MIB_REQUIRE(b.capture().start(), "capture start");
    wd.mark("wait frames");
    MIB_REQUIRE(waitFor([&] { return framesDelivered(b) >= 50; }, 5000), "mock frames flowing");
    ctx.sequence = 2;
    const ExperimentSnapshot live = buildExperimentSnapshot(b, ctx);
    MIB_EXPECT(live.acquisition.captureState == "running" && live.acquisition.cameraReady, "capture running");
    MIB_EXPECT(live.acquisition.framesDelivered.known && live.acquisition.framesDelivered.value >= 50, "frames delivered known");
    MIB_EXPECT(live.acquisition.lastFailure == "none", "no failure");
    MIB_EXPECT(live.configuration.cameraSource == "mock" && live.configuration.simulated, "mock source flagged as simulated");
    MIB_EXPECT(live.experimentState == "idle", "no experiment running");
    {
        ExperimentSnapshot back;
        std::string perr;
        MIB_EXPECT(snapshotFromJson(snapshotToJson(live), back, perr), "live snapshot serializes: " + perr);
        MIB_EXPECT(snapshotHash(back) == snapshotHash(live), "live snapshot hash stable");
    }

    // 1c. Start a real experiment (readiness gates settle as mock frames
    // arrive) so the supervisor has an active run to supervise.
    auto& coordinator = b.experiment();
    const std::string outputPath = (dataDir / "e2e_supervisor.h5").string();
    wd.mark("wait readiness");
    MIB_REQUIRE(waitFor([&] { return coordinator.evaluateReadiness(outputPath).ready; }, 10000), "readiness with mock camera");
    backend::app::ExperimentStartRequest req;
    req.outputPath = outputPath;
    req.readinessGeneration = coordinator.evaluateReadiness(outputPath).generation;
    const auto started = coordinator.start(req);
    MIB_REQUIRE(started.started(), std::string("experiment start: ") + started.message);
    MIB_EXPECT(coordinator.state() == backend::app::ExperimentRunState::Active, "experiment active");
    ctx.sequence = 3;
    const ExperimentSnapshot active = buildExperimentSnapshot(b, ctx);
    MIB_EXPECT(active.experimentState == "active" && active.recording.storageReady, "snapshot sees the active run");
    MIB_EXPECT(active.runId == outputPath, "run id is the output path");
    MIB_EXPECT(!active.configuration.processingCoreVersion.empty() && !active.configuration.configSha256.empty(),
               "frozen run configuration copied");

    // 2. Hanging provider beside live capture + active experiment.
    auto owned = std::make_unique<HangingProvider>();
    HangingProvider* hanging = owned.get();
    auto& sup = b.supervisor();
    MIB_REQUIRE(sup.setProvider(std::move(owned)), "install hanging provider");
    sup.setSnapshotSource([&](uint64_t seq) {
        SnapshotBuildContext c;
        c.sequence = seq;
        return buildExperimentSnapshot(b, c);
    });
    SupervisorConfig cfg;
    cfg.mode = SupervisorMode::Shadow;
    cfg.intervalMs = 500;
    cfg.policy.minFramesForModelDecision = 0; // ring frames may yield no objects; supervise anyway
    cfg.policy.minElapsedSecondsForModel = 0.0;
    cfg.policy.providerTimeoutMs = 8000;
    cfg.logPath = (dataDir / "e2e.supervisor.jsonl").string();
    cfg.runId = "e2e";
    std::string err;
    MIB_REQUIRE(sup.configure(cfg, &err), "configure: " + err);
    MIB_REQUIRE(sup.start(&err), "start: " + err);
    wd.mark("wait provider hang");
    if (!waitFor([&] { return hanging->calls.load() >= 1 && sup.status().inFlight; }, 5000)) {
        const auto st = sup.status();
        std::fprintf(stderr, "supervisor evaluations=%llu policy=%llu skipped=%llu lastError=%s rule=%s\n",
                     static_cast<unsigned long long>(st.evaluations), static_cast<unsigned long long>(st.policyDecisions),
                     static_cast<unsigned long long>(st.skippedInactive), st.lastError.c_str(),
                     st.lastRecord ? st.lastRecord->policy.ruleId.c_str() : "-");
        MIB_REQUIRE(false, "provider hanging");
    }
    // Live view/status stay responsive; frames keep flowing at the same rate.
    const uint64_t f0 = framesDelivered(b);
    const auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i) {
        (void)b.capture().lifecycleSnapshot();
        (void)b.capture().telemetrySnapshot();
        (void)b.experiment().status();
        (void)sup.status();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const uint64_t f1 = framesDelivered(b);
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const double fps = static_cast<double>(f1 - f0) / secs;
    std::printf("  frames while provider hung: %llu in %.2f s (%.0f fps)\n",
                static_cast<unsigned long long>(f1 - f0), secs, fps);
    MIB_EXPECT(f1 > f0 + 100, "capture kept delivering while the provider hung");
    MIB_EXPECT(b.capture().telemetrySnapshot().transportLostFrames.value == 0, "no loss induced by the supervisor");
    MIB_EXPECT(sup.status().inFlight && hanging->calls.load() == 1, "still hung, single in-flight call");
    MIB_EXPECT(b.trigger().getTriggerCount() == 0, "no trigger issued by a recommendation");

    // Stop the experiment and capture as an operator would; neither waits on
    // the hung provider.
    const auto t1 = Clock::now();
    MIB_EXPECT(coordinator.requestStop(false) == backend::app::ExperimentStopOutcome::Accepted, "stop accepted");
    wd.mark("wait finalize");
    MIB_EXPECT(waitFor([&] { return coordinator.status().terminal; }, 15000), "finalization completed while the provider hung");
    MIB_EXPECT(coordinator.status().finalizationOk, "finalization ok");
    b.capture().stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t1).count();
    std::printf("  stop + finalize with provider hung: %lld ms\n", static_cast<long long>(stopMs));
    MIB_EXPECT(stopMs < 6000, "experiment stop/finalize not delayed by the hung provider");
    MIB_EXPECT(!b.capture().isRunning(), "capture stopped");
    MIB_EXPECT(sup.status().inFlight && hanging->calls.load() == 1, "provider still hung through stop and finalize");
    wd.mark("shutdown");
    const auto t2 = Clock::now();
    b.shutdown();
    const auto shutdownMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t2).count();
    MIB_EXPECT(shutdownMs < 4000, "shutdown returned well inside the 8 s provider timeout (cancel honoured)");
    MIB_EXPECT(hanging->cancelled.load(), "provider cancelled on shutdown");
    const auto st = sup.status();
    MIB_EXPECT(!st.running, "supervisor stopped");
    MIB_EXPECT(st.providerFailures >= 1 && st.lastRecord && !st.lastRecord->executed, "hung call recorded as failure, not executed");
    MIB_EXPECT(st.lastRecord->recommendation.action == NextAction::HumanReview, "fail closed");
    MIB_EXPECT(st.lastRecord->snapshot.acquisition.framesDelivered.known, "record snapshot carries live telemetry");
    std::vector<DecisionRecord> logged;
    MIB_EXPECT(DecisionLog::readAll(cfg.logPath, logged, err) && !logged.empty(), "sidecar written: " + err);
}

void testEnvBoot(const std::filesystem::path& dataDirA, const std::filesystem::path& dataDirB)
{
    mib::test::Watchdog wd(30);
    {
        setEnv("MIB_SUPERVISOR_MODE", "shadow");
        setEnv("MIB_SUPERVISOR_INTERVAL_MS", "500");
        unsetEnv("MIB_SUPERVISOR_PROVIDER");
        backend::AppBackend b;
        MIB_REQUIRE(b.initialize(dataDirA.string()), "initialize A");
        const auto st = b.supervisor().status();
        MIB_EXPECT(st.mode == SupervisorMode::Shadow && st.running, "shadow started from env");
        MIB_EXPECT(st.providerName == "rule", "rule provider by default");
        MIB_EXPECT(b.supervisor().config().intervalMs == 500, "interval from env");
        MIB_EXPECT(!b.supervisor().config().logPath.empty(), "sidecar path configured under data/supervisor");
        wd.mark("wait tick");
        MIB_EXPECT(waitFor([&] { return b.supervisor().status().skippedInactive >= 1; }, 5000),
                   "ticks skip while no experiment is active");
        MIB_EXPECT(b.supervisor().status().evaluations == 0, "no evaluation without an active experiment");
        b.shutdown();
        MIB_EXPECT(!b.supervisor().isRunning(), "stopped on shutdown");
    }
    {
        setEnv("MIB_SUPERVISOR_MODE", "shadow");
        setEnv("MIB_SUPERVISOR_PROVIDER", "jev");
        unsetEnv(kJevEndpointEnvVar);
        unsetEnv(kJevModelEnvVar);
        backend::AppBackend b;
        MIB_REQUIRE(b.initialize(dataDirB.string()), "initialize B");
        MIB_EXPECT(b.supervisor().status().providerName == "jev", "jev provider selected");
        ExperimentSnapshot s;
        s.experimentState = "active";
        s.elapsedSeconds = 60;
        s.acquisition.cameraReady = true;
        s.acquisition.lastFailure = "none";
        s.detection.framesProcessed = Metric::of(1000);
        const auto rec = b.supervisor().evaluateNow(s);
        MIB_EXPECT(rec.decidedBy == "fail_closed" && rec.provider.error.kind == ProviderErrorKind::NotConfigured,
                   "unconfigured JEV fails closed");
        MIB_EXPECT(rec.recommendation.action == NextAction::HumanReview, "HUMAN_REVIEW");
        // Ordinary operation unaffected.
        MIB_EXPECT(b.capture().lifecycleSnapshot().state == backend::services::CaptureLifecycleState::Idle, "capture idle and usable");
        // Injecting a transport later re-creates the provider with it.
        b.setSupervisorHttpPost([](const HttpPostRequest&) { HttpPostResult r; r.ok = true; r.status = 200; r.body = "{}"; return r; });
        MIB_EXPECT(b.supervisor().status().providerName == "jev", "still jev after transport injection");
        b.shutdown();
    }
    unsetEnv("MIB_SUPERVISOR_MODE");
    unsetEnv("MIB_SUPERVISOR_PROVIDER");
    unsetEnv("MIB_SUPERVISOR_INTERVAL_MS");
}

} // namespace

int main()
{
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
    mib::test::TempDir td("e2e_supervisor");
    const auto frames = td / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 128, 96), "write mock frames");
    std::filesystem::create_directories(td / "a");
    std::filesystem::create_directories(td / "b");
    std::filesystem::create_directories(td / "c");
    testBuilderAndIsolation(frames, td / "a");
    testEnvBoot(td / "b", td / "c");
    std::printf("e2e_supervisor_shadow_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
