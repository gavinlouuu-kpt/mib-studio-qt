// supervisor_service_test — shadow-mode SupervisorService lifecycle,
// concurrency stress and fault injection (issue #422, phase D).
//
//   1. Disabled by default: no worker, no records, start() refused while Off.
//   2. Shadow mode ticks at the configured cadence, skips inactive
//      snapshots, records policy/provider decisions with executed == false,
//      writes the sidecar, and reports status truthfully.
//   3. A slow/hung provider never blocks configure/status/requestStop; stop
//      returns promptly (cancel is honoured) and the timeout is recorded.
//   4. A throwing provider / throwing snapshot source / failing log path are
//      contained: the worker keeps running and status shows the error.
//   5. Stress: repeated start/stop cycles and concurrent status readers with a
//      watchdog; no leaked worker, exactly one terminal state per cycle.
//   6. Structural: the service holds no actuation authority (status flag
//      recommendationOnly, records executed == false, operator actions are
//      associations only).

#include "backend/supervisor/DecisionProvider.h"
#include "backend/supervisor/RuleProvider.h"
#include "backend/supervisor/SupervisorService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace backend::supervisor;
using Clock = std::chrono::steady_clock;

namespace {

ExperimentSnapshot healthy(uint64_t seq, const char* state = "active")
{
    ExperimentSnapshot s;
    s.sequence = seq;
    s.runId = "svc-run";
    s.experimentState = state;
    s.elapsedSeconds = 60.0;
    s.acquisition.cameraReady = true;
    s.acquisition.lastFailure = "none";
    s.acquisition.framesDelivered = Metric::of(30000);
    s.acquisition.transportLostFrames = Metric::of(0);
    s.detection.framesProcessed = Metric::of(30000);
    s.detection.validObjects = Metric::of(900);
    s.detection.invalidObjects = Metric::of(300);
    s.detection.contrastMean = Metric::of(60);
    s.detection.brightnessMedianMean = Metric::of(120);
    s.detection.brightnessMaxMean = Metric::of(200);
    s.recording.persistenceFailed = Metric::of(0);
    return s;
}

// Blocks inside evaluate() until cancelled or released.
struct BlockingProvider final : DecisionProvider {
    std::mutex m;
    std::condition_variable cv;
    bool release{false};
    std::atomic<bool> cancelled{false};
    std::atomic<int> calls{0};
    std::string name() const override { return "blocking"; }
    std::string version() const override { return "blocking/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy& policy) override
    {
        ++calls;
        std::unique_lock<std::mutex> lk(m);
        const bool released = cv.wait_for(lk, std::chrono::milliseconds(policy.providerTimeoutMs),
                                          [&] { return release || cancelled.load(); });
        DecisionResult r;
        r.providerName = name();
        if (!released) { r.error = {ProviderErrorKind::Timeout, "blocking provider timed out"}; return r; }
        if (cancelled.load()) { r.error = {ProviderErrorKind::Cancelled, "cancelled"}; return r; }
        r.answers.quality = RunQuality::Good;
        r.answers.problem = PrimaryProblem::None;
        r.answers.action = NextAction::Continue;
        return r;
    }
    void cancel() override { cancelled.store(true); cv.notify_all(); }
};

struct ThrowingProvider final : DecisionProvider {
    std::string name() const override { return "throwing"; }
    std::string version() const override { return "throwing/1"; }
    DecisionResult evaluate(const ExperimentSnapshot&, const DecisionPolicy&) override
    {
        throw std::runtime_error("provider exploded");
    }
};

bool waitFor(const std::function<bool()>& pred, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void testDisabledByDefault()
{
    SupervisorService svc;
    const auto st = svc.status();
    MIB_EXPECT(st.mode == SupervisorMode::Off && !st.running, "off by default");
    MIB_EXPECT(st.recommendationOnly, "recommendation-only flag");
    std::string err;
    MIB_EXPECT(!svc.start(&err) && err.find("off") != std::string::npos, "start refused while off");
    svc.setProvider(std::make_unique<RuleProvider>());
    SupervisorConfig cfg;
    cfg.mode = SupervisorMode::Shadow;
    MIB_EXPECT(svc.configure(cfg), "configure shadow");
    MIB_EXPECT(!svc.start(&err) && err.find("snapshot source") != std::string::npos, "start refused without source");
    cfg.intervalMs = 10;
    MIB_EXPECT(!svc.configure(cfg, &err) && !err.empty(), "interval below minimum rejected");
    cfg.intervalMs = 1000;
    cfg.policy.providerTimeoutMs = 0;
    MIB_EXPECT(!svc.configure(cfg, &err), "zero timeout rejected");
    cfg.policy.providerTimeoutMs = 1000;
    cfg.policy.hardFrameLossFraction = 0.0;
    MIB_EXPECT(!svc.configure(cfg, &err), "zero frame-loss limit rejected");
    // evaluateNow works synchronously even while Off (harness/operator use).
    SupervisorService off;
    off.setProvider(std::make_unique<RuleProvider>());
    const auto rec = off.evaluateNow(healthy(1));
    MIB_EXPECT(rec.decidedBy == "provider" && rec.recommendation.action == NextAction::Continue, "evaluateNow while off");
    MIB_EXPECT(!rec.executed, "not executed");
    MIB_EXPECT(off.status().evaluations == 1 && off.status().providerDecisions == 1, "stats count evaluateNow");
}

void testShadowTicks()
{
    mib::test::Watchdog wd(20);
    mib::test::TempDir td("supervisor_svc");
    SupervisorService svc;
    svc.setProvider(std::make_unique<RuleProvider>());
    std::atomic<uint64_t> sourceCalls{0};
    svc.setSnapshotSource([&](uint64_t seq) -> std::optional<ExperimentSnapshot> {
        const auto n = ++sourceCalls;
        if (n == 1) return healthy(seq, "idle");     // skipped: not active
        if (n == 2) return std::nullopt;              // skipped: nothing to snapshot
        auto s = healthy(seq);
        if (n == 4) s.acquisition.transportLostFrames = Metric::of(50000); // policy stop
        return s;
    });
    std::atomic<int> observed{0};
    svc.addObserver([&](const DecisionRecord& r) { MIB_EXPECT(!r.executed, "observer sees executed=false"); ++observed; });
    SupervisorConfig cfg;
    cfg.mode = SupervisorMode::Shadow;
    cfg.intervalMs = 20;
    cfg.minIntervalMs = 20;
    cfg.logPath = (td / "shadow.supervisor.jsonl").string();
    cfg.runId = "svc-run";
    MIB_REQUIRE(svc.configure(cfg), "configure");
    std::string err;
    MIB_REQUIRE(svc.start(&err), "start: " + err);
    MIB_EXPECT(svc.isRunning(), "running");
    MIB_EXPECT(svc.start(), "start is idempotent");
    wd.mark("wait for evaluations");
    MIB_EXPECT(waitFor([&] { return svc.status().evaluations >= 4; }, 5000), "at least four evaluations");
    wd.mark("stop");
    svc.requestStop();
    svc.shutdown();
    MIB_EXPECT(!svc.isRunning(), "stopped");
    const auto st = svc.status();
    MIB_EXPECT(st.skippedInactive >= 1, "inactive snapshot skipped");
    MIB_EXPECT(st.policyDecisions >= 1, "policy decision counted");
    MIB_EXPECT(st.providerDecisions >= 2, "provider decisions counted");
    MIB_EXPECT(st.providerFailures == 0, "no failures");
    MIB_EXPECT(st.lastRecord && !st.lastRecord->executed, "last record not executed");
    MIB_EXPECT(observed.load() == static_cast<int>(st.evaluations), "observer saw every record");
    const auto recent = svc.recentRecords();
    MIB_EXPECT(recent.size() == st.evaluations, "recent records retained");
    bool sawPrior = false;
    for (const auto& r : recent) if (!r.snapshot.priorRecommendations.empty()) sawPrior = true;
    MIB_EXPECT(sawPrior, "later snapshots carry prior recommendations");
    for (const auto& r : recent) MIB_EXPECT(r.mode == "shadow" && !r.executed, "shadow records");
    // Sequence numbers strictly increase (one decision point per tick).
    for (size_t i = 1; i < recent.size(); ++i) MIB_EXPECT(recent[i].sequence > recent[i - 1].sequence, "monotonic sequence");
    std::vector<DecisionRecord> logged;
    MIB_REQUIRE(DecisionLog::readAll(cfg.logPath, logged, err), "read sidecar: " + err);
    MIB_EXPECT(logged.size() == st.evaluations, "sidecar has every record");
    // Operator action association after stop is still recorded (in memory).
    svc.recordOperatorAction("STOP_SUCCESS");
    MIB_EXPECT(svc.recentRecords().back().operatorAction == "STOP_SUCCESS", "operator action associated with last record");
    MIB_EXPECT(svc.status().lastRecord->operatorAction == "STOP_SUCCESS", "status reflects the association");
    // Restart after stop works (worker reaped).
    MIB_REQUIRE(svc.start(&err), "restart: " + err);
    MIB_EXPECT(waitFor([&] { return svc.status().evaluations > st.evaluations; }, 5000), "ticks after restart");
    svc.shutdown();
}

void testSlowProviderNeverBlocks()
{
    mib::test::Watchdog wd(30);
    SupervisorService svc;
    auto owned = std::make_unique<BlockingProvider>();
    BlockingProvider* blocking = owned.get();
    svc.setProvider(std::move(owned));
    svc.setSnapshotSource([](uint64_t seq) { return healthy(seq); });
    SupervisorConfig cfg;
    cfg.mode = SupervisorMode::Shadow;
    cfg.intervalMs = 500;
    cfg.policy.providerTimeoutMs = 5000;
    MIB_REQUIRE(svc.configure(cfg), "configure");
    MIB_REQUIRE(svc.start(), "start");
    wd.mark("wait in-flight");
    MIB_EXPECT(waitFor([&] { return blocking->calls.load() >= 1 && svc.status().inFlight; }, 3000), "provider call in flight");
    // Control-plane calls return immediately while the provider blocks.
    const auto t0 = Clock::now();
    (void)svc.status();
    (void)svc.config();
    (void)svc.recentRecords();
    svc.recordOperatorAction("noop");
    cfg.objective = "changed while blocked";
    MIB_EXPECT(svc.configure(cfg), "configure while blocked");
    const auto controlMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    MIB_EXPECT(controlMs < 500, "control calls do not wait on the provider");
    MIB_EXPECT(svc.status().evaluations == 0, "no record while blocked");
    // Stop cancels the in-flight call; shutdown returns well inside the provider timeout.
    wd.mark("stop while blocked");
    const auto t1 = Clock::now();
    svc.requestStop();
    svc.shutdown();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t1).count();
    MIB_EXPECT(stopMs < 2500, "shutdown returned before the 5 s provider timeout (cancel honoured)");
    MIB_EXPECT(blocking->cancelled.load(), "cancel was delivered");
    const auto st = svc.status();
    MIB_EXPECT(st.evaluations == 1 && st.providerFailures == 1, "cancelled call recorded as a failure");
    MIB_EXPECT(st.lastRecord && st.lastRecord->decidedBy == "fail_closed", "fail closed");
    MIB_EXPECT(st.lastRecord->recommendation.action == NextAction::HumanReview, "HUMAN_REVIEW after cancel");
    MIB_EXPECT(st.lastError.find("cancelled") != std::string::npos, "status reports the error");

    // Timeout path: the provider's bounded wait expires on its own.
    SupervisorService svc2;
    svc2.setProvider(std::make_unique<BlockingProvider>());
    svc2.setSnapshotSource([](uint64_t seq) { return healthy(seq); });
    cfg.policy.providerTimeoutMs = 100;
    MIB_REQUIRE(svc2.configure(cfg), "configure 2");
    MIB_REQUIRE(svc2.start(), "start 2");
    wd.mark("wait timeout");
    MIB_EXPECT(waitFor([&] { return svc2.status().providerFailures >= 1; }, 3000), "timeout recorded");
    svc2.shutdown();
    const auto st2 = svc2.status();
    MIB_EXPECT(st2.lastRecord && st2.lastRecord->provider.error.kind == ProviderErrorKind::Timeout, "timeout kind");
    MIB_EXPECT(st2.lastRecord->recommendation.action == NextAction::HumanReview, "timeout -> HUMAN_REVIEW");
}

void testFaultsContained()
{
    mib::test::Watchdog wd(20);
    mib::test::TempDir td("supervisor_faults");
    {
        SupervisorService svc;
        svc.setProvider(std::make_unique<ThrowingProvider>());
        std::atomic<int> n{0};
        svc.setSnapshotSource([&](uint64_t seq) -> std::optional<ExperimentSnapshot> {
            if (++n == 2) throw std::runtime_error("source exploded");
            return healthy(seq);
        });
        SupervisorConfig cfg;
        cfg.mode = SupervisorMode::Shadow;
        cfg.intervalMs = 20;
        cfg.minIntervalMs = 20;
        cfg.logPath = (td / "no" / "such" / "dir" / "x.jsonl").string(); // open fails; run continues
        MIB_REQUIRE(svc.configure(cfg), "configure");
        MIB_REQUIRE(svc.start(), "start despite bad log path");
        wd.mark("wait for exceptions");
        MIB_EXPECT(waitFor([&] { return svc.status().evaluations >= 3; }, 5000), "worker survives throwing provider and source");
        svc.shutdown();
        const auto st = svc.status();
        MIB_EXPECT(st.providerFailures == st.evaluations, "every provider call failed");
        MIB_EXPECT(st.logWriteFailures >= 1, "log open failure reported");
        MIB_EXPECT(st.lastRecord && st.lastRecord->provider.error.kind == ProviderErrorKind::Exception, "exception mapped");
        MIB_EXPECT(st.lastRecord->recommendation.action == NextAction::HumanReview, "fail closed");
    }
    {
        // Replacing the provider while running is refused; after stop it works.
        SupervisorService svc;
        svc.setProvider(std::make_unique<RuleProvider>());
        svc.setSnapshotSource([](uint64_t seq) { return healthy(seq); });
        SupervisorConfig cfg;
        cfg.mode = SupervisorMode::Shadow;
        cfg.intervalMs = 500;
        MIB_REQUIRE(svc.configure(cfg), "configure");
        MIB_REQUIRE(svc.start(), "start");
        MIB_EXPECT(!svc.setProvider(std::make_unique<RuleProvider>()), "provider swap refused while running");
        cfg.mode = SupervisorMode::Off;
        MIB_EXPECT(svc.configure(cfg), "switch to off");
        wd.mark("wait for off");
        MIB_EXPECT(waitFor([&] { return !svc.isRunning(); }, 3000), "worker exits when mode goes Off");
        svc.shutdown();
        MIB_EXPECT(svc.setProvider(std::make_unique<RuleProvider>()), "provider swap allowed when stopped");
    }
}

void testStress(int cycles)
{
    mib::test::Watchdog wd(30);
    SupervisorService svc;
    svc.setProvider(std::make_unique<RuleProvider>());
    std::atomic<uint64_t> produced{0};
    svc.setSnapshotSource([&](uint64_t seq) { ++produced; return healthy(seq); });
    SupervisorConfig cfg;
    cfg.mode = SupervisorMode::Shadow;
    cfg.intervalMs = 20;
    cfg.minIntervalMs = 20;
    MIB_REQUIRE(svc.configure(cfg), "configure");
    std::atomic<bool> stopReaders{false};
    std::atomic<uint64_t> reads{0};
    std::thread reader([&] {
        while (!stopReaders.load()) {
            (void)svc.status();
            (void)svc.recentRecords();
            svc.recordOperatorAction("reader");
            ++reads;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    for (int i = 0; i < cycles; ++i) {
        wd.mark("cycle start");
        MIB_REQUIRE(svc.start(), "start cycle");
        std::this_thread::sleep_for(std::chrono::milliseconds(i % 3 == 0 ? 0 : 25));
        wd.mark("cycle stop");
        svc.requestStop();
        svc.shutdown();
        MIB_EXPECT(!svc.isRunning(), "not running after shutdown");
    }
    stopReaders.store(true);
    reader.join();
    MIB_EXPECT(reads.load() > 0, "readers ran concurrently");
    const auto st = svc.status();
    MIB_EXPECT(st.evaluations <= produced.load(), "every evaluation came from a produced snapshot");
    MIB_EXPECT(!st.inFlight, "nothing in flight after the last shutdown");
    std::printf("  stress: cycles=%d evaluations=%llu reads=%llu\n", cycles,
                static_cast<unsigned long long>(st.evaluations), static_cast<unsigned long long>(reads.load()));
}

} // namespace

int main(int argc, char** argv)
{
    const int cycles = argc > 1 ? std::atoi(argv[1]) : 30;
    testDisabledByDefault();
    testShadowTicks();
    testSlowProviderNeverBlocks();
    testFaultsContained();
    testStress(cycles);
    std::printf("supervisor_service_test: %s\n", mib::test::exitCode() == 0 ? "OK" : "FAILED");
    return mib::test::exitCode();
}
