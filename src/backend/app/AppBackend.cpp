#include "backend/app/AppBackend.h"

#include "backend/services/Logger.h"
#include "backend/services/CrashReporter.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/database/SqliteService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfWriteQueue.h"
#include "backend/recording/RoiCrop.h"
#include "backend/services/CaptureService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/playback/PlaybackService.h"
#include "backend/playback/FrameStore.h"
#include "backend/camera/egrabber/EGrabberCamera.h"
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/services/CameraControlService.h"
#include "backend/services/AutofocusService.h"
#include "backend/services/TriggerService.h"
#include "backend/services/YoloService.h"
#include "backend/services/SyringePumpService.h"
#include "backend/services/PulseGeneratorService.h"
#include "backend/processing/EModulusLutCatalog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <QString>
#include <spdlog/spdlog.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#ifndef MIB_HAS_EGRABBER
#define MIB_HAS_EGRABBER 0
#endif
#ifndef MIB_HAS_MINDVISION
#define MIB_HAS_MINDVISION 0
#endif

namespace backend
{
    namespace
    {
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

    AppBackend::AppBackend() = default;

    AppBackend::~AppBackend() {
        shutdown();
    }

    void AppBackend::shutdown() {
        // Stop threads before member destruction begins. Members are destroyed
        // in reverse declaration order, so triggerService_/autofocusService_
        // die before processingService_ — a still-running realtime loop would
        // invoke its callbacks on freed services. Every call below is
        // idempotent, so shutdown() may run more than once.
        if (captureService_) {
            captureService_->stop();
        }
        if (triggerService_) {
            triggerService_->stop();
        }
        stopFrameRecording();
        if (processingService_) {
            processingService_->stopRealtime();
            processingService_->stopBatchPipeline();
            processingService_->stop();
        }
        // All pipeline threads are stopped now, so the dump is an exact
        // snapshot of the recorded latency data.
        dumpPipelineTimingIfEnabled();
    }

