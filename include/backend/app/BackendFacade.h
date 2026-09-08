#pragma once

#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace backend
{
    class AppBackend;
}

namespace backend::bridge
{

    // Values are the bridge contract's command_types (bridge-contract.json):
    // append only, never renumber. 5 is reserved for Operation.
    enum class BackendCommandType
    {
        Camera = 0,
        Recording = 1,
        ProcessingSettings = 2,
        RecordingLoad = 3,
        PlaybackSeek = 4,
        Experiment = 6,
    };

    enum class CameraCommandAction
    {
        ConfigureMockCamera,
        SelectHardwareCamera,
        SelectMindVisionCamera,
        ApplyCameraScript,
        ResetSelectedHardwareCamera,
        SoftTriggerCamera,
        StartCapture,
        StopCapture,
    };

    struct CameraCommand
    {
        CameraCommandAction action{CameraCommandAction::StartCapture};
        std::string mockFrameDirectory;
        int mockFrameIntervalMs{33};
        bool mockLoopFiles{true};
        int hardwareInterfaceIndex{-1};
        int hardwareDeviceIndex{-1};
        std::string hardwareLabel;
        int mindVisionCameraIndex{-1};
        std::string mindVisionLabel;
        std::string mindVisionConfigPath;
        std::string cameraScriptPath;
    };

    enum class RecordingCommandAction
    {
        StartFrameRecording,
        StopFrameRecording,
    };

    struct RecordingCommand
    {
        RecordingCommandAction action{RecordingCommandAction::StartFrameRecording};
        std::string filePath;
    };

    struct ProcessingSettingsCommand
    {
        std::optional<services::ProcessingConfig> config;
        std::optional<services::ProcessingService::Roi> roi;
        std::optional<bool> realtimeEnabled;
        std::optional<bool> realtimeDropFrames;
        std::optional<services::ProcessingService::RealtimeProcessingMode> realtimeProcessingMode;
        std::optional<services::ProcessingService::RealtimeBatchSettings> realtimeBatchSettings;
        std::optional<double> pixelToMicronFactor;
    };

    struct RecordingLoadCommand
    {
        std::string filePath;
        bool readMetadata{true};
    };

    enum class PlaybackSeekMode
    {
        Latest,
        AbsoluteIndex,
    };

    struct PlaybackSeekCommand
    {
        PlaybackSeekMode mode{PlaybackSeekMode::Latest};
        std::uint64_t frameIndex{0};
    };

    // Experiment lifecycle over the shared ExperimentCoordinator (issue #372
    // G2/G3). Contract-pinned values (experiment_command_actions).
    enum class ExperimentCommandAction
    {
        EvaluateReadiness = 0,
        Start = 1,
        Stop = 2,
        Status = 3,
    };

    struct ExperimentCommand
    {
        ExperimentCommandAction action{ExperimentCommandAction::Status};
        std::string outputPath;                 // Start / EvaluateReadiness
        std::uint64_t readinessGeneration{0};   // Start
        std::string profileId;                  // Start / EvaluateReadiness
        bool acknowledgeLatestFrameDrops{false}; // Start
        bool cancelled{false};                  // Stop
    };

    using BackendCommand = std::variant<CameraCommand,
                                        RecordingCommand,
                                        ProcessingSettingsCommand,
                                        RecordingLoadCommand,
                                        PlaybackSeekCommand,
                                        ExperimentCommand>;

    struct BackendCommandResult
    {
        bool ok{false};
        BackendCommandType command{BackendCommandType::Camera};
        std::string message;
        // Typed outcomes of ExperimentCommand Start / Stop so acceptance,
        // rejection and completion stay distinct. Completion is observed via
        // ExperimentStatusEvent / fetchExperimentStatus (terminal == true).
        std::optional<app::ExperimentStartOutcome> experimentStartOutcome;
        std::optional<app::ExperimentStopOutcome> experimentStopOutcome;
    };

    enum class FrameReadySource
    {
        LiveCapture,
        Playback,
        BackgroundCapture,
    };

    struct FrameReadyEvent
    {
        FrameReadySource source{FrameReadySource::Playback};
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        std::uint64_t width{0};
        std::uint64_t height{0};
        std::uint64_t pixelFormat{0};
        std::size_t strideBytes{0};
        std::size_t byteSize{0};
    };

    enum class CameraState
    {
        Unconfigured,
        Configured,
        Starting,
        Running,
        Stopped,
        Error,
    };

