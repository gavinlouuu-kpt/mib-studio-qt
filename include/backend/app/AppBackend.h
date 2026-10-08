#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backend/app/BackgroundFrame.h"
#include "backend/processing/EModulusLutCatalog.h" // HttpGetFn seam (ADR 0002)
#include "backend/app/ExperimentReadiness.h"
#include "backend/diagnostics/MemoryBudget.h"
#include "backend/profiles/InstrumentIdentity.h"
#include "backend/profiles/ProfileCache.h" // MethodDraft
#include "backend/profiles/SupabaseProfileRegistry.h" // RegistryHttpTransport seam (ADR 0002)
#include "backend/recording/RecordingAccounting.h"

namespace backend::processing { class IExecutionProvider; }
namespace backend::pz { class PzPlatformMonitor; class PzInstrumentControl; class IPzControlRegisters; enum class InstrumentMode; }

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
    class DotGridService;
    class SyringePumpService;
    class PulseGeneratorService;
    class StageService;
    class RfGeneratorService;
    class MonitoringDensityService;
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
namespace backend::profiles { class ProfileRegistryWorker; }
namespace backend::discovery
{
    class DeviceDiscoveryService;
    class StartupDiscoveryCoordinator;
}

namespace backend
{

    class AppBackend
    {
    public:
        AppBackend();
        ~AppBackend();

        bool initialize(const std::string &dataDir);
        // Shell-resolved read-only install resources; set before initialize.
        void setResourceRoot(std::string path) { resourceRoot_ = std::move(path); }

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

        // Central profile registry (#398): the shell injects the HTTPS POST the
        // registry worker uses (ADR 0002: the backend links no HTTP client).
        // The registry is enabled by MIB_PROFILE_REGISTRY_URL +
        // MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY; without them (or without a
        // transport) the worker is inert. Call before initialize().
        void setProfileRegistryTransport(profiles::RegistryHttpTransport transport)
        {
            profileRegistryTransport_ = std::move(transport);
        }

        // Stop every service-owned thread in dependency order (capture →
        // trigger → recording → realtime/processing). Idempotent; called by
        // the destructor so teardown never depends on GUI close handling.
        void shutdown();

        services::SqliteService &sqlite();
        services::Hdf5Service &hdf5();
        services::CaptureService &capture();
        services::ProcessingService &processing();
        // Source of per-frame results when the science runs on the PL
        // (MIB_EXECUTION_PROVIDER, YOFO S1); null when none is configured.
        processing::IExecutionProvider *executionProvider();
        // Read-only PZ7035 identity and health (#501); null off the instrument.
        pz::PzPlatformMonitor *pzPlatformMonitor();
        // The data directory given to initialize() (recordings default under it).
        const std::string &dataDir() const { return dataDir_; }

