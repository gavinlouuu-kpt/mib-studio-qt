#pragma once

#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace backend
{
    class AppBackend;
}

namespace backend::bridge
{

    // Values are part of the bridge contract
    // (crates/mib-bridge/contract/bridge-contract.json, ADR 0004) and are
    // append-only: never renumber or repurpose.
    enum class BackendCommandType
    {
        Camera = 0,
        Recording = 1,
        ProcessingSettings = 2,
        RecordingLoad = 3,
        PlaybackSeek = 4,
        Operation = 5,
        Experiment = 6,
        Monitoring = 7,
        Trigger = 8,
        Review = 9,
        Pump = 10,
        Autofocus = 11,
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
        // BE-3 (#273): merge-apply a config document (the schema of
        // fetchProcessingConfigJson) — only keys present in the JSON change;
        // malformed values fail the whole command without touching state.
        std::optional<std::string> configJson;
        std::optional<std::size_t> flushInterval;
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

    // Long-running actions (recording load today; experiment, export,
    // reanalysis as they land) are tracked as operations with explicit IDs and
    // terminal states so the shell can correlate progress events, cancel, and
    // detect late events safely (BE-1, ADR 0004). Values are contract-pinned
    // and append-only.
    enum class BackendOperationKind
    {
        RecordingLoad,
        Experiment,
        Export,
        BatchMetrics,
        MaskRegeneration,
        Reanalysis,
        PumpScan,
    };

    enum class BackendOperationState
    {
        Started,
        Progress,
        Completed,
        Failed,
        Cancelled,
        TimedOut,
    };

    struct OperationStatusEvent
    {
        std::uint64_t operationId{0};
        BackendOperationKind kind{BackendOperationKind::RecordingLoad};
        BackendOperationState state{BackendOperationState::Started};
        std::uint64_t progress{0};
        std::uint64_t total{0};
        std::string message;
    };

    enum class OperationCommandAction
    {
        Cancel,
    };

    struct OperationCommand
    {
        OperationCommandAction action{OperationCommandAction::Cancel};
        std::uint64_t operationId{0};
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

    // Monitoring accumulation control (BE-5): visibility-gated enable/disable
    // plus an atomic clear. Data flows through fetchMonitoringSnapshot.
    enum class MonitoringCommandAction
    {
        Enable,
        Disable,
        Clear,
    };

    struct MonitoringCommand
    {
        MonitoringCommandAction action{MonitoringCommandAction::Enable};
    };

    // Sorter trigger control (BE-5) over TriggerService.
    enum class TriggerCommandAction
    {
        SetPulseDuration,
        ManualPulse,
        StartPeriodicTest,
        StopPeriodicTest,
    };

    struct TriggerCommand
    {
        TriggerCommandAction action{TriggerCommandAction::ManualPulse};
        int pulseDurationUs{0};   // SetPulseDuration
        int periodicIntervalMs{0}; // StartPeriodicTest
    };

    // HDF5 review job commands (BE-6, #276). Jobs run on their own reader
    // instance/thread as tracked operations (BE-1): the source file is opened
    // read-only and partial outputs are cleaned on cancel/failure.
    enum class ReviewCommandAction
    {
        ExportMetricsCsv,
    };

    struct ReviewCommand
    {
        ReviewCommandAction action{ReviewCommandAction::ExportMetricsCsv};
        std::string outputPath;
    };

    // Syringe-pump commands (BE-7, #277) over the Qt-free SyringePumpService.
    // Sample and Sheath are independent identities; serial-port conflicts
    // (other pump or the autofocus controller on the same COM port) are
    // rejected with structured errors before touching the port.
    enum class PumpCommandAction
    {
        Connect,
        Disconnect,
        SetFlowRate,
        SetDirection,
        Start,
        Stop,
        Purge,
        StopPurge,
        SetSyringeVolume,
        PollStatus,
        ScanAddresses,
    };

