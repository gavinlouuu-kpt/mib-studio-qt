// windows.h (via the MindVision SDK headers below) must not define min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "backend/app/AppBackend.h"
#include "backend/app/ApplicationIdentity.h"
#include <thread>
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/MethodApply.h"
#include "backend/app/Tools.h"
#include "backend/app/WallClock.h"

#include "backend/services/Logger.h"
#include "backend/services/CrashReporter.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/database/SqliteService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfWriteQueue.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/recording/RoiCrop.h"
#include "backend/services/CaptureService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/app/SciencePlacement.h"
#include "backend/processing/pz/ExecutionProviderFactory.h"
#include "backend/playback/PlaybackService.h"
#include "backend/playback/FrameStore.h"
#include "backend/camera/egrabber/EGrabberCamera.h"
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/TriggerService.h"
#include "backend/services/DotGridService.h"
#include "backend/services/SerialBus.h"
#include "backend/services/SyringePumpService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/services/StageService.h"
#include "backend/services/RfGeneratorService.h"
#include "backend/services/MonitoringDensityService.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/discovery/providers/CameraEnumerationProvider.h"
#include "backend/discovery/providers/NanopositionerProvider.h"
#include "backend/discovery/providers/Zc300Provider.h"
#include "backend/discovery/providers/PulseGeneratorProvider.h"
#include "backend/processing/EModulusLutCatalog.h"
#include "backend/profiles/ProfileRegistryWorker.h"

#include "backend/camera/mindvision/MindVisionConfig.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <spdlog/spdlog.h>
#include <opencv2/core.hpp>
#include "backend/processing/OpenCvThreads.h"
#include "backend/pz/PzBridgePreviewCamera.h"
#include "backend/pz/PzInstrumentControl.h"
#include "backend/pz/PzPlatformMonitor.h"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#ifndef MIB_HAS_EGRABBER
#define MIB_HAS_EGRABBER 0
#endif
#ifndef MIB_HAS_MINDVISION
#define MIB_HAS_MINDVISION 0
#endif
#ifndef MIB_HAS_ARAVIS
#define MIB_HAS_ARAVIS 0
#endif
#if MIB_HAS_ARAVIS
#include "backend/camera/aravis/AravisCamera.h"
#endif

namespace backend
{
    namespace
    {
    // Reads a variable from the OS environment, not the C runtime's startup
    // copy: on Windows the MSVC CRT snapshots the environment at process start,
    // so a variable the host sets later (the Tauri shell or a Rust test via
    // std::env::set_var -> SetEnvironmentVariable) is invisible to std::getenv
    // (#571; the bridge shim's queueCapacityFromEnv does the same).
    std::optional<std::string> processEnvironmentValue(const char* name)
    {
#ifdef _WIN32
        const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
        if (size == 0) return std::nullopt;
        std::string value(size, '\0');
        const DWORD n = GetEnvironmentVariableA(name, value.data(), size);
        if (n == 0 || n >= size) return std::nullopt;
        value.resize(n);
        return value;
#else
        if (const char* value = std::getenv(name)) return std::string(value);
        return std::nullopt;
#endif
    }

    // OpenCV's MSVC build parallelises through the Concurrency Runtime: one worker
    // per logical CPU whose idle workers spin. A per-frame parallel call in the
    // realtime loop kept ~31 of 32 workers busy on the rig PC and starved the
    // processing thread (5000 fps experiments fell to ~2900 processed/s).
    // Processing already spreads frames over its own threads, so OpenCV's inner
    // parallel_for is off by default (OpenCvThreads.h; processing-core plugins
    // apply the same setting to their own, statically linked OpenCV).
    void configureOpenCvThreads()
    {
        const auto setting = processing::applyOpenCvThreadsFromEnvironment();
        if (setting.invalid)
        {
            SPDLOG_WARN("AppBackend: ignoring invalid MIB_OPENCV_THREADS='{}'", setting.raw);
        }
        if (setting.keepOpenCvDefault)
        {
            SPDLOG_INFO("AppBackend: OpenCV threads left at OpenCV default ({})", cv::getNumThreads());
            return;
        }
        SPDLOG_INFO("AppBackend: OpenCV threads set to {} (getNumThreads={})", setting.threads, cv::getNumThreads());
    }

    // Builds the capture-owned MindVision camera for `path`. When the saved
    // profile enables illuminated Live View, the same validated parse the
    // camera uses at start (parseConfig: connection, range and timing rules)
    // decides here whether a generator session is attached, so a profile that
    // would fail at Play is rejected when it is staged. Throws std::runtime_error
    // with the operator-facing reason; no hardware is touched.
    std::unique_ptr<::camera::common::ICamera>
    makeLiveCamera(int index, const std::string& path, services::PulseGeneratorService& generator,
                   bool overview,
                   std::function<void(const camera::mindvision::SdkCapability&)> capabilitySink) {
        std::shared_ptr<services::IlluminationSession> session;
        camera::mindvision::Config effective;
        if (!path.empty()) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot read saved MindVision setup: " + path);
            const std::string bytes((std::istreambuf_iterator<char>(input)), {});
            const auto parsed = backend::camera::mindvision::parseConfig(bytes);
            if (!parsed.ok) throw std::runtime_error(parsed.error);
            effective =
                overview ? camera::mindvision::overviewConfig(parsed.config) : parsed.config;
            effective.requireExactGeometry = true;
            if (effective.illuminatedLive) {
                const auto timingError =
                    camera::mindvision::detail::validateLiveViewTiming(effective);
                if (!timingError.empty()) throw std::runtime_error(timingError);
                const auto& lv = effective.liveView;
                services::PulseGeneratorService::Config cfg;
                cfg.portName = lv.port;
                cfg.modbusAddress = static_cast<uint8_t>(lv.address);
                cfg.serial.baudRate = lv.baud;
                cfg.serial.dataBits = lv.dataBits;
                cfg.serial.parity = lv.parity;
                cfg.serial.stopBits = lv.stopBits;
                const int channel = lv.channel - 1; // service channels are 0-based
                const double hz = lv.frequencyHz;
                const double duty = lv.dutyPercent;
                session = std::make_shared<services::IlluminationSession>();
                const auto owner = std::make_shared<char>();
                session->prepare = [&generator, cfg, channel, hz, duty, owner] {
                    auto resolved = cfg;
                    if (resolved.portName == "auto") {
                        std::string error;
                        if (!generator.discoverLiveView(resolved, channel, services::serialbus::availablePorts(), &error)) {
                            SPDLOG_ERROR("Illuminated Live View: {}", error);
                            throw std::runtime_error(error);
                        }
                        SPDLOG_INFO("Illuminated Live View: pulse generator discovered on {} (addr {})",
                                    resolved.portName, resolved.modbusAddress);
                    }
                    return generator.beginLiveView(resolved, channel, hz, duty, owner.get());
                };
                session->enable = [&generator, owner] {
                    return generator.enableLiveView(owner.get());
                };
                session->disable = [&generator, owner] {
                    return generator.endLiveView(owner.get());
                };
            }
        }
        return std::make_unique<::camera::common::MindVisionCamera>(
            index, path, nullptr, session, overview,
            path.empty() ? std::nullopt : std::make_optional(effective), std::move(capabilitySink));
    }

        // Get a user-writable log path, falling back to dataDir if needed
        std::string getLogPath(const std::string &dataDir)
        {
            std::filesystem::path dataPath(dataDir);
            std::string logPath;

#ifdef _WIN32
            // Check if dataDir is in Program Files (requires admin to write)
            std::string dataDirLower = dataDir;
            std::transform(dataDirLower.begin(), dataDirLower.end(), dataDirLower.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });

            // Check if path contains "program files" (common install location)
            if (dataDirLower.find("program files") != std::string::npos ||
                dataDirLower.find("program files (x86)") != std::string::npos)
            {
                // Use user-writable location instead
                char appDataPath[MAX_PATH];
                if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath)))
                {
                    std::filesystem::path userLogDir = std::filesystem::path(appDataPath) / "MIB_Studio_Qt" / "logs";
                    std::filesystem::create_directories(userLogDir);
                    logPath = (userLogDir / "app.log").string();
                    return logPath;
                }
            }
