#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "backend/app/BackgroundFrame.h"
#include "backend/processing/EModulusLutCatalog.h" // HttpGetFn seam (ADR 0002)
#include "backend/app/ExperimentReadiness.h"
#include "backend/diagnostics/MemoryBudget.h"
#include "backend/recording/RecordingAccounting.h"

namespace backend::services
{
    class SqliteService;
    class Hdf5Service;
    class CaptureService;
    class ProcessingService;
    class PlaybackService;
    class CameraControlService;
    class AutofocusService;
    class TriggerService;
    class YoloService;
    class SyringePumpService;
    class PulseGeneratorService;
    namespace serialbus
    {
        class SerialBusManager;
    }
}

namespace backend
{
    namespace playback
    {
        class FrameStore;
    }
}
namespace camera::mock
{
    struct MockCameraOptions;
}

namespace backend::app { class ExperimentCoordinator; }

namespace backend
{

    enum class ApplicationMode { Instrument, AnalysisOnly };

    class AppBackend
    {
    public:
        AppBackend();
        ~AppBackend();

        bool initialize(const std::string &dataDir, ApplicationMode mode = ApplicationMode::Instrument);
        ApplicationMode applicationMode() const { return applicationMode_; }

        // Inject the HTTP GET used to fetch the E-modulus LUT manifest/blob
        // (ADR 0002); the shell supplies it so the backend links no Qt
        // networking. Without a fetcher, remote LUT fetch is skipped and the
        // cached/bundled LUT is used. Call before initialize().
        void setLutHttpFetcher(HttpGetFn fetcher) { lutHttpGet_ = std::move(fetcher); }
        // Base directory for the LUT cache — the shell passes the platform
        // app-data dir so the on-disk cache location is unchanged. Call before
        // initialize(). (The env override MIB_STUDIO_EMODULUS_LUT_CACHE_DIR
        // still takes precedence.)
        void setLutAppDataDir(std::string dir) { lutAppDataDir_ = std::move(dir); }

        // Stop every service-owned thread in dependency order (capture →
        // trigger → recording → realtime/processing). Idempotent; called by
        // the destructor so teardown never depends on GUI close handling.
        void shutdown();

        services::SqliteService &sqlite();
        services::Hdf5Service &hdf5();
        services::CaptureService &capture();
        services::ProcessingService &processing();
        services::PlaybackService &playback();
        services::CameraControlService &cameraControl();
        services::AutofocusService &autofocus();
        services::TriggerService &trigger();
        services::YoloService &yolo();
        services::SyringePumpService &syringePump();
        services::PulseGeneratorService &pulseGenerator();
        
        // Get frame store for service lifecycle management
        std::shared_ptr<playback::FrameStore> getFrameStore() const { return frameStore_; }

        void configureMockCamera(const camera::mock::MockCameraOptions &options);

        // Select a specific hardware device (does not start capture)
        void setHardwareCameraSelection(int interfaceIndex, int deviceIndex, const std::string &label);

        // Select a MindVision camera by enumeration index (does not start capture)
        void setMindVisionCameraSelection(int cameraIndex, const std::string &label);

        // Apply a JS camera script to currently selected hardware device.
        // If capture is running, it will be stopped first. Capture remains stopped.
        bool applyCameraScriptFromFile(const std::string &path, std::string *errorOut = nullptr);

        // Apply a JSON config file to the currently selected MindVision camera.
        // If capture is running, it will be stopped first. Capture remains stopped.
        bool applyMindVisionConfigFromFile(const std::string &path, std::string *errorOut = nullptr);

        // Returns true if a MindVision camera is currently selected.
        bool isMindVisionCameraSelected() const;

        // Fire one software acquisition trigger on the live capture camera
        // (camera must be running in soft-trigger mode). NOT the sort pulse.
        bool softTriggerCamera(std::string *errorOut = nullptr);

        // Issue GenICam DeviceReset to the selected hardware camera.
        // If capture is running, it will be stopped first. Capture remains stopped.
        bool resetSelectedHardwareCamera(std::string *errorOut = nullptr);

        // Check if a camera is configured (either hardware or mock)
        bool isCameraConfigured() const;

        // Authoritative selected-device snapshot (BE-2, #272): which source is
        // selected, its identity/labels/indices, applied config/script paths,
        // and the mock parameters. Values survive capture start/stop.
        struct CameraSelectionSnapshot
        {
            enum class Mode
            {
                None,
                Mock,
                Hardware,
                MindVision,
            };
            Mode mode{Mode::None};
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
        };
        CameraSelectionSnapshot cameraSelection() const;
        // Requested vs effective camera source (issue #369). A hardware
        // selection that could not be honored is reported as a fallback —
        // readiness refuses to treat it as a successful hardware run, and
        // it is never silently presented as "mock selected".
        app::CameraSourceInfo cameraSourceInfo() const;

        // Backend-owned experiment readiness + Start transaction (issue #369).
        app::ExperimentCoordinator& experiment();
        // Shared RS485 bus registry (pump, pulse generator); tests inject a
        // fake serial-port factory here.
        services::serialbus::SerialBusManager& serialBus();

