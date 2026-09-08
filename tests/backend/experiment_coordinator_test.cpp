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
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include <functional>

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
    const cv::Mat frame(96, 512, CV_8UC1, cv::Scalar(180));
    if (!cv::imwrite((mockDir / "frame_000.tiff").string(), frame))
    {
        std::cerr << "failed to write mock frame fixture\n";
        return 1;
    }

    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    int rc = 0;
    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);

        std::mutex eventsMutex;
        std::vector<bridge::BackendEvent> events;
        facade.setEventSink([&](const bridge::BackendEvent &event) {
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

        // Configure + start the mock camera.
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = 1;
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

        // Start the experiment.
        result = startViaFacade(facade, exp1);
        if (!result.ok || result.operationId == 0)
        {
            std::cerr << "experiment start failed: " << result.message << "\n";
            return 6;
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
            reader.closeFile();
        }

        // Cancel path: terminal status is cancelled but the file finalizes.
        const std::string exp2 = (dataDir / "exp2.h5").string();
        if (!startViaFacade(facade, exp2).ok)
        {
            std::cerr << "second experiment start failed\n";
            return 14;
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
        coordinator.onFatalSaveError("Injected: disk full while flushing");
        if (!waitFor([&] {
                return statusIsTerminal(coordinator.status(), app::ExperimentRunState::Failed);
            }, 15000))
        {
            std::cerr << "fatal error did not drive the experiment to Failed\n";
            return 33;
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
