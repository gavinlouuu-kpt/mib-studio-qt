// Typed start outcomes and the expected run on experiment_stop (#741, bridge ABI 40).
//
// Drives the shared ExperimentCoordinator through the BackendFacade command surface with a mock
// camera, as the bridge does:
//   1. every ExperimentStartOutcome reaches the command result (BackendCommandResult
//      .experimentStartOutcome, which the bridge copies into the reply's start_outcome);
//   2. a stop that names a run (ExperimentCommand.expectedStartGeneration) stops only that run:
//      a stale or unknown generation, or any generation while idle, is RunMismatch with a plain
//      reason and the run continues; the matching generation stops it; a stop without one behaves
//      exactly as before;
//   3. concurrency stress: threads hammer stops with stale generations through the facade and
//      the coordinator while the main thread starts and stops runs; none is ever accepted and
//      every run ends through the stop that named it (the comparison happens under the lock that
//      accepts the stop, not before it).

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace bridge = backend::bridge;
namespace app = backend::app;
using app::ExperimentRunState;
using app::ExperimentStartOutcome;
using app::ExperimentStopOutcome;

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

std::string startName(const std::optional<ExperimentStartOutcome>& o)
{
    return o ? app::toString(*o) : "none";
}

std::string stopName(const std::optional<ExperimentStopOutcome>& o)
{
    return o ? app::toString(*o) : "none";
}

// Start through the facade the way the bridge does: evaluate readiness for the path, present
// that generation (or `generationOverride`), then dispatch Start.
bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path,
                                      const std::string& startRequestJson = {},
                                      std::optional<std::uint64_t> generationOverride = std::nullopt)
{
    app::ExperimentReadinessSnapshot readiness;
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    if (facade.fetchExperimentReadiness(readiness, path)) start.readinessGeneration = readiness.generation;
    if (generationOverride) start.readinessGeneration = *generationOverride;
    start.startRequestJson = startRequestJson;
    return facade.dispatch(start);
}