#endif
            // Default: use dataDir/logs/app.log
            std::filesystem::create_directories(dataPath / "logs");
            logPath = (dataPath / "logs" / "app.log").string();
            return logPath;
        }

        BackgroundFramePixelFormat pixelFormatForMatType(int type)
        {
            switch (type)
            {
            case CV_8UC1:
                return BackgroundFramePixelFormat::Gray8;
            case CV_8UC3:
                return BackgroundFramePixelFormat::Bgr8;
            case CV_8UC4:
                return BackgroundFramePixelFormat::Bgra8;
            default:
                return BackgroundFramePixelFormat::Unknown;
            }
        }

        BackgroundFrame makeBackgroundFrame(const cv::Mat &image)
        {
            BackgroundFrame frame;
            if (image.empty())
            {
                return frame;
            }

            const auto pixelFormat = pixelFormatForMatType(image.type());
            if (pixelFormat == BackgroundFramePixelFormat::Unknown)
            {
                SPDLOG_WARN("AppBackend: unsupported background frame type {}", image.type());
                return frame;
            }

            frame.width = static_cast<std::uint64_t>(image.cols);
            frame.height = static_cast<std::uint64_t>(image.rows);
            frame.strideBytes = static_cast<std::size_t>(image.cols) * image.elemSize();
            frame.pixelFormat = pixelFormat;
            frame.data.resize(frame.strideBytes * static_cast<std::size_t>(image.rows));

            for (int row = 0; row < image.rows; ++row)
            {
                const auto *src = image.ptr<std::uint8_t>(row);
                auto *dst = frame.data.data() + static_cast<std::size_t>(row) * frame.strideBytes;
                std::memcpy(dst, src, frame.strideBytes);
            }

            return frame;
        }
    }

    AppBackend::AppBackend()
    {
        // A PL-science instrument is the PZ7035: no RTC, so an unsynced clock is labelled as such in saved files (G14).
        app::WallClock::setBoardWithoutRtc(!app::hostProcessingAvailable());
        // Exists from construction so shells can bind to it before
        // initialize(); it stays idle until a shell enables it, and its
        // callbacks tolerate services that are not built yet.
        monitoringDensity_ = std::make_unique<services::MonitoringDensityService>(
            [this] {
                // Valid monitoring cells in chart units (µm², deformability).
                services::MonitoringDensityInput in;
                if (!processingService_) return in;
                in.pixelToMicron = processingService_->getPixelToMicronFactor();
                const double areaFactor = in.pixelToMicron * in.pixelToMicron;
                const auto cells = processingService_->getMonitoringValidPoints();
                in.frameIndices.reserve(cells.size());
                in.points.reserve(cells.size());
                for (const auto& c : cells) {
                    in.frameIndices.push_back(c.index);
                    in.points.push_back({c.area * areaFactor, c.deformability});
                }
                return in;
            },
            [this] {
                // Falling behind: frames dropped by the batch queue or the
                // experiment buffer, or a batch queue backlog.
                services::MonitoringPipelineLoad load;
                if (!processingService_) return load;
                const auto batch = processingService_->getBatchPipelineStats();
                load.droppedFrames = batch.framesDropped + processingService_->getDroppedValidFrames() +
                                     processingService_->getDroppedInvalidFrames();
                load.queueDepth = batch.running ? batch.currentQueueDepth : 0;
                load.queueCapacity = batch.running ? batch.queueCapacity : 0;
                return load;
            },
            [this](std::string json) {
                if (experimentCoordinator_) experimentCoordinator_->setLiveKdeCoreRecord(std::move(json));
            });
    }

    AppBackend::~AppBackend() {
        shutdown();
    }

    void AppBackend::shutdown() {
        SPDLOG_INFO("AppBackend: shutdown begin");
        // The registry worker shares nothing with the instrument; stop it
        // first so an in-flight request is aborted rather than waited out.
        if (profileRegistry_) {
            profileRegistry_->shutdown();
        }
        // Discovery first (issue #419): stop the startup policy so no late
        // result can select or connect anything, refuse new jobs, cancel and
        // join every discovery worker. Only then may serial adapters and the
        // camera be released below: a probe must never observe a half
        // torn-down service graph.
        if (startupDiscovery_) {
            startupDiscovery_->stop();
        }
        if (deviceDiscovery_) {
            SPDLOG_INFO("AppBackend: shutdown draining device discovery");
            deviceDiscovery_->shutdownDiscovery();
        }
        // Stop threads before member destruction begins. Members are destroyed
        // in reverse declaration order, so triggerService_/autofocusService_
        // die before processingService_ — a still-running realtime loop would
        // invoke its callbacks on freed services. Every call below is
        // idempotent, so shutdown() may run more than once.

        // The density worker reads the monitoring ring and hands records to
        // the coordinator: join it before either is finalized or stopped.
        if (monitoringDensity_) {
            monitoringDensity_->stop();
        }

        // An active experiment is finalized (file closed, accounting written)
        // while every service it needs is still alive.
        if (experimentCoordinator_) {
            experimentCoordinator_->shutdown();
        }

        // Stop admitting new trigger requests before anything is torn down.
        stopLiveResults();
        if (executionProvider_) {
            executionProvider_->stop(); // no further ingest into processing
        }
        if (processingService_) {
            processingService_->setTargetGroupCallback({});
            processingService_->setBackgroundCaptureCallback({});
        }

        // Teardown order (issue #365): capture stop runs with the camera-ready
        // callback still wired, so TriggerService unbinds (waiting for any
        // in-flight pulse) and stops while the camera object is still valid.
        // Only after that is the trigger service stopped a second time
        // (idempotent) and the callback cleared. Clearing the callback first
        // left TriggerService holding a camera pointer across the camera's
        // destruction on the capture thread.
        if (captureService_) {
            SPDLOG_INFO("AppBackend: shutdown stopping capture and releasing camera");
            captureService_->stop();
        }
        // The strobe keeps pulsing after the process exits: an Align or Run session must not leave
        // the LED lit. A blank PL (no PCFG_DONE) or another image is refused and writes nothing.
        // Once: shutdown() runs again from the destructor, when a test's injected registers may be gone.
        if (pzControl_ && !instrumentStopped_.exchange(true)) {
            std::string ledError;
            if (!pzControl_->ledOff(&ledError)) SPDLOG_INFO("AppBackend: LED left as it is at shutdown: {}", ledError);
        }
        if (triggerService_) {
            triggerService_->setCamera(nullptr);
            triggerService_->stop();
        }
        if (captureService_) {
            captureService_->setCameraReadyCallback({});
        }
        if (dotGridService_) {
            dotGridService_->setPoseCallback({});
            dotGridService_->stop();
        }
        stopFrameRecording();
        if (processingService_) {
            SPDLOG_INFO("AppBackend: shutdown stopping processing");
            processingService_->stopBackgroundCalibration();
            processingService_->stopRealtime();
            processingService_->stopBatchPipeline();
            processingService_->stop();
        }
        // Release hardware during explicit shutdown, while the service graph
        // is still alive. Capture has ended its owned generator session and
        // processing can no longer submit autofocus or trigger requests.
        if (autofocusService_) {
            SPDLOG_INFO("AppBackend: shutdown disconnecting nanopositioner");
            autofocusService_->disconnect();
        }
        if (syringePumpService_) {
            SPDLOG_INFO("AppBackend: shutdown disconnecting syringe pumps");
            syringePumpService_->disconnect(services::SyringePumpService::PumpId::Sample);
            syringePumpService_->disconnect(services::SyringePumpService::PumpId::Sheath);
        }
        if (pulseGeneratorService_) {
            SPDLOG_INFO("AppBackend: shutdown disconnecting pulse generator");
            pulseGeneratorService_->disconnect();
        }
        if (stageService_) {
            // Cancels any operation (stopping the axis), joins the stage
            // worker, then releases the port while the bus is alive.
            SPDLOG_INFO("AppBackend: shutdown stopping the Z stage");
            stageService_->shutdown();
        }
        if (rfGeneratorService_) rfGeneratorService_->disconnect();
        // All pipeline threads are stopped now, so the dump is an exact
        // snapshot of the recorded latency data.
        dumpPipelineTimingIfEnabled();
        SPDLOG_INFO("AppBackend: shutdown complete");
    }

    bool AppBackend::initialize(const std::string &dataDir)
    {
        dataDir_ = dataDir;
        std::filesystem::create_directories(dataDir);

        // Use user-writable location for logs if dataDir is in Program Files
        std::string logPath = getLogPath(dataDir);
        backend::services::Logger::init(logPath);

        // Pipeline latency instrumentation: opt in via environment so field
        // diagnosis needs no rebuild. CSVs land in pipelineTimingDir_ on
        // capture stop / shutdown (see PipelineTimingRecorder).
        pipelineTimingDir_ = (std::filesystem::path(dataDir) / "pipeline_timing").string();
        if (const char *timingDir = std::getenv("MIB_PIPELINE_TIMING_DIR"))
        {
            if (*timingDir != '\0') pipelineTimingDir_ = timingDir;
        }
        if (const char *timingEnv = std::getenv("MIB_PIPELINE_TIMING"))
        {
            const std::string value(timingEnv);
            if (value == "1" || value == "true" || value == "on")
            {
                diagnostics::PipelineTimingRecorder::instance().setEnabled(true);
                SPDLOG_INFO("AppBackend: pipeline timing instrumentation enabled "
                            "(MIB_PIPELINE_TIMING), dump dir: {}",
                            pipelineTimingDir_);
            }
        }

        {
            const char *instrumentName = std::getenv("MIB_INSTRUMENT_NAME");
            std::string identityWarning;
            instrumentIdentity_ = profiles::loadOrCreateInstrumentIdentity(
                dataDir, instrumentName ? instrumentName : "", &identityWarning);
            if (!identityWarning.empty()) SPDLOG_WARN("AppBackend: {}", identityWarning);
            SPDLOG_INFO("AppBackend: instrument id {}{}",
                        instrumentIdentity_.id.empty() ? "<unknown>" : instrumentIdentity_.id,
                        instrumentIdentity_.name.empty() ? "" : " (" + instrumentIdentity_.name + ")");
        }

        {
            profiles::RegistryWorkerConfig registryConfig;
            if (auto url = processEnvironmentValue("MIB_PROFILE_REGISTRY_URL")) registryConfig.origin = *url;
            if (auto key = processEnvironmentValue("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY"))
                registryConfig.publishableKey = *key;
            registryConfig.cacheDir = std::filesystem::path(dataDir) / "profile_registry";
            registryConfig.methodsDir = std::filesystem::path(dataDir) / "methods";
            if (registryConfig.configured() && !profileRegistryTransport_)
                SPDLOG_WARN("AppBackend: profile registry configured but the shell supplied no "
                            "HTTP transport; registry disabled");
            profileRegistry_ = std::make_unique<profiles::ProfileRegistryWorker>(
                std::move(registryConfig), profileRegistryTransport_);
            SPDLOG_INFO("AppBackend: central profile registry {}",
                        profileRegistry_->snapshot().configured ? "enabled" : "disabled");
        }

        sqliteService_ = std::make_unique<services::SqliteService>();
        hdf5Service_ = std::make_unique<services::Hdf5Service>();
        captureService_ = std::make_unique<services::CaptureService>();
        processingService_ = std::make_unique<services::ProcessingService>();
        experimentCoordinator_ = std::make_unique<app::ExperimentCoordinator>(*this);
        experimentCoordinator_->setApplicationIdentity(
            MIB_APPLICATION_VERSION, MIB_APPLICATION_BUILD_ID, MIB_APPLICATION_OS);
        processingService_->setBackgroundPublicationTransaction([this](const std::function<void()>& apply) {
            // A worker must not block on a transaction that may join that worker,
            // so it only try-locks. Status polls hold the coordinator mutex
            // briefly, so retry for a bounded time before giving up. Otherwise a
            // calibration would be cancelled by mere contention. A joiner that
            // holds the mutex is only delayed by this bound.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            do {
                if (experimentCoordinator_->withIdleConfiguration(apply, false)) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        });
        // Funnel experiment flush-write failures to the coordinator (which
        // finalizes the run as Failed) and to the fatal-save-error sink the UI
        // surfaces.
        processingService_->setFlushErrorCallback([this](const std::string& msg) {
            if (experimentCoordinator_) experimentCoordinator_->onFatalSaveError(msg);
            reportFatalSaveError(msg);
        });
        processingService_->setFlushRequestCallback([this] {
            if (experimentCoordinator_) experimentCoordinator_->requestFlush();
        });
        playbackService_ = std::make_unique<services::PlaybackService>();
        cameraControlService_ = std::make_unique<services::CameraControlService>();
        autofocusService_ = std::make_unique<services::AutofocusService>();
        triggerService_ = std::make_unique<services::TriggerService>();
        serialBusManager_ = std::make_unique<services::serialbus::SerialBusManager>();
        syringePumpService_ = std::make_unique<services::SyringePumpService>(*serialBusManager_);
        pulseGeneratorService_ = std::make_unique<services::PulseGeneratorService>(*serialBusManager_);
        // Nothing connects or moves here: the shell applies the stage block
        // and calls startup(), which only connects (ADR 0013 §5). The stage is
        // never homed (Amendment 1). The limit-switch record, written only by
        // `zc300ctl verify-limits --supervised`, just clears the panel's
        // "wiring unverified" badge (#464).
        stageService_ = std::make_unique<services::StageService>(
            *serialBusManager_,
            std::make_unique<services::FileStageReferenceStore>(
                (std::filesystem::path(dataDir) / "stage_reference.json").string()),
            std::make_shared<stage::LimitsVerificationStore>(
                (std::filesystem::path(dataDir) / "stage_limits_verified.json").string()));
        // An experiment and a moving stage exclude each other (#533): Start is refused while a
        // stage operation is active, and the stage worker re-checks right before every opcode.
        experimentCoordinator_->setStageBusyProbe(
            [this] { return stageService_ && stageService_->motionPossible(); });
        stageService_->setMotionGate(
            [this] { return !experimentCoordinator_ || experimentCoordinator_->withIdleConfiguration([] {}); });
        rfGeneratorService_ = std::make_unique<services::RfGeneratorService>();
        frameStore_ = std::make_shared<playback::FrameStore>(5000);
        dotGridService_ = std::make_unique<services::DotGridService>();
        dotGridService_->setFrameStore(frameStore_);

        // Device discovery (issue #419, ADR 0005): one job service, compiled-in
        // providers wrapping the existing enumeration/probe code, a camera
        // guard so enumeration never runs behind a live capture, and the
        // startup policy with the pre-#419 defaults (started by the shell).
        deviceDiscovery_ = std::make_unique<discovery::DeviceDiscoveryService>();
        deviceDiscovery_->registerProvider(
            discovery::CameraEnumerationProvider::mindVision(*cameraControlService_));
        deviceDiscovery_->registerProvider(
            discovery::CameraEnumerationProvider::eGrabber(*cameraControlService_));
        deviceDiscovery_->registerProvider(
            discovery::CameraEnumerationProvider::eGrabberFramegrabbers(*cameraControlService_));
        deviceDiscovery_->registerProvider(discovery::NanopositionerProvider::production());
        deviceDiscovery_->registerProvider(
            std::make_unique<discovery::PulseGeneratorProvider>(*pulseGeneratorService_));
        // Z stage (#464): identity-only FC04 reads over an explicit scope;
        // never connects, homes or moves (ADR 0013 §5).
        deviceDiscovery_->registerProvider(std::make_unique<discovery::Zc300Provider>(*serialBusManager_));
        const auto captureBusy = [this] { return captureService_ && captureService_->isRunning(); };
        if (const auto blockReason = plScienceSerialBlockReason(); !blockReason.empty())
            deviceDiscovery_->setBlockedKinds({discovery::DeviceKind::Nanopositioner,
                                               discovery::DeviceKind::PulseGenerator,
                                               discovery::DeviceKind::MotionStage},
                                              blockReason);
        deviceDiscovery_->setResourceGuard(discovery::DeviceKind::Camera, captureBusy);
        deviceDiscovery_->setResourceGuard(discovery::DeviceKind::Framegrabber, captureBusy);

        discovery::StartupDiscoveryCoordinator::Hooks hooks;
        hooks.cameraConfigured = [this] { return isCameraConfigured(); };
        hooks.captureRunning = captureBusy;
        hooks.nanopositionerConnected = [this] {
            return autofocusService_ && autofocusService_->isConnected();
        };
        hooks.selectCamera = [this](const discovery::DiscoveredDevice &device) {
            if (!device.camera || !experimentCoordinator_) return false;
            bool selected = false;
            experimentCoordinator_->withIdleConfiguration([&] {
                if (captureService_->isRunning() || isCameraConfigured()) return;
                const auto &cam = *device.camera;
                if (cam.cameraType == services::CameraType::MindVision)
                    setMindVisionCameraSelection(cam.cameraIndex, cam.label);
                else setHardwareCameraSelection(cam.interfaceIndex, cam.deviceIndex, cam.label);
                selected = true;
            });
            return selected;
        };
        hooks.connectNanopositioner = [this](const nanopositioner::Endpoint &endpoint) {
            if (!experimentCoordinator_) return false;
            bool connected = false;
            experimentCoordinator_->withIdleConfiguration([&] {
                if (captureService_->isRunning() || !autofocusService_ || autofocusService_->isConnected()) return;
                connected = autofocusService_->connect(endpoint);
            });
            return connected;
        };
        startupDiscovery_ =
            std::make_unique<discovery::StartupDiscoveryCoordinator>(*deviceDiscovery_, hooks);

        bool bootSqlite = true;
        bool bootHdf5 = true;
        bool bootProcessing = true;
        bool bootAutofocus = true;
        bool bootTrigger = true;
        bool bootDotGrid = true;
        bool bootCapture = true;
        bool bootPlayback = true;
        if (const char *rawDisabledServices = std::getenv("MIB_DISABLED_SERVICES"))
        {
            std::string disabled(rawDisabledServices);
            auto normalize = [](std::string token)
            {
                constexpr const char *whitespace = " \t\r\n";
                const auto first = token.find_first_not_of(whitespace);
                if (first == std::string::npos)
                {
                    return std::string{};
                }
                const auto last = token.find_last_not_of(whitespace);
                token = token.substr(first, last - first + 1);
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
                std::replace(token.begin(), token.end(), '-', '_');
                return token;
            };
            size_t cursor = 0;
            while (cursor <= disabled.size())
            {
                const size_t next = disabled.find(',', cursor);
                const std::string token = normalize(disabled.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor));
                if (token == "all")
                {
                    bootSqlite = false;
                    bootHdf5 = false;
                    bootProcessing = false;
                    bootAutofocus = false;
                    bootTrigger = false;
                    bootCapture = false;
                    bootPlayback = false;
                    bootDotGrid = false;
                }
                else if (token == "sqlite")
                {
                    bootSqlite = false;
                }
                else if (token == "hdf5")
                {
                    bootHdf5 = false;
                }
                else if (token == "processing")
                {
                    bootProcessing = false;
                }
                else if (token == "yolo")
                {
                    // YoloService was removed (#565); the token stays accepted
                    // so existing environments keep booting.
                }
                else if (token == "autofocus")
                {
                    bootAutofocus = false;
                }
                else if (token == "trigger")
                {
                    bootTrigger = false;
                }
                else if (token == "dot_grid" || token == "dotgrid")
                {
                    bootDotGrid = false;
                }
                else if (token == "capture" || token == "camera")
                {
                    bootCapture = false;
                }
                else if (token == "playback")
                {
                    bootPlayback = false;
                }
                else if (token == "auto_update")
                {
                    // Handled by frontend startup path.
                }
                else if (!token.empty())
                {
                    SPDLOG_WARN("Unknown token '{}' in MIB_DISABLED_SERVICES", token);
                }
                if (next == std::string::npos)
                {
                    break;
                }
                cursor = next + 1;
            }
        }

        SPDLOG_INFO("AppBackend boot toggles: sqlite={}, hdf5={}, processing={}, autofocus={}, trigger={}, capture={}, playback={}, dot_grid={}",
                    bootSqlite, bootHdf5, bootProcessing, bootAutofocus, bootTrigger, bootCapture, bootPlayback, bootDotGrid);

        // Dot-grid wafer localization: the thread idles until the frontend
        // enables it (config "dot_grid.enabled"); it only ever reads FrameStore.
        if (bootDotGrid)
        {
            dotGridService_->start();
        }
        else
        {
            SPDLOG_WARN("AppBackend: dot-grid localization disabled by MIB_DISABLED_SERVICES");
        }

        if (bootSqlite)
        {
            sqliteService_->initialize((std::filesystem::path(dataDir) / "app.sqlite3").string());
        }
        else
        {
            SPDLOG_WARN("AppBackend: sqlite bootstrap disabled by MIB_DISABLED_SERVICES");
        }

        if (bootHdf5)
        {
            hdf5Service_->initialize(dataDir);
        }
        else
        {
            SPDLOG_WARN("AppBackend: hdf5 bootstrap disabled by MIB_DISABLED_SERVICES");
        }

        std::filesystem::path dataPath(dataDir);
        std::filesystem::path exeDir = resourceRoot_.empty() ? dataPath.parent_path() : std::filesystem::path(resourceRoot_);

        // Load Young's modulus LUT for emodulus gating
        if (bootProcessing)
        {
            std::filesystem::path bundledLutPath = exeDir / "resources" / "isoelastic_curve" / "scaled_isoelastic_data_LUT_6.16-4.24.txt";
            std::string appVersion;
#ifdef MIB_STUDIO_QT_VERSION
            appVersion = MIB_STUDIO_QT_VERSION;
#endif
            // HTTP GET is injected by the shell (ADR 0002); backend links no Qt
            // networking. Without a fetcher, remote fetch is skipped (cache/bundled).
            backend::EModulusLutCatalog lutCatalog(lutHttpGet_, appVersion, lutAppDataDir_);
            backend::EModulusLutCatalog::ManagedLutInfo lutInfo;
            const std::string bundledLutStr = bundledLutPath.string();
            std::string activeLutPath = bundledLutStr;
            std::string managedLutPath = activeLutPath;
            std::string managedError;
            if (!lutCatalog.ensureManagedLut(bundledLutStr, &managedLutPath, &lutInfo, &managedError))
            {
                SPDLOG_WARN("AppBackend: LUT catalog resolution failed, falling back to bundled path {}: {}",
                            bundledLutStr, managedError);
                activeLutPath = bundledLutStr;
                lutInfo.sourceType = "bundled-fallback";
                lutInfo.revision = "bundled";
                lutInfo.localPath = activeLutPath;
                lutInfo.checksumStatus = "unknown";
            }
            else
            {
                activeLutPath = managedLutPath;
            }

            if (!processingService_->loadEModulusLut(activeLutPath))
            {
                if (activeLutPath != bundledLutStr && processingService_->loadEModulusLut(bundledLutStr))
                {
                    activeLutPath = bundledLutStr;
                    lutInfo.sourceType = "bundled-fallback";
                    lutInfo.revision = "bundled";
                    lutInfo.localPath = activeLutPath;
                    lutInfo.usedBundledFallback = true;
                    SPDLOG_WARN("AppBackend: managed LUT load failed, bundled fallback succeeded");
                }
                else
                {
                    SPDLOG_WARN("Young's modulus LUT not loaded - emodulus gating will not be available");
                }
            }
            SPDLOG_INFO("AppBackend: Young's modulus LUT source={}, revision={}, path={}, checksum_status={}, remote_updated={}, bundled_fallback={}, manifest={}",
                        lutInfo.sourceType,
                        lutInfo.revision,
                        lutInfo.localPath.empty() ? activeLutPath : lutInfo.localPath,
                        lutInfo.checksumStatus,
                        lutInfo.remoteUpdated,
                        lutInfo.usedBundledFallback,
                        lutInfo.manifestUrl);
            configureOpenCvThreads();
            processingService_->start();
        }
        else
        {
            SPDLOG_WARN("AppBackend: processing bootstrap disabled by MIB_DISABLED_SERVICES");
        }
        // Note: startRealtime() is now called when Experiment tab becomes active, not during initialization

        // PL science (ADR 0008): per-frame results come from an execution
        // provider instead of the host pipeline (YOFO S1).
        if (bootProcessing && !app::hostProcessingAvailable())
        {
            std::string providerError;
            executionProvider_ = processing::pz::makeExecutionProviderFromEnv(&providerError);
            if (executionProvider_)
            {
                executionProvider_->setSink([this](processing::ProviderFrame &&frame)
                                            { processingService_->ingestProviderFrame(frame); });
                SPDLOG_INFO("AppBackend: PL results from execution provider '{}'", executionProvider_->name());
            }
            else if (!providerError.empty())
            {
                SPDLOG_ERROR("AppBackend: {}", providerError);
            }
            // PL identity and health for preflight (#501): read-only, beside
            // the board provider. Elsewhere the monitor reports why it is idle.
            std::unique_ptr<pz::IPzPlatformRegisters> platformRegisters;
            std::string platformError = "the execution provider is not the PZ7035 board (MIB_EXECUTION_PROVIDER=pz)";
            if (executionProvider_ && executionProvider_->name() == "pz-devmem")
            {
#if defined(__linux__)
                platformError.clear();
                platformRegisters = pz::openDevMemPlatformRegisters(&platformError);
                if (!platformRegisters) SPDLOG_ERROR("AppBackend: PZ7035 platform registers: {}", platformError);
#endif
            }
            pzPlatformMonitor_ = std::make_unique<pz::PzPlatformMonitor>(std::move(platformRegisters), platformError);
#if defined(__linux__)
            // The one writer of LED, cell path and cell capture (#501 P1). Mapping reads no PL
            // register. The LED and the cell path are written only when the operator picks a camera
            // mode; the one write at start is the RXH1 v1 self-heal CTRL (a diagnostics-block setting,
            // not an actuator), only on a configured cell-image PL.
            if (executionProvider_ && executionProvider_->name() == "pz-devmem")
            {
                std::string controlError;
                if (auto registers = pz::openDevMemControlRegisters(&controlError))
                {
                    pzControl_ = std::make_unique<pz::PzInstrumentControl>(std::move(registers));
                    applyRxHealStandingCtrl();
                }
                else
                    SPDLOG_ERROR("AppBackend: PZ7035 control registers: {}", controlError);
            }
#endif
        }

        // Wire autofocus service to receive ring ratios from processing service
        if (bootProcessing && bootAutofocus)
        {
            processingService_->setRingRatioCallback([this](double ringRatio, int64_t timestampNs)
                                                     {
                if (autofocusService_) {
                    autofocusService_->onRingRatio(ringRatio, timestampNs);
                } });
            // Contracts 2 and 3: per-object Laplacian variance drives the
            // focus-score peak-seeker instead of the ring-width setpoint.
            processingService_->setFocusSampleCallback(
                [this](double laplacianVariance, int64_t timestampNs, uint64_t frameIndex,
                       int objectId, int trackId)
                {
                    if (autofocusService_) {
                        autofocusService_->onFocusSample(backend::services::autofocus::FocusSample{
                            laplacianVariance, timestampNs, frameIndex, objectId, trackId});
                    }
                });
        }
        else
        {
            processingService_->setRingRatioCallback({});
            processingService_->setFocusSampleCallback({});
            if (!bootAutofocus)
            {
                SPDLOG_WARN("AppBackend: autofocus ring-ratio callback disabled by MIB_DISABLED_SERVICES");
            }
        }

        // Wire target group trigger: processing -> trigger service
        if (bootProcessing && bootTrigger)
        {
            processingService_->setTargetGroupCallback([this](const services::TargetGroupEvent& event) {
                if (triggerService_) {
                    services::TargetGroupSignal signal;
                    signal.isTargetGroup = event.isTargetGroup;
                    signal.objectId = event.objectId;
                    signal.trackId = event.trackId;
                    signal.frameIndex = event.frameIndex;
                    signal.hostTimestampUs = event.hostTimestampUs;
                    triggerService_->onTargetGroupResult(signal);
                }
            });
            // Pulse records ride the experiment flush into /trigger_events so
            // every sort decision is stored against its source frame.
            processingService_->setTriggerEventSource([this]() {
                return triggerService_ ? triggerService_->drainEvents()
                                       : std::vector<recording::TriggerEventRecord>{};
            });
        }
        else
        {
            processingService_->setTargetGroupCallback({});
            processingService_->setTriggerEventSource({});
            if (!bootTrigger)
            {
                SPDLOG_WARN("AppBackend: trigger callback wiring disabled by MIB_DISABLED_SERVICES");
            }
        }

        // Wire camera lifecycle to trigger service
        if (bootCapture && bootTrigger)
        {
            captureService_->setCameraReadyCallback([this](::camera::common::ICamera* cam,
                                                           uint64_t generation) {
                if (triggerService_) {
                    triggerService_->setCamera(cam, generation);
                    if (cam) {
                        triggerService_->start();
                    } else {
                        triggerService_->stop();
                        // Capture just stopped and the trigger thread has been
                        // joined: dump the latency CSVs for this session.
                        dumpPipelineTimingIfEnabled();
                    }
                }
            });
        }
        else
        {
            captureService_->setCameraReadyCallback({});
            if (triggerService_)
            {
                triggerService_->setCamera(nullptr);
                triggerService_->stop();
            }
        }

        // Wire background capture into an application callback without exposing Qt types.
        if (bootProcessing)
        {
            processingService_->setBackgroundCaptureCallback([this](const cv::Mat& bg, uint64_t frameIndex) {
                BackgroundCaptureCallback callback;
                {
                    std::scoped_lock lk(backgroundCaptureCallbackMutex_);
                    callback = backgroundCaptureCallback_;
                }

                if (callback) {
                    BackgroundCaptureEvent event{makeBackgroundFrame(bg), frameIndex};
                    if (!event.frame.empty()) {
                        callback(event);
                    }
                }
                SPDLOG_INFO("Background auto-captured at frame {}", frameIndex);
            });
        }
        else
        {
            processingService_->setBackgroundCaptureCallback({});
        }

        auto toLower = [](std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return value;
        };

        std::string cameraMode = "hardware";
        if (const char *envMode = std::getenv("MIB_CAMERA_MODE"))
        {
            cameraMode = toLower(envMode);
        }

        if (bootCapture)
        {
            // Wire capture -> frame store for playback/display
            captureService_->setFrameStore(frameStore_);

            // Configure camera source (hardware, MindVision, or mock) before we start streaming.
            auto configureMock = [&]()
            {
                ::camera::mock::MockCameraOptions options;
                if (const char *envDir = std::getenv("MIB_MOCK_CAMERA_DIR"))
                {
                    options.folder = std::filesystem::path(envDir);
                }
                else
                {
                    options.folder = std::filesystem::path(dataDir) / "mock_frames";
                }
                if (const char *envInterval = std::getenv("MIB_MOCK_CAMERA_INTERVAL_MS"))
                {
                    try
                    {
                        const int ms = std::stoi(envInterval);
                        if (ms > 0)
                        {
                            options.frameInterval = std::chrono::milliseconds(ms);
                        }
                    }
                    catch (const std::exception &)
                    {
                        SPDLOG_WARN("Invalid MIB_MOCK_CAMERA_INTERVAL_MS value: {}", envInterval);
                    }
                }
                if (const char *envLoop = std::getenv("MIB_MOCK_CAMERA_LOOP"))
                {
                    const std::string loopValue = toLower(envLoop);
                    options.loopFiles = (loopValue != "false" && loopValue != "0" && loopValue != "no");
                }

                const auto intervalUs = options.frameInterval.count();
                const double configuredFps = intervalUs > 0 ? 1'000'000.0 / static_cast<double>(intervalUs) : 0.0;
                SPDLOG_INFO("AppBackend: configuring MockCamera (folder={}, interval={} us, ~{:.1f} fps, loop={})",
                            options.folder.string(),
                            intervalUs,
                            configuredFps,
                            options.loopFiles);

                captureService_->setCameraFactory([options]() mutable
                                                  { return std::make_unique<::camera::mock::MockCamera>(options); });
                mockCameraConfigured_ = true;
                aravisCameraConfigured_ = false;
                aravisFake_ = false;
                aravisDeviceId_.clear();
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                selectedMvCameraIndex_ = -1;
                selectedLabel_.clear();
                lastMindVisionConfigPath_.clear();
                // Keep the selection snapshot authoritative (BE-2).
                mockFrameDir_ = options.folder.string();
                mockIntervalMs_ = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(options.frameInterval).count());
                mockLoop_ = options.loopFiles;
                effectiveCameraSource_ = "mock";
            };

            requestedCameraSource_ = cameraMode == "mock" ? "mock"
                                     : cameraMode == "aravis" ? "aravis"
                                     : cameraMode == "mindvision" ? "mindvision"
                                     : (cameraMode == "egrabber" || cameraMode == "hardware") ? "egrabber"
                                     : cameraMode;
            cameraFallbackReason_.clear();
            if (cameraMode == "mock")
            {
                configureMock();
            }
            else if (cameraMode == "aravis")
            {
#if MIB_HAS_ARAVIS
                ::camera::aravis::AravisCameraOptions options;
                if (const char *envId = std::getenv("MIB_ARAVIS_DEVICE_ID"))
                    options.deviceId = envId;
                if (const char *envFake = std::getenv("MIB_ARAVIS_FAKE"))
                {
                    const auto fakeValue = toLower(envFake);
                    options.useFake = fakeValue == "1" || fakeValue == "true" ||
                                      fakeValue == "yes";
                }
                if (const char *envGige = std::getenv("MIB_ARAVIS_GIGE"))
                {
                    const auto gigeValue = toLower(envGige);
                    options.enableGigEVision = gigeValue == "1" || gigeValue == "true" ||
                                               gigeValue == "yes";
                }
                aravisDeviceId_ = options.deviceId;
                aravisFake_ = options.useFake;
                aravisGigE_ = options.enableGigEVision;
                aravisOverview_.store(false);
                loadAravisProfile();
                mockCameraConfigured_ = false;
                aravisCameraConfigured_ = true;
                installAravisFactory();
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                selectedMvCameraIndex_ = -1;
                selectedLabel_ = options.deviceId.empty() ? "Aravis camera (auto)" :
                                 "Aravis camera " + options.deviceId;
                if (options.useFake)
                    selectedLabel_ += " (Fake)";
                lastMindVisionConfigPath_.clear();
                effectiveCameraSource_ = "aravis";
                SPDLOG_INFO("AppBackend: configuring Aravis camera (device={}, fake={})",
                            options.deviceId.empty() ? "<auto>" : options.deviceId,
                            options.useFake);
#else
                // An explicit Aravis request is a hard configuration error in
                // an Aravis-disabled binary. Keep a null factory so capture
                // reports the failure instead of silently running MockCamera.
                captureService_->setCameraFactory([]() -> std::unique_ptr<::camera::common::ICamera> {
                    return nullptr;
                });
                mockCameraConfigured_ = false;
                // Keep the explicit source selected so startup discovery does
                // not silently replace it with a hardware/mock backend.
                aravisCameraConfigured_ = true;
                aravisFake_ = false;
                aravisDeviceId_.clear();
                effectiveCameraSource_ = "unavailable";
                cameraFallbackReason_ = "Aravis support is disabled in this build (MIB_ENABLE_ARAVIS=OFF)";
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                selectedMvCameraIndex_ = -1;
                selectedLabel_.clear();
                lastMindVisionConfigPath_.clear();
                SPDLOG_ERROR("AppBackend: Aravis mode requested but Aravis support is unavailable");
#endif
            }
            else if (cameraMode == "mindvision")
            {
#if MIB_HAS_MINDVISION
                int cameraIndex = 0;
                if (const char *envIndex = std::getenv("MIB_MINDVISION_CAMERA_INDEX"))
                {
                    try
                    {
                        cameraIndex = (std::max)(0, std::stoi(envIndex));
                    }
                    catch (const std::exception &)
                    {
                        SPDLOG_WARN("Invalid MIB_MINDVISION_CAMERA_INDEX value: {}", envIndex);
                    }
                }
                std::string configPath;
                if (const char *envConfig = std::getenv("MIB_MINDVISION_CONFIG"))
                {
                    configPath = envConfig;
                }
                SPDLOG_INFO("AppBackend: configuring MindVision camera (index={}, config={})",
                            cameraIndex, configPath.empty() ? "<none>" : configPath);
                captureService_->setCameraFactory([this, cameraIndex, configPath]() mutable {
                    return makeLiveCamera(
                        cameraIndex, configPath, *pulseGeneratorService_,
                        mindVisionOverview_.load(),
                        [this](const camera::mindvision::SdkCapability& sensor) {
                            std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                            mindVisionSensor_ = {sensor.sensorWidth, sensor.sensorHeight,
                                                 sensor.minWidth, sensor.minHeight};
                        });
                });
                mockCameraConfigured_ = false;
                aravisCameraConfigured_ = false;
                aravisFake_ = false;
                aravisDeviceId_.clear();
                effectiveCameraSource_ = "mindvision";
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                {
                    std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                    mindVisionSensor_ = {};
                }
                selectedMvCameraIndex_ = cameraIndex;
                selectedLabel_ = configPath.empty()
                    ? std::string("MindVision camera ") + std::to_string(cameraIndex)
                    : std::string("MindVision camera ") + std::to_string(cameraIndex) + " (" + configPath + ")";
                lastMindVisionConfigPath_ = configPath;
#else
                SPDLOG_WARN("AppBackend: MindVision mode requested but MindVision SDK is unavailable; falling back to mock camera");
                cameraMode = "mock";
                configureMock();
                cameraFallbackReason_ = "MindVision SDK is unavailable in this build";
#endif
            }
            else if (cameraMode == "egrabber" || cameraMode == "hardware")
            {
#if MIB_HAS_EGRABBER
                SPDLOG_INFO("AppBackend: configuring hardware EGrabber camera");
                captureService_->setCameraFactory([]()
                                                  { return std::make_unique<::camera::common::EGrabberCamera>(); });
                mockCameraConfigured_ = false;
                aravisCameraConfigured_ = false;
                aravisFake_ = false;
                aravisDeviceId_.clear();
                effectiveCameraSource_ = "egrabber";
                selectedMvCameraIndex_ = -1;
            #else
                SPDLOG_WARN("AppBackend: hardware mode requested but EGrabber SDK is unavailable; keeping mock camera");
                cameraMode = "mock";
                configureMock();
                cameraFallbackReason_ = "EGrabber SDK is unavailable in this build";
#if MIB_HAS_MINDVISION
                // An implicit fallback is not an operator selection. Leave
                // startup discovery enabled so a single MindVision camera
                // can be selected without a separate Connect action.
                if (std::getenv("MIB_CAMERA_MODE") == nullptr) {
                    mockCameraConfigured_ = false;
                }
#endif
#endif
            }
            else
            {
                SPDLOG_WARN("AppBackend: unknown camera mode '{}'; falling back to hardware/mock defaults", cameraMode);
#if MIB_HAS_EGRABBER
                captureService_->setCameraFactory([]()
                                                  { return std::make_unique<::camera::common::EGrabberCamera>(); });
                mockCameraConfigured_ = false;
                effectiveCameraSource_ = "egrabber";
                selectedMvCameraIndex_ = -1;
#else
                cameraMode = "mock";
                configureMock();
#endif
                cameraFallbackReason_ = "unknown camera mode requested";
            }

            // No per-frame logging; rely on periodic capture stats
            captureService_->setFrameCallback(nullptr);
        }
        else
        {
            SPDLOG_WARN("AppBackend: capture bootstrap disabled by MIB_DISABLED_SERVICES");
            cameraMode = "disabled";
            selectedIfIndex_ = -1;
            selectedDevIndex_ = -1;
            selectedMvCameraIndex_ = -1;
            selectedLabel_.clear();
            lastMindVisionConfigPath_.clear();
            mockCameraConfigured_ = false;
            aravisCameraConfigured_ = false;
            aravisFake_ = false;
            aravisDeviceId_.clear();
        }

        if (bootPlayback)
        {
            playbackService_->setFrameStore(frameStore_);
        }
        else
        {
            SPDLOG_WARN("AppBackend: playback bootstrap disabled by MIB_DISABLED_SERVICES");
        }

        // Wire up the crash-state mirror so post-crash dumps include the
        // current camera / data-dir context. (CrashReporter::init() runs
        // earlier in main(), before AppBackend exists.)
        {
            auto& mirror = backend::diagnostics::CrashStateMirror::instance();
            mirror.setDataDir(dataDir);
            mirror.app.mockCamera.store(mockCameraConfigured_);
            mirror.app.selectedInterface.store(selectedIfIndex_);
            mirror.app.selectedDevice.store(selectedDevIndex_);
            mirror.setCameraLabel(selectedLabel_);
            mirror.frameStore.capacity.store(5000);
            backend::services::CrashReporter::setTag("camera_mode", cameraMode);
            backend::services::CrashReporter::setTag("data_dir", dataDir);
            backend::services::CrashReporter::breadcrumb("lifecycle",
                "AppBackend initialized");
        }

        loadInstrumentRunWindow();
        SPDLOG_INFO("Backend initialized.");
        return true;
    }

    services::SqliteService &AppBackend::sqlite() { return *sqliteService_; }
    services::Hdf5Service &AppBackend::hdf5() { return *hdf5Service_; }
    services::CaptureService &AppBackend::capture() { return *captureService_; }
    services::ProcessingService &AppBackend::processing() { return *processingService_; }
    processing::IExecutionProvider *AppBackend::executionProvider() { return executionProvider_.get(); }
    pz::PzPlatformMonitor *AppBackend::pzPlatformMonitor() { return pzPlatformMonitor_.get(); }

    // ---- PZ7035 camera modes (#501 P1; pz7035-imx426 docs/YOFO_HOST_INTERFACE.md) ----

    namespace {
    // Producer settings that give the qualified timings (HMAX/VMAX/SHS from rate and exposure,
    // pz7035_gentl.c choose_hmax): Run 58/256/64, Align 116/1280/64.
    constexpr double kRunFps = 5000.0, kRunExposureUs = 150.0;
    // Align, whole frames from the bridge (results8 on): HMAX 232 / VMAX 800 / SHS 64, 400 fps;
    // HMAX 116 would overflow the bridge tap. Banded fallback for older images: 116 / 1280 / 64.
    constexpr double kAlignFps = 400.0, kAlignExposureUs = 2299.7;
    constexpr int kAlignHmax = 232;
    constexpr double kAlignBandsFps = 500.0, kAlignBandsExposureUs = 1899.7;
    constexpr int kRunWidth = 512, kRunHeight = 96;
    // ROI offsets: the producer takes x in steps of 8; the sensor lands y on a multiple of 4.
    constexpr int kRunXStep = 8, kRunYStep = 4, kRunXMax = 816 - kRunWidth, kRunYMax = 624 - kRunHeight;
    constexpr auto kCameraStartTimeout = std::chrono::seconds(10);
    } // namespace

    void AppBackend::applyRxHealStandingCtrl()
    {
        // The v1 receiver self-heal block: the standing persist time is 250 ms (written once here, read
        // back). Studio is the /dev/mem owner, so the write belongs here, not in the restore script.
        // Refused (nothing read or written) while the PL is blank or not the cell image.
        if (!pzControl_) return;
        pz::PzInstrumentControl::RxHealCtrlResult result;
        std::string why;
        if (!pzControl_->applyRxHealCtrl(pz::kRxHealCtrlPersist250, result, &why))
        {
            SPDLOG_INFO("AppBackend: RXH1 CTRL not set: {}", why);
            return;
        }
        using Kind = pz::PzInstrumentControl::RxHealCtrlResult::Kind;
        switch (result.kind)
        {
        case Kind::Absent: SPDLOG_INFO("AppBackend: no RXH1 self-heal block on this PL"); break;
        case Kind::V2: SPDLOG_INFO("AppBackend: RXH1 v2: CTRL left at 0x{:08X}", result.before); break;
        case Kind::AlreadySet: SPDLOG_INFO("AppBackend: RXH1 CTRL already 0x{:08X} (persist 250 ms)", result.after); break;
        case Kind::Written:
            SPDLOG_INFO("AppBackend: RXH1 CTRL 0x{:08X} -> 0x{:08X} (persist 250 ms), read back 0x{:08X}", result.before,
                        pz::kRxHealCtrlPersist250, result.after);
            break;
        case Kind::Mismatch:
            SPDLOG_WARN("AppBackend: RXH1 CTRL wrote 0x{:08X} over 0x{:08X} but reads back 0x{:08X}", pz::kRxHealCtrlPersist250,
                        result.before, result.after);
            break;
        }
    }

    bool AppBackend::instrumentControlAvailable() const { return pzControl_ != nullptr; }

    pz::InstrumentMode AppBackend::instrumentMode() const
    {
        return static_cast<pz::InstrumentMode>(instrumentMode_.load());
    }

    std::pair<int, int> AppBackend::instrumentRunOffset() const
    {
        return {instrumentRunX_.load(), instrumentRunY_.load()};
    }

    std::string AppBackend::plScienceSerialBlockReason() const
    {
        if (app::hostProcessingAvailable()) return {};
        return "This instrument runs science on the PL: the nanopositioner, pulse generator and ZC300 stage "
               "are not used here, and their serial ports are never probed (the pumps share the RS485 bus)";
    }

    bool AppBackend::instrumentIdle() const { return instrumentIdle_.load(); }

    processing::pz::CompiledProfile AppBackend::compilePlProfile()
    {
        auto &proc = processing();
        processing::pz::UnetCellsProfileInputs in;
        in.config = proc.getEffectiveProcessingConfig(); // includes the detected channel band
        in.pixelToMicron = proc.getPixelToMicronFactor();
        in.storeInvalidEveryN = static_cast<uint32_t>(std::min<size_t>(proc.getInvalidFrameSamplingRate(), 0xFFFF));
        in.lut = proc.eModulusLut().isLoaded() ? &proc.eModulusLut() : nullptr;
        return processing::pz::compileUnetCellsV2(in);
    }

    bool AppBackend::startLiveResults(std::string *errorOut)
    {
        std::unique_lock<std::mutex> lock(liveMutex_);
        auto fail = [&](const std::string &why) {
            if (errorOut) *errorOut = why;
            SPDLOG_WARN("AppBackend: live results not started: {}", why);
            return false;
        };
        if (liveResultsActive_.load()) return true;
        if (!executionProvider_ || !pzControl_) return fail("no PL execution provider");
        if (instrumentMode() != pz::InstrumentMode::Run) return fail("the instrument is not in Run");
        auto profile = compilePlProfile();
        if (!profile.ok()) {
            std::string why;
            for (const auto &e : profile.errors) why += (why.empty() ? "" : "; ") + e;
            return fail("the settings do not compile into the PL profile: " + why);
        }
        std::string providerError;
        const uint64_t runId = 0; // a live session is not a run: nothing is admitted while no experiment is active
        if (!executionProvider_->configure(profile, &providerError) || !executionProvider_->start(runId, &providerError))
            return fail("the provider did not start: " + providerError);
        liveProfile_ = std::move(profile);
        liveConfigVersion_ = processing().getConfigVersion();
        liveResultsActive_.store(true);
        liveWatcherExit_ = false;
        if (!liveWatcher_.joinable()) liveWatcher_ = std::thread([this] { liveWatcherLoop(); });
        SPDLOG_INFO("AppBackend: live PL results started (Run, no experiment)");
        return true;
    }

    void AppBackend::stopLiveResults()
    {
        std::thread watcher;
        {
            std::unique_lock<std::mutex> lock(liveMutex_);
            if (!liveResultsActive_.exchange(false)) return;
            if (executionProvider_) executionProvider_->stop();
            liveWatcherExit_ = true;
            liveCv_.notify_all();
            watcher = std::move(liveWatcher_);
        }
        if (watcher.joinable()) watcher.join();
        SPDLOG_INFO("AppBackend: live PL results stopped");
    }

    void AppBackend::resumeLiveResults()
    {
        if (instrumentMode() == pz::InstrumentMode::Run) (void)startLiveResults(nullptr);
    }

    // A tuned setting takes effect without a file: when the processing configuration changes, the profile is
    // recompiled and, if the PL page or table differ, the provider is restarted with it (configure needs it stopped).
    void AppBackend::liveWatcherLoop()
    {
        std::unique_lock<std::mutex> lock(liveMutex_);
        while (!liveWatcherExit_) {
            liveCv_.wait_for(lock, std::chrono::milliseconds(250));
            if (liveWatcherExit_ || !liveResultsActive_.load()) break;
            const uint64_t version = processing().getConfigVersion();
            if (version == liveConfigVersion_) continue;
            liveConfigVersion_ = version;
            auto profile = compilePlProfile();
            if (!profile.ok()) {
                std::string why;
                for (const auto &e : profile.errors) why += (why.empty() ? "" : "; ") + e;
                SPDLOG_WARN("AppBackend: live PL results keep the previous profile: {}", why);
                continue;
            }
            if (profile.page == liveProfile_.page && profile.table0 == liveProfile_.table0) continue;
            executionProvider_->stop();
            std::string providerError;
            if (executionProvider_->configure(profile, &providerError) && executionProvider_->start(0, &providerError)) {
                liveProfile_ = std::move(profile);
                SPDLOG_INFO("AppBackend: live PL results restarted with the new profile");
            } else {
                // Put the previous profile back so the session keeps running.
                SPDLOG_WARN("AppBackend: live PL results could not restart ({}); restoring the previous profile", providerError);
                std::string again;
                if (!(executionProvider_->configure(liveProfile_, &again) && executionProvider_->start(0, &again))) {
                    SPDLOG_ERROR("AppBackend: live PL results stopped: {}", again);
                    liveResultsActive_.store(false);
                    break;
                }
            }
        }
    }

    bool AppBackend::instrumentRunWindowSet() const { return instrumentRunSet_.load(); }
    std::optional<uint32_t> AppBackend::plReceiverAutoResets()
    {
        return pzPlatformMonitor_ ? pzPlatformMonitor_->rxHealAutoResets() : std::nullopt;
    }

    std::optional<uint32_t> AppBackend::plNoFsFrames()
    {
        return pzPlatformMonitor_ ? pzPlatformMonitor_->rxNoFsFrames() : std::nullopt;
    }

    bool AppBackend::plNoFsRunBegin() { return pzPlatformMonitor_ && pzPlatformMonitor_->beginNoFsRun(); }
    std::optional<uint64_t> AppBackend::plNoFsRunEnd()
    {
        return pzPlatformMonitor_ ? pzPlatformMonitor_->endNoFsRun() : std::nullopt;
    }

    AppBackend::AlignLockCounters AppBackend::alignLockCounters() const
    {
        return {alignReceiverClears_.load(), alignLockFailures_.load(), alignLastStuckP13_.load(), alignPlGaveUps_.load(), alignLastPlGaveUpMs_.load()};
    }

    namespace
    {
        std::filesystem::path runWindowFile(const std::string &dataDir)
        {
            return std::filesystem::path(dataDir) / "instrument_run_window.json";
        }

        void saveRunWindow(const std::string &dataDir, int x, int y)
        {
            if (dataDir.empty()) return;
            try
            {
                const auto path = runWindowFile(dataDir);
                const auto tmp = std::filesystem::path(path).concat(".tmp");
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    out << nlohmann::json{{"x", x}, {"y", y}, {"set", true}}.dump();
                    if (!out) throw std::runtime_error("write failed");
                }
                std::filesystem::rename(tmp, path);
            }
            catch (const std::exception &ex)
            {
                SPDLOG_WARN("AppBackend: the Run window was not saved: {}", ex.what());
            }
        }
    } // namespace

    void AppBackend::loadInstrumentRunWindow()
    {
        if (dataDir_.empty()) return;
        std::error_code ec;
        const auto path = runWindowFile(dataDir_);
        if (!std::filesystem::exists(path, ec)) return;
        try
        {
            std::ifstream in(path, std::ios::binary);
            const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
            // Only a window the Run switch could have applied: on the grid and on the sensor.
            if (!j.is_object() || !j.value("set", false) || !j.contains("x") || !j.contains("y") ||
                !j["x"].is_number_integer() || !j["y"].is_number_integer())
            {
                SPDLOG_WARN("AppBackend: {} is not a Run window; ignored", path.string());
                return;
            }
            const int x = j["x"].get<int>(), y = j["y"].get<int>();
            if (x < 0 || y < 0 || x > kRunXMax || y > kRunYMax || x % kRunXStep != 0 || y % kRunYStep != 0)
            {
                SPDLOG_WARN("AppBackend: the saved Run window ({}, {}) is off the grid or sensor; ignored", x, y);
                return;
            }
            instrumentRunX_.store(x);
            instrumentRunY_.store(y);
            instrumentRunSet_.store(true);
            SPDLOG_INFO("AppBackend: Run window restored at ({}, {})", x, y);
        }
        catch (const std::exception &ex)
        {
            SPDLOG_WARN("AppBackend: the saved Run window could not be read: {}", ex.what());
        }
    }

    void AppBackend::setServiceMode(bool on) { serviceMode_.store(on); }

    std::string AppBackend::alignSource() const
    {
        const int s = alignSource_.load();
        return s == 1 ? "bridge" : s == 2 ? "bands" : "";
    }

    bool AppBackend::alignWholeFrameAvailable(std::string *why)
    {
        auto *provider = executionProvider_.get();
        if (!provider || provider->name() != "pz-devmem") {
            if (why) *why = "no PZ7035 results bridge";
            return false;
        }
        std::string error;
        const auto expected = pz::loadExpectedCore(pz::kExpectedCorePath, &error);
        if (!expected) {
            if (why) *why = "expected-core.json: " + (error.empty() ? std::string("missing") : error);
            return false;
        }
        if (!expected->has(pz::kFeatureAlignWholeFrame)) {
            if (why) *why = "the loaded build has no whole-frame Align preview (results8 or later)";
            return false;
        }
        const auto identity = provider->identity();
        if (!identity.valid || identity.buildId != expected->buildId) {
            if (why) *why = "the PL BUILD_ID does not match expected-core.json";
            return false;
        }
        return true;
    }
    bool AppBackend::serviceMode() const { return serviceMode_.load(); }

    void AppBackend::setExecutionProviderForTesting(std::unique_ptr<processing::IExecutionProvider> provider)
    {
        executionProvider_ = std::move(provider);
        if (executionProvider_ && processingService_)
            executionProvider_->setSink([this](processing::ProviderFrame &&frame) { processingService_->ingestProviderFrame(frame); });
    }

    void AppBackend::setInstrumentControlForTesting(std::unique_ptr<pz::IPzControlRegisters> registers)
    {
        pzControl_ = std::make_unique<pz::PzInstrumentControl>(std::move(registers));
        applyRxHealStandingCtrl(); // as at service start, so the tests pin the start-up write allow-list
    }

    bool AppBackend::syncWallClock(int64_t unixMs, std::string *why)
    {
        app::WallClock::setBoardWithoutRtc(!app::hostProcessingAvailable());
        const auto run = experimentCoordinator_ ? experimentCoordinator_->state() : app::ExperimentRunState::Idle;
        const bool locked = isFrameRecording() || run == app::ExperimentRunState::Starting ||
                            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping;
        return app::WallClock::sync(unixMs, locked, why);
    }

    bool AppBackend::setInstrumentMode(pz::InstrumentMode mode, int x, int y, std::string *errorOut)
    {
        std::lock_guard<std::mutex> lock(instrumentModeMutex_);
        instrumentIdle_.store(false);
        auto fail = [&](const std::string &message) {
            if (errorOut) *errorOut = message;
            SPDLOG_WARN("AppBackend: instrument mode {}: {}", pz::instrumentModeName(mode), message);
            return false;
        };
        if (!pzControl_) return fail("no PZ7035 control on this platform");
        if (mode == pz::InstrumentMode::Unknown) return fail("choose Align or Run");
        if (!captureService_) return fail("the backend is not initialized");
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting ||
            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping)
            return fail("Stop the experiment or recording before changing camera mode");
        if (mode == pz::InstrumentMode::Run) {
            x = std::clamp(x - x % kRunXStep, 0, kRunXMax - kRunXMax % kRunXStep);
            y = std::clamp(y - y % kRunYStep, 0, kRunYMax);
        }
        stopLiveResults(); // the provider arms the bridge Align uses; the mode switch restarts it for Run (G5)
        std::string err;
        // 1. LED off, cell path off: no producer AcquisitionStart may run with the U-Net enable set
        //    (its command pulses clear bit 4), and the guard never sees a timing transient lit.
        if (!pzControl_->ledOff(&err) || !pzControl_->setCellPath(false, &err)) return fail(err);
        instrumentMode_.store(static_cast<int>(pz::InstrumentMode::Unknown));
        captureService_->stop(); // also stops Align's bridge previews (bridge STOP)
        alignSource_.store(0);
        // Whole-frame Align needs results8 or later (expected-core.json feature + BUILD_ID match);
        // otherwise the producer's banded preview at the older timing.
        std::string wholeFrameWhy;
        const bool wholeFrame = mode == pz::InstrumentMode::Align && alignWholeFrameAvailable(&wholeFrameWhy);
        if (mode == pz::InstrumentMode::Align && !wholeFrame)
            SPDLOG_INFO("AppBackend: Align uses the banded preview: {}", wholeFrameWhy);