    struct PumpCommand
    {
        PumpCommandAction action{PumpCommandAction::PollStatus};
        int pumpId{0}; // 0 Sample, 1 Sheath (contract pump_ids)
        int comPort{-1};
        int baudRate{115200};
        int modbusAddress{1};
        double flowRate{0.0};
        int flowRateUnit{100};
        int direction{0}; // 0 Infuse, 1 Withdraw (contract pump_directions)
        int syringeVolume{0};
        int syringeVolumeUnit{0};
        int scanStartAddress{1};
        int scanEndAddress{8};
        int scanTimeoutMs{300};
    };

    // Autofocus / nanopositioner commands (BE-8, #278) over AutofocusService.
    // On platforms without the Coremor SDK the service is a stub whose
    // connect fails with a structured message — commands stay safe.
    enum class AutofocusCommandAction
    {
        Connect,
        Disconnect,
        SetEnabled,
        IncreaseVoltage,
        DecreaseVoltage,
        SetConfig,
    };

    struct AutofocusCommand
    {
        AutofocusCommandAction action{AutofocusCommandAction::SetEnabled};
        int comPort{-1};
        int baudRate{115200};
        int deviceAddress{1};
        bool enabled{false};
        services::AutofocusService::Config config{};
    };

    using BackendCommand = std::variant<CameraCommand,
                                        RecordingCommand,
                                        ProcessingSettingsCommand,
                                        RecordingLoadCommand,
                                        PlaybackSeekCommand,
                                        OperationCommand,
                                        ExperimentCommand,
                                        MonitoringCommand,
                                        TriggerCommand,
                                        ReviewCommand,
                                        PumpCommand,
                                        AutofocusCommand>;

    struct BackendCommandResult
    {
        bool ok{false};
        BackendCommandType command{BackendCommandType::Camera};
        std::string message;
        // Non-zero when the command started (or targeted) a tracked operation;
        // correlates with OperationStatusEvent::operationId.
        std::uint64_t operationId{0};
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

    // Contract-pinned and append-only (ADR 0004). The tail entries cover the
    // workflows being migrated behind the bridge (BE-2…BE-9).
    enum class BackendErrorSource
    {
        Lifecycle,
        Camera,
        Recording,
        Processing,
        Playback,
        Experiment,
        Monitoring,
        Hardware,
        ConfigCore,
        Review,
        Export,
        Platform,
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

    // Variant order defines the bridge event-kind values — append-only.
    using BackendEvent = std::variant<FrameReadyEvent,
                                      CameraStatusEvent,
                                      RecordingStatusEvent,
                                      ProcessingResultEvent,
                                      PlaybackPositionEvent,
                                      BackendErrorEvent,
                                      OperationStatusEvent,
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

    // Pollable snapshot of the realtime processing pipeline (pulled on demand by
    // the shell, symmetric with fetchLatestFrame — no callback wiring).
    struct BackendProcessingStats
    {
        double algoFps1s{0.0};
        double validFps1s{0.0};
        double invalidFps1s{0.0};
        double pixelToMicronFactor{0.0};
    };

    // One monitoring metric row (BE-5): the per-object measurements that feed
    // the Monitoring charts. (frameIndex, objectId) is a stable identity for
    // frontend reconciliation. Deliberately carries NO image/mask payloads —
    // those move through dedicated binary pulls only when explicitly requested.
    struct MonitoringObjectRow
    {
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        bool valid{false};
        bool targetGroup{false};
        int objectId{-1};
        int objectCount{0};
        int trackId{-1};
        double centroidX{0.0};
        double centroidY{0.0};
        double area{0.0};
        double deformability{0.0};
        double areaRatio{0.0};
        double ringRatio{0.0};
        double youngsModulus{0.0};
    };

    // Bounded monitoring snapshot (BE-5). Totals/appended counts make ring
    // evictions observable (evicted = appended - held); latestTimestampNs is
    // the freshness signal.
    struct BackendMonitoringSnapshot
    {
        bool monitoringActive{false};
        std::uint64_t validHeld{0};
        std::uint64_t invalidHeld{0};
        std::uint64_t validAppended{0};
        std::uint64_t invalidAppended{0};
        std::uint64_t capacity{0};
        std::uint64_t latestTimestampNs{0};
        std::vector<MonitoringObjectRow> rows;
    };