        // ---- PZ7035 camera modes (#501 P1) ----
        // Align = the full sensor at 500 fps, LED 0/125 µs, the producer streaming previews.
        // Run = the 512x96 window at (x, y), 5 kHz, the U-Net cell path on, LED 7/60 µs, the
        // producer stopped (its AcquisitionStart would clear the U-Net enable): previews come
        // from the PL cell capture (fetchRunPreview). The producer applies timing, ROI and the
        // receiver reset at AcquisitionStart; PzInstrumentControl writes the LED and cell path.
        // Refused during an experiment or recording, and when the PL is not configured.
        bool instrumentControlAvailable() const;
        bool setInstrumentMode(pz::InstrumentMode mode, int x, int y, std::string *errorOut);
        pz::InstrumentMode instrumentMode() const;
        // The safe state of an unattended instrument (no client for the server's grace time): LED
        // off, cell path off, the camera (producer stream / bridge previews) released. The sensor
        // keeps the timing it was last given. Refused while an experiment or recording runs.
        // instrumentIdle() stays true until the next mode switch (the UI resumes Align/Run).
        bool enterInstrumentIdle(std::string *errorOut);
        bool instrumentIdle() const;
        // The Run window (x, y) last applied or requested; snapped to the producer's steps.
        std::pair<int, int> instrumentRunOffset() const;
        // True once a Run switch has succeeded in this process: instrumentRunOffset() is then the
        // operator's window, not the (0, 0) default (the UI restores its window from it, #501).
        bool instrumentRunWindowSet() const;
        // How often Align's ingress recovery fired in this process (#629): receiver resets issued and
        // switches that never locked.
        struct AlignLockCounters { uint64_t receiverClears{0}, failures{0}; uint32_t lastStuckP13{0}; };
        AlignLockCounters alignLockCounters() const;
        // The PL receiver auto-reset count (results9 RXH1); nullopt without the block.
        std::optional<uint32_t> plReceiverAutoResets();
        // Service / Commissioning mode, latched by the shell: raw LED values are refused
        // outside it, on the backend side (not only in the UI).
        void setServiceMode(bool on);
        // Empty, or why this instrument never opens the nanopositioner, pulse generator or ZC300
        // stage ports: science runs on the PL (PZ7035), whose RS485 bus carries the pumps.
        std::string plScienceSerialBlockReason() const;
        // The Run window survives a restart: written by every successful Run switch to
        // <dataDir>/instrument_run_window.json, read at initialize(). Never cleared, only replaced.
        void loadInstrumentRunWindow();
        bool serviceMode() const;
        // How Align shows the sensor: "bridge" (whole frames from the results bridge) or "bands"
        // (the producer's banded grabber, images before results8); "" outside Align.
        std::string alignSource() const;
        // Service mode only, within the current mode's limits (pz::checkLed); refused during
        // an experiment. The next mode switch restores the preset.
        bool setInstrumentLed(double delayUs, double widthUs, std::string *errorOut);
        // Run mode: one cell capture as a run-preview packet (pz::encodeRunPreview).
        bool fetchRunPreview(std::vector<uint8_t> &out, std::string *errorOut);
        void setInstrumentControlForTesting(std::unique_ptr<pz::IPzControlRegisters> registers);
        services::PlaybackService &playback();
        services::CameraControlService &cameraControl();
        services::AutofocusService &autofocus();
        services::TriggerService &trigger();
        services::DotGridService &dotGrid();
        services::SyringePumpService &syringePump();
        services::PulseGeneratorService &pulseGenerator();
        // Motorized Z stage (ADR 0013): observe-only until an operator homes it.
        services::StageService &stage();
        // SIGLENT SSG3021X sort generator: SCPI readback/provenance link
        // (never timing). Configured by the `rf_generator` block of the
        // application config JSON; consulted by the experiment readiness gate.
        services::RfGeneratorService &rfGenerator();
        // Device discovery job service (issue #419, ADR 0005): every camera /
        // nanopositioner / pulse-generator scan runs through it. Frontends
        // start jobs and poll snapshots; they never enumerate hardware.
        discovery::DeviceDiscoveryService &deviceDiscovery();
        // Startup selection/connection policy over the discovery service.
        // Constructed here but started by the shell (Qt adapter) so headless
        // consumers keep today's no-auto-connect behaviour.
        discovery::StartupDiscoveryCoordinator &startupDiscovery();
        // Central profile registry worker (#398): sign-in, refresh, download and
        // the per-user revision cache on its own thread. Shells enqueue commands
        // and poll snapshots; it never touches capture, recording or Start.
        profiles::ProfileRegistryWorker &profileRegistry();
        // This instrument PC's stable identity (#398 M2): UUID persisted in
        // <dataDir>/instrument_identity.json plus MIB_INSTRUMENT_NAME. Local
        // method validations and run provenance are keyed by it. Empty id
        // before initialize() or when the data dir is unwritable.
        const profiles::InstrumentIdentity &instrumentIdentity() const { return instrumentIdentity_; }
        // The local context a method validation binds to: this instrument,
        // the active processing core build and the effective camera source.
        profiles::MethodContext methodContext() const;
        // "Mark validated" (#398 M2b): checks that the evidence test-run
        // file's frozen /run_provenance names this exact revision, this
        // instrument and the current method context (checkValidationEvidence),
        // then enqueues the worker's RecordValidation. jobId 0 = refused;
        // `error` says why.
        struct MethodValidationRequestResult
        {
            std::uint64_t jobId{0};
            std::string error;
        };
        MethodValidationRequestResult requestMethodValidation(const std::string &revisionId,
                                                              const std::string &evidenceFile,
                                                              bool passed);
        // Draft content from the instrument (#398 M3b): the applied
        // config.json and the active processing core (version as core id,
        // its contract version). Camera script / compatibility are left empty
        // for SaveDraft to take from a base revision. `error` set (and the
        // draft empty) when no config.json is applied.
        profiles::MethodDraft currentConfigDraft(std::string *error = nullptr) const;
        