bridge::BackendCommandResult stopRun(bridge::BackendFacade& facade, std::optional<std::uint64_t> expected)
{
    bridge::ExperimentCommand stop;
    stop.action = bridge::ExperimentCommandAction::Stop;
    stop.expectedStartGeneration = expected;
    return facade.dispatch(stop);
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

int main()
{
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("mib_stop_generation");
    const auto mockDir = td.path() / "mock_frames";
    std::filesystem::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (td.path() / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        std::mutex eventsMutex;
        std::vector<app::ExperimentStatus> statuses;
        facade.setEventSink([&](const bridge::BackendEvent& event) {
            if (const auto* s = std::get_if<bridge::ExperimentStatusEvent>(&event)) {
                std::scoped_lock lock(eventsMutex);
                statuses.push_back(s->status);
            }
        });
        auto stoppingSeenFor = [&](std::uint64_t generation) {
            std::scoped_lock lock(eventsMutex);
            for (const auto& s : statuses)
                if (s.state == ExperimentRunState::Stopping && s.startGeneration == generation) return true;
            return false;
        };
        MIB_REQUIRE(facade.initialize(td.path().string()), "facade initialize");
        auto& coordinator = backendApp.experiment();
        std::atomic<bool> stageBusy{false};
        coordinator.setStageBusyProbe([&] { return stageBusy.load(); });

        // ---- 1. Every start outcome reaches the command result -------------------------------
        wd.mark("start outcomes");
        {
            // NotReady: no running camera (the camera.session gate).
            const auto r = startRun(facade, (td.path() / "not_ready.h5").string());
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::NotReady,
                       "no camera -> NotReady, got " + startName(r.experimentStartOutcome) + ": " + r.message);
        }
        {
            bridge::CameraCommand configure;
            configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
            configure.mockFrameDirectory = mockDir.string();
            configure.mockFrameIntervalMs = 2;
            configure.mockLoopFiles = true;
            MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
            bridge::CameraCommand startCapture;
            startCapture.action = bridge::CameraCommandAction::StartCapture;
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
            const auto probe = (td.path() / "probe.h5").string();
            MIB_REQUIRE(waitFor(
                            [&] {
                                app::ExperimentReadinessSnapshot readiness;
                                return facade.fetchExperimentReadiness(readiness, probe) && readiness.ready;
                            },
                            std::chrono::seconds(15)),
                        "readiness passes with a running mock camera");
        }
        {
            // StaleReadiness: a generation the coordinator never issued.
            const auto path = (td.path() / "stale.h5").string();
            app::ExperimentReadinessSnapshot readiness;
            MIB_REQUIRE(facade.fetchExperimentReadiness(readiness, path), "readiness");
            const auto r = startRun(facade, path, {}, readiness.generation + 1000000);
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::StaleReadiness,
                       "stale readiness -> StaleReadiness, got " + startName(r.experimentStartOutcome));
        }
        {
            // InvalidRequest: the facade's strict start request parser (ABI 39); the message keeps
            // its prefix for callers without the code.
            const auto r = startRun(facade, (td.path() / "invalid.h5").string(), R"({"operator_name":"Alice"})");
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::InvalidRequest,
                       "unknown key -> InvalidRequest, got " + startName(r.experimentStartOutcome));
            MIB_EXPECT(r.message.rfind("invalid start request: ", 0) == 0, "the message is unchanged: " + r.message);
        }
        {
            // Busy: a Z stage operation is active (#533).
            stageBusy.store(true);
            const auto r = startRun(facade, (td.path() / "busy.h5").string());
            stageBusy.store(false);
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::Busy,
                       "stage busy -> Busy, got " + startName(r.experimentStartOutcome));
        }
        {
            // StorageFailed: readiness checks the directory, but the file name is longer than any
            // file system allows, so the HDF5 file cannot be created.
            const auto path = (td.path() / (std::string(300, 'n') + ".h5")).string();
            const auto r = startRun(facade, path);
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::StorageFailed,
                       "uncreatable file -> StorageFailed, got " + startName(r.experimentStartOutcome) + ": " +
                           r.message);
        }
        {
            // ProvenanceFailed: the run snapshot cannot be persisted.
            backendApp.hdf5().setRunProvenanceWriteFaultForTesting(true);
            const auto r = startRun(facade, (td.path() / "provenance.h5").string());
            backendApp.hdf5().setRunProvenanceWriteFaultForTesting(false);
            MIB_EXPECT(!r.ok && r.experimentStartOutcome == ExperimentStartOutcome::ProvenanceFailed,
                       "snapshot write fault -> ProvenanceFailed, got " + startName(r.experimentStartOutcome));
        }
        MIB_REQUIRE(coordinator.state() == ExperimentRunState::Idle && !coordinator.activeRun(),
                    "refused starts leave no run");

        // ---- 2. Stop with an expected start generation --------------------------------------
        wd.mark("stop with a generation while idle");
        {
            const auto idle = coordinator.status().startGeneration;
            for (const std::uint64_t expected : {idle, idle + 1, std::uint64_t{0}}) {
                const auto r = stopRun(facade, expected);
                MIB_EXPECT(!r.ok && r.experimentStopOutcome == ExperimentStopOutcome::RunMismatch,
                           "a generation while idle -> RunMismatch, got " + stopName(r.experimentStopOutcome));
                MIB_EXPECT(r.message == "stop refused: expected run " + std::to_string(expected) +
                                            ", but no run is active; nothing was stopped",
                           "plain reason while idle: " + r.message);
            }
            const auto plain = stopRun(facade, std::nullopt);
            MIB_EXPECT(!plain.ok && plain.experimentStopOutcome == ExperimentStopOutcome::NotActive &&
                           plain.message == "notActive",
                       "no generation while idle -> NotActive as before, got " + stopName(plain.experimentStopOutcome));
        }

        wd.mark("started and already active");
        const auto runA = startRun(facade, (td.path() / "run_a.h5").string());
        MIB_REQUIRE(runA.ok && runA.experimentStartOutcome == ExperimentStartOutcome::Started && runA.operationId != 0,
                    "start -> Started, got " + startName(runA.experimentStartOutcome) + ": " + runA.message);
        {
            const auto again = startRun(facade, (td.path() / "run_a2.h5").string());
            MIB_EXPECT(!again.ok && again.experimentStartOutcome == ExperimentStartOutcome::AlreadyActive,
                       "second start -> AlreadyActive, got " + startName(again.experimentStartOutcome));
        }
        const std::uint64_t genA = coordinator.status().startGeneration;
        MIB_REQUIRE(genA > 0 && coordinator.status().state == ExperimentRunState::Active, "run A is active");

        wd.mark("stale generation refused, run continues");
        for (const std::uint64_t stale : {genA - 1, genA + 1, std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
            const auto r = stopRun(facade, stale);
            MIB_EXPECT(!r.ok && r.experimentStopOutcome == ExperimentStopOutcome::RunMismatch,
                       "generation " + std::to_string(stale) + " while run " + std::to_string(genA) +
                           " is active -> RunMismatch, got " + stopName(r.experimentStopOutcome));
            MIB_EXPECT(r.message == "stop refused: expected run " + std::to_string(stale) + ", but run " +
                                        std::to_string(genA) + " is active; nothing was stopped",
                       "plain reason: " + r.message);
        }
        // The cancel path shares the check: a mismatch never cancels either.
        {
            std::string why;
            MIB_EXPECT(coordinator.requestStop(true, genA + 7, &why) == ExperimentStopOutcome::RunMismatch &&
                           contains(why, "expected run " + std::to_string(genA + 7)),
                       "a mismatched cancel is refused: " + why);
        }
        // Longer than the worker's 250 ms wait: a stop queued by mistake would have been taken by now.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        {
            const auto s = coordinator.status();
            MIB_EXPECT(s.state == ExperimentRunState::Active && s.startGeneration == genA && !s.terminal,
                       std::string("run A continues after the refused stops, state ") + app::toString(s.state));
            MIB_EXPECT(!stoppingSeenFor(genA), "no Stopping status was published for run A");
        }

        wd.mark("matching generation stops");
        {
            const auto r = stopRun(facade, genA);
            MIB_EXPECT(r.ok && r.experimentStopOutcome == ExperimentStopOutcome::Accepted && r.message == "accepted",
                       "the matching generation -> Accepted (no stop was queued before), got " +
                           stopName(r.experimentStopOutcome) + ": " + r.message);
            MIB_EXPECT(r.operationId == runA.operationId, "the stop targets run A's operation");
            // Matching again while it finalizes: the outcome without a generation (Busy, or NotActive once done).
            const auto again = stopRun(facade, genA);
            MIB_EXPECT(again.experimentStopOutcome == ExperimentStopOutcome::Busy ||
                           again.experimentStopOutcome == ExperimentStopOutcome::RunMismatch,
                       "matching while stopping -> Busy (RunMismatch once nothing is held), got " +
                           stopName(again.experimentStopOutcome));
            const auto staleWhileStopping = stopRun(facade, genA - 1);
            MIB_EXPECT(staleWhileStopping.experimentStopOutcome == ExperimentStopOutcome::RunMismatch,
                       "stale while stopping -> RunMismatch, got " + stopName(staleWhileStopping.experimentStopOutcome));
        }
        MIB_REQUIRE(waitFor([&] { const auto s = coordinator.status(); return s.terminal && s.state == ExperimentRunState::Idle; },
                            std::chrono::seconds(20)),
                    "run A finalizes");
        MIB_EXPECT(coordinator.status().startGeneration == genA && !coordinator.status().cancelled,
                   "run A ended by the stop that named it");
        {
            const auto r = stopRun(facade, genA);
            MIB_EXPECT(r.experimentStopOutcome == ExperimentStopOutcome::RunMismatch &&
                           contains(r.message, "no run is active"),
                       "the finished run's generation -> RunMismatch: " + r.message);
        }

        wd.mark("a newer run is not stopped by the older generation");
        const auto runB = startRun(facade, (td.path() / "run_b.h5").string());
        MIB_REQUIRE(runB.ok && runB.experimentStartOutcome == ExperimentStartOutcome::Started, "run B starts");
        const std::uint64_t genB = coordinator.status().startGeneration;
        MIB_REQUIRE(genB == genA + 1, "run B has the next generation");
        {
            const auto r = stopRun(facade, genA);
            MIB_EXPECT(!r.ok && r.experimentStopOutcome == ExperimentStopOutcome::RunMismatch &&
                           contains(r.message, "expected run " + std::to_string(genA) + ", but run " +
                                                   std::to_string(genB) + " is active"),
                       "run A's generation cannot stop run B: " + r.message);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            MIB_EXPECT(coordinator.status().state == ExperimentRunState::Active && !stoppingSeenFor(genB),
                       "run B continues");
        }
        wd.mark("stop without a generation behaves as before");
        {
            const auto r = stopRun(facade, std::nullopt);
            MIB_EXPECT(r.ok && r.experimentStopOutcome == ExperimentStopOutcome::Accepted && r.message == "accepted",
                       "no generation stops the active run, got " + stopName(r.experimentStopOutcome));
            const auto again = stopRun(facade, std::nullopt);
            MIB_EXPECT(again.experimentStopOutcome == ExperimentStopOutcome::Busy ||
                           again.experimentStopOutcome == ExperimentStopOutcome::NotActive,
                       "a second stop without a generation is Busy or NotActive as before, got " +
                           stopName(again.experimentStopOutcome));
        }
        MIB_REQUIRE(waitFor([&] { const auto s = coordinator.status(); return s.terminal && s.state == ExperimentRunState::Idle; },
                            std::chrono::seconds(20)),
                    "run B finalizes");

        // ---- 3. Concurrency stress: stale stops race the run lifecycle ----------------------
        wd.mark("stress");
        {
            constexpr int kCycles = 6;
            std::atomic<std::uint64_t> lastFinished{genB};   // only ever a generation that has ended
            std::atomic<bool> done{false};
            std::atomic<int> refused{0};
            std::atomic<int> wrong{0};
            std::mutex wrongMutex;
            std::string firstWrong;
            auto hammer = [&](bool throughFacade) {
                std::uint64_t i = 0;
                while (!done.load()) {
                    // Every pick names a run that has already ended (or none): never the current one.
                    const std::uint64_t ended = lastFinished.load();
                    const std::uint64_t k = i++ % 3;
                    const std::uint64_t pick = k == 0 ? 0 : k == 1 ? ended : ended - 1;
                    ExperimentStopOutcome outcome;
                    if (throughFacade) {
                        const auto r = stopRun(facade, pick);
                        outcome = r.experimentStopOutcome.value_or(ExperimentStopOutcome::Accepted);
                    } else {
                        outcome = coordinator.requestStop(false, pick, nullptr);
                    }
                    if (outcome == ExperimentStopOutcome::RunMismatch) {
                        refused.fetch_add(1);
                    } else {
                        wrong.fetch_add(1);
                        std::scoped_lock lock(wrongMutex);
                        if (firstWrong.empty())
                            firstWrong = "stale generation " + std::to_string(pick) + " got " + app::toString(outcome);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            };
            // Each refusal is logged as a warning; thousands of them would only bury a failure.
            const auto logLevel = spdlog::get_level();
            spdlog::set_level(spdlog::level::err);
            std::thread viaFacade(hammer, true);
            std::thread viaCoordinator(hammer, false);
            for (int cycle = 0; cycle < kCycles; ++cycle) {
                wd.mark("stress cycle");
                const auto path = (td.path() / ("stress_" + std::to_string(cycle) + ".h5")).string();
                bridge::BackendCommandResult started;
                // A start can meet the previous run's Idle publish (Busy); retry, bounded.
                MIB_REQUIRE(waitFor([&] { started = startRun(facade, path); return started.ok; }, std::chrono::seconds(15)),
                            "stress start: " + started.message);
                const std::uint64_t gen = coordinator.status().startGeneration;
                MIB_REQUIRE(gen > lastFinished.load(), "a new generation");
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                {
                    const auto s = coordinator.status();
                    MIB_EXPECT(s.state == ExperimentRunState::Active && s.startGeneration == gen,
                               "the hammered run stays active");
                }
                const auto stop = stopRun(facade, gen);
                MIB_EXPECT(stop.experimentStopOutcome == ExperimentStopOutcome::Accepted,
                           "the run's own generation stops it, got " + stopName(stop.experimentStopOutcome));
                MIB_REQUIRE(waitFor([&] { const auto s = coordinator.status(); return s.terminal && s.startGeneration == gen; },
                                    std::chrono::seconds(20)),
                            "stress run finalizes");
                lastFinished.store(gen);
            }
            done.store(true);
            viaFacade.join();
            viaCoordinator.join();
            spdlog::set_level(logLevel);
            MIB_EXPECT(wrong.load() == 0, "no stale stop was ever accepted: " + firstWrong);
            MIB_EXPECT(refused.load() > 0, "the hammer ran");
            std::fprintf(stderr, "stress: %d cycles, %d stale stops refused\n", kCycles, refused.load());
        }

        facade.shutdown();
    }
    return mib::test::exitCode();
}