    // Typed camera discovery results (BE-2, #272). `type` values are
    // contract-pinned: 0 EGrabber, 1 MindVision, 2 Mock (the mock source is a
    // synthetic entry so discovery/selection is headless-testable).
    struct BackendDiscoveredCamera
    {
        int type{0};
        int cameraIndex{-1};
        int interfaceIndex{-1};
        int deviceIndex{-1};
        std::string interfaceId;
        std::string deviceId;
        std::string modelName;
        std::string firmwareVersion;
        std::string label;
    };

    struct BackendDiscoveredFramegrabber
    {
        int interfaceIndex{-1};
        int deviceIndex{-1};
        int streamIndex{-1};
        std::string interfaceId;
        std::string deviceId;
        std::string streamId;
        std::string modelName;
        std::string label;
    };

    struct BackendCameraDiscovery
    {
        std::vector<BackendDiscoveredCamera> cameras;
        std::vector<BackendDiscoveredFramegrabber> framegrabbers;
    };

    // Authoritative selected-device snapshot (BE-2). `mode` values are
    // contract-pinned: 0 None, 1 Mock, 2 Hardware, 3 MindVision.
    struct BackendCameraSelection
    {
        int mode{0};
        int interfaceIndex{-1};
        int deviceIndex{-1};
        std::string label;
        int mindVisionIndex{-1};
        std::string mindVisionConfigPath;
        std::string cameraScriptPath;
        std::string mockFrameDir;
        int mockIntervalMs{0};
        bool mockLoop{true};
        bool configured{false};
        bool running{false};
    };

    // Review metadata for the loaded HDF5 file (BE-6, #276): recording vs
    // experiment mode, times/counts, ROI, provenance, and per-dataset
    // capabilities (counts/dims discovered via getDatasetInfo).
    struct BackendReviewDatasetInfo
    {
        bool present{false};
        std::uint64_t count{0};
        int height{0};
        int width{0};
        int channels{0};
    };

    struct BackendReviewMetadata
    {
        bool fileOpen{false};
        bool recordingFile{false};
        std::uint64_t startTimeNs{0};
        std::uint64_t endTimeNs{0};
        std::uint64_t totalValid{0};
        std::uint64_t totalInvalid{0};
        services::ProcessingService::Roi roi{};
        bool hasBackground{false};
        bool hasCoreIdentity{false};
        std::string coreVersion;
        std::string coreSource;
        std::string coreReleaseTag;
        BackendReviewDatasetInfo validImages;
        BackendReviewDatasetInfo invalidImages;
        BackendReviewDatasetInfo validMasks;
        BackendReviewDatasetInfo invalidMasks;
        BackendReviewDatasetInfo recordedImages;
        std::string filePath;
    };

    // Review image datasets addressable by the binary pull (contract-pinned
    // values; append-only).
    enum class ReviewImageDataset
    {
        ValidImage = 0,
        InvalidImage = 1,
        RecordedImage = 2,
        ValidMask = 3,
        InvalidMask = 4,
    };

    // Processing-core identity/trust status (BE-3, #273). Trust verification
    // and activation stay backend-owned; this is observability only.
    struct BackendProcessingCoreStatus
    {
        std::string activeVersion;
        std::uint32_t contractVersion{0};
        std::uint32_t engineAbiVersion{0};
        std::string source;
        std::string releaseTag;
        std::string buildId;
        std::string artifactSha256;
        std::string requiredVersion; // administrator pin ("" = unpinned)
        bool pinSatisfied{true};
    };