        // Get frame store for service lifecycle management
        std::shared_ptr<playback::FrameStore> getFrameStore() const { return frameStore_; }

        void configureMockCamera(const ::camera::mock::MockCameraOptions& options);

        // Select a specific hardware device (does not start capture)
        void setHardwareCameraSelection(int interfaceIndex, int deviceIndex, const std::string &label);

        // Select a MindVision camera by enumeration index (does not start capture)
        void setMindVisionCameraSelection(int cameraIndex, const std::string &label);

        // Apply a JS camera script to currently selected hardware device.
        // If capture is running, it will be stopped first. Capture remains stopped.
        bool applyCameraScriptFromFile(const std::string &path, std::string *errorOut = nullptr);

        // Apply a JSON config file to the currently selected MindVision camera.
        // If capture is running, it will be stopped first. Capture remains stopped.
        // Save/select the next-start profile without opening the camera.
        bool stageMindVisionConfigFromFile(const std::string& path,
                                           std::string* errorOut = nullptr);
        bool applyMindVisionConfigFromFile(const std::string &path, std::string *errorOut = nullptr);

        // Returns true if a MindVision camera is currently selected.
        bool isMindVisionCameraSelected() const;

        // Lifecycle-owner calls only. Stops capture/processing, replaces the frame
        // store, stages the mode; caller decides whether to restart. No hardware
        // is opened while idle. Rejected during an experiment or recording.
        bool setMindVisionOverview(bool overview, std::string* errorOut = nullptr);
        bool isMindVisionOverview() const { return mindVisionOverview_.load(); }
        struct MindVisionSensor {
            int sensorWidth{0}, sensorHeight{0}, minWidth{1}, minHeight{1};
        };
        MindVisionSensor mindVisionSensor() const;
        // Atomic experiment-profile update; does not reconfigure the live overview.
        bool saveMindVisionRoi(int x, int y, int width, int height,
                               std::string* errorOut = nullptr);

        // Camera & Alignment (Overview) for any camera that has one, as the Qt Overview tab
        // does for MindVision: the whole sensor is shown and the experiment window (ROI 1,
        // sensor coordinates) is placed on it. MindVision uses its profile; an Aravis camera
        // (YOFO Studio, PZ7035 producer) uses <data>/config/aravis-camera.json and the
        // Overview preset there. Same rules as setMindVisionOverview: stops capture and
        // realtime processing, swaps the frame store, rejected during an experiment or
        // recording; the caller restarts capture.
        struct CameraGeometry {
            bool supported{false};   // the selected camera has an Overview mode
            bool overview{false};
            std::string camera;      // "mindvision" | "aravis" | ""
            int sensorWidth{0}, sensorHeight{0};   // 0 = not known yet (no start so far)
            int roiX{0}, roiY{0}, roiWidth{0}, roiHeight{0};
            int widthIncrement{1}, heightIncrement{1}, offsetXIncrement{1}, offsetYIncrement{1};
            int minWidth{1}, minHeight{1};
            std::string sessionJson{"{}"}; // last camera read-back (Aravis: rate model)
        };
        CameraGeometry cameraGeometry() const;
        bool setCameraOverview(bool overview, std::string* errorOut = nullptr);
        bool isCameraOverview() const;
        bool saveCameraRoi(int x, int y, int width, int height, std::string* errorOut = nullptr);

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
                Aravis,
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
        // Live Monitoring scatter density (KDE) and core contour: the shells
        // push settings and read results; the provisional record goes to the
        // experiment coordinator from the backend worker.
        services::MonitoringDensityService& monitoringDensity();
        // Shared RS485 bus registry (pump, pulse generator); tests inject a
        // fake serial-port factory here.
        services::serialbus::SerialBusManager& serialBus();

