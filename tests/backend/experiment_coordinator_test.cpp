// ExperimentCoordinator end-to-end test (BE-4, issue #274, epic #246; ported
// to the shared reliability coordinator, issue #372).
//
// Drives the backend-owned experiment lifecycle through the BackendFacade
// command surface with a mock camera: preconditions (readiness gate
// `camera.session`), readiness-generation handshake, start → periodic
// accumulation → asynchronous stop finalization, double start/stop safety,
// cancel, the fatal-save-error funnel, and idempotent shutdown during an
// active experiment (the HDF5 file must stay readable).

#include "backend/app/AppBackend.h"
#include "backend/app/ApplicationIdentity.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/WallClock.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/MonitoringDensityService.h"
#include "backend/services/CaptureService.h"

#include <functional>
#include "support/frames.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    namespace bridge = backend::bridge;
    namespace app = backend::app;

    std::filesystem::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto path = std::filesystem::temp_directory_path() /
                              ("mib_experiment_coord_" + std::to_string(dist(gen)));
            std::error_code ec;
            if (std::filesystem::create_directories(path, ec))
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

    // No naked join/wait that can hang CI: print the stuck location and
    // _Exit(99) (not abort — the crash handler intercepts abort).
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
                std::cerr << "watchdog: experiment_coordinator_test stuck — exiting\n";
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
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // Start through the facade the way a client does: preflight readiness for
    // the output path, then present that generation with the Start command.
    bridge::BackendCommandResult startViaFacade(bridge::BackendFacade &facade,
                                                const std::string &outputPath)
    {
        app::ExperimentReadinessSnapshot readiness;
        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = outputPath;
        if (facade.fetchExperimentReadiness(readiness, outputPath))
        {
            start.readinessGeneration = readiness.generation;
        }
        return facade.dispatch(start);
    }

    bool statusIsTerminal(const app::ExperimentStatus &s, app::ExperimentRunState state)
    {
        return s.terminal && s.state == state;
    }
} // namespace