    // Authoritative per-pump snapshot (BE-7): connection, run state, rates,
    // and the applied configuration.
    struct BackendPumpStatus
    {
        bool connected{false};
        int runStatus{0}; // contract pump_run_states
        double currentFlowRate{0.0};
        double accumulatedVolume{0.0};
        double minFlowRate{0.0};
        double maxFlowRate{0.0};
        bool stalled{false};
        int comPort{-1};
        int baudRate{115200};
        int modbusAddress{1};
        double configuredFlowRate{0.0};
        int flowRateUnit{100};
        int direction{0};
    };

    // Autofocus / nanopositioner status snapshot (BE-8): connection, enable
    // state, live voltage, and ring-ratio focus metrics with freshness (age)
    // so stale metrics are observable and can never silently drive a move.
    struct BackendAutofocusStatus
    {
        bool connected{false};
        bool enabled{false};
        double currentVoltage{0.0};
        int comPort{-1};
        double averageRingRatio{0.0};
        double medianRingRatio{0.0};
        std::uint64_t lastRingRatioUpdateUs{0};
        std::uint64_t ringRatioAgeUs{0}; // now - last update; 0 when never
    };

    // Sorter trigger status snapshot (BE-5).
    struct BackendTriggerStatus
    {
        bool cameraAttached{false};
        int pulseDurationUs{0};
        std::uint64_t triggerCount{0};
        double lastOnsetUs{0.0};
        int lastObjectId{-1};
        int lastTrackId{-1};
        bool periodicActive{false};
        int periodicIntervalMs{0};
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
        bool fetchProcessingStats(BackendProcessingStats &out) const;
        // Bounded monitoring pull (BE-5): at most maxRows most-recent metric
        // rows across the valid+invalid ring buffers (metrics only, no images).
        bool fetchMonitoringSnapshot(BackendMonitoringSnapshot &out, std::size_t maxRows) const;
        bool fetchTriggerStatus(BackendTriggerStatus &out) const;
        // Camera discovery/selection pulls (BE-2). Discovery enumerates
        // EGrabber + MindVision devices (empty without the SDKs) plus the
        // synthetic mock entry; the selection snapshot is authoritative.
        bool fetchCameraDiscovery(BackendCameraDiscovery &out) const;
        bool fetchCameraSelection(BackendCameraSelection &out) const;
        bool fetchPumpStatus(int pumpId, BackendPumpStatus &out) const;
        bool fetchAutofocusStatus(BackendAutofocusStatus &out) const;
        bool fetchAutofocusConfig(services::AutofocusService::Config &out) const;
        // Processing configuration (BE-3): the full lossless config document
        // as JSON — image_processing (config.json schema), realtime settings,
        // flush interval, pixel→micron, ROI, background flag, and the
        // monotonic config_version for external-change detection.
        bool fetchProcessingConfigJson(std::string &out) const;
        bool fetchProcessingCoreStatus(BackendProcessingCoreStatus &out) const;
        // HDF5 review pulls (BE-6). Metrics pages are served from a lazily
        // cached metadata read (metrics only, no image payloads) that is
        // invalidated on every RecordingLoad; images/masks are pulled one at
        // a time via hyperslab reads (bounded memory).
        bool fetchReviewMetadata(BackendReviewMetadata &out) const;
        bool fetchReviewMetricsPage(bool valid,
                                    std::uint64_t offset,
                                    std::uint64_t count,
                                    std::vector<MonitoringObjectRow> &rows,
                                    std::uint64_t &totalOut) const;
        bool fetchReviewImage(ReviewImageDataset dataset,
                              std::uint64_t index,
                              BackendFrame &out) const;

        // Background image (BE-3): binary get/set/clear (grayscale Mono8).
        bool fetchBackgroundImage(BackendFrame &out) const;
        BackendCommandResult setBackgroundImage(std::uint64_t width,
                                                std::uint64_t height,
                                                const std::uint8_t *data,
                                                std::size_t byteLen);
        BackendCommandResult clearBackgroundImage();