        // Frame recording mode: record non-empty frames directly to HDF5 (images + metadata only, no contour processing)
        // Returns false if recording cannot start (e.g., capture not running, file error)
        bool startFrameRecording(const std::string& hdf5FilePath, std::string* error = nullptr);
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
        bool startFrameRecordingLocked(const std::string& hdf5FilePath, std::string* error);
        void reportFatalSaveError(const std::string& msg);
        // Best-effort auto-dump used at capture stop/shutdown; logs on failure.
        void dumpPipelineTimingIfEnabled();

        FatalSaveErrorCallback fatalSaveErrorCb_;

        std::unique_ptr<services::SqliteService> sqliteService_;
        std::unique_ptr<services::Hdf5Service> hdf5Service_;
        std::unique_ptr<services::CaptureService> captureService_;
        std::unique_ptr<services::ProcessingService> processingService_;
        // Declared after processingService_: destroyed (and its thread stopped)
        // before the service it feeds.
        std::unique_ptr<processing::IExecutionProvider> executionProvider_;
        std::unique_ptr<pz::PzPlatformMonitor> pzPlatformMonitor_;
        std::unique_ptr<pz::PzInstrumentControl> pzControl_;
        mutable std::mutex instrumentModeMutex_; // serialises mode switches
        std::atomic<int> instrumentMode_{0};      // pz::InstrumentMode
        std::atomic<bool> instrumentStopped_{false}; // shutdown() switched the LED off already
        std::atomic<int> instrumentRunX_{0}, instrumentRunY_{0};
        std::atomic<bool> instrumentRunSet_{false};
        std::atomic<bool> instrumentIdle_{false};
        std::atomic<uint64_t> alignReceiverClears_{0}, alignLockFailures_{0};
        std::atomic<uint32_t> alignLastStuckP13_{0}; // P[13] read before the latest recovery attempt
        std::atomic<bool> serviceMode_{false};
        // Align live view: "bridge" (whole frames, results8 on) or "bands" (producer grabber).
        std::atomic<int> alignSource_{0}; // 0 none, 1 bridge, 2 bands (read by the status poll)
        bool alignWholeFrameAvailable(std::string *why);
        std::unique_ptr<services::PlaybackService> playbackService_;
        std::unique_ptr<services::CameraControlService> cameraControlService_;
        std::unique_ptr<services::AutofocusService> autofocusService_;
        std::unique_ptr<services::TriggerService> triggerService_;
        std::unique_ptr<services::DotGridService> dotGridService_;
        // Shared RS485/Modbus bus registry — declared before the serial
        // services so it outlives their sessions.
        std::unique_ptr<services::serialbus::SerialBusManager> serialBusManager_;
        std::unique_ptr<services::SyringePumpService> syringePumpService_;
        std::unique_ptr<services::PulseGeneratorService> pulseGeneratorService_;
        // After serialBusManager_: destroyed first, so its port closes on a live bus.
        std::unique_ptr<services::StageService> stageService_;
        std::unique_ptr<services::RfGeneratorService> rfGeneratorService_;
        // Declared after every service the providers/hooks reference so the
        // discovery workers and the coordinator are destroyed first.
        std::unique_ptr<discovery::DeviceDiscoveryService> deviceDiscovery_;
        std::unique_ptr<discovery::StartupDiscoveryCoordinator> startupDiscovery_;
        std::shared_ptr<playback::FrameStore> frameStore_;

