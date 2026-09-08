#include "backend/app/BackendFacade.h"

#include "backend/app/AppBackend.h"
#include "backend/app/BackgroundFrame.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/playback/PlaybackService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/CaptureService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <type_traits>
#include <utility>

namespace backend::bridge
{
    namespace
    {
        BackendCommandType commandType(const BackendCommand &command)
        {
            return std::visit([](const auto &typedCommand) -> BackendCommandType {
                using Command = std::decay_t<decltype(typedCommand)>;
                if constexpr (std::is_same_v<Command, CameraCommand>)
                {
                    return BackendCommandType::Camera;
                }
                else if constexpr (std::is_same_v<Command, RecordingCommand>)
                {
                    return BackendCommandType::Recording;
                }
                else if constexpr (std::is_same_v<Command, ProcessingSettingsCommand>)
                {
                    return BackendCommandType::ProcessingSettings;
                }
                else if constexpr (std::is_same_v<Command, RecordingLoadCommand>)
                {
                    return BackendCommandType::RecordingLoad;
                }
                else
                {
                    return BackendCommandType::PlaybackSeek;
                }
            }, command);
        }

        FrameReadyEvent makePlaybackFrameReady(std::uint64_t frameIndex,
                                               const playback::Frame &frame)
        {
            return FrameReadyEvent{
                FrameReadySource::Playback,
                frameIndex,
                frame.timestamp,
                frame.width,
                frame.height,
                frame.pixelFormat,
                frame.linePitch,
                frame.data.size(),
            };
        }

        BackendFrame makeBackendFrame(std::uint64_t frameIndex,
                                      playback::Frame frame)
        {
            BackendFrame out;
            out.frameIndex = frameIndex;
            out.timestampNs = frame.timestamp;
            out.width = frame.width;
            out.height = frame.height;
            out.pixelFormat = frame.pixelFormat;
            out.strideBytes = frame.linePitch;
            out.data = std::move(frame.data);
            return out;
        }

        BackendErrorSource errorSourceForCommand(BackendCommandType command)
        {
            switch (command)
            {
            case BackendCommandType::Camera:
                return BackendErrorSource::Camera;
            case BackendCommandType::Recording:
            case BackendCommandType::RecordingLoad:
                return BackendErrorSource::Recording;
            case BackendCommandType::ProcessingSettings:
                return BackendErrorSource::Processing;
            case BackendCommandType::PlaybackSeek:
                return BackendErrorSource::Playback;
            }
            return BackendErrorSource::Lifecycle;
        }

        std::uint64_t atomicLoad(const std::atomic<std::uint64_t> &value)
        {
            return value.load(std::memory_order_relaxed);
        }
    } // namespace

    BackendFacade::BackendFacade(AppBackend &backend)
        : backend_(backend)
    {
    }

    BackendFacade::~BackendFacade()
    {
        backend_.setBackgroundCaptureCallback({});
    }

    bool BackendFacade::initialize(const std::string &dataDir)
    {
        if (initialized_)
        {
            return true;
        }

        if (!backend_.initialize(dataDir))
        {
            emitEvent(BackendErrorEvent{
                BackendErrorSource::Lifecycle,
                BackendCommandType::Camera,
                "Backend initialization failed",
            });
            return false;
        }

        initialized_ = true;
        emitEvent(makeCameraStatus(backend_.isCameraConfigured()
                                       ? CameraState::Configured
                                       : CameraState::Unconfigured));
        return true;
    }

    void BackendFacade::shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        backend_.stopFrameRecording();
        backend_.capture().stop();
        backend_.processing().stopRealtime();
        backend_.processing().stop();
        if (backend_.hdf5().isFileOpen())
        {
            backend_.hdf5().closeFile();
        }
        initialized_ = false;