        // ---- Operation tracking (BE-1, ADR 0004) ----
        // Long-running actions register here so they get a correlatable ID,
        // Started/Progress/terminal events, and a cancel flag the runner must
        // observe. finishOperation() is idempotent per ID: the first terminal
        // state wins and later calls are dropped (late-event policy).
        using CancelFlag = std::shared_ptr<std::atomic<bool>>;
        std::uint64_t beginOperation(BackendOperationKind kind,
                                     CancelFlag *cancelFlagOut = nullptr,
                                     const std::string &message = {});
        void reportOperationProgress(std::uint64_t operationId,
                                     std::uint64_t progress,
                                     std::uint64_t total,
                                     const std::string &message = {});
        void finishOperation(std::uint64_t operationId,
                             BackendOperationState terminalState,
                             const std::string &message = {});
        // Request cancellation; returns true if the operation was active. The
        // runner observes the flag and emits the Cancelled terminal event.
        bool requestOperationCancel(std::uint64_t operationId);
        std::size_t activeOperationCount() const;

        // Experiment readiness (fresh evaluation; the returned generation is
        // what a Start must present) and lifecycle status pulls. Both return
        // false when the facade is not initialized.
        bool fetchExperimentReadiness(app::ExperimentReadinessSnapshot &out,
                                      const std::string &outputPath = {},
                                      const std::string &profileId = {}) const;
        bool fetchExperimentStatus(app::ExperimentStatus &out) const;

    private:
        BackendCommandResult handleCameraCommand(const CameraCommand &command);
        BackendCommandResult handleRecordingCommand(const RecordingCommand &command);
        BackendCommandResult handleProcessingSettingsCommand(const ProcessingSettingsCommand &command);
        BackendCommandResult handleRecordingLoadCommand(const RecordingLoadCommand &command);
        BackendCommandResult handlePlaybackSeekCommand(const PlaybackSeekCommand &command);
        BackendCommandResult handleOperationCommand(const OperationCommand &command);
        BackendCommandResult handleExperimentCommand(const ExperimentCommand &command);
        BackendCommandResult handleMonitoringCommand(const MonitoringCommand &command);
        BackendCommandResult handleTriggerCommand(const TriggerCommand &command);
        BackendCommandResult handleReviewCommand(const ReviewCommand &command);
        BackendCommandResult handlePumpCommand(const PumpCommand &command);
        BackendCommandResult handleAutofocusCommand(const AutofocusCommand &command);

        BackendCommandResult lifecycleError(BackendCommandType command, const std::string &message);
        void emitEvent(const BackendEvent &event) const;
        CameraStatusEvent makeCameraStatus(CameraState state, std::string label = {}) const;
        // Shutdown path: mark every active operation cancelled (flag + event)
        // so no operation outlives the facade silently.
        void cancelAllOperations(const std::string &reason);

        struct ActiveOperation
        {
            BackendOperationKind kind{BackendOperationKind::RecordingLoad};
            CancelFlag cancelRequested;
        };

        AppBackend &backend_;
        mutable std::mutex eventSinkMutex_;
        EventSink eventSink_;
        bool initialized_{false};

        mutable std::mutex operationsMutex_;
        std::unordered_map<std::uint64_t, ActiveOperation> activeOperations_;
        std::atomic<std::uint64_t> nextOperationId_{1};

        // Backend-owned experiment state machine (BE-4). The running
        // experiment is also a tracked operation so it can be correlated and
        // cancelled through the generic operation surface.
        std::atomic<std::uint64_t> experimentOperationId_{0};

        // Review state (BE-6): the loaded file path (jobs open their own
        // read-only reader on it) and the lazily cached metrics metadata.
        mutable std::mutex reviewMutex_;
        std::string loadedRecordingPath_;
        mutable bool reviewMetricsLoaded_{false};
        mutable std::vector<services::ProcessedFrame> reviewValidMeta_;
        mutable std::vector<services::ProcessedFrame> reviewInvalidMeta_;
        // Export jobs run detached; joined at shutdown.
        std::vector<std::thread> reviewJobThreads_;
        std::mutex reviewJobsMutex_;
    };

} // namespace backend::bridge