        profiles::RegistryHttpTransport profileRegistryTransport_;
        std::unique_ptr<profiles::ProfileRegistryWorker> profileRegistry_;
        profiles::InstrumentIdentity instrumentIdentity_;

        // Shell-injected LUT fetch config (ADR 0002).
        HttpGetFn lutHttpGet_;
        std::string lutAppDataDir_;
        std::string resourceRoot_;

        // Last selected hardware device (for script apply)
        int selectedIfIndex_{-1};
        int selectedDevIndex_{-1};
        std::string selectedLabel_;
        int selectedMvCameraIndex_{-1};
        std::string lastMindVisionConfigPath_;
        std::string savedMindVisionConfigPath_;
        void releaseMindVisionOverviewStore();
        std::atomic<bool> mindVisionOverview_{false};
        size_t mindVisionExperimentCapacity_{5000};
        mutable std::mutex mindVisionSensorMutex_;
        MindVisionSensor mindVisionSensor_{};
        bool mockCameraConfigured_{false};
        bool aravisCameraConfigured_{false};
        bool aravisFake_{false};
        std::string aravisDeviceId_;
        bool aravisGigE_{false};
        // Aravis camera profile (<data>/config/aravis-camera.json): experiment window and the
        // rate/exposure of each mode. 0 = leave the device's value.
        struct AravisProfile {
            bool hasRoi{false};
            int x{0}, y{0}, width{0}, height{0};
            double experimentFps{0.0}, experimentExposureUs{0.0};
            double overviewFps{830.0}, overviewExposureUs{900.0}; // lit full field (PZ7035)
            // PZ7035 line period for the overview (PzHmax): 0 = the producer's rule; 232 for the
            // whole-frame Align on results8 on. Experiment windows always use 0.
            int overviewHmax{0};
            // Previews the PS asks for per second (PzPreviewRate); the PL sees every frame.
            // 60 keeps a 30 fps display fresh with margin; 0 = every delivered frame.
            double previewRateHz{60.0};
        };
        AravisProfile aravisProfile_;
        std::atomic<bool> aravisOverview_{false};
        size_t aravisExperimentCapacity_{0};
        bool aravisRealtimeBeforeOverview_{false};
        struct AravisSessionGeometry {
            int sensorWidth{0}, sensorHeight{0};
            int widthIncrement{1}, heightIncrement{1}, offsetXIncrement{1}, offsetYIncrement{1};
            std::string json{"{}"};
        };
        mutable std::mutex aravisSessionMutex_;
        AravisSessionGeometry aravisSession_;
        std::string dataDir_;
        std::string aravisProfilePath() const;
        void loadAravisProfile();
        bool saveAravisProfile(const AravisProfile& profile, std::string* errorOut);
        void installAravisFactory();
        bool setAravisOverview(bool overview, std::string* errorOut);
        bool saveAravisRoi(int x, int y, int width, int height, std::string* errorOut);
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
        // Declared after the coordinator and processing so it is destroyed
        // first: its worker reads the monitoring ring and feeds the coordinator.
        std::unique_ptr<services::MonitoringDensityService> monitoringDensity_;

        // Where pipeline-timing CSVs are dumped (set in initialize()).
        std::string pipelineTimingDir_;

        // Frame recording state
        std::unique_ptr<std::thread> frameRecordingThread_;
        std::mutex frameRecordingLifecycleMutex_;
        std::atomic<bool> frameRecordingOwned_{
            false}; // retained through Stop/join, including worker failure
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