int main()
{
    Watchdog watchdog(120);

    const auto dataDir = makeTempDir();
    const auto mockDir = dataDir / "mock_frames";
    std::filesystem::create_directories(mockDir);
    const bool repro = std::getenv("MIB_REPRO_BUFFER_CAP") != nullptr;
    auto reproInt = [](const char* key, int fallback) {
        const char* v = std::getenv(key);
        return v ? std::atoi(v) : fallback;
    };
    const cv::Mat frame =
        repro ? mib::test::ringFrame(512, 96, 0) : cv::Mat(96, 512, CV_8UC1, cv::Scalar(180));
    if (!cv::imwrite((mockDir / "frame_000.tiff").string(), frame))
    {
        std::cerr << "failed to write mock frame fixture\n";
        return 1;
    }

    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (dataDir / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    int rc = 0;
    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);

        std::mutex eventsMutex;
        std::vector<bridge::BackendEvent> events;
        std::atomic<int> transitionRefusals{0};
        std::atomic<bool> transitionConflictAccepted{false};
        facade.setEventSink([&](const bridge::BackendEvent& event) {
            if (const auto* status = std::get_if<bridge::ExperimentStatusEvent>(&event)) {
                if (status->status.state == app::ExperimentRunState::Starting ||
                    status->status.state == app::ExperimentRunState::Stopping) {
                    if (backendApp.startFrameRecording(
                            (dataDir / "transition-recording.h5").string()))
                        transitionConflictAccepted.store(true);
                    else
                        transitionRefusals.fetch_add(1);
                }
            }
            std::scoped_lock lock(eventsMutex);
            events.push_back(event);
        });
        auto countExperimentTerminal = [&](app::ExperimentRunState state) {
            std::scoped_lock lock(eventsMutex);
            int n = 0;
            for (const auto &event : events)
            {
                if (const auto *e = std::get_if<bridge::ExperimentStatusEvent>(&event))
                {
                    if (statusIsTerminal(e->status, state))
                    {
                        ++n;
                    }
                }
            }
            return n;
        };

        if (!facade.initialize(dataDir.string()))
        {
            std::cerr << "facade initialize failed\n";
            return 2;
        }

        // Precondition: no running camera → the camera.session gate blocks.
        const std::string exp1 = (dataDir / "exp1.h5").string();
        auto result = startViaFacade(facade, exp1);
        if (result.ok || result.message.find("camera.session") == std::string::npos)
        {
            std::cerr << "missing camera-running precondition: " << result.message << "\n";
            return 3;
        }

        if (repro) {
            auto& proc = backendApp.processing();
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
            cfg.multi_image_count = reproInt("MIB_REPRO_SERIES", 1);
            cfg.multi_image_enabled = cfg.multi_image_count > 1;
            proc.setProcessingConfig(cfg);
            proc.setRealtimeRoi({0, 0, 512, 96});
            proc.setInvalidFrameSamplingRate(1);
            proc.setFlushInterval(reproInt("MIB_REPRO_FLUSH", 100));
            proc.setMaxBufferedBytes(
                std::strtoull(std::getenv("MIB_REPRO_BUFFER_CAP"), nullptr, 10));
        }
        // Configure + start the mock camera.
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = repro ? reproInt("MIB_REPRO_INTERVAL_MS", 1) : 1;
        configure.mockLoopFiles = true;
        if (!facade.dispatch(configure).ok)
        {
            std::cerr << "mock camera configure failed\n";
            return 4;
        }
        bridge::CameraCommand startCapture;
        startCapture.action = bridge::CameraCommandAction::StartCapture;
        if (!facade.dispatch(startCapture).ok)
        {
            std::cerr << "capture start failed\n";
            return 5;
        }

        // The readiness gates (camera.session, geometry, delivery mode,
        // transport loss) settle once the mock camera delivers frames.
        if (!waitFor([&] {
                app::ExperimentReadinessSnapshot readiness;
                return facade.fetchExperimentReadiness(readiness, exp1) && readiness.ready;
            }, 10000))
        {
            app::ExperimentReadinessSnapshot readiness;
            facade.fetchExperimentReadiness(readiness, exp1);
            std::cerr << "readiness never passed with a running mock camera\n";
            for (const auto &gate : readiness.gates)
            {
                std::cerr << "  " << gate.id << " " << app::toString(gate.status) << " " << gate.reason << "\n";
            }
            return 5;
        }

        // A KDE core record offered while no run is active is ignored.
        backendApp.experiment().setLiveKdeCoreRecord("{\"idle\":true}");

        if (repro) {
            backendApp.processing().setRealtimeProcessingMode(
                backend::services::ProcessingService::RealtimeProcessingMode::Inline);
            backendApp.processing().startRealtime(backendApp.getFrameStore());
        }

        // Start the experiment.
        result = startViaFacade(facade, exp1);
        if (!result.ok || result.operationId == 0)
        {
            std::cerr << "experiment start failed: " << result.message << "\n";
            return 6;
        }
        if (repro) {
            auto& proc = backendApp.processing();
            const bool series = reproInt("MIB_REPRO_SERIES", 1) > 1;
            // Validation precedes series assembly; stop capture to freeze the
            // fixture, then Stop must hand off the owned, incomplete series.
            if (!waitFor(
                    [&] {
                        const auto a = proc.experimentAccountingSnapshot();
                        return series ? a.processed > 0 : a.persistenceCommitted > 0;
                    },
                    10000))
                return 80;
            if (std::getenv("MIB_REPRO_NO_MORE_FRAMES")) backendApp.capture().stop();
            auto stopAndCheck = [&](const std::string& path) {
                if (backendApp.experiment().requestStop(false) !=
                        app::ExperimentStopOutcome::Accepted ||
                    !waitFor([&] { return backendApp.experiment().status().terminal; }, 15000))
                    return false;
                const auto live = proc.experimentAccountingSnapshot();
                backend::services::Hdf5Service saved;
                backend::recording::RecordingAccountingSnapshot disk;
                std::vector<backend::services::ProcessedFrame> rows;
                const bool read = saved.loadFile(path) && saved.readRunAccounting(disk) &&
                                  saved.readValidFrames(rows);
                size_t records = 0, images = 0;
                int height = 0, width = 0;
                const bool partial =
                    !series ||
                    (saved.getSeriesImageInfo(records, images, height, width) && records > 0 &&
                     images > 0 && images < static_cast<size_t>(reproInt("MIB_REPRO_SERIES", 1)));
                saved.closeFile();
                return read && partial && live.persistenceCommitted > 0 &&
                       live.persistenceCancelledByPolicy == 0 &&
                       live.persistencePendingAtStop == 0 &&
                       proc.getBufferedFrameCounts().total() == 0 &&
                       disk.persistenceAdmitted == live.persistenceAdmitted &&
                       disk.persistenceCommitted == live.persistenceCommitted;
            };
            if (!stopAndCheck(exp1)) return 84;
            if (std::getenv("MIB_REPRO_RESTART")) {
                const auto next = (dataDir / "restart.h5").string();
                if (!startViaFacade(facade, next).ok ||
                    !waitFor([&] { return proc.experimentAccountingSnapshot().processed > 0; },
                             10000) ||
                    !stopAndCheck(next))
                    return 87;
            }
            facade.shutdown();
            return 0;
        }
        const auto defaultRun = backendApp.experiment().activeRun();
        if (!defaultRun || defaultRun->applicationVersion != MIB_APPLICATION_VERSION ||
            defaultRun->applicationVersion.empty() || defaultRun->applicationVersion == "unknown" ||
            defaultRun->buildId.empty() || defaultRun->buildId != MIB_APPLICATION_BUILD_ID ||
            defaultRun->operatingSystem.empty() ||
            defaultRun->operatingSystem != MIB_APPLICATION_OS) {
            std::cerr << "backend initialization did not supply application provenance\n";
            return 47;
        }
        backendApp.experiment().setApplicationIdentity("shell-version", "shell-build", "shell-os");
        const auto frozenRun = backendApp.experiment().activeRun();
        if (!frozenRun || frozenRun->applicationVersion != defaultRun->applicationVersion ||
            frozenRun->buildId != defaultRun->buildId ||
            frozenRun->operatingSystem != defaultRun->operatingSystem) {
            std::cerr << "identity override changed an already frozen run\n";
            return 48;
        }
        if (!waitFor([&] { return backendApp.processing().getBufferedFrameCounts().total() > 0; },
                     10000))
            return 53;
        const auto persistedBeforeRefusal =
            backendApp.processing().flushBufferedFrames(backendApp.hdf5());
        if (persistedBeforeRefusal == 0 || !backendApp.processing().finishFlush()) return 59;
        if (!waitFor([&] { return backendApp.processing().getBufferedFrameCounts().total() > 0; },
                     10000))
            return 60;
        const auto bufferedBeforeRefusal = backendApp.processing().getBufferedFrameCounts().total();
        // Frames that entered the experiment buffer so far: handed to the
        // writer plus still buffered. The buffered count alone is not
        // monotonic: the blank mock frames are all invalid, only every 100th
        // is buffered and the background flush drains it, so on a slow
        // (TSan) runner it sits at 0..1 and never exceeds its earlier value.
        auto enteredBuffer = [&] {
            return backendApp.processing().experimentAccountingSnapshot().persistenceAdmitted +
                   backendApp.processing().getBufferedFrameCounts().total();
        };
        const auto enteredBeforeRefusal = enteredBuffer();

        // Conflicting requests must preserve the experiment writer, including
        // a same-path request that would otherwise truncate its output.
        const auto rejectedOutput = dataDir / "rejected-recording.h5";
        for (const auto& path :
             {rejectedOutput.string(), exp1, (mockDir / "frame_000.tiff" / "record.h5").string()}) {
            if (backendApp.startFrameRecording(path) || !backendApp.hdf5().isFileOpen() ||
                backendApp.experiment().state() != app::ExperimentRunState::Active) {
                std::cerr << "raw recording replaced the experiment writer\n";
                return 40;
            }
        }
        if (std::filesystem::exists(rejectedOutput)) return 41;
        bridge::RecordingCommand conflictingRecording;
        conflictingRecording.action = bridge::RecordingCommandAction::StartFrameRecording;
        conflictingRecording.filePath = rejectedOutput.string();
        const auto refused = facade.dispatch(conflictingRecording);
        if (refused.ok || refused.message.find("experiment") == std::string::npos) return 54;
        if (!waitFor(
                [&] { return enteredBuffer() > enteredBeforeRefusal; },
                10000))
            return 55;

        // The Monitoring view pushes its latest provisional KDE core record;
        // the last one before Stop is what the file must carry.
        backendApp.experiment().setLiveKdeCoreRecord("{\"schema_version\":1,\"n\":1}");
        const std::string kdeRecord = "{\"schema_version\":1,\"n\":2,\"contours\":[]}";
        backendApp.experiment().setLiveKdeCoreRecord(kdeRecord);

        if (!backendApp.processing().isRealtimeRunning() || !backendApp.processing().isRealtimeEnabled()) {
            std::cerr << "Start must own a running enabled processing pipeline, not produce an empty shell-only run\n";
            return 30;
        }
        // Double start fails without desynchronizing.
        if (startViaFacade(facade, exp1).ok)
        {
            std::cerr << "duplicate experiment start should fail\n";
            return 7;
        }

        // Camera stop is blocked during an active experiment (Qt parity).
        bridge::CameraCommand stopCapture;
        stopCapture.action = bridge::CameraCommandAction::StopCapture;
        if (facade.dispatch(stopCapture).ok)
        {
            std::cerr << "camera stop should be blocked during an experiment\n";
            return 8;
        }

        // Stop → asynchronous finalization to a terminal Idle.
        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        if (!facade.dispatch(stop).ok)
        {
            std::cerr << "experiment stop failed\n";
            return 9;
        }
        if (!waitFor([&] {
                app::ExperimentStatus s;
                return facade.fetchExperimentStatus(s) &&
                       statusIsTerminal(s, app::ExperimentRunState::Idle);
            }, 15000))
        {
            std::cerr << "experiment did not finalize\n";
            return 10;
        }
        if (countExperimentTerminal(app::ExperimentRunState::Idle) < 1)
        {
            std::cerr << "no terminal Idle ExperimentStatus event\n";
            return 11;
        }

        if (transitionConflictAccepted.load() || transitionRefusals.load() < 2 ||
            std::filesystem::exists(dataDir / "transition-recording.h5"))
            return 61;

        // Double stop fails safely.
        if (facade.dispatch(stop).ok)
        {
            std::cerr << "duplicate experiment stop should fail\n";
            return 12;
        }

        // The finalized file is a readable experiment file with metadata.
        {
            backend::services::Hdf5Service reader;
            if (!reader.loadFile(exp1))
            {
                std::cerr << "finalized experiment file failed to load\n";
                return 13;
            }
            std::vector<backend::services::ProcessedFrame> invalid;
            if (!reader.readInvalidFrames(invalid) ||
                invalid.size() <= persistedBeforeRefusal + bufferedBeforeRefusal) {
                std::cerr << "experiment did not persist frames acquired after recording refusal\n";
                return 56;
            }
            for (const auto& saved : invalid) {
                if (saved.originalImage.empty() ||
                    cv::countNonZero(saved.originalImage != 180) != 0) {
                    std::cerr << "experiment pixels changed after recording refusal\n";
                    return 57;
                }
            }
            std::string stored;
            if (!reader.readKdeLiveJson(stored) || stored != kdeRecord)
            {
                std::cerr << "finalized file lacks the last live KDE core record (got '" << stored << "')\n";
                return 40;
            }
            reader.closeFile();
        }

        // Cancel path: terminal status is cancelled but the file finalizes.
        const std::string exp2 = (dataDir / "exp2.h5").string();
        if (!startViaFacade(facade, exp2).ok)
        {
            std::cerr << "second experiment start failed\n";
            return 14;
        }
        const auto overriddenRun = backendApp.experiment().activeRun();
        if (!overriddenRun || overriddenRun->applicationVersion != "shell-version" ||
            overriddenRun->buildId != "shell-build" ||
            overriddenRun->operatingSystem != "shell-os") {
            std::cerr << "explicit application identity did not override backend defaults\n";
            return 49;
        }
        bridge::ExperimentCommand cancel;
        cancel.action = bridge::ExperimentCommandAction::Stop;
        cancel.cancelled = true;
        if (!facade.dispatch(cancel).ok)
        {
            std::cerr << "experiment cancel failed\n";
            return 15;
        }
        app::ExperimentStatus cancelled;
        if (!waitFor([&] {
                return facade.fetchExperimentStatus(cancelled) &&
                       statusIsTerminal(cancelled, app::ExperimentRunState::Idle) &&
                       cancelled.cancelled;
            }, 15000))
        {
            std::cerr << "cancelled experiment did not finalize\n";
            return 16;
        }
        // The second run received no record: it must not inherit the first one.
        {
            backend::services::Hdf5Service reader;
            std::string stored;
            if (!reader.loadFile(exp2) || reader.readKdeLiveJson(stored))
            {
                std::cerr << "second run inherited a KDE core record or failed to load\n";
                return 41;
            }
            reader.closeFile();
        }

        // The backend density service supplies the record itself, with no
        // shell involved: synthetic cells in the monitoring ring (monitoring
        // accumulation is off, so the mock camera adds none), an estimate
        // during the run, and the finalized file carries that provisional
        // record.
        {
            for (uint64_t i = 0; i < 400; ++i)
            {
                backend::services::ProcessedFrame f;
                f.index = 900000 + i;
                f.validation.isValid = true;
                f.validation.area = 300.0 + static_cast<double>(i % 23);
                f.validation.deformability = 0.05 + 0.001 * static_cast<double>(i % 17);
                backendApp.processing().appendMonitoringFrameForTests(f);
            }
            const std::string exp3 = (dataDir / "exp3.h5").string();
            if (!startViaFacade(facade, exp3).ok)
            {
                std::cerr << "density-record experiment start failed\n";
                return 42;
            }
            auto& density = backendApp.monitoringDensity();
            const uint64_t gen0 = density.generation();
            backend::services::MonitoringDensitySettings settings;
            settings.enabled = true;
            settings.intervalMs = 60000;
            density.setSettings(settings);
            if (!waitFor([&] { return density.generation() > gen0 && !density.busy(); }, 10000))
            {
                std::cerr << "density estimate did not land during the run\n";
                return 43;
            }
            bridge::ExperimentCommand stop3;
            stop3.action = bridge::ExperimentCommandAction::Stop;
            if (!facade.dispatch(stop3).ok ||
                !waitFor([&] {
                    app::ExperimentStatus s;
                    return facade.fetchExperimentStatus(s) && statusIsTerminal(s, app::ExperimentRunState::Idle);
                }, 15000))
            {
                std::cerr << "density-record experiment did not finalize\n";
                return 44;
            }
            settings.enabled = false;
            density.setSettings(settings);
            backend::services::Hdf5Service reader;
            std::string stored, why;
            if (!reader.loadFile(exp3) || !reader.readKdeLiveJson(stored))
            {
                std::cerr << "finalized file lacks the service's live KDE core record\n";
                return 45;
            }
            reader.closeFile();
            const auto record = backend::monitoring::fromJson(stored, &why);
            if (!record || !record->provisional || record->source != "live-buffer" ||
                record->populationCount != 400 || record->contours.empty())
            {
                std::cerr << "stored service record is wrong: " << why << " " << stored.substr(0, 200) << "\n";
                return 46;
            }
        }

        // A reader/other owner must also survive a same-path recording request.
        if (!backendApp.hdf5().loadFile(exp1) || backendApp.startFrameRecording(exp1) ||
            !backendApp.hdf5().isFileOpen())
            return 47;
        backendApp.hdf5().closeFile();

        // A failed recording open must release admission for a later writer.
        if (backendApp.startFrameRecording((mockDir / "frame_000.tiff" / "record.h5").string()) ||
            backendApp.isFrameRecording() || backendApp.hdf5().isFileOpen())
            return 58;

        // Barrier-controlled competing starts: exactly one writer may win.
        for (int iteration = 0; iteration < 10; ++iteration) {
            const auto experimentPath =
                (dataDir / ("race-exp-" + std::to_string(iteration) + ".h5")).string();
            const auto recordingPath =
                (dataDir / ("race-rec-" + std::to_string(iteration) + ".h5")).string();
            auto& coordinator = backendApp.experiment();
            app::ExperimentStartRequest request;
            request.outputPath = experimentPath;
            const auto readiness = coordinator.evaluateReadiness(experimentPath);
            if (!readiness.ready) return 48;
            request.readinessGeneration = readiness.generation;
            std::atomic<int> arrived{0};
            std::atomic<bool> go{false};
            bool recordingStarted = false;
            app::ExperimentStartResult experimentStarted;
            auto barrier = [&] {
                arrived.fetch_add(1);
                while (!go.load())
                    std::this_thread::yield();
            };
            std::thread experimentThread([&] {
                barrier();
                experimentStarted = coordinator.start(request);
            });
            std::thread recordingThread([&] {
                barrier();
                recordingStarted = backendApp.startFrameRecording(recordingPath);
            });
            while (arrived.load() != 2)
                std::this_thread::yield();
            go.store(true);
            experimentThread.join();
            recordingThread.join();
            if (recordingStarted == experimentStarted.started()) {
                std::cerr << "competing starts did not admit exactly one writer\n";
                return 49;
            }
            if (recordingStarted) {
                if (std::filesystem::exists(experimentPath)) return 50;
                backendApp.stopFrameRecording();
            } else {
                if (std::filesystem::exists(recordingPath)) return 51;
                coordinator.requestStop(false);
                if (!waitFor(
                        [&] {
                            return statusIsTerminal(coordinator.status(),
                                                    app::ExperimentRunState::Idle);
                        },
                        15000))
                    return 52;
            }
        }

        // Exercise the config-json write path on the next run.
        backendApp.setLastConfigJson("{}");

        // Shutdown during an active experiment finalizes without corrupting
        // the file (idempotent close).
        const std::string exp4 = (dataDir / "exp4.h5").string();
        if (!startViaFacade(facade, exp4).ok)
        {
            std::cerr << "fourth experiment start failed\n";
            return 21;
        }
        facade.shutdown();
        {
            backend::services::Hdf5Service reader;
            if (!reader.loadFile(exp4))
            {
                std::cerr << "experiment file corrupted by shutdown\n";
                return 22;
            }
            reader.closeFile();
        }
    }

    // Facade destroyed while a run is live and shutdown() was never called:
    // AppBackend's own shutdown later finalizes the run and publishes a status.
    // The facade must have detached its status callback (which captures it)
    // so that publish never reaches freed memory (sanitizer builds catch it).
    {
        backend::AppBackend backendApp;
        std::atomic<int> eventsSeen{0};
        {
            bridge::BackendFacade facade(backendApp);
            facade.setEventSink([&](const bridge::BackendEvent&) { eventsSeen.fetch_add(1); });
            if (!facade.initialize(dataDir.string())) return 70;
            bridge::CameraCommand configure;
            configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
            configure.mockFrameDirectory = mockDir.string();
            configure.mockFrameIntervalMs = 1;
            configure.mockLoopFiles = true;
            bridge::CameraCommand startCapture;
            startCapture.action = bridge::CameraCommandAction::StartCapture;
            if (!facade.dispatch(configure).ok || !facade.dispatch(startCapture).ok) return 71;
            const std::string expTeardown = (dataDir / "exp-facade-teardown.h5").string();
            if (!waitFor([&] {
                    app::ExperimentReadinessSnapshot readiness;
                    return facade.fetchExperimentReadiness(readiness, expTeardown) && readiness.ready;
                }, 10000))
                return 72;
            if (!startViaFacade(facade, expTeardown).ok) return 73;
            if (!waitFor([&] { return backendApp.experiment().status().state ==
                                      app::ExperimentRunState::Active; }, 10000))
                return 74;
        } // ~BackendFacade without shutdown()
        const int seenAtDetach = eventsSeen.load();
        backendApp.shutdown(); // finalizes the run; must not call the dead facade
        if (eventsSeen.load() != seenAtDetach)
        {
            std::cerr << "status published to a destroyed facade\n";
            return 75;
        }
    }

    // Fatal save-error funnel (direct coordinator): the experiment
    // transitions to Failed and finalizes without corrupting the file.
    {
        backend::AppBackend backendApp;
        if (!backendApp.initialize((dataDir / "fatal_data").string()))
        {
            std::cerr << "backend initialize failed (fatal path)\n";
            return 30;
        }
        camera::mock::MockCameraOptions options;
        options.folder = mockDir;
        options.frameInterval = std::chrono::milliseconds(1);
        options.loopFiles = true;
        backendApp.configureMockCamera(options);
        if (!backendApp.capture().start())
        {
            std::cerr << "capture start failed (fatal path)\n";
            return 31;
        }

        app::ExperimentCoordinator &coordinator = backendApp.experiment();
        const std::string expFatal = (dataDir / "exp_fatal.h5").string();
        // Preflight → start handshake. The gates settle as the mock camera's
        // first frames arrive and each change bumps the readiness generation,
        // so re-run the preflight while the coordinator reports NotReady or
        // StaleReadiness (bounded, like a client would).
        app::ExperimentStartResult started;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            app::ExperimentStartRequest request;
            request.outputPath = expFatal;
            request.readinessGeneration = coordinator.evaluateReadiness(expFatal).generation;
            started = coordinator.start(request);
            if (started.started() ||
                (started.outcome != app::ExperimentStartOutcome::NotReady &&
                 started.outcome != app::ExperimentStartOutcome::StaleReadiness))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!started.started())
        {
            std::cerr << "fatal-path experiment start failed: " << started.message << "\n";
            return 32;
        }
        // G14: the run holds the wall clock from its start stamp until it has stamped and persisted its end,
        // so a resync cannot move the end's offset, also while a failed run drains its writers.
        if (app::WallClock::status().holds != 1)
        {
            std::cerr << "an active run must hold the wall clock\n";
            return 135;
        }
        coordinator.onFatalSaveError("Injected: disk full while flushing");
        if (!waitFor([&] {
                return statusIsTerminal(coordinator.status(), app::ExperimentRunState::Failed);
            }, 15000))
        {
            std::cerr << "fatal error did not drive the experiment to Failed\n";
            return 33;
        }
        if (app::WallClock::status().holds != 0)
        {
            std::cerr << "the wall clock must be released at the failed run's terminal status\n";
            return 136;
        }
        {
            const auto s = coordinator.status();
            if (s.faultMessage.find("Injected") == std::string::npos &&
                s.message.find("Injected") == std::string::npos)
            {
                std::cerr << "fatal message lost: '" << s.faultMessage << "' / '" << s.message << "'\n";
                return 34;
            }
        }
        // The file was still finalized (data flushed, closed) and reopens.
        {
            backend::services::Hdf5Service reader;
            if (!reader.loadFile(expFatal))
            {
                std::cerr << "fatal-path experiment file failed to load\n";
                return 35;
            }
            reader.closeFile();
        }
        coordinator.shutdown(); // idempotent after Failed
        backendApp.capture().stop();
        backendApp.shutdown();
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(dataDir, cleanupError);
    std::cout << "experiment_coordinator_test passed\n";
    return rc;
}
