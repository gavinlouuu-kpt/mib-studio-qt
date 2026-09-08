#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "backend/app/BackgroundFrame.h"

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

namespace backend
{

    class AppBackend
    {
    public:
        AppBackend();
        ~AppBackend();

        bool initialize(const std::string &dataDir);

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

        // Select a MindVision camera by enumeration index (does not start capture).
        // Fails (returning false with an actionable errorOut) when this build was
        // compiled without MindVision support instead of silently falling back to
        // the mock camera (issue #338).
        bool setMindVisionCameraSelection(int cameraIndex, const std::string &label,
                                          std::string *errorOut = nullptr);

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

        // Frame recording mode: record non-empty frames directly to HDF5 (images + metadata only, no contour processing)
        // Returns false if recording cannot start (e.g., capture not running, file error)
        bool startFrameRecording(const std::string& hdf5FilePath);
        void stopFrameRecording();
        bool isFrameRecording() const;
        uint64_t frameRecordingCount() const;     // Frames written so far
        uint64_t frameRecordingFiltered() const;   // Empty frames skipped

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
        std::unique_ptr<services::SyringePumpService> syringePumpService_;
        std::unique_ptr<services::PulseGeneratorService> pulseGeneratorService_;
        std::shared_ptr<playback::FrameStore> frameStore_;

        // Last selected hardware device (for script apply)
        int selectedIfIndex_{-1};
        int selectedDevIndex_{-1};
        std::string selectedLabel_;
        int selectedMvCameraIndex_{-1};
        std::string lastMindVisionConfigPath_;
        bool mockCameraConfigured_{false};

        // Where pipeline-timing CSVs are dumped (set in initialize()).
        std::string pipelineTimingDir_;

        // Frame recording state
        std::unique_ptr<std::thread> frameRecordingThread_;
        std::atomic<bool> frameRecordingRunning_{false};
        std::atomic<uint64_t> frameRecordingWritten_{0};
        std::atomic<uint64_t> frameRecordingFiltered_{0};
        std::string frameRecordingPath_;

        mutable std::mutex backgroundCaptureCallbackMutex_;
        BackgroundCaptureCallback backgroundCaptureCallback_;

        // Raw config JSON for HDF5 metadata persistence
        mutable std::mutex configJsonMutex_;
        std::string lastConfigJson_;
    };

} // namespace backend
