// Experiment vs manual frame recording: single HDF5 writer owner (issue #451).
//
// Regression for the operator report "started manual recording during an
// experiment; the app crashed and experiment data was lost". Before the fix,
// AppBackend::startFrameRecording closed the experiment's open Hdf5Service
// file (the one ExperimentCoordinator flushes and finalizes) and opened the
// recording destination in its place.
//
// Mock camera + disposable temp outputs only. Covers:
//   A. experiment -> recording (distinct, same, invalid destination; direct
//      AppBackend and bridge callers; repeated clicks from many threads):
//      rejected with no file created/truncated, the experiment keeps its
//      writer claim, capture keeps running, and the run finalizes normally
//      into a readable experiment file with no persistence failures;
//   B. recording -> experiment rejected (coordinator and bridge review load),
//      legal standalone flows still work afterwards;
//   C. concurrent starts from Idle: never both owners;
//   D. fault injection: failed starts release only their own claim (no stuck
//      busy state).

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/app/PersistenceOwnership.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace app = backend::app;
    namespace bridge = backend::bridge;
    namespace fs = std::filesystem;

    std::atomic<const char *> gPhase{"init"};

    fs::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto path = fs::temp_directory_path() / ("mib_exp_rec_excl_" + std::to_string(dist(gen)));
            std::error_code ec;
            if (fs::create_directories(path, ec))
            {
                return path;
            }
        }
        throw std::runtime_error("failed to create temporary directory");
    }

    void setEnv(const char *name, const char *value)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    // No naked join/wait that can hang CI: report the phase and _Exit(99).
    struct Watchdog
    {
        std::thread thread;
        std::atomic<bool> done{false};
        explicit Watchdog(int seconds)
        {
            thread = std::thread([this, seconds] {
                for (int i = 0; i < seconds * 10; ++i)
                {
                    if (done.load())
                    {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                std::cerr << "watchdog: experiment_recording_exclusion_test stuck in phase '" << gPhase.load()
                          << "'\n";
                std::_Exit(99);
            });
        }
        ~Watchdog()
        {
            done.store(true);
            if (thread.joinable())
            {
                thread.join();
            }
        }
    };

    bool waitFor(const std::function<bool()> &pred, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    }

    int fail(int code, const std::string &what)
    {
        std::cerr << "FAIL [" << gPhase.load() << "] " << what << "\n";
        return code;
    }

    // Preflight -> start handshake, re-running the preflight while the gates
    // settle (bounded, like a client).
    app::ExperimentStartResult startExperiment(app::ExperimentCoordinator &coordinator, const std::string &path,
                                               int attempts = 100)
    {
        app::ExperimentStartResult started;
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            app::ExperimentStartRequest request;
            request.outputPath = path;
            request.readinessGeneration = coordinator.evaluateReadiness(path).generation;
            started = coordinator.start(request);
            if (started.started() || (started.outcome != app::ExperimentStartOutcome::NotReady &&
                                      started.outcome != app::ExperimentStartOutcome::StaleReadiness))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return started;
    }

    bool stopExperimentAndWait(app::ExperimentCoordinator &coordinator, app::ExperimentStatus &out)
    {
        if (coordinator.requestStop(false) != app::ExperimentStopOutcome::Accepted)
        {
            return false;
        }
        return waitFor([&] {
            out = coordinator.status();
            return out.terminal && (out.state == app::ExperimentRunState::Idle ||
                                    out.state == app::ExperimentRunState::Failed);
        }, 20000);
    }

    bool writerFree(backend::AppBackend &b)
    {
        return waitFor([&] { return !b.persistenceOwnership().isHeld(); }, 10000);
    }
} // namespace

int main()
{
    Watchdog watchdog(170);

    const auto dataDir = makeTempDir();
    const auto mockDir = dataDir / "mock_frames";
    fs::create_directories(mockDir);
    {
        // Two alternating frames so the run has non-empty content to persist.
        cv::Mat plain(96, 512, CV_8UC1, cv::Scalar(180));
        cv::Mat blob = plain.clone();
        cv::circle(blob, cv::Point(256, 48), 14, cv::Scalar(60), cv::FILLED);
        if (!cv::imwrite((mockDir / "frame_000.tiff").string(), plain) ||
            !cv::imwrite((mockDir / "frame_001.tiff").string(), blob))
        {
            std::cerr << "failed to write mock frame fixtures\n";
            return 1;
        }
    }
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
    // Unwritable destination: its "parent directory" is a regular file.
    const fs::path blockerFile = dataDir / "not_a_directory";
    {
        std::ofstream(blockerFile) << "x";
    }

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        if (!facade.initialize((dataDir / "data").string()))
        {
            return fail(2, "facade initialize failed");
        }
        camera::mock::MockCameraOptions options;
        options.folder = mockDir;
        options.frameInterval = std::chrono::milliseconds(2);
        options.loopFiles = true;
        backendApp.configureMockCamera(options);
        if (!backendApp.capture().start())
        {
            return fail(3, "capture start failed");
        }
        auto &coordinator = backendApp.experiment();
        auto &ownership = backendApp.persistenceOwnership();

        // ---- A. experiment -> manual recording ---------------------------------
        gPhase = "A.start-experiment";
        const fs::path expPath = dataDir / "experiment.h5";
        const auto started = startExperiment(coordinator, expPath.string());
        if (!started.started())
        {
            return fail(10, "experiment start failed: " + started.message);
        }
        const auto expOwner = ownership.snapshot();
        if (expOwner.owner != app::PersistenceOwner::Experiment)
        {
            return fail(11, "an active experiment must own the HDF5 writer");
        }
        // Let the run accumulate and flush before the conflicting request.
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const auto sizeBefore = fs::file_size(expPath);

        gPhase = "A.distinct-destination";
        const fs::path recPath = dataDir / "manual_recording.h5";
        std::string error;
        if (backendApp.startFrameRecording(recPath.string(), &error))
        {
            backendApp.stopFrameRecording();
            return fail(12, "manual recording started while an experiment owned the writer");
        }
        if (error.find("experiment") == std::string::npos)
        {
            return fail(13, "rejection must name the experiment conflict, got: '" + error + "'");
        }
        if (fs::exists(recPath))
        {
            return fail(14, "a rejected recording must not create its destination");
        }
        if (backendApp.frameRecordingBlockedReason().empty())
        {
            return fail(15, "frameRecordingBlockedReason must explain the conflict during an experiment");
        }

        gPhase = "A.same-destination";
        if (backendApp.startFrameRecording(expPath.string(), &error))
        {
            backendApp.stopFrameRecording();
            return fail(16, "recording onto the experiment's own file was admitted");
        }
        if (!fs::exists(expPath) || fs::file_size(expPath) < sizeBefore)
        {
            return fail(17, "same-path rejection truncated the experiment file");
        }

        gPhase = "A.invalid-destination";
        if (backendApp.startFrameRecording((blockerFile / "x.h5").string(), &error))
        {
            return fail(18, "invalid-destination recording was admitted during an experiment");
        }

        gPhase = "A.bridge-recording";
        {
            bridge::RecordingCommand cmd;
            cmd.action = bridge::RecordingCommandAction::StartFrameRecording;
            cmd.filePath = (dataDir / "bridge_recording.h5").string();
            const auto r = facade.dispatch(cmd);
            if (r.ok || r.message.find("experiment") == std::string::npos)
            {
                return fail(19, "bridge recording start must be rejected with the conflict: " + r.message);
            }
            if (fs::exists(cmd.filePath))
            {
                return fail(20, "bridge-rejected recording created its destination");
            }
        }

        gPhase = "A.bridge-review-load";
        {
            bridge::RecordingLoadCommand load;
            load.filePath = expPath.string();
            const auto r = facade.dispatch(load);
            if (r.ok)
            {
                return fail(21, "loading a file for review closed the experiment's writer");
            }
        }

        gPhase = "A.repeated-clicks";
        {
            // Repeated/concurrent clicks from several callers: none admitted.
            std::atomic<int> admitted{0};
            std::vector<std::thread> clickers;
            for (int t = 0; t < 8; ++t)
            {
                clickers.emplace_back([&, t] {
                    for (int i = 0; i < 25; ++i)
                    {
                        const auto p = dataDir / ("click_" + std::to_string(t) + "_" + std::to_string(i) + ".h5");
                        if (backendApp.startFrameRecording(p.string()))
                        {
                            admitted.fetch_add(1);
                        }
                    }
                });
            }
            for (auto &c : clickers)
            {
                c.join();
            }
            if (admitted.load() != 0)
            {
                backendApp.stopFrameRecording();
                return fail(22, "a concurrent click was admitted during the experiment");
            }
        }

        gPhase = "A.experiment-intact";
        {
            const auto now = ownership.snapshot();
            if (now.owner != app::PersistenceOwner::Experiment || now.runId != expOwner.runId)
            {
                return fail(23, "rejected requests changed the experiment's writer claim");
            }
            if (coordinator.state() != app::ExperimentRunState::Active)
            {
                return fail(24, std::string("experiment left Active: ") + app::toString(coordinator.state()));
            }
            if (!backendApp.capture().isRunning() || backendApp.isFrameRecording())
            {
                return fail(25, "rejection stopped capture or left recording flagged");
            }
            if (!backendApp.hdf5().isFileOpen())
            {
                return fail(26, "the experiment's HDF5 file was closed by a rejected request");
            }
        }
        const uint64_t committedAtRejection = coordinator.status().persistenceCommitted;
        // The run keeps persisting after the rejections.
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        gPhase = "A.finalize";
        app::ExperimentStatus final1;
        if (!stopExperimentAndWait(coordinator, final1))
        {
            return fail(27, "experiment did not finalize");
        }
        if (final1.state != app::ExperimentRunState::Idle || !final1.finalizationOk || final1.persistenceFailed != 0)
        {
            return fail(28, std::string("experiment finalization degraded: state=") + app::toString(final1.state) +
                                " ok=" + std::to_string(final1.finalizationOk) +
                                " failed=" + std::to_string(final1.persistenceFailed) + " reason=" +
                                final1.completionReason);
        }
        if (final1.persistenceCommitted < committedAtRejection)
        {
            return fail(29, "committed frames went backwards after the rejected recording");
        }
        std::cout << "experiment committed " << committedAtRejection << " frames at rejection, "
                  << final1.persistenceCommitted << " at finalization\n";
        {
            backend::services::Hdf5Service reader;
            if (!reader.loadFile(expPath.string()))
            {
                return fail(30, "finalized experiment file failed to load");
            }
            if (reader.isRecordingFile())
            {
                return fail(31, "experiment file was overwritten with a raw recording layout");
            }
            reader.closeFile();
        }
        if (!writerFree(backendApp))
        {
            return fail(32, "writer claim not released after experiment finalization");
        }

        // ---- B. manual recording -> experiment ----------------------------------
        gPhase = "B.recording-owns";
        const fs::path rec2 = dataDir / "standalone_recording.h5";
        if (!backendApp.startFrameRecording(rec2.string(), &error))
        {
            return fail(40, "standalone recording failed after the experiment: " + error);
        }
        if (ownership.snapshot().owner != app::PersistenceOwner::FrameRecording)
        {
            return fail(41, "an active recording must own the HDF5 writer");
        }
        {
            app::ExperimentStartRequest request;
            request.outputPath = (dataDir / "blocked_experiment.h5").string();
            request.readinessGeneration = coordinator.evaluateReadiness(request.outputPath).generation;
            const auto r = coordinator.start(request);
            if (r.started())
            {
                return fail(42, "experiment started while manual recording owned the writer");
            }
            if (fs::exists(request.outputPath))
            {
                return fail(43, "rejected experiment created its destination");
            }
        }
        {
            bridge::RecordingLoadCommand load;
            load.filePath = expPath.string();
            if (facade.dispatch(load).ok)
            {
                return fail(44, "review load replaced the file while recording");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        backendApp.stopFrameRecording();
        if (!writerFree(backendApp))
        {
            return fail(45, "recording did not release the writer after stop");
        }
        {
            backend::services::Hdf5Service reader;
            if (!reader.loadFile(rec2.string()) || !reader.isRecordingFile())
            {
                return fail(46, "standalone recording file unreadable");
            }
            reader.closeFile();
        }

        // Legal review load of the finished experiment still works.
        gPhase = "B.review-load";
        {
            bridge::RecordingLoadCommand load;
            load.filePath = expPath.string();
            const auto r = facade.dispatch(load);
            if (!r.ok)
            {
                return fail(47, "review load failed with no writer active: " + r.message);
            }
            if (!writerFree(backendApp))
            {
                return fail(48, "review load kept the writer claim");
            }
        }
        // A reviewed file is not a writer: recording may replace it.
        const fs::path rec3 = dataDir / "after_review.h5";
        if (!backendApp.startFrameRecording(rec3.string(), &error))
        {
            return fail(49, "recording after a review load failed: " + error);
        }
        backendApp.stopFrameRecording();
        if (!writerFree(backendApp))
        {
            return fail(50, "writer not released after the second recording");
        }

        // ---- C. concurrent starts from Idle ----------------------------------------
        gPhase = "C.race";
        int bothOwned = 0;
        int recWins = 0;
        int expWins = 0;
        for (int cycle = 0; cycle < 12; ++cycle)
        {
            const auto e = dataDir / ("race_exp_" + std::to_string(cycle) + ".h5");
            const auto r = dataDir / ("race_rec_" + std::to_string(cycle) + ".h5");
            app::ExperimentStartRequest request;
            request.outputPath = e.string();
            request.readinessGeneration = coordinator.evaluateReadiness(request.outputPath).generation;
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            bool recOk = false;
            app::ExperimentStartResult expResult;
            std::thread tRec([&] {
                ready.fetch_add(1);
                while (!go.load()) std::this_thread::yield();
                recOk = backendApp.startFrameRecording(r.string());
            });
            std::thread tExp([&] {
                ready.fetch_add(1);
                while (!go.load()) std::this_thread::yield();
                expResult = coordinator.start(request);
            });
            while (ready.load() < 2) std::this_thread::yield();
            go.store(true);
            tRec.join();
            tExp.join();
            if (recOk && expResult.started())
            {
                ++bothOwned;
            }
            recWins += recOk ? 1 : 0;
            expWins += expResult.started() ? 1 : 0;
            if (recOk)
            {
                backendApp.stopFrameRecording();
            }
            if (expResult.started())
            {
                app::ExperimentStatus s;
                if (!stopExperimentAndWait(coordinator, s))
                {
                    return fail(60, "race-cycle experiment did not finalize");
                }
            }
            if (!writerFree(backendApp))
            {
                return fail(61, "writer stuck after race cycle " + std::to_string(cycle));
            }
        }
        std::cout << "race: recording won " << recWins << ", experiment won " << expWins << "\n";
        if (bothOwned != 0)
        {
            return fail(62, "experiment and recording both owned the writer in " + std::to_string(bothOwned) +
                                " cycle(s)");
        }

        // ---- D. fault injection: failed starts leave no stuck owner ---------------
        gPhase = "D.faults";
        if (backendApp.startFrameRecording((blockerFile / "r.h5").string(), &error))
        {
            return fail(70, "recording into an unwritable destination was admitted");
        }
        if (ownership.isHeld())
        {
            return fail(71, "failed recording open left the writer claimed");
        }
        {
            const auto r = startExperiment(coordinator, (blockerFile / "e.h5").string(), 5);
            if (r.started())
            {
                return fail(72, "experiment into an unwritable destination was admitted");
            }
            if (ownership.isHeld())
            {
                return fail(73, "failed experiment start left the writer claimed");
            }
        }
        // Stale release attempts never free a live claim.
        const auto claim = ownership.tryAcquire(app::PersistenceOwner::Experiment, "test probe");
        if (!claim.granted || ownership.release(app::PersistenceOwner::FrameRecording, claim.runId) ||
            ownership.release(app::PersistenceOwner::Experiment, claim.runId + 1) || !ownership.isHeld())
        {
            return fail(74, "a mismatched release freed the claim");
        }
        if (!ownership.release(app::PersistenceOwner::Experiment, claim.runId) || ownership.isHeld())
        {
            return fail(75, "the owner's release did not free the claim");
        }
        // Legal standalone experiment still works at the end.
        const auto last = startExperiment(coordinator, (dataDir / "final_experiment.h5").string());
        if (!last.started())
        {
            return fail(76, "standalone experiment failed at the end: " + last.message);
        }
        app::ExperimentStatus s;
        if (!stopExperimentAndWait(coordinator, s) || s.state != app::ExperimentRunState::Idle)
        {
            return fail(77, "final standalone experiment did not finalize");
        }

        gPhase = "shutdown";
        facade.shutdown();
        backendApp.shutdown();
    }

    std::error_code cleanupError;
    fs::remove_all(dataDir, cleanupError);
    std::cout << "experiment_recording_exclusion_test passed\n";
    return 0;
}