        emitEvent(RecordingStatusEvent{
            RecordingState::Stopped,
            {},
            false,
            backend_.frameRecordingCount(),
            backend_.frameRecordingFiltered(),
            0,
            0,
        });
        emitEvent(makeCameraStatus(CameraState::Stopped));
    }

    bool BackendFacade::isInitialized() const
    {
        return initialized_;
    }

    void BackendFacade::setEventSink(EventSink sink)
    {
        bool hasSink = false;
        {
            std::scoped_lock lock(eventSinkMutex_);
            eventSink_ = std::move(sink);
            hasSink = static_cast<bool>(eventSink_);
        }

        if (!hasSink)
        {
            backend_.setBackgroundCaptureCallback({});
            return;
        }

        backend_.setBackgroundCaptureCallback([this](const BackgroundCaptureEvent &event) {
            if (event.frame.empty())
            {
                return;
            }

            emitEvent(FrameReadyEvent{
                FrameReadySource::BackgroundCapture,
                event.frameIndex,
                0,
                event.frame.width,
                event.frame.height,
                static_cast<std::uint64_t>(event.frame.pixelFormat),
                event.frame.strideBytes,
                event.frame.data.size(),
            });
        });
    }

    BackendCommandResult BackendFacade::dispatch(const BackendCommand &command)
    {
        if (!initialized_)
        {
            return lifecycleError(commandType(command), "Backend facade is not initialized");
        }

        return std::visit([this](const auto &typedCommand) {
            using Command = std::decay_t<decltype(typedCommand)>;
            if constexpr (std::is_same_v<Command, CameraCommand>)
            {
                return handleCameraCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, RecordingCommand>)
            {
                return handleRecordingCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, ProcessingSettingsCommand>)
            {
                return handleProcessingSettingsCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, RecordingLoadCommand>)
            {
                return handleRecordingLoadCommand(typedCommand);
            }
            else
            {
                return handlePlaybackSeekCommand(typedCommand);
            }
        }, command);
    }

    bool BackendFacade::fetchLatestFrame(BackendFrame &out) const
    {
        if (!initialized_)
        {
            return false;
        }

        std::uint64_t earliest = 0;
        std::uint64_t latest = 0;
        std::size_t count = 0;
        if (!backend_.playback().queryRange(earliest, latest, count))
        {
            return false;
        }

        playback::Frame frame;
        if (!backend_.playback().fetchLatest(frame))
        {
            return false;
        }

        out = makeBackendFrame(latest, std::move(frame));
        return true;
    }

    bool BackendFacade::fetchFrameByIndex(std::uint64_t frameIndex, BackendFrame &out) const
    {
        if (!initialized_)
        {
            return false;
        }

        playback::Frame frame;
        if (!backend_.playback().fetchByIndex(frameIndex, frame))
        {
            return false;
        }

        out = makeBackendFrame(frameIndex, std::move(frame));
        return true;
    }

    BackendCommandResult BackendFacade::handleCameraCommand(const CameraCommand &command)
    {
        switch (command.action)
        {
        case CameraCommandAction::ConfigureMockCamera:
        {
            camera::mock::MockCameraOptions options;
            options.folder = std::filesystem::path(command.mockFrameDirectory);
            options.frameInterval = std::chrono::milliseconds(std::max(1, command.mockFrameIntervalMs));
            options.loopFiles = command.mockLoopFiles;
            backend_.configureMockCamera(options);
            emitEvent(makeCameraStatus(CameraState::Configured, "mock"));
            return {true, BackendCommandType::Camera, "Mock camera configured"};
        }
        case CameraCommandAction::SelectHardwareCamera:
            backend_.setHardwareCameraSelection(command.hardwareInterfaceIndex,
                                                command.hardwareDeviceIndex,
                                                command.hardwareLabel);
            emitEvent(makeCameraStatus(CameraState::Configured, command.hardwareLabel));
            return {true, BackendCommandType::Camera, "Hardware camera selected"};
        case CameraCommandAction::SelectMindVisionCamera:
        {
            std::string selectError;
            if (!backend_.setMindVisionCameraSelection(command.mindVisionCameraIndex,
                                                       command.mindVisionLabel, &selectError))
            {
                const std::string message =
                    selectError.empty() ? "MindVision camera selection failed" : selectError;
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            if (!command.mindVisionConfigPath.empty())
            {
                std::string error;
                if (!backend_.applyMindVisionConfigFromFile(command.mindVisionConfigPath, &error))
                {
                    const std::string message = error.empty() ? "MindVision config apply failed" : error;
                    emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                    return {false, BackendCommandType::Camera, message};
                }
            }
            emitEvent(makeCameraStatus(CameraState::Configured, command.mindVisionLabel));
            return {true, BackendCommandType::Camera, "MindVision camera selected"};
        }
        case CameraCommandAction::ApplyCameraScript:
        {
            std::string error;
            if (!backend_.applyCameraScriptFromFile(command.cameraScriptPath, &error))
            {
                const std::string message = error.empty() ? "Camera script apply failed" : error;
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            emitEvent(makeCameraStatus(CameraState::Configured));
            return {true, BackendCommandType::Camera, "Camera script applied"};
        }
        case CameraCommandAction::SoftTriggerCamera:
        {
            std::string error;
            if (!backend_.softTriggerCamera(&error))
            {
                const std::string message = error.empty() ? "Software trigger failed" : error;
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            return {true, BackendCommandType::Camera, "Software trigger fired"};
        }
        case CameraCommandAction::ResetSelectedHardwareCamera:
        {
            std::string error;
            if (!backend_.resetSelectedHardwareCamera(&error))
            {
                const std::string message = error.empty() ? "Camera reset failed" : error;
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            emitEvent(makeCameraStatus(CameraState::Configured));
            return {true, BackendCommandType::Camera, "Camera reset requested"};
        }
        case CameraCommandAction::StartCapture:
            emitEvent(makeCameraStatus(CameraState::Starting));
            if (!backend_.capture().start())
            {
                emitEvent(makeCameraStatus(CameraState::Error));
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera,
                                            BackendCommandType::Camera,
                                            "Capture start failed"});
                return {false, BackendCommandType::Camera, "Capture start failed"};
            }
            emitEvent(makeCameraStatus(CameraState::Running));
            return {true, BackendCommandType::Camera, "Capture started"};
        case CameraCommandAction::StopCapture:
            backend_.capture().stop();
            emitEvent(makeCameraStatus(CameraState::Stopped));
            return {true, BackendCommandType::Camera, "Capture stopped"};
        }

        emitEvent(BackendErrorEvent{BackendErrorSource::Camera,
                                    BackendCommandType::Camera,
                                    "Unknown camera command"});
        return {false, BackendCommandType::Camera, "Unknown camera command"};
    }

    BackendCommandResult BackendFacade::handleRecordingCommand(const RecordingCommand &command)
    {
        switch (command.action)
        {
        case RecordingCommandAction::StartFrameRecording:
            emitEvent(RecordingStatusEvent{
                RecordingState::Starting,
                command.filePath,
                false,
                backend_.frameRecordingCount(),
                backend_.frameRecordingFiltered(),
                0,
                0,
            });
            if (!backend_.startFrameRecording(command.filePath))
            {
                emitEvent(RecordingStatusEvent{
                    RecordingState::Error,
                    command.filePath,
                    false,
                    backend_.frameRecordingCount(),
                    backend_.frameRecordingFiltered(),
                    0,
                    0,
                });
                emitEvent(BackendErrorEvent{BackendErrorSource::Recording,
                                            BackendCommandType::Recording,
                                            "Frame recording start failed"});
                return {false, BackendCommandType::Recording, "Frame recording start failed"};
            }
            emitEvent(RecordingStatusEvent{
                RecordingState::Recording,
                command.filePath,
                false,
                backend_.frameRecordingCount(),
                backend_.frameRecordingFiltered(),
                0,
                0,
            });
            return {true, BackendCommandType::Recording, "Frame recording started"};
        case RecordingCommandAction::StopFrameRecording:
            backend_.stopFrameRecording();
            emitEvent(RecordingStatusEvent{
                RecordingState::Stopped,
                command.filePath,
                false,
                backend_.frameRecordingCount(),
                backend_.frameRecordingFiltered(),
                0,
                0,
            });
            return {true, BackendCommandType::Recording, "Frame recording stopped"};
        }

        emitEvent(BackendErrorEvent{BackendErrorSource::Recording,
                                    BackendCommandType::Recording,
                                    "Unknown recording command"});
        return {false, BackendCommandType::Recording, "Unknown recording command"};
    }

    BackendCommandResult BackendFacade::handleProcessingSettingsCommand(const ProcessingSettingsCommand &command)
    {
        auto &processing = backend_.processing();
        if (command.config)
        {
            processing.setProcessingConfig(*command.config);
        }
        if (command.roi)
        {
            processing.setRealtimeRoi(*command.roi);
        }
        if (command.realtimeEnabled)
        {
            processing.setRealtimeEnabled(*command.realtimeEnabled);
        }
        if (command.realtimeDropFrames)
        {
            processing.setRealtimeDropFrames(*command.realtimeDropFrames);
        }
        if (command.realtimeProcessingMode)
        {
            processing.setRealtimeProcessingMode(*command.realtimeProcessingMode);
        }
        if (command.realtimeBatchSettings)
        {
            processing.setRealtimeBatchSettings(*command.realtimeBatchSettings);
        }
        if (command.pixelToMicronFactor)
        {
            processing.setPixelToMicronFactor(*command.pixelToMicronFactor);
        }

        emitEvent(ProcessingResultEvent{
            0,
            0,
            processing.getAlgoFps1s(),
            processing.getValidFps1s(),
            processing.getInvalidFps1s(),
            {},
        });
        return {true, BackendCommandType::ProcessingSettings, "Processing settings applied"};
    }

    BackendCommandResult BackendFacade::handleRecordingLoadCommand(const RecordingLoadCommand &command)
    {
        auto &hdf5 = backend_.hdf5();
        if (!hdf5.loadFile(command.filePath))
        {
            emitEvent(RecordingStatusEvent{
                RecordingState::Error,
                command.filePath,
                false,
                0,
                0,
                0,
                0,
            });
            emitEvent(BackendErrorEvent{BackendErrorSource::Recording,
                                        BackendCommandType::RecordingLoad,
                                        "Recording load failed"});
            return {false, BackendCommandType::RecordingLoad, "Recording load failed"};
        }

        RecordingStatusEvent event;
        event.state = RecordingState::Loaded;
        event.filePath = command.filePath;

        if (command.readMetadata)
        {
            event.recordingFile = hdf5.isRecordingFile();
            if (event.recordingFile)
            {
                std::uint64_t startTimeNs = 0;
                std::uint64_t endTimeNs = 0;
                std::uint64_t totalFrames = 0;
                std::uint64_t filteredFrames = 0;
                if (hdf5.readRecordingInfo(startTimeNs, endTimeNs, totalFrames, filteredFrames))
                {
                    event.framesWritten = totalFrames;
                    event.framesFiltered = filteredFrames;
                }

                std::vector<services::ProcessedFrame> frames;
                if (hdf5.readRecordingMetadata(frames))
                {
                    event.loadedValidFrames = frames.size();
                }
            }
            else
            {
                std::vector<services::ProcessedFrame> validFrames;
                std::vector<services::ProcessedFrame> invalidFrames;
                if (hdf5.readValidMetadata(validFrames))
                {
                    event.loadedValidFrames = validFrames.size();
                }
                if (hdf5.readInvalidMetadata(invalidFrames))
                {
                    event.loadedInvalidFrames = invalidFrames.size();
                }
            }
        }

        emitEvent(event);
        return {true, BackendCommandType::RecordingLoad, "Recording loaded"};
    }

    BackendCommandResult BackendFacade::handlePlaybackSeekCommand(const PlaybackSeekCommand &command)
    {
        auto &playback = backend_.playback();

        std::uint64_t earliest = 0;
        std::uint64_t latest = 0;
        std::size_t count = 0;
        const bool hasRange = playback.queryRange(earliest, latest, count);
        const std::uint64_t desiredIndex =
            command.mode == PlaybackSeekMode::Latest ? latest : command.frameIndex;

        playback::Frame frame;
        const bool hasFrame = hasRange && playback.fetchByIndex(desiredIndex, frame);
        PlaybackPositionEvent position;
        position.hasFrame = hasFrame;
        position.playing = playback.isPlaying();
        position.frameIndex = desiredIndex;
        position.timestampNs = hasFrame ? frame.timestamp : 0;
        position.earliestIndex = earliest;
        position.latestIndex = latest;
        position.availableCount = count;

        emitEvent(position);
        if (!hasFrame)
        {
            return {false, BackendCommandType::PlaybackSeek, "Playback frame not available"};
        }

        emitEvent(makePlaybackFrameReady(desiredIndex, frame));
        return {true, BackendCommandType::PlaybackSeek, "Playback seek resolved"};
    }

    BackendCommandResult BackendFacade::lifecycleError(BackendCommandType command, const std::string &message)
    {
        emitEvent(BackendErrorEvent{
            errorSourceForCommand(command),
            command,
            message,
        });
        return {false, command, message};
    }

    void BackendFacade::emitEvent(const BackendEvent &event) const
    {
        EventSink sink;
        {
            std::scoped_lock lock(eventSinkMutex_);
            sink = eventSink_;
        }
        if (sink)
        {
            sink(event);
        }
    }

    CameraStatusEvent BackendFacade::makeCameraStatus(CameraState state, std::string label) const
    {
        const auto &stats = backend_.capture().stats();
        return CameraStatusEvent{
            state,
            backend_.isCameraConfigured(),
            backend_.capture().isRunning(),
            atomicLoad(stats.framesProcessed),
            atomicLoad(stats.lastFrameRate),
            atomicLoad(stats.lastDataRateMBps),
            std::move(label),
        };
    }

} // namespace backend::bridge