        // Frame recording mode: record non-empty frames directly to HDF5 (images + metadata only, no contour processing)
        // Returns false if recording cannot start (e.g., capture not running, file error)
        bool startFrameRecording(const std::string& hdf5FilePath);
        void stopFrameRecording();
        bool isFrameRecording() const;
        uint64_t frameRecordingCount() const;     // Frames written so far
        uint64_t frameRecordingFiltered() const;   // Empty frames skipped
        // Explicit per-run frame accounting (issue #367): live (reconciled on
        // demand) while recording, otherwise the final snapshot of the last
        // run including its Complete/Partial/Loss/Failed completion state.
        backend::recording::RecordingAccountingSnapshot recordingAccounting() const;

        // Issue #370: byte-budget view of every host-path memory owner
        // (camera/SDK buffers, FrameStore, processing queues/retention,
        // persistence queue, presentation snapshot, exporter) plus process
        // RSS. Unknown vendor memory is reported as Unknown, never as 0.
        backend::diagnostics::HostMemoryBudgetSnapshot memoryBudgetSnapshot() const;

        // Raw config JSON storage (set by config watcher, read at experiment save)
        void setLastConfigJson(const std::string& json);
        std::string getLastConfigJson() const;

        using BackgroundCaptureCallback = std::function<void(const BackgroundCaptureEvent& event)>;
        void setBackgroundCaptureCallback(BackgroundCaptureCallback callback);

        // Fatal save-error sink: invoked (possibly on a writer thread) when an
        // experiment flush or recording write fails, or the write queue
        // overflows. The active operation is stopped; the UI should surface the
        // message. Funnels both recording and experiment flush failures.
        using FatalSaveErrorCallback = std::function<void(const std::string&)>;
        void setFatalSaveErrorCallback(FatalSaveErrorCallback callback);

        // Pipeline latency instrumentation (PipelineTimingRecorder). Enabled at
        // startup via MIB_PIPELINE_TIMING=1 (dump directory override:
        // MIB_PIPELINE_TIMING_DIR) or at runtime through these methods. CSVs
        // are dumped automatically on capture stop and shutdown, or on demand.
        void setPipelineTimingEnabled(bool enabled);
        bool isPipelineTimingEnabled() const;
        // Dump to `directory` (empty = configured/default directory). Returns
        // false and fills errorOut on failure.
        bool dumpPipelineTiming(const std::string& directory = {},
                                std::string* errorOut = nullptr);

    private:
        ApplicationMode applicationMode_{ApplicationMode::Instrument};
        void reportFatalSaveError(const std::string& msg);
        // Best-effort auto-dump used at capture stop/shutdown; logs on failure.
        void dumpPipelineTimingIfEnabled();

        FatalSaveErrorCallback fatalSaveErrorCb_;

        std::unique_ptr<services::SqliteService> sqliteService_;
        std::unique_ptr<services::Hdf5Service> hdf5Service_;
        std::unique_ptr<services::CaptureService> captureService_;
        std::unique_ptr<services::ProcessingService> processingService_;
        std::unique_ptr<services::PlaybackService> playbackService_;
        std::unique_ptr<services::CameraControlService> cameraControlService_;
        std::unique_ptr<services::AutofocusService> autofocusService_;
        std::unique_ptr<services::TriggerService> triggerService_;
        std::unique_ptr<services::YoloService> yoloService_;
        // Shared RS485/Modbus bus registry — declared before the serial
        // services so it outlives their sessions.
        std::unique_ptr<services::serialbus::SerialBusManager> serialBusManager_;
        std::unique_ptr<services::SyringePumpService> syringePumpService_;
        std::unique_ptr<services::PulseGeneratorService> pulseGeneratorService_;
        std::shared_ptr<playback::FrameStore> frameStore_;

        // Shell-injected LUT fetch config (ADR 0002).
        HttpGetFn lutHttpGet_;
        std::string lutAppDataDir_;

        // Last selected hardware device (for script apply)
        int selectedIfIndex_{-1};
        int selectedDevIndex_{-1};
        std::string selectedLabel_;
        int selectedMvCameraIndex_{-1};
        std::string lastMindVisionConfigPath_;
        bool mockCameraConfigured_{false};
        // Selection-snapshot extras (BE-2): last applied camera script and the
        // active mock parameters.
        std::string lastCameraScriptPath_;
        std::string mockFrameDir_;
        int mockIntervalMs_{0};
        bool mockLoop_{true};
        // Issue #369: what was asked for vs what the capture factory builds.
        std::string requestedCameraSource_{"unknown"};
        std::string effectiveCameraSource_{"unknown"};
        std::string cameraFallbackReason_;
        std::unique_ptr<app::ExperimentCoordinator> experimentCoordinator_;

        // Where pipeline-timing CSVs are dumped (set in initialize()).
        std::string pipelineTimingDir_;

        // Frame recording state
        std::unique_ptr<std::thread> frameRecordingThread_;
        std::atomic<bool> frameRecordingRunning_{false};
        std::atomic<uint64_t> frameRecordingWritten_{0};
        std::atomic<uint64_t> frameRecordingFiltered_{0};
        std::string frameRecordingPath_;
        mutable std::mutex recordingAccountingMutex_;
        backend::recording::RecordingAccounting recordingAccounting_;
        backend::recording::RecordingAccountingSnapshot lastRecordingAccounting_;

        mutable std::mutex backgroundCaptureCallbackMutex_;
        BackgroundCaptureCallback backgroundCaptureCallback_;

        // Raw config JSON for HDF5 metadata persistence
        mutable std::mutex configJsonMutex_;
        std::string lastConfigJson_;
    };

} // namespace backend