    struct CameraStatusEvent
    {
        CameraState state{CameraState::Unconfigured};
        bool configured{false};
        bool running{false};
        std::uint64_t framesProcessed{0};
        std::uint64_t frameRate{0};
        std::uint64_t dataRateMBps{0};
        std::string label;
    };

    enum class RecordingState
    {
        Idle,
        Starting,
        Recording,
        Stopped,
        Loaded,
        Error,
    };

    struct RecordingStatusEvent
    {
        RecordingState state{RecordingState::Idle};
        std::string filePath;
        bool recordingFile{false};
        std::uint64_t framesWritten{0};
        std::uint64_t framesFiltered{0};
        std::uint64_t loadedValidFrames{0};
        std::uint64_t loadedInvalidFrames{0};
    };

    struct ProcessingObjectSummary
    {
        bool valid{false};
        bool targetGroup{false};
        int objectId{-1};
        int objectCount{0};
        int trackId{-1};
        double centroidX{0.0};
        double centroidY{0.0};
        double area{0.0};
        double deformability{0.0};
        double ringRatio{0.0};
        double youngsModulus{0.0};
    };

    struct ProcessingResultEvent
    {
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        double algoFps1s{0.0};
        double validFps1s{0.0};
        double invalidFps1s{0.0};
        std::vector<ProcessingObjectSummary> objects;
    };

    struct PlaybackPositionEvent
    {
        bool hasFrame{false};
        bool playing{false};
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        std::uint64_t earliestIndex{0};
        std::uint64_t latestIndex{0};
        std::size_t availableCount{0};
    };

    enum class BackendErrorSource
    {
        Lifecycle,
        Camera,
        Recording,
        Processing,
        Playback,
    };

    struct BackendErrorEvent
    {
        BackendErrorSource source{BackendErrorSource::Lifecycle};
        BackendCommandType command{BackendCommandType::Camera};
        std::string message;
    };

    // Forwarded from ExperimentCoordinator's status callback on every
    // transition (event kind 8 in the bridge contract). Delivered on the
    // coordinator's calling thread (worker for Stopping/terminal).
    struct ExperimentStatusEvent
    {
        app::ExperimentStatus status;
    };

    using BackendEvent = std::variant<FrameReadyEvent,
                                      CameraStatusEvent,
                                      RecordingStatusEvent,
                                      ProcessingResultEvent,
                                      PlaybackPositionEvent,
                                      BackendErrorEvent,
                                      ExperimentStatusEvent>;

    struct BackendFrame
    {
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        std::uint64_t width{0};
        std::uint64_t height{0};
        std::uint64_t pixelFormat{0};
        std::size_t strideBytes{0};
        std::vector<std::uint8_t> data;
    };

    class BackendFacade
    {
    public:
        using EventSink = std::function<void(const BackendEvent &)>;

        explicit BackendFacade(AppBackend &backend);
        ~BackendFacade();

        BackendFacade(const BackendFacade &) = delete;
        BackendFacade &operator=(const BackendFacade &) = delete;

        bool initialize(const std::string &dataDir);
        void shutdown();
        bool isInitialized() const;

        void setEventSink(EventSink sink);
        BackendCommandResult dispatch(const BackendCommand &command);

        bool fetchLatestFrame(BackendFrame &out) const;
        bool fetchFrameByIndex(std::uint64_t frameIndex, BackendFrame &out) const;

        // Experiment readiness (fresh evaluation; the returned generation is
        // what a Start must present) and lifecycle status pulls. Both return
        // false when the facade is not initialized.
        bool fetchExperimentReadiness(app::ExperimentReadinessSnapshot &out,
                                      const std::string &outputPath = {},
                                      const std::string &profileId = {}) const;
        bool fetchExperimentStatus(app::ExperimentStatus &out) const;

    private:
        BackendCommandResult handleExperimentCommand(const ExperimentCommand &command);
        BackendCommandResult handleCameraCommand(const CameraCommand &command);
        BackendCommandResult handleRecordingCommand(const RecordingCommand &command);
        BackendCommandResult handleProcessingSettingsCommand(const ProcessingSettingsCommand &command);
        BackendCommandResult handleRecordingLoadCommand(const RecordingLoadCommand &command);
        BackendCommandResult handlePlaybackSeekCommand(const PlaybackSeekCommand &command);

        BackendCommandResult lifecycleError(BackendCommandType command, const std::string &message);
        void emitEvent(const BackendEvent &event) const;
        CameraStatusEvent makeCameraStatus(CameraState state, std::string label = {}) const;

        AppBackend &backend_;
        mutable std::mutex eventSinkMutex_;
        EventSink eventSink_;
        bool initialized_{false};
    };

} // namespace backend::bridge