#if MIB_HAS_ARAVIS
        // 2. Stage the producer settings. Every capture start reopens the device and writes rate,
        //    exposure and region, so the producer re-applies the mode even if something else
        //    touched the sensor meanwhile.
        if (aravisCameraConfigured_) {
            AravisProfile next = aravisProfile_;
            if (mode == pz::InstrumentMode::Align) {
                next.overviewFps = wholeFrame ? kAlignFps : kAlignBandsFps;
                next.overviewExposureUs = wholeFrame ? kAlignExposureUs : kAlignBandsExposureUs;
                next.overviewHmax = wholeFrame ? kAlignHmax : 0;
            } else {
                next.hasRoi = true;
                next.x = x;
                next.y = y;
                next.width = kRunWidth;
                next.height = kRunHeight;
                next.experimentFps = kRunFps;
                next.experimentExposureUs = kRunExposureUs;
            }
            if (!saveAravisProfile(next, &err)) return fail(err);
            aravisProfile_ = next;
            if (!setAravisOverview(mode == pz::InstrumentMode::Align, &err)) return fail(err);
            installAravisFactory();
        }
#endif
        // 3. AcquisitionStart applies ROI, timing, ingress geometry and the receiver reset.
        const auto startProducer = [&]() -> std::string {
            captureService_->requestStart();
            const auto until = std::chrono::steady_clock::now() + kCameraStartTimeout;
            services::CaptureLifecycleSnapshot started = captureService_->lifecycleSnapshot();
            while (!started.cameraReady && std::chrono::steady_clock::now() < until) {
                if (started.state == services::CaptureLifecycleState::Faulted) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                started = captureService_->lifecycleSnapshot();
            }
            if (started.cameraReady) return {};
            captureService_->stop();
            return "the camera did not start" +
                   (started.lastFailureMessage.empty() ? std::string() : ": " + started.lastFailureMessage);
        };
        if (const auto why = startProducer(); !why.empty()) return fail(why);
        services::CaptureLifecycleSnapshot snap;
        if (mode == pz::InstrumentMode::Align && wholeFrame) {
            // 4a. Align (results8 on): stop the producer stream (the sensor keeps the applied
            //     232/800/64 timing), then whole frames from the bridge's preview slots through a
            //     camera behind CaptureService, so the live view is unchanged. LED 100/135.
            const auto startPreview = [&]() -> std::string {
                captureService_->stop();
                processing::pz::BridgePreviewConfig preview; // 816x624, every 40th frame, 0x3F100000
                auto *provider = executionProvider_.get();
                captureService_->setCameraFactory([provider, preview]() -> std::unique_ptr<::camera::common::ICamera> {
                    if (!provider) return nullptr;
                    return std::make_unique<pz::PzBridgePreviewCamera>(*provider, preview);
                });
                captureService_->requestStart();
                const auto previewUntil = std::chrono::steady_clock::now() + kCameraStartTimeout;
                auto started = captureService_->lifecycleSnapshot();
                while (!started.cameraReady && std::chrono::steady_clock::now() < previewUntil &&
                       started.state != services::CaptureLifecycleState::Faulted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    started = captureService_->lifecycleSnapshot();
                }
                if (started.cameraReady) return {};
                captureService_->stop();
                return "Align previews did not start" +
                       (started.lastFailureMessage.empty() ? std::string() : ": " + started.lastFailureMessage);
            };
            if (const auto why = startPreview(); !why.empty()) return fail(why);
            // Success means a whole frame arrived, not only an armed bridge. The ingress can sit in
            // a stuck state (sticky lane overflow flags, every frame lost): detect it, clear it
            // with receiver resets, and say so plainly when it never locks (#629).
            auto framesBefore = captureService_->stats().framesProcessed.load();
            pz::AlignLockHooks hooks;
            hooks.waitPreview = [&](std::chrono::milliseconds wait) {
                const auto until = std::chrono::steady_clock::now() + wait;
                while (captureService_->stats().framesProcessed.load() == framesBefore &&
                       std::chrono::steady_clock::now() < until && captureService_->lifecycleSnapshot().cameraReady)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return captureService_->stats().framesProcessed.load() != framesBefore;
            };
            hooks.readStatus = [&](pz::IngressStatus &st) { return pzControl_->ingressStatus(st, nullptr); };
            hooks.readHeal = [&](pz::RxHealStatus &heal) { return pzControl_->rxHealStatus(heal, nullptr); };
            hooks.clearFlags = [&](std::chrono::milliseconds hold) {
                return pzControl_->resetReceiver(std::chrono::duration_cast<std::chrono::microseconds>(hold), nullptr);
            };
            hooks.onPlGaveUp = [&](const pz::RxHealStatus &heal, bool gaveUp, long long afterMs) {
                ++alignPlGaveUps_;
                alignLastPlGaveUpMs_.store(afterMs);
                SPDLOG_WARN("AppBackend: Align: the PL receiver self-heal {} after {} tries, {} ms after the stream started "
                            "(lane flags 0x{:02X}): host recovery takes over",
                            gaveUp ? "gave up" : "did not finish", heal.tries(), afterMs, heal.laneFlags());
            };
            hooks.onAttempt = [&](int attempt, const pz::IngressStatus &st) {
                ++alignReceiverClears_;
                alignLastStuckP13_.store(st.status);
                SPDLOG_WARN("AppBackend: Align preview stuck (P[13] 0x{:08X}, lane overflow 0x{:02X}, errors {}, resyncs {}): "
                            "receiver reset attempt {}",
                            st.status, st.laneOverflow(), st.errors, st.resyncs, attempt);
            };
            hooks.pause = [](std::chrono::milliseconds wait) { std::this_thread::sleep_for(wait); };
            const auto lock = pz::awaitAlignLock(hooks);
            if (lock.recovered)
                SPDLOG_WARN("AppBackend: Align preview {} after {} host receiver reset(s){}", lock.locked ? "recovered" : "did not lock",
                            lock.clears, lock.healedByPl ? " (PL self-heal)" : lock.plGaveUp && lock.locked ? " (pl_gave_up -> host recovered)" : "");
            if (!lock.locked) {
                ++alignLockFailures_;
                snap = captureService_->lifecycleSnapshot();
                captureService_->stop();
                return fail(lock.error + (snap.lastFailureMessage.empty() ? std::string() : ": " + snap.lastFailureMessage));
            }
            if (!pzControl_->setLed(pz::kAlignLed, &err)) return fail(err);
            alignSource_.store(1);
        } else if (mode == pz::InstrumentMode::Align) {
            // 4a'. Align on older images: previews from the producer's band capture, LED 0/125.
            if (!pzControl_->setLed(pz::kAlignBandsLed, &err)) return fail(err);
            alignSource_.store(2);
        } else {
            // 4b. Run: stop the producer stream (the sensor keeps streaming at the applied
            //     timing), then the cell path, the latency monitor and the LED Run preset.
            captureService_->stop();
            if (!pzControl_->setCellPath(true, &err) || !pzControl_->clearLatency(&err) ||
                !pzControl_->setLed(pz::kRunLed, &err))
                return fail(err);
            instrumentRunX_.store(x);
            instrumentRunY_.store(y);
            instrumentRunSet_.store(true);
            saveRunWindow(dataDir_, x, y);
        }
        instrumentMode_.store(static_cast<int>(mode));
        if (mode == pz::InstrumentMode::Run) (void)startLiveResults(nullptr); // live monitoring in Run without a file (G5)
        // The receiver and the sensor were reset: the link counters jump for about a second.
        if (pzPlatformMonitor_)
            pzPlatformMonitor_->settle(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count()));
        SPDLOG_INFO("AppBackend: instrument mode {}{}", pz::instrumentModeName(mode),
                    mode == pz::InstrumentMode::Run ? fmt::format(" at ({}, {})", x, y) : std::string());
        return true;
    }

    bool AppBackend::enterInstrumentIdle(std::string *errorOut)
    {
        std::lock_guard<std::mutex> lock(instrumentModeMutex_);
        auto fail = [&](const std::string &message) {
            if (errorOut) *errorOut = message;
            SPDLOG_WARN("AppBackend: instrument idle: {}", message);
            return false;
        };
        if (!pzControl_) return fail("no PZ7035 control on this platform");
        if (!captureService_) return fail("the backend is not initialized");
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting || run == app::ExperimentRunState::Active ||
            run == app::ExperimentRunState::Stopping)
            return fail("an experiment or recording is still running");
        std::string err;
        // The same order as a mode switch: nothing lit while the cell path or the camera changes.
        stopLiveResults();
        if (!pzControl_->ledOff(&err) || !pzControl_->setCellPath(false, &err)) return fail(err);
        instrumentMode_.store(static_cast<int>(pz::InstrumentMode::Unknown));
        captureService_->stop();
        alignSource_.store(0);
        instrumentIdle_.store(true);
        SPDLOG_INFO("AppBackend: instrument idle (LED off, cell path off, camera released)");
        return true;
    }

    bool AppBackend::setInstrumentLed(double delayUs, double widthUs, std::string *errorOut)
    {
        std::lock_guard<std::mutex> lock(instrumentModeMutex_);
        auto fail = [&](const std::string &message) {
            if (errorOut) *errorOut = message;
            return false;
        };
        if (!pzControl_) return fail("no PZ7035 control on this platform");
        if (!serviceMode_.load()) return fail("raw LED values need Service / Commissioning mode");
        const auto run = experimentCoordinator_->state();
        if (run == app::ExperimentRunState::Starting || run == app::ExperimentRunState::Active ||
            run == app::ExperimentRunState::Stopping)
            return fail("the LED cannot change during an experiment");
        const pz::LedSetting setting{delayUs, widthUs};
        if (auto why = pz::checkLed(instrumentMode(), setting); !why.empty()) return fail(why);
        std::string err;
        if (!pzControl_->setLed(setting, &err)) return fail(err);
        SPDLOG_INFO("AppBackend: LED {:.1f}/{:.1f} µs (service, {})", delayUs, widthUs,
                    pz::instrumentModeName(instrumentMode()));
        return true;
    }

    bool AppBackend::fetchRunPreview(std::vector<uint8_t> &out, std::string *errorOut)
    {
        if (!pzControl_) {
            if (errorOut) *errorOut = "no PZ7035 control on this platform";
            return false;
        }
        if (instrumentMode() != pz::InstrumentMode::Run) {
            if (errorOut) *errorOut = "the run preview needs Run mode";
            return false;
        }
        pz::PzCellCapture capture;
        if (!pzControl_->captureCell(capture, std::chrono::milliseconds(100), errorOut)) return false;
        out = pz::encodeRunPreview(capture);
        return true;
    }
    services::PlaybackService &AppBackend::playback() { return *playbackService_; }
    services::CameraControlService &AppBackend::cameraControl() { return *cameraControlService_; }
    services::AutofocusService &AppBackend::autofocus() { return *autofocusService_; }
    services::TriggerService &AppBackend::trigger() { return *triggerService_; }
    services::DotGridService &AppBackend::dotGrid() { return *dotGridService_; }
    services::SyringePumpService &AppBackend::syringePump() { return *syringePumpService_; }
    services::PulseGeneratorService &AppBackend::pulseGenerator() { return *pulseGeneratorService_; }
    services::StageService &AppBackend::stage() { return *stageService_; }
    services::RfGeneratorService &AppBackend::rfGenerator() { return *rfGeneratorService_; }
    discovery::DeviceDiscoveryService &AppBackend::deviceDiscovery() { return *deviceDiscovery_; }
    discovery::StartupDiscoveryCoordinator &AppBackend::startupDiscovery() { return *startupDiscovery_; }
    profiles::ProfileRegistryWorker &AppBackend::profileRegistry() { return *profileRegistry_; }

    profiles::MethodContext AppBackend::methodContext() const
    {
        profiles::MethodContext context;
        context.instrumentId = instrumentIdentity_.id;
        if (processingService_)
        {
            const auto core = processingService_->activeProcessingCoreIdentity();
            context.processingCoreVersion = core.version;
            context.processingCoreSha256 = core.artifactSha256;
        }
        context.cameraSource = cameraSourceInfo().effective;
        return context;
    }

    AppBackend::MethodValidationRequestResult AppBackend::requestMethodValidation(
        const std::string &revisionId, const std::string &evidenceFile, bool passed)
    {
        MethodValidationRequestResult result;
        if (!profileRegistry_)
        {
            result.error = "Backend not initialized";
            return result;
        }
        std::string contentHash;
        for (const auto &r : profileRegistry_->snapshot().revisions)
            if (r.revisionId == revisionId) contentHash = r.contentHash;
        if (contentHash.empty())
        {
            result.error = "Revision is not in the local cache";
            return result;
        }
        std::string runJson;
        {
            // Separate read-only service: never touches the run being recorded.
            services::Hdf5Service reader;
            if (!reader.loadFile(evidenceFile))
            {
                result.error = "Cannot open the test-run file: " + evidenceFile;
                return result;
            }
            reader.readRunSnapshotJson(runJson);
            reader.closeFile();
        }
        const auto context = methodContext();
        result.error = app::checkValidationEvidence(runJson, revisionId, contentHash, context.instrumentId,
                                                    profiles::methodContextHash(context));
        if (!result.error.empty()) return result;
        profiles::LocalValidationRequest request;
        request.revisionId = revisionId;
        request.context = context;
        request.instrumentName = instrumentIdentity_.name;
        request.evidenceFile = evidenceFile;
        request.passed = passed;
        result.jobId = profileRegistry_->requestRecordValidation(std::move(request));
        if (result.jobId == 0) result.error = "The registry worker refused the request";
        return result;
    }

    profiles::MethodDraft AppBackend::currentConfigDraft(std::string *error) const
    {
        profiles::MethodDraft draft;
        const auto config = getLastConfigJson();
        if (config.empty())
        {
            if (error) *error = "No config.json is applied";
            return draft;
        }
        draft.configJson = config;
        if (processingService_)
        {
            const auto core = processingService_->activeProcessingCoreIdentity();
            draft.processingCoreId = core.version;
            draft.processingContractVersion = static_cast<int>(core.contractVersion);
        }
        draft.hardwareCompatibilityJson.clear();
        return draft;
    }

    void AppBackend::configureMockCamera(const ::camera::mock::MockCameraOptions &options)
    {
        if (!captureService_)
            return;
        releaseMindVisionOverviewStore();
        requestedCameraSource_ = "mock";
        effectiveCameraSource_ = "mock";
        cameraFallbackReason_.clear();
        aravisCameraConfigured_ = false;
        aravisFake_ = false;
        aravisDeviceId_.clear();
        captureService_->setCameraFactory([options]() mutable
                                          { return std::make_unique<::camera::mock::MockCamera>(options); });
        selectedIfIndex_ = -1;
        selectedDevIndex_ = -1;
        selectedMvCameraIndex_ = -1;
        selectedLabel_.clear();
        lastMindVisionConfigPath_.clear();
        mockCameraConfigured_ = true;
        mockFrameDir_ = options.folder.string();
        mockIntervalMs_ = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(options.frameInterval).count());
        mockLoop_ = options.loopFiles;
    }

    AppBackend::CameraSelectionSnapshot AppBackend::cameraSelection() const
    {
        CameraSelectionSnapshot out;
        if (mockCameraConfigured_)
        {
            out.mode = CameraSelectionSnapshot::Mode::Mock;
        }
        else if (aravisCameraConfigured_)
        {
            out.mode = CameraSelectionSnapshot::Mode::Aravis;
        }
        else if (selectedMvCameraIndex_ >= 0)
        {
            out.mode = CameraSelectionSnapshot::Mode::MindVision;
        }
        else if (selectedIfIndex_ >= 0 && selectedDevIndex_ >= 0)
        {
            out.mode = CameraSelectionSnapshot::Mode::Hardware;
        }
        out.interfaceIndex = selectedIfIndex_;
        out.deviceIndex = selectedDevIndex_;
        out.label = selectedLabel_;
        out.mindVisionIndex = selectedMvCameraIndex_;
        out.mindVisionConfigPath = lastMindVisionConfigPath_;
        out.cameraScriptPath = lastCameraScriptPath_;
        out.mockFrameDir = mockFrameDir_;
        out.mockIntervalMs = mockIntervalMs_;
        out.mockLoop = mockLoop_;
        out.configured = isCameraConfigured();
        return out;
    }

    void AppBackend::setHardwareCameraSelection(int interfaceIndex, int deviceIndex, const std::string &label)
    {
        if (!captureService_)
            return;
        releaseMindVisionOverviewStore();
        requestedCameraSource_ = "egrabber";
        cameraFallbackReason_.clear();

#if !MIB_HAS_EGRABBER
        SPDLOG_WARN("Hardware camera selection ignored: EGrabber SDK is unavailable on this platform");
        effectiveCameraSource_ = "mock";
        cameraFallbackReason_ = "EGrabber SDK is unavailable in this build";
        ::camera::mock::MockCameraOptions options;
        options.folder = std::filesystem::path("data") / "mock_frames";
        captureService_->setCameraFactory([options]() mutable
                                          { return std::make_unique<::camera::mock::MockCamera>(options); });
        selectedIfIndex_ = -1;
        selectedDevIndex_ = -1;
        selectedMvCameraIndex_ = -1;
        selectedLabel_.clear();
        lastMindVisionConfigPath_.clear();
        mockCameraConfigured_ = true;
        {
            auto& mirror = backend::diagnostics::CrashStateMirror::instance();
            mirror.app.selectedInterface.store(-1);
            mirror.app.selectedDevice.store(-1);
            mirror.app.mockCamera.store(true);
            mirror.setCameraLabel("");
        }
        return;
#else
        {
            auto& mirror = backend::diagnostics::CrashStateMirror::instance();
            mirror.app.selectedInterface.store(interfaceIndex);
            mirror.app.selectedDevice.store(deviceIndex);
            mirror.app.mockCamera.store(false);
            mirror.setCameraLabel(label);
        }
#endif

        selectedIfIndex_ = interfaceIndex;
        selectedDevIndex_ = deviceIndex;
        selectedLabel_ = label;
        selectedMvCameraIndex_ = -1;
        lastMindVisionConfigPath_.clear();
        mockCameraConfigured_ = false;
        aravisCameraConfigured_ = false;
        aravisFake_ = false;
        aravisDeviceId_.clear();
        effectiveCameraSource_ = "egrabber";

        captureService_->setCameraFactory([interfaceIndex, deviceIndex]()
                                          { return std::make_unique<::camera::common::EGrabberCamera>(interfaceIndex, deviceIndex); });
        SPDLOG_INFO("Hardware camera selected: {} (if={}, dev={})",
                    label, interfaceIndex, deviceIndex);
    }

    void AppBackend::setMindVisionCameraSelection(int cameraIndex, const std::string &label)
    {
        if (!captureService_)
            return;

        {
            std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
            mindVisionSensor_ = {};
        }
        selectedMvCameraIndex_ = cameraIndex;
        selectedIfIndex_ = -1;
        selectedDevIndex_ = -1;
        selectedLabel_ = label;
        mockCameraConfigured_ = false;
        aravisCameraConfigured_ = false;
        aravisFake_ = false;
        aravisDeviceId_.clear();
        requestedCameraSource_ = "mindvision";
        cameraFallbackReason_.clear();

#if MIB_HAS_MINDVISION
        if (lastMindVisionConfigPath_.empty())
            lastMindVisionConfigPath_ = savedMindVisionConfigPath_;
        const std::string configPath = lastMindVisionConfigPath_;
        captureService_->setCameraFactory([this, cameraIndex, configPath]() {
            return makeLiveCamera(cameraIndex, configPath, *pulseGeneratorService_,
                                  mindVisionOverview_.load(),
                                  [this](const camera::mindvision::SdkCapability& sensor) {
                                      std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                                      mindVisionSensor_ = {sensor.sensorWidth, sensor.sensorHeight,
                                                           sensor.minWidth, sensor.minHeight};
                                  });
        });
        effectiveCameraSource_ = "mindvision";
#else
        SPDLOG_WARN("MindVision camera selection requested but MindVision SDK is unavailable; falling back to mock camera");
        ::camera::mock::MockCameraOptions options;
        options.folder = std::filesystem::path("data") / "mock_frames";
        captureService_->setCameraFactory([options]() mutable
                                          { return std::make_unique<::camera::mock::MockCamera>(options); });
        lastMindVisionConfigPath_.clear();
        mockCameraConfigured_ = false;
        effectiveCameraSource_ = "mock";
        cameraFallbackReason_ = "MindVision SDK is unavailable in this build";
#endif

        auto &mirror = backend::diagnostics::CrashStateMirror::instance();
        mirror.app.selectedInterface.store(-1);
        mirror.app.selectedDevice.store(-1);
        mirror.app.mockCamera.store(mockCameraConfigured_);
        mirror.setCameraLabel(selectedLabel_);

        SPDLOG_INFO("MindVision camera selected: {} (index={})", label, cameraIndex);
    }

    bool AppBackend::applyCameraScriptFromFile(const std::string &path, std::string *errorOut)
    {
        if (selectedIfIndex_ < 0 || selectedDevIndex_ < 0)
        {
            if (errorOut)
                *errorOut = "No hardware camera selected";
            return false;
        }
        // Validate the script file before touching the device: a bogus path
        // should fail with a clear message instead of opening the camera (and
        // possibly disrupting acquisition) only to throw a cryptic SDK error.
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec))
            {
                if (errorOut)
                    *errorOut = "Camera script file not found: " + path;
                SPDLOG_ERROR("Camera script file not found: {}", path);
                return false;
            }
        }
        // Ensure capture thread is stopped
        if (captureService_ && captureService_->isRunning())
        {
            SPDLOG_INFO("Stopping capture before applying camera script");
            captureService_->stop();
        }
        SPDLOG_INFO("Applying camera script to {} from {}", selectedLabel_, path);
        const bool ok =
            cameraControlService_->applyScriptToDevice(selectedIfIndex_, selectedDevIndex_, path, errorOut);
        if (ok)
        {
            lastCameraScriptPath_ = path;
        }
        return ok;
    }

    bool AppBackend::stageMindVisionConfigFromFile(const std::string& path, std::string* errorOut) {
        if (captureService_->isRunning()) {
            if (errorOut) *errorOut = "Stop Live View before changing the saved camera setup";
            return false;
        }
        try {
            std::ifstream input(path);
            if (!input) throw std::runtime_error("Cannot open camera setup");
            const std::string bytes((std::istreambuf_iterator<char>(input)), {});
            const auto parsed = backend::camera::mindvision::parseConfig(bytes);
            if (!parsed.ok) throw std::runtime_error(parsed.error);
            // Construct only: validates generator settings, no SDK/serial I/O.
            auto candidate = makeLiveCamera(
                selectedMvCameraIndex_, path, *pulseGeneratorService_, mindVisionOverview_.load(),
                [this](const camera::mindvision::SdkCapability& sensor) {
                    std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                    mindVisionSensor_ = {sensor.sensorWidth, sensor.sensorHeight, sensor.minWidth,
                                         sensor.minHeight};
                });
            lastMindVisionConfigPath_ = path;
            savedMindVisionConfigPath_ = path;
            if (selectedMvCameraIndex_ >= 0) {
                const int idx = selectedMvCameraIndex_;
                captureService_->setCameraFactory([this, idx, path] {
                    return makeLiveCamera(
                        idx, path, *pulseGeneratorService_, mindVisionOverview_.load(),
                        [this](const camera::mindvision::SdkCapability& sensor) {
                            std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                            mindVisionSensor_ = {sensor.sensorWidth, sensor.sensorHeight,
                                                 sensor.minWidth, sensor.minHeight};
                        });
                });
            }
            return true;
        } catch (const std::exception& e) {
            if (errorOut) *errorOut = e.what();
            return false;
        }
    }

    bool AppBackend::applyMindVisionConfigFromFile(const std::string &path, std::string *errorOut)
    {
        if (selectedMvCameraIndex_ < 0)
        {
            if (errorOut)
                *errorOut = "No MindVision camera selected";
            return false;
        }
        if (captureService_ && captureService_->isRunning())
        {
            SPDLOG_INFO("Stopping capture before applying MindVision config");
            captureService_->stop();
        }
        SPDLOG_INFO("Applying MindVision config to {} from {}", selectedLabel_, path);
        const bool ok = cameraControlService_->applyMindVisionConfig(selectedMvCameraIndex_, path, errorOut);
        if (ok)
        {
            lastMindVisionConfigPath_ = path;
            savedMindVisionConfigPath_ = path;
            const int idx = selectedMvCameraIndex_;
            const std::string configPath = lastMindVisionConfigPath_;
            captureService_->setCameraFactory([this, idx, configPath]() {
                return makeLiveCamera(
                    idx, configPath, *pulseGeneratorService_, mindVisionOverview_.load(),
                    [this](const camera::mindvision::SdkCapability& sensor) {
                        std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
                        mindVisionSensor_ = {sensor.sensorWidth, sensor.sensorHeight,
                                             sensor.minWidth, sensor.minHeight};
                    });
            });
            SPDLOG_INFO("MindVision capture factory updated with config: {}", path);
        }
        return ok;
    }

    void AppBackend::releaseMindVisionOverviewStore() {
        // Also releases an Aravis Overview: any camera re-selection leaves Overview.
        const bool aravis = aravisOverview_.load();
        if (!mindVisionOverview_.load() && !aravis) return;
        captureService_->stop();
        processingService_->stopRealtime();
        const size_t capacity = aravis ? (aravisExperimentCapacity_ ? aravisExperimentCapacity_ : 512)
                                       : mindVisionExperimentCapacity_;
        frameStore_ = std::make_shared<playback::FrameStore>(capacity);
        captureService_->setFrameStore(frameStore_);
        playbackService_->setFrameStore(frameStore_);
        mindVisionOverview_.store(false);
        aravisOverview_.store(false);
    }

    AppBackend::MindVisionSensor AppBackend::mindVisionSensor() const {
        std::lock_guard<std::mutex> lock(mindVisionSensorMutex_);
        return mindVisionSensor_;
    }

    bool AppBackend::setMindVisionOverview(bool overview, std::string* errorOut) {
        auto fail = [&](const std::string& message) {
            if (errorOut) *errorOut = message;
            return false;
        };
        if (!isMindVisionCameraSelected()) return fail("No MindVision camera selected");
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting ||
            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping)
            return fail("Stop the experiment or recording before changing camera mode");
        if (mindVisionOverview_.load() == overview) return true;
        captureService_->stop();
        processingService_->stopRealtime();
        processingService_->setRealtimeEnabled(false);
        try {
            // Camera offsets are sensor coordinates. Processing receives the
            // already-cropped image and uses local coordinates.
            std::ifstream input(std::filesystem::u8path(lastMindVisionConfigPath_));
            const auto parsed = camera::mindvision::parseConfig(
                std::string(std::istreambuf_iterator<char>(input), {}));
            if (!input || !parsed.ok) return fail("Cannot load MindVision experiment ROI");
            processingService_->setRealtimeRoi({0, 0, parsed.config.width, parsed.config.height});
            // A new store prevents stale overview images reaching processing and
            // bounds full-sensor preview memory. Existing store readers can finish
            // using their shared ownership; capture is joined before replacement.
            if (overview) mindVisionExperimentCapacity_ = frameStore_->capacity();
            auto next = std::make_shared<playback::FrameStore>(
                overview ? 8 : mindVisionExperimentCapacity_);
            frameStore_ = std::move(next);
            captureService_->setFrameStore(frameStore_);
            playbackService_->setFrameStore(frameStore_);
            mindVisionOverview_.store(overview);
            SPDLOG_INFO("MindVision mode staged: {}", overview ? "Overview" : "Experiment");
            return true;
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }

    bool AppBackend::saveMindVisionRoi(int x, int y, int width, int height, std::string* errorOut) {
        auto fail = [&](const std::string& message) {
            if (errorOut) *errorOut = message;
            return false;
        };
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting ||
            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping)
            return fail("Stop the experiment before editing its ROI");
        if (!isMindVisionCameraSelected() || lastMindVisionConfigPath_.empty())
            return fail("Select a saved MindVision profile first");
        const auto sensor = mindVisionSensor();
        if (x < 0 || y < 0 || width < std::max(1, sensor.minWidth) ||
            height < std::max(1, sensor.minHeight) || x > 65535 || y > 65535 || width > 65535 ||
            height > 65535 ||
            (sensor.sensorWidth > 0 &&
             (width > sensor.sensorWidth || x > sensor.sensorWidth - width)) ||
            (sensor.sensorHeight > 0 &&
             (height > sensor.sensorHeight || y > sensor.sensorHeight - height)))
            return fail("ROI is outside the camera sensor bounds");
        try {
            const auto path = std::filesystem::u8path(lastMindVisionConfigPath_);
            std::ifstream input(path, std::ios::binary);
            if (!input) return fail("Cannot read MindVision experiment profile");
            auto json = nlohmann::json::parse(input);
            input.close();
            json["offset_x"] = x;
            json["offset_y"] = y;
            json["width"] = width;
            json["height"] = height;
            const auto bytes = json.dump(2) + "\n";
            const auto parsed = camera::mindvision::parseConfig(bytes);
            if (!parsed.ok) return fail(parsed.error);
            auto temporary = path;
            temporary += ".roi-" + std::to_string(Tools::getTimestamp()) + ".tmp";
            struct Cleanup {
                std::filesystem::path path;
                ~Cleanup() {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                }
            } cleanup{temporary};
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) return fail("Cannot write temporary MindVision ROI profile");
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return fail("Cannot replace MindVision profile (Windows error " +
                            std::to_string(GetLastError()) + ")");