    bool AppBackend::initialize(const std::string &dataDir)
    {
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

        sqliteService_ = std::make_unique<services::SqliteService>();
        hdf5Service_ = std::make_unique<services::Hdf5Service>();
        captureService_ = std::make_unique<services::CaptureService>();
        processingService_ = std::make_unique<services::ProcessingService>();
        // Funnel experiment flush-write failures through the same fatal-save-error
        // sink as recording, so the UI surfaces them and stops the experiment.
        processingService_->setFlushErrorCallback([this](const std::string& msg) {
            reportFatalSaveError(msg);
        });
        playbackService_ = std::make_unique<services::PlaybackService>();
        cameraControlService_ = std::make_unique<services::CameraControlService>();
        autofocusService_ = std::make_unique<services::AutofocusService>();
        triggerService_ = std::make_unique<services::TriggerService>();
        yoloService_ = std::make_unique<services::YoloService>();
        syringePumpService_ = std::make_unique<services::SyringePumpService>();
        pulseGeneratorService_ = std::make_unique<services::PulseGeneratorService>();
        frameStore_ = std::make_shared<playback::FrameStore>(5000);

        bool bootSqlite = true;
        bool bootHdf5 = true;
        bool bootProcessing = true;
        bool bootYolo = true;
        bool bootAutofocus = true;
        bool bootTrigger = true;
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
                    bootYolo = false;
                    bootAutofocus = false;
                    bootTrigger = false;
                    bootCapture = false;
                    bootPlayback = false;
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
                    bootYolo = false;
                }
                else if (token == "autofocus")
                {
                    bootAutofocus = false;
                }
                else if (token == "trigger")
                {
                    bootTrigger = false;
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

        SPDLOG_INFO("AppBackend boot toggles: sqlite={}, hdf5={}, processing={}, yolo={}, autofocus={}, trigger={}, capture={}, playback={}",
                    bootSqlite, bootHdf5, bootProcessing, bootYolo, bootAutofocus, bootTrigger, bootCapture, bootPlayback);

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

        // Initialize YOLO service - resolve model path relative to data directory
        // dataDir is typically {exeDir}/data, so we go up one level to get exeDir
        std::filesystem::path dataPath(dataDir);
        std::filesystem::path exeDir = dataPath.parent_path();
        std::filesystem::path modelPath = exeDir / "resources" / "models" / "yolo11n-seg.onnx";
        if (bootYolo)
        {
            if (!yoloService_->initialize(modelPath.string()))
            {
                SPDLOG_WARN("YOLO model not loaded - segmentation features will not be available");
            }
        }
        else
        {
            SPDLOG_WARN("AppBackend: yolo bootstrap disabled by MIB_DISABLED_SERVICES");
        }

        // Load Young's modulus LUT for emodulus gating
        if (bootProcessing)
        {
            std::filesystem::path bundledLutPath = exeDir / "resources" / "isoelastic_curve" / "scaled_isoelastic_data_LUT_6.16-4.24.txt";
            backend::EModulusLutCatalog lutCatalog;
            backend::EModulusLutCatalog::ManagedLutInfo lutInfo;
            QString activeLutPath = QString::fromStdString(bundledLutPath.string());
            QString managedLutPath = activeLutPath;
            QString managedError;
            if (!lutCatalog.ensureManagedLut(QString::fromStdString(bundledLutPath.string()), &managedLutPath, &lutInfo, &managedError))
            {
                SPDLOG_WARN("AppBackend: LUT catalog resolution failed, falling back to bundled path {}: {}",
                            bundledLutPath.string(), managedError.toStdString());
                activeLutPath = QString::fromStdString(bundledLutPath.string());
                lutInfo.sourceType = "bundled-fallback";
                lutInfo.revision = "bundled";
                lutInfo.localPath = activeLutPath;
                lutInfo.checksumStatus = "unknown";
            }
            else
            {
                activeLutPath = managedLutPath;
            }

            if (!processingService_->loadEModulusLut(activeLutPath.toStdString()))
            {
                const QString bundledLutPathStr = QString::fromStdString(bundledLutPath.string());
                if (activeLutPath != bundledLutPathStr && processingService_->loadEModulusLut(bundledLutPath.string()))
                {
                    activeLutPath = bundledLutPathStr;
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
                        lutInfo.sourceType.toStdString(),
                        lutInfo.revision.toStdString(),
                        lutInfo.localPath.toStdString().empty() ? activeLutPath.toStdString() : lutInfo.localPath.toStdString(),
                        lutInfo.checksumStatus.toStdString(),
                        lutInfo.remoteUpdated,
                        lutInfo.usedBundledFallback,
                        lutInfo.manifestUrl.toStdString());
            processingService_->start();
        }
        else
        {
            SPDLOG_WARN("AppBackend: processing bootstrap disabled by MIB_DISABLED_SERVICES");
        }
        // Note: startRealtime() is now called when Experiment tab becomes active, not during initialization

        // Wire autofocus service to receive ring ratios from processing service
        if (bootProcessing && bootAutofocus)
        {
            processingService_->setRingRatioCallback([this](double ringRatio, int64_t timestampNs)
                                                     {
                if (autofocusService_) {
                    autofocusService_->onRingRatio(ringRatio, timestampNs);
                } });
        }
        else
        {
            processingService_->setRingRatioCallback({});
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
        }
        else
        {
            processingService_->setTargetGroupCallback({});
            if (!bootTrigger)
            {
                SPDLOG_WARN("AppBackend: trigger callback wiring disabled by MIB_DISABLED_SERVICES");
            }
        }

        // Wire camera lifecycle to trigger service
        if (bootCapture && bootTrigger)
        {
            captureService_->setCameraReadyCallback([this](camera::common::ICamera* cam) {
                if (triggerService_) {
                    triggerService_->setCamera(cam);
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
                camera::mock::MockCameraOptions options;
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
                                                  { return std::make_unique<camera::mock::MockCamera>(options); });
                mockCameraConfigured_ = true;
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                selectedMvCameraIndex_ = -1;
                selectedLabel_.clear();
                lastMindVisionConfigPath_.clear();
            };

            if (cameraMode == "mock")
            {
                configureMock();
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
                captureService_->setCameraFactory([cameraIndex, configPath]() mutable
                                                  { return std::make_unique<camera::common::MindVisionCamera>(cameraIndex, configPath); });
                mockCameraConfigured_ = false;
                selectedIfIndex_ = -1;
                selectedDevIndex_ = -1;
                selectedMvCameraIndex_ = cameraIndex;
                selectedLabel_ = configPath.empty()
                    ? std::string("MindVision camera ") + std::to_string(cameraIndex)
                    : std::string("MindVision camera ") + std::to_string(cameraIndex) + " (" + configPath + ")";
                lastMindVisionConfigPath_ = configPath;
#else
                SPDLOG_ERROR("AppBackend: profile requests a MindVision camera but this build "
                             "does not include MindVision support (compiled with "
                             "MIB_ENABLE_MINDVISION=OFF); falling back to mock camera. Install "
                             "a build with the MindVision SDK enabled to use this profile.");
                cameraMode = "mock";
                configureMock();
#endif
            }
            else if (cameraMode == "egrabber" || cameraMode == "hardware")
            {
#if MIB_HAS_EGRABBER
                SPDLOG_INFO("AppBackend: configuring hardware EGrabber camera");
                captureService_->setCameraFactory([]()
                                                  { return std::make_unique<camera::common::EGrabberCamera>(); });
                mockCameraConfigured_ = false;
                selectedMvCameraIndex_ = -1;
            #else
                SPDLOG_WARN("AppBackend: hardware mode requested but EGrabber SDK is unavailable; keeping mock camera");
                cameraMode = "mock";
                configureMock();
#endif
            }
            else
            {
                SPDLOG_WARN("AppBackend: unknown camera mode '{}'; falling back to hardware/mock defaults", cameraMode);
#if MIB_HAS_EGRABBER
                captureService_->setCameraFactory([]()
                                                  { return std::make_unique<camera::common::EGrabberCamera>(); });
                mockCameraConfigured_ = false;
                selectedMvCameraIndex_ = -1;
#else
                cameraMode = "mock";
                configureMock();
#endif
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

        SPDLOG_INFO("Backend initialized.");
        return true;
    }

    services::SqliteService &AppBackend::sqlite() { return *sqliteService_; }
    services::Hdf5Service &AppBackend::hdf5() { return *hdf5Service_; }
    services::CaptureService &AppBackend::capture() { return *captureService_; }
    services::ProcessingService &AppBackend::processing() { return *processingService_; }
    services::PlaybackService &AppBackend::playback() { return *playbackService_; }
    services::CameraControlService &AppBackend::cameraControl() { return *cameraControlService_; }
    services::AutofocusService &AppBackend::autofocus() { return *autofocusService_; }
    services::TriggerService &AppBackend::trigger() { return *triggerService_; }
    services::YoloService &AppBackend::yolo() { return *yoloService_; }
    services::SyringePumpService &AppBackend::syringePump() { return *syringePumpService_; }
    services::PulseGeneratorService &AppBackend::pulseGenerator() { return *pulseGeneratorService_; }

    void AppBackend::configureMockCamera(const camera::mock::MockCameraOptions &options)
    {
        if (!captureService_)
            return;
        captureService_->setCameraFactory([options]() mutable
                                          { return std::make_unique<camera::mock::MockCamera>(options); });
        selectedIfIndex_ = -1;
        selectedDevIndex_ = -1;
        selectedMvCameraIndex_ = -1;
        selectedLabel_.clear();
        lastMindVisionConfigPath_.clear();
        mockCameraConfigured_ = true;
    }

    void AppBackend::setHardwareCameraSelection(int interfaceIndex, int deviceIndex, const std::string &label)
    {
        if (!captureService_)
            return;

#if !MIB_HAS_EGRABBER
        SPDLOG_WARN("Hardware camera selection ignored: EGrabber SDK is unavailable on this platform");
        camera::mock::MockCameraOptions options;
        options.folder = std::filesystem::path("data") / "mock_frames";
        captureService_->setCameraFactory([options]() mutable
                                          { return std::make_unique<camera::mock::MockCamera>(options); });
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

        captureService_->setCameraFactory([interfaceIndex, deviceIndex]()
                                          { return std::make_unique<camera::common::EGrabberCamera>(interfaceIndex, deviceIndex); });
        SPDLOG_INFO("Hardware camera selected: {} (if={}, dev={})",
                    label, interfaceIndex, deviceIndex);
    }

    bool AppBackend::setMindVisionCameraSelection(int cameraIndex, const std::string &label,
                                                  std::string *errorOut)
    {
        if (!captureService_)
        {
            if (errorOut)
                *errorOut = "Capture service is not available";
            return false;
        }

#if !MIB_HAS_MINDVISION
        // Never silently fall back to the mock camera here: the user asked for
        // hardware and would otherwise record mock frames without noticing
        // (issue #338). Selection state stays untouched.
        (void)cameraIndex;
        const std::string message =
            "This build does not include MindVision support (compiled with "
            "MIB_ENABLE_MINDVISION=OFF); the camera \"" + label +
            "\" cannot be used. Install a build with the MindVision SDK enabled.";
        SPDLOG_ERROR("AppBackend: {}", message);
        if (errorOut)
            *errorOut = message;
        return false;
#else
        selectedMvCameraIndex_ = cameraIndex;
        selectedIfIndex_ = -1;
        selectedDevIndex_ = -1;
        selectedLabel_ = label;
        mockCameraConfigured_ = false;

        const std::string configPath = lastMindVisionConfigPath_;
        captureService_->setCameraFactory([cameraIndex, configPath]()
                                          { return std::make_unique<camera::common::MindVisionCamera>(cameraIndex, configPath); });

        auto &mirror = backend::diagnostics::CrashStateMirror::instance();
        mirror.app.selectedInterface.store(-1);
        mirror.app.selectedDevice.store(-1);
        mirror.app.mockCamera.store(mockCameraConfigured_);
        mirror.setCameraLabel(selectedLabel_);

        SPDLOG_INFO("MindVision camera selected: {} (index={})", label, cameraIndex);
        return true;
#endif
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
        return cameraControlService_->applyScriptToDevice(selectedIfIndex_, selectedDevIndex_, path, errorOut);
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
            const int idx = selectedMvCameraIndex_;
            const std::string configPath = lastMindVisionConfigPath_;
            captureService_->setCameraFactory([idx, configPath]()
                                              { return std::make_unique<camera::common::MindVisionCamera>(idx, configPath); });
            SPDLOG_INFO("MindVision capture factory updated with config: {}", path);
        }
        return ok;
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

    bool AppBackend::isCameraConfigured() const
    {
        // Camera is configured if a hardware, MindVision, or mock camera is selected.
        return (selectedIfIndex_ >= 0 && selectedDevIndex_ >= 0)
            || (selectedMvCameraIndex_ >= 0)
            || mockCameraConfigured_;
    }

    bool AppBackend::startFrameRecording(const std::string& hdf5FilePath) {
        if (frameRecordingRunning_.load()) {
            SPDLOG_WARN("Frame recording already in progress");
            return false;
        }
        if (!captureService_ || !captureService_->isRunning()) {
            SPDLOG_ERROR("Cannot start frame recording: camera not running");
            return false;
        }
        if (!processingService_ || !processingService_->isProcessingCorePinSatisfied()) {
            SPDLOG_ERROR("Cannot start frame recording: selected processing core is unavailable");
            return false;
        }

        // Open HDF5 file for recording
        auto& hdf5 = *hdf5Service_;
        if (hdf5.isFileOpen()) {
            SPDLOG_WARN("HDF5 file already open, closing before recording");
            hdf5.closeFile();
        }

        std::string path = hdf5FilePath;
        if (path.size() < 3 || (path.substr(path.size() - 3) != ".h5" &&
            (path.size() < 5 || path.substr(path.size() - 5) != ".hdf5"))) {
            path += ".h5";
        }

        if (!hdf5.openFile(path)) {
            SPDLOG_ERROR("Failed to open HDF5 file for recording: {}", path);
            return false;
        }
        if (!hdf5.initializeRecordingDatasets()) {
            hdf5.closeFile();
            return false;
        }
        auto processingCoreLease = processingService_->acquireProcessingCoreOperation();

        frameRecordingPath_ = path;
        frameRecordingWritten_.store(0);
        frameRecordingFiltered_.store(0);
        frameRecordingRunning_.store(true);
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance().recorder;
            m.recording.store(true);
            m.framesWritten.store(0);
            m.framesFiltered.store(0);
        }
        backend::services::CrashReporter::breadcrumb("recording",
            "frame recording started", path);

        // Launch recording thread
        frameRecordingThread_ = std::make_unique<std::thread>(
            [this, processingCoreLease = std::move(processingCoreLease)]() mutable {
            SPDLOG_INFO("Frame recording thread started");

            const uint64_t startTimeNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
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
                    if (!hdf5Service_->appendRecordingFrames(b.images, b.meta)) return false;
                    frameRecordingWritten_.fetch_add(b.images.size(), std::memory_order_relaxed);
                    return true;
                },
                [this](const std::string& msg) {
                    frameRecordingRunning_.store(false);
                    reportFatalSaveError("Recording save failed: " + msg);
                });

            // Hoisted per-poll-batch: refreshed only when configVersion changes.
            // Staleness window is one poll iteration (~ms), which is acceptable
            // and documented. Per-frame refresh was the dominant lock cost (P1).
            uint64_t lastConfigVer = std::numeric_limits<uint64_t>::max();
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

                // Process new frames
                for (uint64_t idx = startIdx; idx <= latestIdx && frameRecordingRunning_.load(); ++idx) {
                    playback::Frame f{};
                    if (!frameStore_->getByWriteIndex(idx, f)) {
                        continue;
                    }
                    if (f.width == 0 || f.height == 0 || f.data.empty()) {
                        continue;
                    }

                    if (processingService_->isFrameEmptyWithActiveKernel(f, config, roi,
                                                                         bgShared)) {
                        frameRecordingFiltered_.fetch_add(1, std::memory_order_relaxed);
                        lastProcessedIdx = idx;
                        continue;
                    }

                    // Convert to cv::Mat, then crop to the preview ROI so the
                    // recording captures exactly the region the user selected
                    // (full frame when no ROI is set). Mirrors the clamp the
                    // processing path applies.
                    const int w = static_cast<int>(f.width);
                    const int h = static_cast<int>(f.height);
                    const size_t step = (f.linePitch == 0 ? static_cast<size_t>(f.width) : f.linePitch);
                    // isFrameEmpty() above already rejects short buffers, but
                    // keep the strided view safe on its own terms.
                    if (f.data.size() < static_cast<size_t>(h - 1) * step + static_cast<size_t>(w)) {
                        frameRecordingFiltered_.fetch_add(1, std::memory_order_relaxed);
                        lastProcessedIdx = idx;
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

                    lastProcessedIdx = idx;

                    // Flush batch when full
                    if (batchImages.size() >= FLUSH_BATCH) {
                        if (!writeQueue.submit(RecordingBatch{std::move(batchImages), std::move(batchMeta)})) {
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
                writeQueue.submit(RecordingBatch{std::move(batchImages), std::move(batchMeta)});
            }
            if (!writeQueue.flushAndStop()) {
                reportFatalSaveError("Recording final flush failed: " + writeQueue.error());
            }

            // Write recording info
            const uint64_t endTimeNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            if (!hdf5Service_->writeRecordingInfo(startTimeNs, endTimeNs,
                                                  frameRecordingWritten_.load(),
                                                  frameRecordingFiltered_.load(),
                                                  recordingMultiImageEnabled,
                                                  recordingMultiImageCount,
                                                  &processingCoreLease.identity())) {
                SPDLOG_ERROR("Frame recording: failed to write recording_info metadata");
                reportFatalSaveError(
                    "Frame recording metadata/processing-core provenance write failed");
            }
            hdf5Service_->closeFile();

            SPDLOG_INFO("Frame recording stopped: {} frames recorded, {} empty filtered, file: {}",
                        frameRecordingWritten_.load(), frameRecordingFiltered_.load(), frameRecordingPath_);
        });

        SPDLOG_INFO("Frame recording started: {}", path);
        return true;
    }

    void AppBackend::stopFrameRecording() {
        if (!frameRecordingRunning_.load() &&
            (!frameRecordingThread_ || !frameRecordingThread_->joinable())) return;

        frameRecordingRunning_.store(false);
        if (frameRecordingThread_ && frameRecordingThread_->joinable()) {
            frameRecordingThread_->join();
        }
        frameRecordingThread_.reset();
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
        return frameRecordingRunning_.load();
    }

    uint64_t AppBackend::frameRecordingCount() const {
        return frameRecordingWritten_.load();
    }

    uint64_t AppBackend::frameRecordingFiltered() const {
        return frameRecordingFiltered_.load();
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
        std::lock_guard<std::mutex> lk(configJsonMutex_);
        lastConfigJson_ = json;
    }

    std::string AppBackend::getLastConfigJson() const {
        std::lock_guard<std::mutex> lk(configJsonMutex_);
        return lastConfigJson_;
    }

} // namespace backend
