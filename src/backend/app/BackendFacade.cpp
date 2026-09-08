#include "backend/app/BackendFacade.h"

#include "backend/app/ExperimentCoordinator.h"

#include "backend/app/AppBackend.h"
#include "backend/app/BackgroundFrame.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/recording/ReviewExport.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <thread>
#include "backend/playback/FrameStore.h"
#include "backend/playback/PlaybackService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/app/Tools.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/SyringePumpService.h"
#include "backend/services/TriggerService.h"

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
                else if constexpr (std::is_same_v<Command, PlaybackSeekCommand>)
                {
                    return BackendCommandType::PlaybackSeek;
                }
                else if constexpr (std::is_same_v<Command, OperationCommand>)
                {
                    return BackendCommandType::Operation;
                }
                else if constexpr (std::is_same_v<Command, ExperimentCommand>)
                {
                    return BackendCommandType::Experiment;
                }
                else if constexpr (std::is_same_v<Command, MonitoringCommand>)
                {
                    return BackendCommandType::Monitoring;
                }
                else if constexpr (std::is_same_v<Command, TriggerCommand>)
                {
                    return BackendCommandType::Trigger;
                }
                else if constexpr (std::is_same_v<Command, ReviewCommand>)
                {
                    return BackendCommandType::Review;
                }
                else if constexpr (std::is_same_v<Command, PumpCommand>)
                {
                    return BackendCommandType::Pump;
                }
                else
                {
                    return BackendCommandType::Autofocus;
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
            case BackendCommandType::Operation:
                return BackendErrorSource::Lifecycle;
            case BackendCommandType::Experiment:
                return BackendErrorSource::Experiment;
            case BackendCommandType::Monitoring:
                return BackendErrorSource::Monitoring;
            case BackendCommandType::Trigger:
                return BackendErrorSource::Hardware;
            case BackendCommandType::Review:
                return BackendErrorSource::Review;
            case BackendCommandType::Pump:
            case BackendCommandType::Autofocus:
                return BackendErrorSource::Hardware;
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

        // Shared experiment coordinator (issue #372): forward its status
        // stream as ExperimentStatusEvent, tie the running experiment to a
        // tracked operation (BE-1), and surface fatal save errors as events
        // (AppBackend already funnels them into the coordinator).
        backend_.experiment().setStatusCallback([this](const app::ExperimentStatus &status) {
            emitEvent(ExperimentStatusEvent{status});
            if (status.terminal)
            {
                if (const std::uint64_t opId = experimentOperationId_.exchange(0))
                {
                    finishOperation(opId,
                                    status.state == app::ExperimentRunState::Failed
                                        ? BackendOperationState::Failed
                                        : (status.cancelled ? BackendOperationState::Cancelled
                                                            : BackendOperationState::Completed),
                                    status.completionReason.empty() ? status.message : status.completionReason);
                }
            }
        });
        backend_.setFatalSaveErrorCallback([this](const std::string &message) {
            // Fires on writer threads: emit only, never join here.
            emitEvent(BackendErrorEvent{BackendErrorSource::Experiment,
                                        BackendCommandType::Experiment,
                                        message});
        });

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

        // Finish/abort an active experiment first (finalizes the HDF5 file so
        // shutdown never corrupts it — BE-4), then drain/cancel the remaining
        // tracked operations so their consumers see a terminal event before
        // service teardown (BE-1 shutdown policy).
        backend_.experiment().shutdown();
        backend_.experiment().setStatusCallback({});
        cancelAllOperations("Backend shutdown");
        // Review export jobs observe their (now set) cancel flags and clean
        // partial outputs; join them so no callback fires after destruction.
        {
            std::vector<std::thread> jobs;
            {
                std::scoped_lock lock(reviewJobsMutex_);
                jobs.swap(reviewJobThreads_);
            }
            for (auto &job : jobs)
            {
                if (job.joinable())
                {
                    job.join();
                }
            }
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
            else if constexpr (std::is_same_v<Command, PlaybackSeekCommand>)
            {
                return handlePlaybackSeekCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, OperationCommand>)
            {
                return handleOperationCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, ExperimentCommand>)
            {
                return handleExperimentCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, MonitoringCommand>)
            {
                return handleMonitoringCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, TriggerCommand>)
            {
                return handleTriggerCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, ReviewCommand>)
            {
                return handleReviewCommand(typedCommand);
            }
            else if constexpr (std::is_same_v<Command, PumpCommand>)
            {
                return handlePumpCommand(typedCommand);
            }
            else
            {
                return handleAutofocusCommand(typedCommand);
            }
        }, command);
    }

    bool BackendFacade::fetchExperimentReadiness(app::ExperimentReadinessSnapshot &out,
                                                 const std::string &outputPath,
                                                 const std::string &profileId) const
    {
        if (!initialized_)
        {
            return false;
        }
        out = backend_.experiment().evaluateReadiness(outputPath, profileId);
        return true;
    }

    bool BackendFacade::fetchExperimentStatus(app::ExperimentStatus &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        out = backend_.experiment().status();
        return true;
    }

    BackendCommandResult BackendFacade::handleExperimentCommand(const ExperimentCommand &command)
    {
        BackendCommandResult result;
        result.command = BackendCommandType::Experiment;
        auto &coordinator = backend_.experiment();
        switch (command.action)
        {
        case ExperimentCommandAction::EvaluateReadiness:
        {
            const auto readiness = coordinator.evaluateReadiness(command.outputPath, command.profileId);
            result.ok = true;
            result.message = readiness.ready ? "ready" : "not ready";
            if (!readiness.ready)
            {
                for (const auto &id : readiness.blockingGateIds())
                {
                    result.message += (result.message == "not ready" ? ": " : ", ") + id;
                }
            }
            return result;
        }
        case ExperimentCommandAction::Start:
        {
            app::ExperimentStartRequest request;
            request.outputPath = command.outputPath;
            request.readinessGeneration = command.readinessGeneration;
            request.profileId = command.profileId;
            request.acknowledgeLatestFrameDrops = command.acknowledgeLatestFrameDrops;
            // Track the operation before Start so a run that fails on the
            // coordinator's worker right after Start (fatal save error) still
            // finds its id in the status callback and gets a terminal
            // OperationStatus (review, 2026-09-08). A refused Start closes it
            // as Failed here.
            const std::uint64_t opId =
                beginOperation(BackendOperationKind::Experiment, nullptr, command.outputPath);
            // Only claim the slot when no experiment operation is live: a
            // duplicate Start while Active must not steal (or later clear)
            // the running experiment's id.
            std::uint64_t noLiveOperation = 0;
            const bool claimed = experimentOperationId_.compare_exchange_strong(noLiveOperation, opId);
            const auto started = coordinator.start(request);
            result.experimentStartOutcome = started.outcome;
            result.ok = started.started();
            result.message = started.message;
            if (!result.ok)
            {
                if (claimed)
                {
                    std::uint64_t ours = opId;
                    experimentOperationId_.compare_exchange_strong(ours, 0);
                }
                finishOperation(opId, BackendOperationState::Failed, started.message);
                emitEvent(BackendErrorEvent{BackendErrorSource::Experiment, BackendCommandType::Experiment,
                                            started.message});
                return result;
            }
            if (!claimed)
            {
                // Started although another id was live: adopt this one.
                experimentOperationId_.store(opId);
            }
            result.operationId = opId;
            return result;
        }
        case ExperimentCommandAction::Stop:
        {
            const auto outcome = coordinator.requestStop(command.cancelled);
            result.experimentStopOutcome = outcome;
            result.ok = outcome == app::ExperimentStopOutcome::Accepted;
            result.message = app::toString(outcome);
            result.operationId = experimentOperationId_.load();
            return result;
        }
        case ExperimentCommandAction::Status:
        {
            const auto status = coordinator.status();
            emitEvent(ExperimentStatusEvent{status});
            result.ok = true;
            result.message = app::toString(status.state);
            result.operationId = experimentOperationId_.load();
            return result;
        }
        }
        return lifecycleError(BackendCommandType::Experiment, "unknown experiment action");
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

    bool BackendFacade::fetchProcessingStats(BackendProcessingStats &out) const
    {
        if (!initialized_)
        {
            return false;
        }

        auto &processing = backend_.processing();
        out.algoFps1s = processing.getAlgoFps1s();
        out.validFps1s = processing.getValidFps1s();
        out.invalidFps1s = processing.getInvalidFps1s();
        out.pixelToMicronFactor = processing.getPixelToMicronFactor();
        return true;
    }

    BackendCommandResult BackendFacade::handleCameraCommand(const CameraCommand &command)
    {
        switch (command.action)
        {
        case CameraCommandAction::ConfigureMockCamera:
        {
            ::camera::mock::MockCameraOptions options;
            options.folder = std::filesystem::path(command.mockFrameDirectory);
            options.frameInterval = std::chrono::milliseconds(std::max(1, command.mockFrameIntervalMs));
            options.loopFiles = command.mockLoopFiles;
            backend_.configureMockCamera(options);
            emitEvent(makeCameraStatus(CameraState::Configured, "mock"));
            return {true, BackendCommandType::Camera, "Mock camera configured"};
        }
        case CameraCommandAction::SelectHardwareCamera:
            // Structured validation (BE-2): invalid indices are rejected here
            // instead of failing deep inside capture start.
            if (command.hardwareInterfaceIndex < 0 || command.hardwareDeviceIndex < 0)
            {
                const std::string message = "Invalid hardware camera indices";
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            backend_.setHardwareCameraSelection(command.hardwareInterfaceIndex,
                                                command.hardwareDeviceIndex,
                                                command.hardwareLabel);
            emitEvent(makeCameraStatus(CameraState::Configured, command.hardwareLabel));
            return {true, BackendCommandType::Camera, "Hardware camera selected"};
        case CameraCommandAction::SelectMindVisionCamera:
        {
            if (command.mindVisionCameraIndex < 0)
            {
                const std::string message = "Invalid MindVision camera index";
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera, BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
            backend_.setMindVisionCameraSelection(command.mindVisionCameraIndex,
                                                 command.mindVisionLabel);
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
            // Qt-parity precondition: the camera cannot stop underneath an
            // active experiment — stop the experiment first (BE-4).
            if (backend_.experiment().state() == app::ExperimentRunState::Active)
            {
                const std::string message =
                    "Cannot stop camera while experiment is active. Please stop the experiment first.";
                emitEvent(BackendErrorEvent{BackendErrorSource::Camera,
                                            BackendCommandType::Camera, message});
                return {false, BackendCommandType::Camera, message};
            }
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
        if (command.configJson)
        {
            // Merge-apply (BE-3): parse against the current config so absent
            // keys keep their values; a malformed document fails the whole
            // command without touching state.
            nlohmann::json doc;
            try
            {
                doc = nlohmann::json::parse(*command.configJson);
            }
            catch (const nlohmann::json::exception &e)
            {
                const std::string message = std::string("Invalid config JSON: ") + e.what();
                emitEvent(BackendErrorEvent{BackendErrorSource::ConfigCore,
                                            BackendCommandType::ProcessingSettings, message});
                return {false, BackendCommandType::ProcessingSettings, message};
            }

            services::ProcessingConfig merged = processing.getProcessingConfig();
            std::string error;
            const auto image = doc.contains("image_processing") ? doc["image_processing"] : doc;
            if (!processing::config_json::fromJson(image, merged, &error))
            {
                emitEvent(BackendErrorEvent{BackendErrorSource::ConfigCore,
                                            BackendCommandType::ProcessingSettings, error});
                return {false, BackendCommandType::ProcessingSettings, error};
            }
            processing.setProcessingConfig(merged);

            if (const auto rt = doc.find("realtime_processing"); rt != doc.end())
            {
                if (const auto mode = rt->find("mode"); mode != rt->end() && mode->is_string())
                {
                    processing.setRealtimeProcessingMode(
                        mode->get<std::string>() == "async_batch"
                            ? services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                            : services::ProcessingService::RealtimeProcessingMode::Inline);
                }
                auto batch = processing.getRealtimeBatchSettings();
                if (const auto v = rt->find("batch_size"); v != rt->end())
                    batch.batchSize = v->get<std::size_t>();
                if (const auto v = rt->find("max_queued_frames"); v != rt->end())
                    batch.maxQueuedFrames = v->get<std::size_t>();
                if (const auto v = rt->find("worker_count"); v != rt->end())
                    batch.workerCount = v->get<std::size_t>();
                if (const auto v = rt->find("max_batch_delay_ms"); v != rt->end())
                    batch.maxBatchDelayMs = v->get<int>();
                processing.setRealtimeBatchSettings(batch);
                if (const auto v = rt->find("drop_frames"); v != rt->end())
                    processing.setRealtimeDropFrames(v->get<bool>());
            }
            if (const auto v = doc.find("flush_interval"); v != doc.end())
                processing.setFlushInterval(v->get<std::size_t>());
            if (const auto v = doc.find("pixel_to_micron"); v != doc.end())
                processing.setPixelToMicronFactor(v->get<double>());
            if (const auto roi = doc.find("roi"); roi != doc.end() && roi->is_object())
            {
                services::ProcessingService::Roi r{};
                r.x = roi->value("x", 0);
                r.y = roi->value("y", 0);
                r.w = roi->value("w", 0);
                r.h = roi->value("h", 0);
                processing.setRealtimeRoi(r);
            }
        }
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
        if (command.flushInterval)
        {
            processing.setFlushInterval(*command.flushInterval);
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
        // The review file cannot replace the HDF5 handle underneath an
        // active experiment (they share the service).
        if (backend_.experiment().state() == app::ExperimentRunState::Active)
        {
            const std::string message = "Cannot load a recording while an experiment is active";
            emitEvent(BackendErrorEvent{BackendErrorSource::Review,
                                        BackendCommandType::RecordingLoad, message});
            return {false, BackendCommandType::RecordingLoad, message};
        }

        // Tracked as an operation (BE-1): synchronous today, but the shell
        // already correlates Started/terminal events by operationId so the
        // load can move off-thread without a contract change.
        const std::uint64_t operationId =
            beginOperation(BackendOperationKind::RecordingLoad, nullptr, command.filePath);

        auto &hdf5 = backend_.hdf5();
        // Loading replaces the currently reviewed file (Qt parity: selecting
        // a new HDF file closes the previous one).
        if (hdf5.isFileOpen())
        {
            hdf5.closeFile();
        }
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
            finishOperation(operationId, BackendOperationState::Failed, "Recording load failed");
            return {false, BackendCommandType::RecordingLoad, "Recording load failed", operationId};
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
        {
            // Remember the loaded path for review jobs and invalidate the
            // paged-metrics cache (BE-6).
            std::scoped_lock lock(reviewMutex_);
            loadedRecordingPath_ = command.filePath;
            reviewMetricsLoaded_ = false;
            reviewValidMeta_.clear();
            reviewInvalidMeta_.clear();
        }
        finishOperation(operationId, BackendOperationState::Completed, command.filePath);
        return {true, BackendCommandType::RecordingLoad, "Recording loaded", operationId};
    }

    BackendCommandResult BackendFacade::handleOperationCommand(const OperationCommand &command)
    {
        switch (command.action)
        {
        case OperationCommandAction::Cancel:
            if (requestOperationCancel(command.operationId))
            {
                return {true, BackendCommandType::Operation, "Operation cancel requested",
                        command.operationId};
            }
            // Unknown or already-finished IDs fail safely without touching
            // state — duplicate cancels cannot desynchronize anything.
            return {false, BackendCommandType::Operation,
                    "Unknown or finished operation", command.operationId};
        }

        return {false, BackendCommandType::Operation, "Unknown operation command"};
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

    BackendCommandResult BackendFacade::handleMonitoringCommand(const MonitoringCommand &command)
    {
        auto &processing = backend_.processing();
        switch (command.action)
        {
        case MonitoringCommandAction::Enable:
            processing.setMonitoringActive(true);
            return {true, BackendCommandType::Monitoring, "Monitoring enabled"};
        case MonitoringCommandAction::Disable:
            // Hiding Monitoring must stop accumulation and the per-frame image
            // clones (the append path is gated on the active flag).
            processing.setMonitoringActive(false);
            return {true, BackendCommandType::Monitoring, "Monitoring disabled"};
        case MonitoringCommandAction::Clear:
            processing.clearMonitoringFrames();
            return {true, BackendCommandType::Monitoring, "Monitoring buffers cleared"};
        }
        return {false, BackendCommandType::Monitoring, "Unknown monitoring command"};
    }

    BackendCommandResult BackendFacade::handleTriggerCommand(const TriggerCommand &command)
    {
        auto &trigger = backend_.trigger();
        switch (command.action)
        {
        case TriggerCommandAction::SetPulseDuration:
            if (command.pulseDurationUs < 1)
            {
                return {false, BackendCommandType::Trigger, "Pulse duration must be >= 1 us"};
            }
            trigger.setPulseDurationUs(command.pulseDurationUs);
            return {true, BackendCommandType::Trigger, "Pulse duration set"};
        case TriggerCommandAction::ManualPulse:
            if (!trigger.hasCamera())
            {
                return {false, BackendCommandType::Trigger,
                        "No camera attached for trigger output"};
            }
            trigger.manualPulse();
            return {true, BackendCommandType::Trigger, "Manual pulse requested"};
        case TriggerCommandAction::StartPeriodicTest:
            if (command.periodicIntervalMs < 1)
            {
                return {false, BackendCommandType::Trigger,
                        "Periodic interval must be >= 1 ms"};
            }
            if (!trigger.hasCamera())
            {
                return {false, BackendCommandType::Trigger,
                        "No camera attached for trigger output"};
            }
            trigger.startPeriodicTest(command.periodicIntervalMs);
            return {true, BackendCommandType::Trigger, "Periodic test started"};
        case TriggerCommandAction::StopPeriodicTest:
            trigger.stopPeriodicTest();
            return {true, BackendCommandType::Trigger, "Periodic test stopped"};
        }
        return {false, BackendCommandType::Trigger, "Unknown trigger command"};
    }

    bool BackendFacade::fetchMonitoringSnapshot(BackendMonitoringSnapshot &out,
                                                std::size_t maxRows) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &processing = backend_.processing();
        const auto validFrames = processing.getMonitoringValidFrames();
        const auto invalidFrames = processing.getMonitoringInvalidFrames();

        out = BackendMonitoringSnapshot{};
        out.monitoringActive = processing.isMonitoringActive();
        out.validHeld = validFrames.size();
        out.invalidHeld = invalidFrames.size();
        out.validAppended = processing.getMonitoringValidAppended();
        out.invalidAppended = processing.getMonitoringInvalidAppended();
        out.capacity = services::ProcessingService::getMonitoringCapacity();

        auto appendRow = [&out](const services::ProcessedFrame &frame) {
            const auto &v = frame.validation;
            MonitoringObjectRow row;
            row.frameIndex = frame.index;
            row.timestampNs = frame.timestampNs;
            row.valid = v.isValid;
            row.targetGroup = v.isTargetGroup;
            row.objectId = v.objectId;
            row.objectCount = v.objectCount;
            row.trackId = v.trackId;
            row.centroidX = v.centroidX;
            row.centroidY = v.centroidY;
            row.area = v.area;
            row.deformability = v.deformability;
            row.areaRatio = v.areaRatio;
            row.ringRatio = v.ringRatio;
            row.youngsModulus = v.youngsModulus;
            out.rows.push_back(row);
            out.latestTimestampNs = std::max(out.latestTimestampNs, frame.timestampNs);
        };

        // Most-recent rows first: take tails of both buffers, bounded by
        // maxRows overall (valid rows favored to match the chart emphasis).
        const std::size_t takeValid = std::min(validFrames.size(), maxRows);
        for (std::size_t i = validFrames.size() - takeValid; i < validFrames.size(); ++i)
        {
            appendRow(validFrames[i]);
        }
        const std::size_t remaining = maxRows > takeValid ? maxRows - takeValid : 0;
        const std::size_t takeInvalid = std::min(invalidFrames.size(), remaining);
        for (std::size_t i = invalidFrames.size() - takeInvalid; i < invalidFrames.size(); ++i)
        {
            appendRow(invalidFrames[i]);
        }
        return true;
    }

    bool BackendFacade::fetchTriggerStatus(BackendTriggerStatus &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &trigger = backend_.trigger();
        out.cameraAttached = trigger.hasCamera();
        out.pulseDurationUs = trigger.getPulseDurationUs();
        out.triggerCount = trigger.getTriggerCount();
        out.lastOnsetUs = trigger.getLastOnsetUs();
        out.lastObjectId = trigger.getLastTriggerObjectId();
        out.lastTrackId = trigger.getLastTriggerTrackId();
        out.periodicActive = trigger.isPeriodicTestActive();
        out.periodicIntervalMs = trigger.getPeriodicTestIntervalMs();
        return true;
    }

    bool BackendFacade::fetchProcessingConfigJson(std::string &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &processing = backend_.processing();
        const auto batch = processing.getRealtimeBatchSettings();
        const auto roi = processing.getRealtimeRoi();
        nlohmann::json doc{
            {"config_version", processing.getConfigVersion()},
            {"image_processing", processing::config_json::toJson(processing.getProcessingConfig())},
            {"realtime_processing",
             {
                 {"enabled", processing.isRealtimeEnabled()},
                 {"mode", processing.getRealtimeProcessingMode() ==
                                  services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                              ? "async_batch"
                              : "inline"},
                 {"batch_size", batch.batchSize},
                 {"max_queued_frames", batch.maxQueuedFrames},
                 {"worker_count", batch.workerCount},
                 {"max_batch_delay_ms", batch.maxBatchDelayMs},
                 {"drop_frames", processing.getRealtimeDropFrames()},
             }},
            {"flush_interval", processing.getFlushInterval()},
            {"pixel_to_micron", processing.getPixelToMicronFactor()},
            {"roi", {{"x", roi.x}, {"y", roi.y}, {"w", roi.w}, {"h", roi.h}}},
            {"background_set", !processing.getRealtimeBackgroundGray().empty()},
        };
        out = doc.dump();
        return true;
    }

    bool BackendFacade::fetchProcessingCoreStatus(BackendProcessingCoreStatus &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &processing = backend_.processing();
        const auto identity = processing.activeProcessingCoreIdentity();
        out.activeVersion = identity.version;
        out.contractVersion = identity.contractVersion;
        out.engineAbiVersion = identity.engineAbiVersion;
        out.source = identity.source;
        out.releaseTag = identity.releaseTag;
        out.buildId = identity.buildId;
        out.artifactSha256 = identity.artifactSha256;
        out.requiredVersion = processing.requiredProcessingCoreVersion();
        out.pinSatisfied = processing.isProcessingCorePinSatisfied();
        return true;
    }

    namespace
    {
        const char *reviewDatasetPath(ReviewImageDataset dataset)
        {
            switch (dataset)
            {
            case ReviewImageDataset::ValidImage:
                return "/valid_frames/images";
            case ReviewImageDataset::InvalidImage:
                return "/invalid_frames/images";
            case ReviewImageDataset::RecordedImage:
                return "/recorded_frames/images";
            case ReviewImageDataset::ValidMask:
                return "/valid_frames/masks";
            case ReviewImageDataset::InvalidMask:
                return "/invalid_frames/masks";
            }
            return nullptr;
        }

        MonitoringObjectRow metricsRowFromFrame(const services::ProcessedFrame &frame)
        {
            const auto &v = frame.validation;
            MonitoringObjectRow row;
            row.frameIndex = frame.index;
            row.timestampNs = frame.timestampNs;
            row.valid = v.isValid;
            row.targetGroup = v.isTargetGroup;
            row.objectId = v.objectId;
            row.objectCount = v.objectCount;
            row.trackId = v.trackId;
            row.centroidX = v.centroidX;
            row.centroidY = v.centroidY;
            row.area = v.area;
            row.deformability = v.deformability;
            row.areaRatio = v.areaRatio;
            row.ringRatio = v.ringRatio;
            row.youngsModulus = v.youngsModulus;
            return row;
        }
    } // namespace

    bool BackendFacade::fetchReviewMetadata(BackendReviewMetadata &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &hdf5 = backend_.hdf5();
        out = BackendReviewMetadata{};
        out.fileOpen = hdf5.isFileOpen();
        if (!out.fileOpen)
        {
            return true;
        }
        {
            std::scoped_lock lock(reviewMutex_);
            out.filePath = loadedRecordingPath_;
        }
        out.recordingFile = hdf5.isRecordingFile();

        if (out.recordingFile)
        {
            std::uint64_t total = 0;
            std::uint64_t filtered = 0;
            hdf5.readRecordingInfo(out.startTimeNs, out.endTimeNs, total, filtered);
            out.totalValid = total;
        }
        else
        {
            std::size_t totalValid = 0;
            std::size_t totalInvalid = 0;
            hdf5.readExperimentInfo(out.startTimeNs, out.endTimeNs, totalValid, totalInvalid, &out.roi);
            out.totalValid = totalValid;
            out.totalInvalid = totalInvalid;
        }

        backend::processing::ProcessingCoreIdentity identity;
        if (hdf5.readProcessingCoreIdentity(identity))
        {
            out.hasCoreIdentity = true;
            out.coreVersion = identity.version;
            out.coreSource = identity.source;
            out.coreReleaseTag = identity.releaseTag;
        }
        {
            cv::Mat bg;
            out.hasBackground = hdf5.readBackgroundImage(bg) && !bg.empty();
        }

        auto fillInfo = [&hdf5](const char *path, BackendReviewDatasetInfo &info) {
            std::size_t count = 0;
            info.present = hdf5.getDatasetInfo(path, count, info.height, info.width, info.channels);
            info.count = count;
        };
        fillInfo("/valid_frames/images", out.validImages);
        fillInfo("/invalid_frames/images", out.invalidImages);
        fillInfo("/valid_frames/masks", out.validMasks);
        fillInfo("/invalid_frames/masks", out.invalidMasks);
        fillInfo("/recorded_frames/images", out.recordedImages);
        return true;
    }

    bool BackendFacade::fetchReviewMetricsPage(bool valid,
                                               std::uint64_t offset,
                                               std::uint64_t count,
                                               std::vector<MonitoringObjectRow> &rows,
                                               std::uint64_t &totalOut) const
    {
        if (!initialized_ || !backend_.hdf5().isFileOpen())
        {
            return false;
        }

        std::scoped_lock lock(reviewMutex_);
        if (!reviewMetricsLoaded_)
        {
            // One metadata-only read per loaded file (no image payloads) —
            // pages are then served from the cache with bounded IPC size.
            auto &hdf5 = backend_.hdf5();
            reviewValidMeta_.clear();
            reviewInvalidMeta_.clear();
            if (hdf5.isRecordingFile())
            {
                hdf5.readRecordingMetadata(reviewValidMeta_);
            }
            else
            {
                hdf5.readValidMetadata(reviewValidMeta_);
                hdf5.readInvalidMetadata(reviewInvalidMeta_);
            }
            reviewMetricsLoaded_ = true;
        }

        const auto &source = valid ? reviewValidMeta_ : reviewInvalidMeta_;
        totalOut = source.size();
        rows.clear();
        if (offset >= source.size())
        {
            return true;
        }
        const std::uint64_t end = std::min<std::uint64_t>(source.size(), offset + count);
        rows.reserve(end - offset);
        for (std::uint64_t i = offset; i < end; ++i)
        {
            rows.push_back(metricsRowFromFrame(source[i]));
        }
        return true;
    }

    bool BackendFacade::fetchReviewImage(ReviewImageDataset dataset,
                                         std::uint64_t index,
                                         BackendFrame &out) const
    {
        if (!initialized_ || !backend_.hdf5().isFileOpen())
        {
            return false;
        }
        const char *path = reviewDatasetPath(dataset);
        if (!path)
        {
            return false;
        }
        cv::Mat image;
        if (!backend_.hdf5().readImageByIndex(path, index, image) || image.empty())
        {
            return false;
        }
        cv::Mat gray;
        if (image.channels() == 1)
        {
            gray = image;
        }
        else
        {
            cv::extractChannel(image, gray, 0);
        }
        out = BackendFrame{};
        out.frameIndex = index;
        out.width = static_cast<std::uint64_t>(gray.cols);
        out.height = static_cast<std::uint64_t>(gray.rows);
        out.pixelFormat = 0;
        out.strideBytes = static_cast<std::size_t>(gray.cols);
        out.data.resize(static_cast<std::size_t>(gray.cols) * gray.rows);
        if (gray.isContinuous())
        {
            std::memcpy(out.data.data(), gray.data, out.data.size());
        }
        else
        {
            for (int row = 0; row < gray.rows; ++row)
            {
                std::memcpy(out.data.data() + static_cast<std::size_t>(row) * gray.cols,
                            gray.ptr(row), static_cast<std::size_t>(gray.cols));
            }
        }
        return true;
    }

    BackendCommandResult BackendFacade::handleReviewCommand(const ReviewCommand &command)
    {
        switch (command.action)
        {
        case ReviewCommandAction::ExportMetricsCsv:
        {
            std::string sourcePath;
            {
                std::scoped_lock lock(reviewMutex_);
                sourcePath = loadedRecordingPath_;
            }
            if (sourcePath.empty())
            {
                const std::string message = "No recording loaded to export";
                emitEvent(BackendErrorEvent{BackendErrorSource::Export,
                                            BackendCommandType::Review, message});
                return {false, BackendCommandType::Review, message};
            }
            if (command.outputPath.empty())
            {
                return {false, BackendCommandType::Review, "Export output path is empty"};
            }

            CancelFlag cancelFlag;
            const std::uint64_t operationId =
                beginOperation(BackendOperationKind::Export, &cancelFlag, command.outputPath);
            const double pixelToMicron = backend_.processing().getPixelToMicronFactor();
            const std::string outputPath = command.outputPath;

            // The job opens its own read-only reader so it never races the
            // interactive review reads, and the source file stays intact.
            std::thread worker([this, operationId, cancelFlag, sourcePath, outputPath,
                                pixelToMicron]() {
                services::Hdf5Service reader;
                std::vector<services::ProcessedFrame> valid;
                std::vector<services::ProcessedFrame> invalid;
                if (!reader.loadFile(sourcePath))
                {
                    finishOperation(operationId, BackendOperationState::Failed,
                                    "Failed to open source recording: " + sourcePath);
                    return;
                }
                if (reader.isRecordingFile())
                {
                    reader.readRecordingMetadata(valid);
                }
                else
                {
                    reader.readValidMetadata(valid);
                    reader.readInvalidMetadata(invalid);
                }
                reader.closeFile();

                std::string error;
                const bool ok = review::writeMetricsCsv(
                    outputPath, valid, invalid, pixelToMicron, &error,
                    [this, operationId, &cancelFlag](std::uint64_t done, std::uint64_t total) {
                        if (cancelFlag->load(std::memory_order_relaxed))
                        {
                            return false;
                        }
                        reportOperationProgress(operationId, done, total);
                        return true;
                    });
                if (ok)
                {
                    finishOperation(operationId, BackendOperationState::Completed, outputPath);
                }
                else if (cancelFlag->load(std::memory_order_relaxed))
                {
                    finishOperation(operationId, BackendOperationState::Cancelled, error);
                }
                else
                {
                    emitEvent(BackendErrorEvent{BackendErrorSource::Export,
                                                BackendCommandType::Review, error});
                    finishOperation(operationId, BackendOperationState::Failed, error);
                }
            });
            {
                std::scoped_lock lock(reviewJobsMutex_);
                reviewJobThreads_.push_back(std::move(worker));
            }
            return {true, BackendCommandType::Review, "Metrics CSV export started", operationId};
        }
        }
        return {false, BackendCommandType::Review, "Unknown review command"};
    }

    BackendCommandResult BackendFacade::handlePumpCommand(const PumpCommand &command)
    {
        using Pump = services::SyringePumpService;
        if (command.pumpId < 0 || command.pumpId >= Pump::PUMP_COUNT)
        {
            return {false, BackendCommandType::Pump, "Invalid pump id"};
        }
        auto &pumps = backend_.syringePump();
        const auto pumpId = static_cast<Pump::PumpId>(command.pumpId);
        auto fail = [this](const std::string &message) -> BackendCommandResult {
            emitEvent(BackendErrorEvent{BackendErrorSource::Hardware,
                                        BackendCommandType::Pump, message});
            return {false, BackendCommandType::Pump, message};
        };

        switch (command.action)
        {
        case PumpCommandAction::Connect:
        {
            if (command.comPort < 0)
            {
                return fail("Invalid COM port");
            }
            if (command.modbusAddress < 1 || command.modbusAddress > 247)
            {
                return fail("Invalid Modbus address (1-247)");
            }
            // Serial-port conflict rules (BE-7): the other pump and the
            // autofocus controller must not share the port.
            const auto otherId = pumpId == Pump::PumpId::Sample ? Pump::PumpId::Sheath
                                                                : Pump::PumpId::Sample;
            if (pumps.isConnected(otherId) && pumps.getComPort(otherId) == command.comPort)
            {
                return fail("COM port already in use by the other pump");
            }
            if (backend_.autofocus().isConnected() &&
                backend_.autofocus().getComPort() == command.comPort)
            {
                return fail("COM port already in use by the autofocus controller");
            }
            if (!pumps.connect(pumpId, command.comPort, command.baudRate,
                               static_cast<std::uint8_t>(command.modbusAddress)))
            {
                return fail("Pump connect failed (no Modbus response)");
            }
            return {true, BackendCommandType::Pump, "Pump connected"};
        }
        case PumpCommandAction::Disconnect:
            // Disconnect safely stops an active run/purge first.
            pumps.stop(pumpId);
            pumps.disconnect(pumpId);
            return {true, BackendCommandType::Pump, "Pump disconnected"};
        case PumpCommandAction::SetFlowRate:
            if (command.flowRate < 0.0)
            {
                return fail("Invalid flow rate");
            }
            if (!pumps.setFlowRate(pumpId, command.flowRate,
                                   static_cast<std::uint16_t>(command.flowRateUnit)))
            {
                return fail("Set flow rate failed");
            }
            return {true, BackendCommandType::Pump, "Flow rate set"};
        case PumpCommandAction::SetDirection:
            if (!pumps.setDirection(pumpId, command.direction == 1
                                                ? Pump::Direction::Withdraw
                                                : Pump::Direction::Infuse))
            {
                return fail("Set direction failed");
            }
            return {true, BackendCommandType::Pump, "Direction set"};
        case PumpCommandAction::Start:
            if (!pumps.start(pumpId))
            {
                return fail("Pump start failed");
            }
            return {true, BackendCommandType::Pump, "Pump started"};
        case PumpCommandAction::Stop:
            if (!pumps.stop(pumpId))
            {
                return fail("Pump stop failed");
            }
            return {true, BackendCommandType::Pump, "Pump stopped"};
        case PumpCommandAction::Purge:
            if (!pumps.purge(pumpId, command.direction == 1 ? Pump::Direction::Withdraw
                                                            : Pump::Direction::Infuse))
            {
                return fail("Pump purge failed");
            }
            return {true, BackendCommandType::Pump, "Pump purge started"};
        case PumpCommandAction::StopPurge:
            if (!pumps.stopPurge(pumpId))
            {
                return fail("Pump purge stop failed");
            }
            return {true, BackendCommandType::Pump, "Pump purge stopped"};
        case PumpCommandAction::SetSyringeVolume:
            if (command.syringeVolume <= 0)
            {
                return fail("Invalid syringe volume");
            }
            if (!pumps.setSyringeVolume(pumpId,
                                        static_cast<std::uint16_t>(command.syringeVolume),
                                        static_cast<std::uint16_t>(command.syringeVolumeUnit)))
            {
                return fail("Set syringe volume failed");
            }
            return {true, BackendCommandType::Pump, "Syringe volume set"};
        case PumpCommandAction::PollStatus:
            pumps.pollStatus(pumpId);
            return {true, BackendCommandType::Pump, "Pump status polled"};
        case PumpCommandAction::ScanAddresses:
        {
            if (command.comPort < 0)
            {
                return fail("Invalid COM port");
            }
            // The scan blocks up to (end-start+1) * timeout — run it as a
            // tracked operation so the bridge stays responsive (BE-1).
            const std::uint64_t operationId = beginOperation(
                BackendOperationKind::PumpScan, nullptr,
                "pump address scan on COM" + std::to_string(command.comPort));
            const int comPort = command.comPort;
            const int baudRate = command.baudRate;
            const int startAddr = std::clamp(command.scanStartAddress, 1, 247);
            const int endAddr = std::clamp(command.scanEndAddress, startAddr, 247);
            const int timeoutMs = std::max(10, command.scanTimeoutMs);
            std::thread worker([this, operationId, comPort, baudRate, startAddr, endAddr,
                                timeoutMs]() {
                const auto addresses = backend_.syringePump().scanModbusAddresses(
                    comPort, baudRate, static_cast<std::uint8_t>(startAddr),
                    static_cast<std::uint8_t>(endAddr), timeoutMs);
                std::string found;
                for (const auto addr : addresses)
                {
                    if (!found.empty())
                    {
                        found += ",";
                    }
                    found += std::to_string(static_cast<int>(addr));
                }
                finishOperation(operationId, BackendOperationState::Completed, found);
            });
            {
                std::scoped_lock lock(reviewJobsMutex_);
                reviewJobThreads_.push_back(std::move(worker));
            }
            return {true, BackendCommandType::Pump, "Pump address scan started", operationId};
        }
        }
        return {false, BackendCommandType::Pump, "Unknown pump command"};
    }

    bool BackendFacade::fetchPumpStatus(int pumpId, BackendPumpStatus &out) const
    {
        using Pump = services::SyringePumpService;
        if (!initialized_ || pumpId < 0 || pumpId >= Pump::PUMP_COUNT)
        {
            return false;
        }
        auto &pumps = backend_.syringePump();
        const auto id = static_cast<Pump::PumpId>(pumpId);
        const auto status = pumps.getStatus(id);
        const auto config = pumps.getConfig(id);
        out.connected = status.connected;
        out.runStatus = static_cast<int>(status.runStatus);
        out.currentFlowRate = status.currentFlowRate;
        out.accumulatedVolume = status.accumulatedVolume;
        out.minFlowRate = status.minFlowRate;
        out.maxFlowRate = status.maxFlowRate;
        out.stalled = status.stalled;
        out.comPort = config.comPort;
        out.baudRate = config.baudRate;
        out.modbusAddress = config.modbusAddress;
        out.configuredFlowRate = config.flowRate;
        out.flowRateUnit = config.flowRateUnit;
        out.direction = static_cast<int>(config.direction);
        return true;
    }

    BackendCommandResult BackendFacade::handleAutofocusCommand(const AutofocusCommand &command)
    {
        auto &autofocus = backend_.autofocus();
        auto fail = [this](const std::string &message) -> BackendCommandResult {
            emitEvent(BackendErrorEvent{BackendErrorSource::Hardware,
                                        BackendCommandType::Autofocus, message});
            return {false, BackendCommandType::Autofocus, message};
        };

        switch (command.action)
        {
        case AutofocusCommandAction::Connect:
        {
            if (command.comPort < 0)
            {
                return fail("Invalid COM port");
            }
            if (command.deviceAddress < 0 || command.deviceAddress > 255)
            {
                return fail("Invalid device address");
            }
            // Serial-port conflict rules (BE-7/BE-8): the pumps must not
            // share the autofocus controller's port.
            using Pump = services::SyringePumpService;
            auto &pumps = backend_.syringePump();
            for (int pumpId = 0; pumpId < Pump::PUMP_COUNT; ++pumpId)
            {
                const auto id = static_cast<Pump::PumpId>(pumpId);
                if (pumps.isConnected(id) && pumps.getComPort(id) == command.comPort)
                {
                    return fail("COM port already in use by a syringe pump");
                }
            }
            if (!autofocus.connect(command.comPort, command.baudRate,
                                   static_cast<unsigned char>(command.deviceAddress)))
            {
                return fail("Autofocus controller connect failed");
            }
            return {true, BackendCommandType::Autofocus, "Autofocus controller connected"};
        }
        case AutofocusCommandAction::Disconnect:
            // Disconnect disables control first so no motion outlives it.
            autofocus.setEnabled(false);
            autofocus.disconnect();
            return {true, BackendCommandType::Autofocus, "Autofocus controller disconnected"};
        case AutofocusCommandAction::SetEnabled:
            if (command.enabled && !autofocus.isConnected())
            {
                return fail("Autofocus controller is not connected");
            }
            autofocus.setEnabled(command.enabled);
            return {true, BackendCommandType::Autofocus,
                    command.enabled ? "Autofocus enabled" : "Autofocus disabled"};
        case AutofocusCommandAction::IncreaseVoltage:
        case AutofocusCommandAction::DecreaseVoltage:
            if (!autofocus.isConnected())
            {
                return fail("Autofocus controller is not connected");
            }
            if (command.action == AutofocusCommandAction::IncreaseVoltage)
            {
                autofocus.increaseVoltage();
            }
            else
            {
                autofocus.decreaseVoltage();
            }
            return {true, BackendCommandType::Autofocus, "Voltage jog requested"};
        case AutofocusCommandAction::SetConfig:
            if (command.config.minVoltage >= command.config.maxVoltage)
            {
                return fail("Invalid voltage range (min must be < max)");
            }
            autofocus.setConfig(command.config);
            return {true, BackendCommandType::Autofocus, "Autofocus config applied"};
        }
        return {false, BackendCommandType::Autofocus, "Unknown autofocus command"};
    }

    bool BackendFacade::fetchAutofocusStatus(BackendAutofocusStatus &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        auto &autofocus = backend_.autofocus();
        out.connected = autofocus.isConnected();
        out.enabled = autofocus.isEnabled();
        out.currentVoltage = autofocus.getCurrentVoltage();
        out.comPort = autofocus.getComPort();
        out.averageRingRatio = autofocus.getAverageRingRatio();
        out.medianRingRatio = autofocus.getMedianRingRatio();
        out.lastRingRatioUpdateUs = autofocus.getLastRingRatioUpdateUs();
        out.ringRatioAgeUs =
            out.lastRingRatioUpdateUs == 0
                ? 0
                : Tools::getTimestamp() - out.lastRingRatioUpdateUs;
        return true;
    }

    bool BackendFacade::fetchAutofocusConfig(services::AutofocusService::Config &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        out = backend_.autofocus().getConfig();
        return true;
    }

    bool BackendFacade::fetchBackgroundImage(BackendFrame &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        const cv::Mat bg = backend_.processing().getRealtimeBackgroundGray();
        if (bg.empty())
        {
            return false;
        }
        out = BackendFrame{};
        out.width = static_cast<std::uint64_t>(bg.cols);
        out.height = static_cast<std::uint64_t>(bg.rows);
        out.pixelFormat = 0; // Mono8
        out.strideBytes = static_cast<std::size_t>(bg.cols);
        out.data.resize(static_cast<std::size_t>(bg.cols) * static_cast<std::size_t>(bg.rows));
        if (bg.isContinuous())
        {
            std::memcpy(out.data.data(), bg.data, out.data.size());
        }
        else
        {
            for (int row = 0; row < bg.rows; ++row)
            {
                std::memcpy(out.data.data() + static_cast<std::size_t>(row) * bg.cols,
                            bg.ptr(row), static_cast<std::size_t>(bg.cols));
            }
        }
        return true;
    }

    BackendCommandResult BackendFacade::setBackgroundImage(std::uint64_t width,
                                                           std::uint64_t height,
                                                           const std::uint8_t *data,
                                                           std::size_t byteLen)
    {
        if (!initialized_)
        {
            return lifecycleError(BackendCommandType::ProcessingSettings,
                                  "Backend facade is not initialized");
        }
        if (width == 0 || height == 0 || data == nullptr || byteLen != width * height)
        {
            const std::string message = "Background image must be Mono8 with byteLen == width*height";
            emitEvent(BackendErrorEvent{BackendErrorSource::ConfigCore,
                                        BackendCommandType::ProcessingSettings, message});
            return {false, BackendCommandType::ProcessingSettings, message};
        }
        cv::Mat bg(static_cast<int>(height), static_cast<int>(width), CV_8UC1);
        std::memcpy(bg.data, data, byteLen);
        backend_.processing().setRealtimeBackgroundGray(bg);
        return {true, BackendCommandType::ProcessingSettings, "Background image set"};
    }

    BackendCommandResult BackendFacade::clearBackgroundImage()
    {
        if (!initialized_)
        {
            return lifecycleError(BackendCommandType::ProcessingSettings,
                                  "Backend facade is not initialized");
        }
        backend_.processing().setRealtimeBackgroundGray(cv::Mat());
        return {true, BackendCommandType::ProcessingSettings, "Background image cleared"};
    }

    bool BackendFacade::fetchCameraDiscovery(BackendCameraDiscovery &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        out = BackendCameraDiscovery{};

        auto &control = backend_.cameraControl();
        for (const auto &cam : control.discoverAllCameras())
        {
            BackendDiscoveredCamera dto;
            dto.type = static_cast<int>(cam.cameraType);
            dto.cameraIndex = cam.cameraIndex;
            dto.interfaceIndex = cam.interfaceIndex;
            dto.deviceIndex = cam.deviceIndex;
            dto.interfaceId = cam.interfaceID;
            dto.deviceId = cam.deviceID;
            dto.modelName = cam.modelName;
            dto.firmwareVersion = cam.firmwareVersion;
            dto.label = cam.label;
            out.cameras.push_back(std::move(dto));
        }
        for (const auto &grabber : control.discoverFramegrabbers())
        {
            BackendDiscoveredFramegrabber dto;
            dto.interfaceIndex = grabber.interfaceIndex;
            dto.deviceIndex = grabber.deviceIndex;
            dto.streamIndex = grabber.streamIndex;
            dto.interfaceId = grabber.interfaceID;
            dto.deviceId = grabber.deviceID;
            dto.streamId = grabber.streamID;
            dto.modelName = grabber.modelName;
            dto.label = grabber.label;
            out.framegrabbers.push_back(std::move(dto));
        }

        // Synthetic mock entry (type 2) so discovery/selection is
        // headless-testable and the shell can always offer the mock source.
        BackendDiscoveredCamera mock;
        mock.type = 2;
        mock.modelName = "Mock camera";
        mock.label = "Mock camera (folder frame stream)";
        out.cameras.push_back(std::move(mock));
        return true;
    }

    bool BackendFacade::fetchCameraSelection(BackendCameraSelection &out) const
    {
        if (!initialized_)
        {
            return false;
        }
        const auto snapshot = backend_.cameraSelection();
        out.mode = static_cast<int>(snapshot.mode);
        out.interfaceIndex = snapshot.interfaceIndex;
        out.deviceIndex = snapshot.deviceIndex;
        out.label = snapshot.label;
        out.mindVisionIndex = snapshot.mindVisionIndex;
        out.mindVisionConfigPath = snapshot.mindVisionConfigPath;
        out.cameraScriptPath = snapshot.cameraScriptPath;
        out.mockFrameDir = snapshot.mockFrameDir;
        out.mockIntervalMs = snapshot.mockIntervalMs;
        out.mockLoop = snapshot.mockLoop;
        out.configured = snapshot.configured;
        out.running = backend_.capture().isRunning();
        return true;
    }

    std::uint64_t BackendFacade::beginOperation(BackendOperationKind kind,
                                                CancelFlag *cancelFlagOut,
                                                const std::string &message)
    {
        const std::uint64_t operationId = nextOperationId_.fetch_add(1, std::memory_order_relaxed);
        auto flag = std::make_shared<std::atomic<bool>>(false);
        {
            std::scoped_lock lock(operationsMutex_);
            activeOperations_[operationId] = ActiveOperation{kind, flag};
        }
        if (cancelFlagOut)
        {
            *cancelFlagOut = flag;
        }
        emitEvent(OperationStatusEvent{operationId, kind, BackendOperationState::Started, 0, 0, message});
        return operationId;
    }

    void BackendFacade::reportOperationProgress(std::uint64_t operationId,
                                                std::uint64_t progress,
                                                std::uint64_t total,
                                                const std::string &message)
    {
        BackendOperationKind kind;
        {
            std::scoped_lock lock(operationsMutex_);
            const auto it = activeOperations_.find(operationId);
            if (it == activeOperations_.end())
            {
                // Late progress after a terminal state: dropped by policy.
                return;
            }
            kind = it->second.kind;
        }
        emitEvent(OperationStatusEvent{operationId, kind, BackendOperationState::Progress,
                                       progress, total, message});
    }

    void BackendFacade::finishOperation(std::uint64_t operationId,
                                        BackendOperationState terminalState,
                                        const std::string &message)
    {
        BackendOperationKind kind;
        {
            std::scoped_lock lock(operationsMutex_);
            const auto it = activeOperations_.find(operationId);
            if (it == activeOperations_.end())
            {
                // First terminal state wins; later finishes are late events
                // and are dropped (BE-1 policy).
                return;
            }
            kind = it->second.kind;
            activeOperations_.erase(it);
        }
        emitEvent(OperationStatusEvent{operationId, kind, terminalState, 0, 0, message});
    }

    bool BackendFacade::requestOperationCancel(std::uint64_t operationId)
    {
        std::scoped_lock lock(operationsMutex_);
        const auto it = activeOperations_.find(operationId);
        if (it == activeOperations_.end())
        {
            return false;
        }
        it->second.cancelRequested->store(true, std::memory_order_relaxed);
        return true;
    }

    std::size_t BackendFacade::activeOperationCount() const
    {
        std::scoped_lock lock(operationsMutex_);
        return activeOperations_.size();
    }

    void BackendFacade::cancelAllOperations(const std::string &reason)
    {
        std::vector<std::pair<std::uint64_t, ActiveOperation>> drained;
        {
            std::scoped_lock lock(operationsMutex_);
            drained.assign(activeOperations_.begin(), activeOperations_.end());
            activeOperations_.clear();
        }
        for (auto &[operationId, op] : drained)
        {
            op.cancelRequested->store(true, std::memory_order_relaxed);
            emitEvent(OperationStatusEvent{operationId, op.kind,
                                           BackendOperationState::Cancelled, 0, 0, reason});
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