#else
            std::error_code ec;
            std::filesystem::rename(temporary, path, ec);
            if (ec) return fail("Cannot replace MindVision profile: " + ec.message());
#endif
            return true;
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }

    std::string AppBackend::aravisProfilePath() const
    {
        if (const char* env = std::getenv("MIB_ARAVIS_PROFILE"); env && *env) return env;
        return (std::filesystem::u8path(dataDir_.empty() ? "." : dataDir_) / "config" / "aravis-camera.json")
            .string();
    }

    void AppBackend::loadAravisProfile()
    {
        AravisProfile profile;
        // The interim deployment variables seed a profile that does not exist yet.
        if (const char* env = std::getenv("MIB_ARAVIS_FPS")) profile.experimentFps = std::atof(env);
        if (const char* env = std::getenv("MIB_ARAVIS_EXPOSURE_US")) profile.experimentExposureUs = std::atof(env);
        if (const char* env = std::getenv("MIB_ARAVIS_PREVIEW_HZ")) profile.previewRateHz = std::atof(env);
        if (const char* env = std::getenv("MIB_ARAVIS_REGION")) {
            if (std::sscanf(env, "%d,%d,%d,%d", &profile.x, &profile.y, &profile.width, &profile.height) == 4)
                profile.hasRoi = true;
            else
                SPDLOG_WARN("AppBackend: ignoring MIB_ARAVIS_REGION='{}' (expected X,Y,W,H)", env);
        }
        const auto path = aravisProfilePath();
        std::ifstream input(std::filesystem::u8path(path));
        if (input) {
            try {
                const auto json = nlohmann::json::parse(input);
                if (json.contains("roi") && json["roi"].is_object()) {
                    const auto& roi = json["roi"];
                    profile.x = roi.value("x", 0);
                    profile.y = roi.value("y", 0);
                    profile.width = roi.value("width", 0);
                    profile.height = roi.value("height", 0);
                    profile.hasRoi = profile.width > 0 && profile.height > 0;
                }
                if (json.contains("experiment")) {
                    profile.experimentFps = json["experiment"].value("frame_rate_hz", profile.experimentFps);
                    profile.experimentExposureUs = json["experiment"].value("exposure_us", profile.experimentExposureUs);
                }
                if (json.contains("overview")) {
                    profile.overviewFps = json["overview"].value("frame_rate_hz", profile.overviewFps);
                    profile.overviewExposureUs = json["overview"].value("exposure_us", profile.overviewExposureUs);
                }
                profile.previewRateHz = json.value("preview_rate_hz", profile.previewRateHz);
            } catch (const std::exception& e) {
                SPDLOG_WARN("AppBackend: ignoring malformed Aravis profile {}: {}", path, e.what());
            }
        }
        aravisProfile_ = profile;
        SPDLOG_INFO("AppBackend: Aravis profile {}: window {}, experiment {} Hz / {} us, overview {} Hz / {} us",
                    path,
                    profile.hasRoi ? fmt::format("{}x{}+{}+{}", profile.width, profile.height, profile.x, profile.y)
                                   : std::string("device default"),
                    profile.experimentFps, profile.experimentExposureUs, profile.overviewFps,
                    profile.overviewExposureUs);
        SPDLOG_INFO("AppBackend: Aravis preview rate {} images/s", profile.previewRateHz);
    }

    bool AppBackend::saveAravisProfile(const AravisProfile& profile, std::string* errorOut)
    {
        try {
            nlohmann::json json;
            if (profile.hasRoi)
                json["roi"] = {{"x", profile.x}, {"y", profile.y}, {"width", profile.width}, {"height", profile.height}};
            json["experiment"] = {{"frame_rate_hz", profile.experimentFps}, {"exposure_us", profile.experimentExposureUs}};
            json["overview"] = {{"frame_rate_hz", profile.overviewFps}, {"exposure_us", profile.overviewExposureUs}};
            json["preview_rate_hz"] = profile.previewRateHz;
            const auto path = std::filesystem::u8path(aravisProfilePath());
            std::filesystem::create_directories(path.parent_path());
            auto temporary = path;
            temporary += ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << json.dump(2) << "\n";
                if (!output) {
                    if (errorOut) *errorOut = "Cannot write the Aravis camera profile";
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                if (errorOut) *errorOut = "Cannot replace the Aravis camera profile: " + ec.message();
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            if (errorOut) *errorOut = e.what();
            return false;
        }
    }

    void AppBackend::installAravisFactory()
    {
#if MIB_HAS_ARAVIS
        ::camera::aravis::AravisCameraOptions options;
        options.deviceId = aravisDeviceId_;
        options.useFake = aravisFake_;
        options.enableGigEVision = aravisGigE_;
        const auto& profile = aravisProfile_;
        if (profile.previewRateHz > 0) options.previewRateHz = profile.previewRateHz;
        // PZ7035 line period: forced only for the whole-frame Align overview, otherwise automatic.
        options.pzHmax = aravisOverview_.load() ? profile.overviewHmax : 0;
        if (aravisOverview_.load()) {
            options.fullSensor = true;
            if (profile.overviewFps > 0) options.frameRateHz = profile.overviewFps;
            if (profile.overviewExposureUs > 0) options.exposureUs = profile.overviewExposureUs;
        } else {
            if (profile.hasRoi)
                options.region = ::camera::aravis::AravisRegion{profile.x, profile.y, profile.width, profile.height};
            if (profile.experimentFps > 0) options.frameRateHz = profile.experimentFps;
            if (profile.experimentExposureUs > 0) options.exposureUs = profile.experimentExposureUs;
        }
        options.onSession = [this, overview = aravisOverview_.load()](const ::camera::aravis::AravisSessionInfo& info) {
            AravisSessionGeometry geometry;
            geometry.sensorWidth = info.sensorWidth;
            geometry.sensorHeight = info.sensorHeight;
            geometry.widthIncrement = info.widthIncrement;
            geometry.heightIncrement = info.heightIncrement;
            geometry.offsetXIncrement = info.offsetXIncrement;
            geometry.offsetYIncrement = info.offsetYIncrement;
            geometry.json = nlohmann::json{
                {"overview", overview},
                {"vendor", info.vendor},
                {"model", info.model},
                {"region", {{"x", info.region.x}, {"y", info.region.y}, {"width", info.region.width},
                            {"height", info.region.height}}},
                {"frame_rate_hz", info.frameRateHz},
                {"frame_rate_max_hz", info.frameRateMaxHz},
                {"frame_rate_clamped", info.frameRateClamped},
                {"frame_rate_limit", info.frameRateLimitReason},
                {"exposure_us", info.exposureUs},
                {"exposure_max_us", info.exposureMaxUs},
                {"exposure_clamped", info.exposureClamped},
                {"band_count", info.bandCount},
                {"delivered_frame_rate_hz", info.deliveredFrameRateHz},
                {"delivered_limit", info.deliveredFrameRateLimit},
                {"preview_rate_hz", info.previewRateHz},
            }.dump();
            std::lock_guard<std::mutex> lock(aravisSessionMutex_);
            aravisSession_ = std::move(geometry);
        };
        captureService_->setCameraFactory([this, options]() mutable -> std::unique_ptr<::camera::common::ICamera> {
            // #501 P1: in Run the PZ7035 cell path is on, and a producer AcquisitionStart would
            // clear the U-Net enable. Whatever asks for a capture start, the producer stays shut.
            if (instrumentMode() == pz::InstrumentMode::Run) {
                SPDLOG_WARN("AppBackend: camera start refused in Run mode (switch to Align for the live camera)");
                return nullptr;
            }
            return std::make_unique<::camera::aravis::AravisCamera>(options);
        });
#endif
    }

    bool AppBackend::setAravisOverview(bool overview, std::string* errorOut)
    {
        auto fail = [&](const std::string& message) {
            if (errorOut) *errorOut = message;
            return false;
        };
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting ||
            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping)
            return fail("Stop the experiment or recording before changing camera mode");
        if (aravisOverview_.load() == overview) return true;
        captureService_->stop();
        processingService_->stopRealtime();
        if (overview) {
            // Overview frames must not reach processing (wrong timing, auto-background).
            aravisRealtimeBeforeOverview_ = processingService_->isRealtimeEnabled();
            processingService_->setRealtimeEnabled(false);
            aravisExperimentCapacity_ = frameStore_->capacity();
        } else {
            processingService_->setRealtimeEnabled(aravisRealtimeBeforeOverview_);
        }
        // A new store keeps full-sensor images out of the experiment store and bounds
        // overview memory, as for MindVision.
        frameStore_ = std::make_shared<playback::FrameStore>(
            overview ? 8 : (aravisExperimentCapacity_ ? aravisExperimentCapacity_ : 512));
        captureService_->setFrameStore(frameStore_);
        playbackService_->setFrameStore(frameStore_);
        aravisOverview_.store(overview);
        installAravisFactory();
        SPDLOG_INFO("Aravis camera mode staged: {}", overview ? "Overview (full sensor)" : "Experiment window");
        return true;
    }

    bool AppBackend::saveAravisRoi(int x, int y, int width, int height, std::string* errorOut)
    {
        auto fail = [&](const std::string& message) {
            if (errorOut) *errorOut = message;
            return false;
        };
        const auto run = experimentCoordinator_->state();
        if (isFrameRecording() || run == app::ExperimentRunState::Starting ||
            run == app::ExperimentRunState::Active || run == app::ExperimentRunState::Stopping)
            return fail("Stop the experiment before editing its ROI");
        AravisSessionGeometry session;
        {
            std::lock_guard<std::mutex> lock(aravisSessionMutex_);
            session = aravisSession_;
        }
        if (session.sensorWidth <= 0 || session.sensorHeight <= 0)
            return fail("The sensor size is not known yet; show the full sensor first");
        if (x < 0 || y < 0 || width < session.widthIncrement || height < session.heightIncrement ||
            width > session.sensorWidth || height > session.sensorHeight || x > session.sensorWidth - width ||
            y > session.sensorHeight - height)
            return fail("ROI is outside the camera sensor bounds");
        if (width % session.widthIncrement || height % session.heightIncrement ||
            x % session.offsetXIncrement || y % session.offsetYIncrement)
            return fail("ROI must be in steps of " + std::to_string(session.offsetXIncrement) + " x " +
                        std::to_string(session.offsetYIncrement) + " (offset) and " +
                        std::to_string(session.widthIncrement) + " x " + std::to_string(session.heightIncrement) +
                        " (size) pixels");
        AravisProfile next = aravisProfile_;
        next.hasRoi = true;
        next.x = x;
        next.y = y;
        next.width = width;
        next.height = height;
        if (!saveAravisProfile(next, errorOut)) return false;
        aravisProfile_ = next;
        // The experiment factory picks the window up; the live Overview keeps the full sensor.
        installAravisFactory();
        return true;
    }

    AppBackend::CameraGeometry AppBackend::cameraGeometry() const
    {
        CameraGeometry g;
        if (isMindVisionCameraSelected()) {
            g.supported = true;
            g.camera = "mindvision";
            g.overview = mindVisionOverview_.load();
            const auto sensor = mindVisionSensor();
            g.sensorWidth = sensor.sensorWidth;
            g.sensorHeight = sensor.sensorHeight;
            g.minWidth = std::max(1, sensor.minWidth);
            g.minHeight = std::max(1, sensor.minHeight);
            try {
                std::ifstream input(std::filesystem::u8path(lastMindVisionConfigPath_));
                const auto parsed = camera::mindvision::parseConfig(
                    std::string(std::istreambuf_iterator<char>(input), {}));
                if (input && parsed.ok) {
                    g.roiX = parsed.config.offsetX;
                    g.roiY = parsed.config.offsetY;
                    g.roiWidth = parsed.config.width;
                    g.roiHeight = parsed.config.height;
                }
            } catch (...) {
            }
            return g;
        }
#if MIB_HAS_ARAVIS
        if (aravisCameraConfigured_) {
            g.supported = true;
            g.camera = "aravis";
            g.overview = aravisOverview_.load();
            {
                std::lock_guard<std::mutex> lock(aravisSessionMutex_);
                g.sensorWidth = aravisSession_.sensorWidth;
                g.sensorHeight = aravisSession_.sensorHeight;
                g.widthIncrement = aravisSession_.widthIncrement;
                g.heightIncrement = aravisSession_.heightIncrement;
                g.offsetXIncrement = aravisSession_.offsetXIncrement;
                g.offsetYIncrement = aravisSession_.offsetYIncrement;
                g.sessionJson = aravisSession_.json;
            }
            g.minWidth = g.widthIncrement;
            g.minHeight = g.heightIncrement;
            if (aravisProfile_.hasRoi) {
                g.roiX = aravisProfile_.x;
                g.roiY = aravisProfile_.y;
                g.roiWidth = aravisProfile_.width;
                g.roiHeight = aravisProfile_.height;
            }
        }
#endif
        return g;
    }

    bool AppBackend::isCameraOverview() const
    {
        return mindVisionOverview_.load() || aravisOverview_.load();
    }

    bool AppBackend::setCameraOverview(bool overview, std::string* errorOut)
    {
        if (instrumentMode() == pz::InstrumentMode::Run) {
            if (errorOut) *errorOut = "The instrument is in Run mode; switch camera modes with Align/Run";
            return false;
        }
        if (isMindVisionCameraSelected()) return setMindVisionOverview(overview, errorOut);
#if MIB_HAS_ARAVIS
        if (aravisCameraConfigured_) return setAravisOverview(overview, errorOut);
#endif
        if (!overview) return true; // nothing to leave
        if (errorOut) *errorOut = "The selected camera has no full-sensor overview";
        return false;
    }

    bool AppBackend::saveCameraRoi(int x, int y, int width, int height, std::string* errorOut)
    {
        if (isMindVisionCameraSelected()) return saveMindVisionRoi(x, y, width, height, errorOut);
#if MIB_HAS_ARAVIS
        if (aravisCameraConfigured_) return saveAravisRoi(x, y, width, height, errorOut);
#endif
        if (errorOut) *errorOut = "The selected camera has no saved window";
        return false;
    }

    bool AppBackend::isMindVisionCameraSelected() const
    {
        return selectedMvCameraIndex_ >= 0;
    }

    bool AppBackend::softTriggerCamera(std::string *errorOut)
    {
        if (!captureService_ || !captureService_->isRunning())
        {
            if (errorOut)
                *errorOut = "Capture is not running";
            return false;
        }
        if (!captureService_->softTriggerActiveCamera())
        {
            if (errorOut)
                *errorOut = "Camera rejected software trigger (not in soft-trigger mode, or unsupported)";
            return false;
        }
        return true;
    }

    bool AppBackend::resetSelectedHardwareCamera(std::string *errorOut)
    {
        if (selectedIfIndex_ < 0 || selectedDevIndex_ < 0)
        {
            if (errorOut)
                *errorOut = "No hardware camera selected";
            return false;
        }
        // Ensure capture thread is stopped
        if (captureService_ && captureService_->isRunning())
        {
            SPDLOG_INFO("Stopping capture before camera reset");
            captureService_->stop();
        }
        SPDLOG_INFO("Resetting camera {}", selectedLabel_);
        return cameraControlService_->deviceReset(selectedIfIndex_, selectedDevIndex_, errorOut);
    }

    app::CameraSourceInfo AppBackend::cameraSourceInfo() const
    {
        app::CameraSourceInfo info;
        info.requested = requestedCameraSource_;
        info.effective = effectiveCameraSource_;
        info.label = selectedLabel_;
        info.simulated = effectiveCameraSource_ == "mock" ||
                         (effectiveCameraSource_ == "aravis" && aravisFake_);
        info.fallback = !cameraFallbackReason_.empty() ||
                        (requestedCameraSource_ != "unknown" && requestedCameraSource_ != effectiveCameraSource_);
        info.fallbackReason = cameraFallbackReason_;
        if (info.fallback && info.fallbackReason.empty())
            info.fallbackReason = "requested " + requestedCameraSource_ + " but " + effectiveCameraSource_ + " is configured";
        return info;
    }

    app::ExperimentCoordinator &AppBackend::experiment() { return *experimentCoordinator_; }

    services::MonitoringDensityService &AppBackend::monitoringDensity() { return *monitoringDensity_; }

    services::serialbus::SerialBusManager &AppBackend::serialBus() { return *serialBusManager_; }

    bool AppBackend::isCameraConfigured() const
    {
        // Camera is configured if a hardware, MindVision, or mock camera is selected.
        return (selectedIfIndex_ >= 0 && selectedDevIndex_ >= 0)
            || (selectedMvCameraIndex_ >= 0)
            || mockCameraConfigured_
            || aravisCameraConfigured_;
    }

    bool AppBackend::startFrameRecording(const std::string& hdf5FilePath, std::string* error) {
        // Raw Record writes the ~60 fps preview frames the producer delivers (Align only), not the 5 kHz
        // PL results, with no free-space guard on the RAM root. Until #649's ring save exists, a PL-science
        // instrument uses an experiment instead (#651 G9).
        if (!app::hostProcessingAvailable()) {
            if (error)
                *error = "Raw recording is not available on a PL-science instrument yet: it would only save the preview "
                         "frames (about 60 fps), not the 5 kHz results. Use an experiment (#649 adds saving the buffered frames).";
            return false;
        }
        std::lock_guard<std::mutex> lock(frameRecordingLifecycleMutex_);
        if (error)
            *error = "Frame recording start failed: check capture and processing-core readiness";
        bool started = false;
        // Use the same transaction mutex as experiment Start, through opening
        // the file and acquiring recording ownership (not just a state check).
        if (!experimentCoordinator_ || !experimentCoordinator_->withIdleConfiguration([&] {
                started = startFrameRecordingLocked(hdf5FilePath, error);
            })) {
            if (error) *error = "Stop or reset the experiment before starting raw recording";
            const auto status =
                experimentCoordinator_ ? experimentCoordinator_->status() : app::ExperimentStatus{};
            SPDLOG_WARN("Frame recording refused: experiment state={}, run={}",
                        app::toString(status.state), status.startGeneration);
            backend::services::CrashReporter::breadcrumb(
                "recording", "start refused: experiment is not idle",
                nlohmann::json{{"operation", "startFrameRecording"},
                               {"owner", "experiment"},
                               {"state", app::toString(status.state)},
                               {"run", status.startGeneration}}
                    .dump());
            return false;
        }
        if (started && error) error->clear();
        backend::services::CrashReporter::breadcrumb(
            "recording", started ? "start admitted: raw writer acquired"
                                 : "start refused: writer or readiness conflict");
        return started;
    }

    bool AppBackend::startFrameRecordingLocked(const std::string& hdf5FilePath,
                                               std::string* error) {
        if (frameRecordingOwned_.load()) {
            if (error) *error = "Stop the existing raw recording before starting another";
            SPDLOG_WARN("Frame recording already in progress");
            return false;
        }
        if (!captureService_ || !captureService_->isRunning()) {
            if (error) *error = "Start capture before starting raw recording";
            SPDLOG_ERROR("Cannot start frame recording: camera not running");
            return false;
        }
        if (!processingService_ || !processingService_->isProcessingCorePinSatisfied()) {
            if (error) *error = "Select an available processing core before starting raw recording";
            SPDLOG_ERROR("Cannot start frame recording: selected processing core is unavailable");
            return false;
        }

        // Open HDF5 file for recording
        auto& hdf5 = *hdf5Service_;
        if (hdf5.isFileOpen()) {
            if (error)
                *error = "Close the existing HDF5 file before starting raw recording; its writer "
                         "will not be replaced";
            SPDLOG_WARN("Frame recording refused: shared HDF5 file already open");
            return false;
        }

        std::string path = hdf5FilePath;
        if (path.size() < 3 || (path.substr(path.size() - 3) != ".h5" &&
            (path.size() < 5 || path.substr(path.size() - 5) != ".hdf5"))) {
            path += ".h5";
        }

        if (!hdf5.openFile(path)) {
            if (error)
                *error = "Cannot open the raw recording output; choose a writable destination";
            SPDLOG_ERROR("Failed to open HDF5 file for recording: {}", path);
            return false;
        }
        if (!hdf5.initializeRecordingDatasets()) {
            if (error) *error = "Cannot initialize raw recording datasets";
            hdf5.closeFile();
            return false;
        }
        auto processingCoreLease = processingService_->acquireProcessingCoreOperation();

        frameRecordingPath_ = path;
        frameRecordingWritten_.store(0);
        frameRecordingFiltered_.store(0);
        {
            std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
            const auto lifecycle = captureService_->lifecycleSnapshot();
            recordingAccounting_.reset(
                lifecycle.generation,
                captureService_->activeDeliveryMode() == ::camera::common::FrameDeliveryMode::LatestFrame);
            lastRecordingAccounting_ = backend::recording::RecordingAccountingSnapshot{};
        }
        frameRecordingOwned_.store(true);
        frameRecordingRunning_.store(true);
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().recorder;
            m.recording.store(true);
            m.framesWritten.store(0);
            m.framesFiltered.store(0);
        }
        backend::services::CrashReporter::breadcrumb("recording", "frame recording started");

        // Launch recording thread
        frameRecordingThread_ = std::make_unique<std::thread>(
            [this, processingCoreLease = std::move(processingCoreLease)]() mutable {
            SPDLOG_INFO("Frame recording thread started");

            const uint64_t startTimeNs = backend::app::WallClock::nowNs();
            const auto recordingWall = backend::app::WallClock::status();
            const auto recordingConfig = processingService_->getProcessingConfig();
            const bool recordingMultiImageEnabled =
                recordingConfig.multi_image_enabled && recordingConfig.multi_image_count > 1;
            const uint64_t recordingMultiImageCount = static_cast<uint64_t>(
                std::max(1, recordingConfig.multi_image_count));

            uint64_t lastProcessedIdx = 0;
            bool firstFrame = true;
            constexpr size_t FLUSH_BATCH = 50; // Flush every N frames

            std::vector<cv::Mat> batchImages;
            std::vector<services::Hdf5Service::RecordingFrameMeta> batchMeta;
            batchImages.reserve(FLUSH_BATCH);
            batchMeta.reserve(FLUSH_BATCH);

            // Decouple HDF5 writes from FrameStore reads via a bounded 3-slot
            // queue. The written count advances only on a confirmed successful
            // write; any failure or overflow stops recording and surfaces a
            // fatal error (no silent data loss).
            struct RecordingBatch {
                std::vector<cv::Mat> images;
                std::vector<services::Hdf5Service::RecordingFrameMeta> meta;
            };
            backend::recording::HdfWriteQueue<RecordingBatch> writeQueue(
                3,
                [this](const RecordingBatch& b) -> bool {
                    if (!hdf5Service_->appendRecordingFrames(b.images, b.meta)) {
                        recordingAccounting_.persistenceFailed.fetch_add(b.images.size(),
                                                                         std::memory_order_relaxed);
                        return false;
                    }
                    frameRecordingWritten_.fetch_add(b.images.size(), std::memory_order_relaxed);
                    recordingAccounting_.persistenceCommitted.fetch_add(b.images.size(),
                                                                        std::memory_order_relaxed);
                    return true;
                },
                [this](const std::string& msg) {
                    frameRecordingRunning_.store(false);
                    {
                        std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
                        recordingAccounting_.setFatal("Recording save failed: " + msg);
                    }
                    reportFatalSaveError("Recording save failed: " + msg);
                });

            // Hoisted per-poll-batch: refreshed only when configVersion changes.
            // Staleness window is one poll iteration (~ms), which is acceptable
            // and documented. Per-frame refresh was the dominant lock cost (P1).
            uint64_t lastConfigVer = (std::numeric_limits<uint64_t>::max)();
            services::ProcessingConfig config;
            services::ProcessingService::Roi roi;
            std::shared_ptr<const cv::Mat> bgShared;

            while (frameRecordingRunning_.load()) {
                // Refresh config/roi/background only when something changed.
                const uint64_t curVer = processingService_->getConfigVersion();
                if (curVer != lastConfigVer) {
                    config    = processingService_->getProcessingConfig();
                    roi       = processingService_->getRealtimeRoi();
                    bgShared  = processingService_->getRealtimeBackgroundGrayShared();
                    lastConfigVer = curVer;
                }

                // Get latest available index from FrameStore
                const uint64_t totalWritten = frameStore_->totalWritten();
                if (totalWritten == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                const uint64_t latestIdx = totalWritten - 1;
                const uint64_t startIdx = firstFrame ? latestIdx : lastProcessedIdx + 1;
                firstFrame = false;

                if (startIdx > latestIdx) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                // Process new frames. Every index claimed here is ADMITTED and
                // must end in exactly one accounting category (issue #367).
                for (uint64_t idx = startIdx; idx <= latestIdx && frameRecordingRunning_.load(); ++idx) {
                    playback::Frame f{};
                    const auto readOutcome = frameStore_->readByWriteIndex(idx, f);
                    if (readOutcome == playback::FrameReadOutcome::NotYetCommitted) {
                        // Reserved but not published yet: wait for the commit
                        // instead of claiming the index (retried next pass).
                        break;
                    }
                    recordingAccounting_.admit(idx);
                    lastProcessedIdx = idx;
                    if (readOutcome == playback::FrameReadOutcome::Overwritten) {
                        recordingAccounting_.count(backend::recording::FrameOutcome::StoreOverwritten);
                        continue;
                    }
                    if (readOutcome != playback::FrameReadOutcome::Available) {
                        recordingAccounting_.count(backend::recording::FrameOutcome::StoreMalformed);
                        continue;
                    }

                    const auto classification =
                        processingService_->classifyFrameWithActiveKernel(f, config, roi, bgShared);
                    using Kind = services::ProcessingService::FrameClassification::Kind;
                    if (classification.kind == Kind::Empty) {
                        frameRecordingFiltered_.fetch_add(1, std::memory_order_relaxed);
                        recordingAccounting_.count(backend::recording::FrameOutcome::Empty);
                        continue;
                    }
                    if (classification.kind == Kind::Malformed) {
                        recordingAccounting_.count(backend::recording::FrameOutcome::StoreMalformed);
                        continue;
                    }
                    if (classification.kind == Kind::ProcessingFailed) {
                        // Never counted as an empty/filtered frame.
                        recordingAccounting_.count(backend::recording::FrameOutcome::ProcessingFailed);
                        SPDLOG_WARN("Frame recording: processing failed for frame {}: {}", idx,
                                    classification.detail);
                        continue;
                    }

                    // Convert to cv::Mat, then crop to the preview ROI so the
                    // recording captures exactly the region the user selected
                    // (full frame when no ROI is set). Mirrors the clamp the
                    // processing path applies.
                    const int w = static_cast<int>(f.width);
                    const int h = static_cast<int>(f.height);
                    const size_t step = (f.linePitch == 0 ? static_cast<size_t>(f.width) : f.linePitch);
                    // classifyFrame above already rejects short buffers, but
                    // keep the strided view safe on its own terms.
                    if (f.data.size() < static_cast<size_t>(h - 1) * step + static_cast<size_t>(w)) {
                        recordingAccounting_.count(backend::recording::FrameOutcome::StoreMalformed);
                        continue;
                    }
                    cv::Mat view(h, w, CV_8UC1, f.data.data(), step);
                    const auto crop = backend::recording::clampRoiToFrame(w, h, roi.x, roi.y, roi.w, roi.h);
                    cv::Mat region = view(cv::Rect(crop.x, crop.y, crop.w, crop.h));
                    batchImages.push_back(region.clone());

                    services::Hdf5Service::RecordingFrameMeta meta;
                    meta.index = idx;
                    meta.timestampNs = f.timestamp;
                    meta.width = static_cast<uint64_t>(crop.w);
                    meta.height = static_cast<uint64_t>(crop.h);
                    batchMeta.push_back(meta);
                    recordingAccounting_.count(backend::recording::FrameOutcome::Processed);

                    // Flush batch when full
                    if (batchImages.size() >= FLUSH_BATCH) {
                        const uint64_t n = static_cast<uint64_t>(batchImages.size());
                        recordingAccounting_.persistenceAdmitted.fetch_add(n, std::memory_order_relaxed);
                        if (!writeQueue.submit(RecordingBatch{std::move(batchImages), std::move(batchMeta)})) {
                            // Overflow/latched error: the batch was refused.
                            recordingAccounting_.persistenceFailed.fetch_add(n, std::memory_order_relaxed);
                            break; // fatal error already surfaced via onError
                        }
                        batchImages.clear();
                        batchMeta.clear();
                        batchImages.reserve(FLUSH_BATCH);
                        batchMeta.reserve(FLUSH_BATCH);
                    }
                }
            }

            // Submit any remaining frames, then drain the writer thread.
            if (!batchImages.empty()) {
                const uint64_t n = static_cast<uint64_t>(batchImages.size());
                recordingAccounting_.persistenceAdmitted.fetch_add(n, std::memory_order_relaxed);
                if (!writeQueue.submit(RecordingBatch{std::move(batchImages), std::move(batchMeta)})) {
                    recordingAccounting_.persistenceFailed.fetch_add(n, std::memory_order_relaxed);
                }
            }
            if (!writeQueue.flushAndStop()) {
                {
                    std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
                    recordingAccounting_.setFatal("Recording final flush failed: " + writeQueue.error());
                }
                reportFatalSaveError("Recording final flush failed: " + writeQueue.error());
            }
            // Any admission the writer neither committed nor reported failed
            // (queue torn down mid-batch) is an explicit pending term.
            {
                const uint64_t admittedP = recordingAccounting_.persistenceAdmitted.load();
                const uint64_t resolved = recordingAccounting_.persistenceCommitted.load() +
                                          recordingAccounting_.persistenceFailed.load();
                if (admittedP > resolved) {
                    recordingAccounting_.persistencePendingAtStop.store(admittedP - resolved);
                }
            }

            // Write recording info
            const uint64_t endTimeNs = backend::app::WallClock::nowNs();

            if (!hdf5Service_->writeRecordingInfo(startTimeNs, endTimeNs,
                                                  frameRecordingWritten_.load(),
                                                  frameRecordingFiltered_.load(),
                                                  recordingMultiImageEnabled,
                                                  recordingMultiImageCount,
                                                  &processingCoreLease.identity())) {
                SPDLOG_ERROR("Frame recording: failed to write recording_info metadata");
                {
                    std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
                    recordingAccounting_.setFatal("recording_info metadata write failed");
                }
                reportFatalSaveError(
                    "Frame recording metadata/processing-core provenance write failed");
            }
            if (!hdf5Service_->writeWallClockProvenance(recordingWall.source, recordingWall.offsetNs)) {
                SPDLOG_ERROR("Frame recording: failed to persist wall-clock provenance");
            }
            // Final reconciliation (issue #367): a run is Complete only when
            // every admitted frame and every writer admission reconcile.
            backend::recording::RecordingAccountingSnapshot finalAccounting;
            {
                std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
                finalAccounting = backend::recording::reconcile(recordingAccounting_.snapshot());
                lastRecordingAccounting_ = finalAccounting;
            }
            if (!hdf5Service_->writeRunAccounting(finalAccounting)) {
                SPDLOG_ERROR("Frame recording: failed to persist run accounting");
            }
            // Time/telemetry provenance (issue #368): what timestampNs means for
            // this file and the final per-metric telemetry with validity.
            if (captureService_ &&
                !hdf5Service_->writeAcquisitionProvenance(captureService_->timestampDescriptor(),
                                                          captureService_->telemetrySnapshot())) {
                SPDLOG_ERROR("Frame recording: failed to persist acquisition provenance");
            }
            hdf5Service_->closeFile();

            SPDLOG_INFO("Frame recording stopped: {} frames recorded, {} empty filtered, "
                        "completion={} ({}), file: {}",
                        frameRecordingWritten_.load(), frameRecordingFiltered_.load(),
                        backend::recording::toString(finalAccounting.completion),
                        finalAccounting.completionReason, frameRecordingPath_);
        });

        SPDLOG_INFO("Frame recording started: {}", path);
        return true;
    }

    void AppBackend::stopFrameRecording() {
        std::lock_guard<std::mutex> lock(frameRecordingLifecycleMutex_);
        if (!frameRecordingRunning_.load() &&
            (!frameRecordingThread_ || !frameRecordingThread_->joinable())) return;

        frameRecordingRunning_.store(false);
        if (frameRecordingThread_ && frameRecordingThread_->joinable()) {
            frameRecordingThread_->join();
        }
        frameRecordingThread_.reset();
        frameRecordingOwned_.store(false);
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().recorder;
            m.recording.store(false);
            m.framesWritten.store(frameRecordingWritten_.load());
            m.framesFiltered.store(frameRecordingFiltered_.load());
        }
        backend::services::CrashReporter::breadcrumb("recording",
            "frame recording stopped");
    }

    bool AppBackend::isFrameRecording() const {
        return frameRecordingOwned_.load();
    }

    uint64_t AppBackend::frameRecordingCount() const {
        return frameRecordingWritten_.load();
    }

    uint64_t AppBackend::frameRecordingFiltered() const {
        return frameRecordingFiltered_.load();
    }

    backend::recording::RecordingAccountingSnapshot AppBackend::recordingAccounting() const {
        std::lock_guard<std::mutex> alk(recordingAccountingMutex_);
        if (frameRecordingRunning_.load()) {
            return backend::recording::reconcile(recordingAccounting_.snapshot());
        }
        return lastRecordingAccounting_;
    }

    void AppBackend::setBackgroundCaptureCallback(BackgroundCaptureCallback callback) {
        std::scoped_lock lk(backgroundCaptureCallbackMutex_);
        backgroundCaptureCallback_ = std::move(callback);
    }

    void AppBackend::setFatalSaveErrorCallback(FatalSaveErrorCallback callback) {
        fatalSaveErrorCb_ = std::move(callback);
    }

    void AppBackend::reportFatalSaveError(const std::string& msg) {
        SPDLOG_ERROR("Fatal save error: {}", msg);
        if (fatalSaveErrorCb_) fatalSaveErrorCb_(msg);
    }

    void AppBackend::setPipelineTimingEnabled(bool enabled) {
        diagnostics::PipelineTimingRecorder::instance().setEnabled(enabled);
        SPDLOG_INFO("AppBackend: pipeline timing instrumentation {}",
                    enabled ? "enabled" : "disabled");
    }

    bool AppBackend::isPipelineTimingEnabled() const {
        return diagnostics::PipelineTimingRecorder::instance().isEnabled();
    }

    bool AppBackend::dumpPipelineTiming(const std::string& directory, std::string* errorOut) {
        const std::string dir = directory.empty() ? pipelineTimingDir_ : directory;
        if (dir.empty()) {
            if (errorOut) *errorOut = "no pipeline timing dump directory configured";
            return false;
        }
        std::string error;
        auto& recorder = diagnostics::PipelineTimingRecorder::instance();
        if (!recorder.dumpCsv(dir, &error)) {
            SPDLOG_ERROR("AppBackend: pipeline timing dump failed: {}", error);
            if (errorOut) *errorOut = error;
            return false;
        }
        SPDLOG_INFO("AppBackend: pipeline timing dumped to {} (frames={}, triggers={})", dir,
                    recorder.frameRecordCount(), recorder.triggerRecordCount());
        return true;
    }

    void AppBackend::dumpPipelineTimingIfEnabled() {
        auto& recorder = diagnostics::PipelineTimingRecorder::instance();
        if (!recorder.isEnabled()) return;
        if (recorder.frameRecordCount() == 0 && recorder.triggerRecordCount() == 0) return;
        dumpPipelineTiming();
    }

    void AppBackend::setLastConfigJson(const std::string& json) {
        {
            std::lock_guard<std::mutex> lk(configJsonMutex_);
            lastConfigJson_ = json;
        }
        // Optional `rf_generator` block: {"enabled":true,"transport":"usb"|"lan",
        // "resource":"auto"|"/dev/usbtmc0"|"USB0::...::INSTR"|"host[:port]",
        // "timeout_ms":1000}. Absent or malformed = link disabled (logged).
        if (!rfGeneratorService_) return;
        services::RfGeneratorService::Config cfg;
        try {
            const auto parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_object() && parsed.contains("rf_generator") && parsed["rf_generator"].is_object()) {
                const auto& rf = parsed["rf_generator"];
                cfg.enabled = rf.value("enabled", false);
                cfg.transport = rf.value("transport", std::string("usb"));
                cfg.resource = rf.value("resource", std::string("auto"));
                cfg.timeoutMs = rf.value("timeout_ms", 1000);
            }
        } catch (const std::exception& ex) {
            SPDLOG_WARN("AppBackend: rf_generator config ignored: {}", ex.what());
            cfg = {};
        }
        rfGeneratorService_->setConfig(cfg);
    }

    std::string AppBackend::getLastConfigJson() const {
        std::lock_guard<std::mutex> lk(configJsonMutex_);
        return lastConfigJson_;
    }

    diagnostics::HostMemoryBudgetSnapshot AppBackend::memoryBudgetSnapshot() const {
        using diagnostics::MemoryKnowledge;
        diagnostics::HostMemoryBudgetSnapshot s;
        s.sampledAtUs = Tools::getTimestamp();
        s.processRssMB = Tools::getProcessMemoryMB();
        s.processPeakRssMB = Tools::getPeakProcessMemoryMB();

        // 1) Camera / SDK buffers: only the buffer *count* is observable, and
        //    only for backends that report it; the vendor's memory is not.
        {
            diagnostics::MemoryOwnerStats o;
            o.name = "capture.sdkBuffers";
            size_t frameBytes = 0;
            if (frameStore_ && captureService_ && captureService_->isRunning()) {
                playback::Frame f;
                if (frameStore_->getLatest(f)) frameBytes = f.data.size();
            }
            const auto t = captureService_ ? captureService_->telemetrySnapshot()
                                           : services::AcquisitionTelemetrySnapshot{};
            if (t.sdkInputBufferCount.hasValue() && frameBytes > 0) {
                o.knowledge = MemoryKnowledge::Estimated;
                o.currentCount = t.sdkInputBufferCount.value;
                o.peakCount = o.currentCount;
                o.currentBytes = o.currentCount * static_cast<uint64_t>(frameBytes);
                o.peakBytes = o.currentBytes;
                o.capacityCount = o.currentCount;
                o.note = "SDK input buffers x last frame payload; vendor allocations are not directly observable";
            } else {
                o.knowledge = MemoryKnowledge::Unknown;
                o.note = t.sessionActive ? "this camera backend does not report its buffer count"
                                         : "no capture session";
            }
            s.owners.push_back(std::move(o));
        }
        // 2) FrameStore ring.
        if (frameStore_) s.owners.push_back(frameStore_->memoryStats());
        // 3) Processing: experiment buffer, monitoring rings, batch queue,
        //    persistence queue, presentation snapshot.
        if (processingService_) {
            for (auto& o : processingService_->memoryStats().all()) s.owners.push_back(std::move(o));
        }
        // 4) Exporter jobs: streaming by design (issue #344), no retained pool.
        {
            diagnostics::MemoryOwnerStats o;
            o.name = "export.jobs";
            o.knowledge = MemoryKnowledge::Estimated;
            o.note = "HdfExportService / Python exporter stream one frame at a time; working set is one frame plus one encode buffer per job";
            s.owners.push_back(std::move(o));
        }
        return s;
    }

} // namespace backend
