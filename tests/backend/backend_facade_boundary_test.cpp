#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    std::filesystem::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto path = std::filesystem::temp_directory_path() /
                              ("mib_backend_facade_" + std::to_string(dist(gen)));
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

    template <typename Event>
    bool hasEvent(const std::vector<backend::bridge::BackendEvent> &events)
    {
        for (const auto &event : events)
        {
            if (std::holds_alternative<Event>(event))
            {
                return true;
            }
        }
        return false;
    }

    bool waitForPlaybackFrame(backend::bridge::BackendFacade &facade,
                              backend::bridge::BackendFrame &out)
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (facade.fetchLatestFrame(out))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
} // namespace

int main()
{
    namespace bridge = backend::bridge;

    const auto dataDir = makeTempDir();
    const auto mockDir = dataDir / "mock_frames";
    std::filesystem::create_directories(mockDir);

    const cv::Mat frame(16, 16, CV_8UC1, cv::Scalar(180));
    const auto mockFramePath = mockDir / "frame_000.tiff";
    if (!cv::imwrite(mockFramePath.string(), frame))
    {
        std::cerr << "failed to write mock frame fixture\n";
        std::error_code cleanupError;
        std::filesystem::remove_all(dataDir, cleanupError);
        return 1;
    }

    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", mockDir.string().c_str());
    setEnv("MIB_MOCK_CAMERA_INTERVAL_MS", "1");
    setEnv("MIB_MOCK_CAMERA_LOOP", "true");

    const int result = [&]() -> int {
        backend::AppBackend backend;
        bridge::BackendFacade facade(backend);
        // The sink is called from backend threads too (the experiment
        // coordinator's worker publishes Stopping/terminal), so guard it.
        std::vector<bridge::BackendEvent> events;
        std::mutex eventsMutex;
        facade.setEventSink([&events, &eventsMutex](const bridge::BackendEvent &event) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.push_back(event);
        });

        if (!facade.initialize(dataDir.string()) || !facade.isInitialized())
        {
            std::cerr << "BackendFacade should initialize AppBackend explicitly\n";
            return 2;
        }

        bridge::ProcessingSettingsCommand processingCommand;
        auto config = backend.processing().getProcessingConfig();
        config.empty_frame_pixel_threshold = 12;
        processingCommand.config = config;
        processingCommand.roi = backend::services::ProcessingService::Roi{1, 2, 8, 9};
        processingCommand.realtimeEnabled = false;
        processingCommand.realtimeDropFrames = true;
        processingCommand.pixelToMicronFactor = 2.5;
        if (!facade.dispatch(processingCommand).ok)
        {
            std::cerr << "ProcessingSettingsCommand should apply through ProcessingService\n";
            return 3;
        }
        if (backend.processing().getProcessingConfig().empty_frame_pixel_threshold != 12 ||
            backend.processing().getRealtimeRoi().w != 8 ||
            !backend.processing().getRealtimeDropFrames() ||
            backend.processing().getPixelToMicronFactor() != 2.5)
        {
            std::cerr << "Processing settings were not delegated to ProcessingService\n";
            return 4;
        }

    bridge::CameraCommand configureMock;
    configureMock.action = bridge::CameraCommandAction::ConfigureMockCamera;
    configureMock.mockFrameDirectory = mockDir.string();
    configureMock.mockFrameIntervalMs = 1;
    configureMock.mockLoopFiles = true;
    if (!facade.dispatch(configureMock).ok)
    {
        std::cerr << "CameraCommand should configure mock camera through AppBackend\n";
        return 5;
    }

    bridge::CameraCommand startCapture;
    startCapture.action = bridge::CameraCommandAction::StartCapture;
    if (!facade.dispatch(startCapture).ok)
    {
        std::cerr << "CameraCommand should start capture through CaptureService\n";
        return 6;
    }

    bridge::BackendFrame latestFrame;
    if (!waitForPlaybackFrame(facade, latestFrame) || latestFrame.data.empty())
    {
        std::cerr << "BackendFacade should expose Qt-widget-free frame copies\n";
        facade.shutdown();
        return 7;
    }

    bridge::PlaybackSeekCommand seekLatest;
    seekLatest.mode = bridge::PlaybackSeekMode::Latest;
    if (!facade.dispatch(seekLatest).ok)
    {
        std::cerr << "PlaybackSeekCommand should resolve through PlaybackService\n";
        facade.shutdown();
        return 8;
    }

    const auto recordingPath = dataDir / "bridge_recording.h5";
    bridge::RecordingCommand startRecording;
    startRecording.action = bridge::RecordingCommandAction::StartFrameRecording;
    startRecording.filePath = recordingPath.string();
    if (!facade.dispatch(startRecording).ok)
    {
        std::cerr << "RecordingCommand should start frame recording through AppBackend\n";
        facade.shutdown();
        return 9;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    bridge::RecordingCommand stopRecording;
    stopRecording.action = bridge::RecordingCommandAction::StopFrameRecording;
    stopRecording.filePath = recordingPath.string();
    if (!facade.dispatch(stopRecording).ok)
    {
        std::cerr << "RecordingCommand should stop frame recording through AppBackend\n";
        facade.shutdown();
        return 10;
    }

    // Experiment lifecycle through the facade only (shared backend, #372
    // G2/G3): readiness pull -> Start -> status events -> Stop -> terminal.
    {
        bridge::ExperimentCommand evaluate;
        evaluate.action = bridge::ExperimentCommandAction::EvaluateReadiness;
        evaluate.outputPath = (dataDir / "facade_run.h5").string();
        const auto evaluated = facade.dispatch(evaluate);
        if (!evaluated.ok)
        {
            std::cerr << "ExperimentCommand EvaluateReadiness failed: " << evaluated.message << "\n";
            facade.shutdown();
            return 20;
        }
        backend::app::ExperimentReadinessSnapshot readiness;
        if (!facade.fetchExperimentReadiness(readiness, evaluate.outputPath) || !readiness.ready)
        {
            std::cerr << "fetchExperimentReadiness should be ready with a running mock camera\n";
            for (const auto &gate : readiness.gates)
            {
                std::cerr << "  " << gate.id << " " << backend::app::toString(gate.status) << " " << gate.reason << "\n";
            }
            facade.shutdown();
            return 21;
        }

        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = evaluate.outputPath;
        start.readinessGeneration = readiness.generation;
        const auto started = facade.dispatch(start);
        if (!started.ok || !started.experimentStartOutcome ||
            *started.experimentStartOutcome != backend::app::ExperimentStartOutcome::Started)
        {
            std::cerr << "ExperimentCommand Start failed: " << started.message << "\n";
            facade.shutdown();
            return 22;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        backend::app::ExperimentStatus status;
        if (!facade.fetchExperimentStatus(status) || status.state != backend::app::ExperimentRunState::Active)
        {
            std::cerr << "fetchExperimentStatus should report Active\n";
            facade.shutdown();
            return 23;
        }
        // A second Start while Active is a typed AlreadyActive, never a second run.
        const auto again = facade.dispatch(start);
        if (again.ok || !again.experimentStartOutcome ||
            *again.experimentStartOutcome != backend::app::ExperimentStartOutcome::AlreadyActive)
        {
            std::cerr << "second Start should be AlreadyActive\n";
            facade.shutdown();
            return 24;
        }

        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        const auto stopped = facade.dispatch(stop);
        if (!stopped.ok || !stopped.experimentStopOutcome ||
            *stopped.experimentStopOutcome != backend::app::ExperimentStopOutcome::Accepted)
        {
            std::cerr << "ExperimentCommand Stop not accepted: " << stopped.message << "\n";
            facade.shutdown();
            return 25;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        bool sawTerminal = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (facade.fetchExperimentStatus(status) && status.terminal)
            {
                sawTerminal = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!sawTerminal || !status.finalizationOk ||
            status.state != backend::app::ExperimentRunState::Idle ||
            status.persistenceCommitted != status.persistenceAdmitted)
        {
            std::cerr << "ExperimentCommand Stop did not finalize cleanly (terminal=" << sawTerminal
                      << " ok=" << status.finalizationOk << " state=" << backend::app::toString(status.state)
                      << " persisted=" << status.persistenceCommitted << "/" << status.persistenceAdmitted << ")\n";
            facade.shutdown();
            return 26;
        }
        const auto stoppedAgain = facade.dispatch(stop);
        if (stoppedAgain.ok || !stoppedAgain.experimentStopOutcome ||
            *stoppedAgain.experimentStopOutcome != backend::app::ExperimentStopOutcome::NotActive)
        {
            std::cerr << "Stop after finalization should be NotActive\n";
            facade.shutdown();
            return 27;
        }
        std::vector<backend::app::ExperimentRunState> seen;
        {
            std::lock_guard<std::mutex> lock(eventsMutex);
            for (const auto &event : events)
            {
                if (const auto *e = std::get_if<bridge::ExperimentStatusEvent>(&event))
                {
                    seen.push_back(e->status.state);
                }
            }
        }
        const bool sequenceOk = seen.size() >= 4 &&
                                seen[0] == backend::app::ExperimentRunState::Starting &&
                                seen[1] == backend::app::ExperimentRunState::Active &&
                                seen[2] == backend::app::ExperimentRunState::Stopping &&
                                seen.back() == backend::app::ExperimentRunState::Idle;
        if (!sequenceOk)
        {
            std::cerr << "expected ExperimentStatusEvent sequence Starting/Active/Stopping/Idle, got " << seen.size()
                      << " events\n";
            facade.shutdown();
            return 28;
        }
    }

    bridge::RecordingLoadCommand loadRecording;
    loadRecording.filePath = recordingPath.string();
    if (!facade.dispatch(loadRecording).ok)
    {
        std::cerr << "RecordingLoadCommand should load recorded HDF5 through Hdf5Service\n";
        facade.shutdown();
        return 11;
    }

    // Soft trigger on a running mock camera: the mock does not support
    // software acquisition triggering, so the command must fail cleanly
    // (ok=false + message) rather than crash or pretend success.
    bridge::CameraCommand softTriggerRunning;
    softTriggerRunning.action = bridge::CameraCommandAction::SoftTriggerCamera;
    {
        const auto result = facade.dispatch(softTriggerRunning);
        if (result.ok || result.message.empty())
        {
            std::cerr << "SoftTriggerCamera should fail with a message on an unsupported camera\n";
            facade.shutdown();
            return 12;
        }
    }

    bridge::CameraCommand stopCapture;
    stopCapture.action = bridge::CameraCommandAction::StopCapture;
    if (!facade.dispatch(stopCapture).ok)
    {
        std::cerr << "CameraCommand should stop capture through CaptureService\n";
        facade.shutdown();
        return 13;
    }

    // Soft trigger with capture stopped: must also fail cleanly.
    bridge::CameraCommand softTriggerStopped;
    softTriggerStopped.action = bridge::CameraCommandAction::SoftTriggerCamera;
    {
        const auto result = facade.dispatch(softTriggerStopped);
        if (result.ok || result.message.empty())
        {
            std::cerr << "SoftTriggerCamera should fail with a message when capture is stopped\n";
            facade.shutdown();
            return 14;
        }
    }

    facade.shutdown();
    if (facade.isInitialized())
    {
        std::cerr << "BackendFacade shutdown should make lifecycle explicit\n";
        return 15;
    }

    std::lock_guard<std::mutex> eventsLock(eventsMutex);
    if (!hasEvent<bridge::CameraStatusEvent>(events) ||
        !hasEvent<bridge::PlaybackPositionEvent>(events) ||
        !hasEvent<bridge::FrameReadyEvent>(events) ||
        !hasEvent<bridge::RecordingStatusEvent>(events) ||
        !hasEvent<bridge::ProcessingResultEvent>(events))
    {
        std::cerr << "BackendFacade should emit frontend-neutral event variants\n";
        return 16;
    }

        return 0;
    }();

    std::error_code cleanupError;
    std::filesystem::remove_all(dataDir, cleanupError);
    return result;
}
