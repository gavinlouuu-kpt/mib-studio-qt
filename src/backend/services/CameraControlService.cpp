#include "backend/services/CameraControlService.h"

#include <spdlog/spdlog.h>

#include <sstream>
#include <string>

#ifndef MIB_HAS_EGRABBER
#define MIB_HAS_EGRABBER 0
#endif
#ifndef MIB_HAS_MINDVISION
#define MIB_HAS_MINDVISION 0
#endif

#if MIB_HAS_EGRABBER
#include <EGrabber.h>
using namespace Euresys;
#endif

#if MIB_HAS_MINDVISION
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#endif

#ifdef _WIN32
#if __has_include(<MindVision/CameraApiLoad.h>)
#include <MindVision/CameraApiLoad.h>
#elif __has_include(<CameraApiLoad.h>)
#include <CameraApiLoad.h>
#else
#error "MindVision CameraApiLoad.h not found"
#endif
#else
#if __has_include(<MindVision/CameraApi.h>)
#include <MindVision/CameraApi.h>
#elif __has_include(<CameraApi.h>)
#include <CameraApi.h>
#else
#error "MindVision CameraApi.h not found"
#endif
#endif

#include "backend/camera/mindvision/MindVisionApply.h"
#include "backend/camera/mindvision/MindVisionConfig.h"

#include <fstream>
#include <iterator>
#include <cstdlib>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#endif

namespace backend::services
{
    namespace
    {
        bool reportUnsupported(const char *operation, std::string *errorOut, const char *message)
        {
            static bool loggedOnce = false;
            if (!loggedOnce)
            {
                SPDLOG_WARN("CameraControlService hardware operations unavailable: {}", message);
                loggedOnce = true;
            }
            if (errorOut)
            {
                *errorOut = message;
            }
            (void)operation;
            return false;
        }

#if MIB_HAS_MINDVISION
        std::string buildMindVisionLabel(const tSdkCameraDevInfo &info, int index)
        {
            std::ostringstream oss;
            if (info.acFriendlyName[0] != '\0')
            {
                oss << info.acFriendlyName;
            }
            else if (info.acProductName[0] != '\0')
            {
                oss << info.acProductName;
            }
            else
            {
                oss << "MindVision " << index;
            }

            if (info.acSn[0] != '\0')
            {
                oss << " [" << info.acSn << "]";
            }
            return oss.str();
        }

        bool applyMindVisionJsonToCamera(CameraHandle hCamera,
                                         const std::string &jsonPath,
                                         std::string *errorOut)
        {
            auto setErr = [&](const std::string &message)
            {
                if (errorOut && errorOut->empty())
                {
                    *errorOut = message;
                }
            };

            std::ifstream file(jsonPath, std::ios::binary);
            if (!file)
            {
                setErr("Failed to open MindVision config file: " + jsonPath);
                return false;
            }

            const std::string bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());

            const auto parsed = backend::camera::mindvision::parseConfig(bytes);
            if (!parsed.ok)
            {
                setErr(parsed.error);
                return false;
            }
            for (const auto& warning : parsed.warnings)
            {
                SPDLOG_WARN("{}", warning);
            }

            // Shared with MindVisionCamera::applyJsonConfig so the two apply
            // paths stay in lockstep (this path historically applied only 7
            // of the config's fields — no strobe, no trigger extras).
            std::string applyError;
            backend::camera::mindvision::applyConfigToHandle(hCamera, parsed.config, &applyError);
            if (!applyError.empty())
            {
                setErr(applyError);
            }

            return true;
        }
#endif
    } // namespace

#if MIB_HAS_EGRABBER

    std::vector<DiscoveredCamera> CameraControlService::discoverCameras()
    {
        std::vector<DiscoveredCamera> results;
        try
        {
            EGenTL genTL;
            EGrabberDiscovery discovery(genTL);
            discovery.discover();

            for (int i = 0; i < discovery.cameraCount(); ++i)
            {
                EGrabberCameraInfo cameraInfo = discovery.cameras(i);
                if (cameraInfo.grabbers.empty())
                {
                    continue;
                }

                const EGrabberInfo &grabberInfo = cameraInfo.grabbers[0];
                const int interfaceIndex = grabberInfo.interfaceIndex;
                const int deviceIndex = grabberInfo.deviceIndex;

                std::string modelName = grabberInfo.isRemoteAvailable ? grabberInfo.deviceModelName : "Unknown";
                std::string firmwareVersion = "Unknown";

                try
                {
                    EGrabber<CallbackOnDemand> probe(genTL, interfaceIndex, deviceIndex);
                    try
                    {
                        if (modelName == "Unknown" || modelName.empty())
                        {
                            modelName = probe.getString<DeviceModule>("DeviceModelName");
                        }
                    }
                    catch (const gentl_error &)
                    {
                    }
                    try
                    {
                        firmwareVersion = probe.getString<DeviceModule>("DeviceVersion");
                    }
                    catch (const gentl_error &)
                    {
                    }
                }
                catch (const std::exception &ex)
                {
                    SPDLOG_WARN("CameraControlService: probe open failed for camera {}/{}: {}",
                                grabberInfo.interfaceID, grabberInfo.deviceID, ex.what());
                }

                DiscoveredCamera dc{};
                dc.cameraType = CameraType::EGrabber;
                dc.cameraIndex = -1;
                dc.interfaceIndex = interfaceIndex;
                dc.deviceIndex = deviceIndex;
                dc.interfaceID = grabberInfo.interfaceID;
                dc.deviceID = grabberInfo.deviceID;
                dc.modelName = modelName;
                dc.firmwareVersion = firmwareVersion;
                std::ostringstream oss;
                oss << grabberInfo.interfaceID << "/" << grabberInfo.deviceID;
                if (!modelName.empty() && modelName != "Unknown")
                {
                    oss << " (" << modelName << ")";
                }
                if (!firmwareVersion.empty() && firmwareVersion != "Unknown")
                {
                    oss << " [Firmware: " << firmwareVersion << "]";
                }
                dc.label = oss.str();
                results.push_back(std::move(dc));
            }

            SPDLOG_INFO("Discovery found {} camera(s)", results.size());
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("CameraControlService::discoverCameras failed: {}", ex.what());
            results.clear();
        }
        return results;
    }

    std::vector<DiscoveredFramegrabber> CameraControlService::discoverFramegrabbers()
    {
        std::vector<DiscoveredFramegrabber> results;
        try
        {
            EGenTL genTL;
            EGrabberDiscovery discovery(genTL);
            discovery.discover();

            for (int i = 0; i < discovery.egrabberCount(); ++i)
            {
                EGrabberInfo grabberInfo = discovery.egrabbers(i);
                const int interfaceIndex = grabberInfo.interfaceIndex;
                const int deviceIndex = grabberInfo.deviceIndex;
                const int streamIndex = grabberInfo.streamIndex;

                std::string modelName = grabberInfo.isRemoteAvailable ? grabberInfo.deviceModelName : "Unknown";

                try
                {
                    EGrabber<CallbackOnDemand> probe(genTL, interfaceIndex, deviceIndex);
                    try
                    {
                        if (modelName == "Unknown" || modelName.empty())
                        {
                            modelName = probe.getString<DeviceModule>("DeviceModelName");
                        }
                    }
                    catch (const gentl_error &)
                    {
                    }
                }
                catch (const std::exception &ex)
                {
                    SPDLOG_WARN("CameraControlService: probe open failed for framegrabber {}/{}/{}: {}",
                                grabberInfo.interfaceID, grabberInfo.deviceID, grabberInfo.streamID, ex.what());
                }

                DiscoveredFramegrabber df{};
                df.interfaceIndex = interfaceIndex;
                df.deviceIndex = deviceIndex;
                df.streamIndex = streamIndex;
                df.interfaceID = grabberInfo.interfaceID;
                df.deviceID = grabberInfo.deviceID;
                df.streamID = grabberInfo.streamID;
                df.modelName = modelName;
                std::ostringstream oss;
                oss << grabberInfo.interfaceID << "/" << grabberInfo.deviceID << "/" << grabberInfo.streamID;
                if (!modelName.empty() && modelName != "Unknown")
                {
                    oss << " (" << modelName << ")";
                }
                df.label = oss.str();
                results.push_back(std::move(df));
            }

            SPDLOG_INFO("Discovery found {} framegrabber(s)", results.size());
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("CameraControlService::discoverFramegrabbers failed: {}", ex.what());
            results.clear();
        }
        return results;
    }

    bool CameraControlService::applyScriptToDevice(int interfaceIndex,
                                                   int deviceIndex,
                                                   const std::string &scriptPath,
                                                   std::string *errorOut)
    {
        try
        {
            EGenTL genTL;
            EGrabber<CallbackOnDemand> g(genTL, interfaceIndex, deviceIndex);

            SPDLOG_INFO("Applying script to camera [{}:{}]: {}", interfaceIndex, deviceIndex, scriptPath);
            g.runScript(scriptPath);

            try
            {
                g.execute<RemoteModule>("AcquisitionStop");
            }
            catch (const std::exception &ex)
            {
                SPDLOG_WARN("AcquisitionStop after script failed: {}", ex.what());
            }
            SPDLOG_INFO("Script applied successfully");
            return true;
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("applyScriptToDevice failed: {}", ex.what());
            if (errorOut)
            {
                *errorOut = ex.what();
            }
            return false;
        }
    }

    bool CameraControlService::deviceReset(int interfaceIndex,
                                           int deviceIndex,
                                           std::string *errorOut)
    {
        try
        {
            EGenTL genTL;
            EGrabber<CallbackOnDemand> g(genTL, interfaceIndex, deviceIndex);

            try
            {
                g.execute<RemoteModule>("AcquisitionStop");
            }
            catch (const std::exception &ex)
            {
                SPDLOG_WARN("AcquisitionStop before reset failed: {}", ex.what());
            }

            SPDLOG_INFO("Issuing DeviceReset to camera [{}:{}]", interfaceIndex, deviceIndex);
            g.execute<DeviceModule>("DeviceReset");
            SPDLOG_INFO("DeviceReset command sent");
            return true;
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("deviceReset failed: {}", ex.what());
            if (errorOut)
            {
                *errorOut = ex.what();
            }
            return false;
        }
    }

#else

    std::vector<DiscoveredCamera> CameraControlService::discoverCameras()
    {
        reportUnsupported("discoverCameras", nullptr, "EGrabber SDK is unavailable on this platform");
        return {};
    }

    std::vector<DiscoveredFramegrabber> CameraControlService::discoverFramegrabbers()
    {
        reportUnsupported("discoverFramegrabbers", nullptr, "EGrabber SDK is unavailable on this platform");
        return {};
    }

    bool CameraControlService::applyScriptToDevice(int interfaceIndex,
                                                   int deviceIndex,
                                                   const std::string &scriptPath,
                                                   std::string *errorOut)
    {
        (void)interfaceIndex;
        (void)deviceIndex;
        (void)scriptPath;
        return reportUnsupported("applyScriptToDevice", errorOut, "EGrabber SDK is unavailable on this platform");
    }

    bool CameraControlService::deviceReset(int interfaceIndex,
                                           int deviceIndex,
                                           std::string *errorOut)
    {
        (void)interfaceIndex;
        (void)deviceIndex;
        return reportUnsupported("deviceReset", errorOut, "EGrabber SDK is unavailable on this platform");
    }

#endif

#if MIB_HAS_MINDVISION

    std::vector<DiscoveredCamera> CameraControlService::discoverMindVisionCameras()
    {
        std::vector<DiscoveredCamera> results;
#if MIB_HAS_EGRABBER
        // The MindVision SDK's CameraEnumerateDevice() leaves the Euresys GenTL
        // producer unopenable for the rest of the process: every later EGenTL
        // construction fails with GC_ERR_RESOURCE_IN_USE (GenTL -1004), the
        // order of the two SDKs does not matter, and no MindVision call undoes
        // it (CameraSdkInit alone is harmless). Verified on a Coaxlink Quad
        // CXP-12 with MindVision SDK 2.1.10 and eGrabber 25.10; regression
        // guard: tests/hardware/hw_discovery_reentry_test.cpp. So a process
        // that has an EGrabber framegrabber never enumerates MindVision
        // devices. Decided once per process (PCIe grabbers do not hot-plug).
        static const bool blockedByEGrabber = [this]()
        {
            const char *force = std::getenv("MIB_MINDVISION_ENUMERATE_WITH_EGRABBER");
            if (force != nullptr && *force == '1')
            {
                SPDLOG_WARN("CameraControlService: MIB_MINDVISION_ENUMERATE_WITH_EGRABBER=1 - "
                            "MindVision enumeration allowed alongside EGrabber; the EGrabber "
                            "camera will be unusable afterwards in this process");
                return false;
            }
            return !discoverFramegrabbers().empty();
        }();
        if (blockedByEGrabber)
        {
            SPDLOG_INFO("CameraControlService: MindVision enumeration skipped - an EGrabber "
                        "framegrabber is present and the MindVision SDK would lock it out "
                        "(set MIB_MINDVISION_ENUMERATE_WITH_EGRABBER=1 to override)");
            return results;
        }
#endif
        try
        {
#ifdef _WIN32
            if (LoadSdkApi() != CAMERA_STATUS_SUCCESS)
            {
                SPDLOG_WARN("CameraControlService::discoverMindVisionCameras: SDK DLL not available");
                return results;
            }
#endif

            CameraSdkStatus status = CameraSdkInit(0);
            if (status != CAMERA_STATUS_SUCCESS)
            {
                SPDLOG_WARN("CameraControlService: CameraSdkInit returned {}", status);
            }

            tSdkCameraDevInfo devList[32];
            INT count = 32;
            status = CameraEnumerateDevice(devList, &count);
            if (status != CAMERA_STATUS_SUCCESS || count <= 0)
            {
                SPDLOG_WARN("CameraControlService: CameraEnumerateDevice returned {} (count={})", status, count);
                return results;
            }

            for (int i = 0; i < count; ++i)
            {
                const tSdkCameraDevInfo &info = devList[i];

                DiscoveredCamera dc{};
                dc.cameraType = CameraType::MindVision;
                dc.cameraIndex = i;
                dc.interfaceIndex = -1;
                dc.deviceIndex = -1;
                dc.modelName = info.acProductName[0] != '\0' ? info.acProductName : "Unknown";
                dc.label = buildMindVisionLabel(info, i);
                results.push_back(std::move(dc));
            }

            SPDLOG_INFO("MindVision discovery found {} camera(s)", results.size());
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("CameraControlService::discoverMindVisionCameras failed: {}", ex.what());
            results.clear();
        }
        return results;
    }

    bool CameraControlService::applyMindVisionConfig(int cameraIndex,
                                                     const std::string &jsonPath,
                                                     std::string *errorOut)
    {
        auto setErr = [&](const std::string &message)
        {
            if (errorOut && errorOut->empty())
            {
                *errorOut = message;
            }
        };

        try
        {
#ifdef _WIN32
            if (LoadSdkApi() != CAMERA_STATUS_SUCCESS)
            {
                setErr("MindVision SDK DLL not available");
                return false;
            }
#endif

            CameraSdkStatus status = CameraSdkInit(0);
            if (status != CAMERA_STATUS_SUCCESS)
            {
                SPDLOG_WARN("CameraControlService: CameraSdkInit returned {}", status);
            }

            tSdkCameraDevInfo devList[32];
            INT count = 32;
            status = CameraEnumerateDevice(devList, &count);
            if (status != CAMERA_STATUS_SUCCESS || count <= 0)
            {
                setErr("CameraEnumerateDevice failed (status=" + std::to_string(status) + ", count=" + std::to_string(count) + ")");
                return false;
            }
            if (cameraIndex < 0 || cameraIndex >= count)
            {
                setErr("MindVision camera index out of range");
                return false;
            }

            CameraHandle hCamera = -1;
            status = CameraInit(&devList[cameraIndex], -1, -1, &hCamera);
            if (status != CAMERA_STATUS_SUCCESS)
            {
                setErr("CameraInit failed (status=" + std::to_string(status) + ")");
                return false;
            }

            const bool ok = applyMindVisionJsonToCamera(hCamera, jsonPath, errorOut);
            CameraUnInit(hCamera);
            if (ok)
            {
                SPDLOG_INFO("CameraControlService: MindVision config applied to camera index {} from {}", cameraIndex, jsonPath);
            }
            else
            {
                SPDLOG_ERROR("CameraControlService: MindVision config application failed for camera index {}", cameraIndex);
            }
            return ok;
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("applyMindVisionConfig failed: {}", ex.what());
            if (errorOut)
            {
                *errorOut = ex.what();
            }
            return false;
        }
    }

#else

    std::vector<DiscoveredCamera> CameraControlService::discoverMindVisionCameras()
    {
        reportUnsupported("discoverMindVisionCameras", nullptr, "MindVision SDK is disabled at build time");
        return {};
    }

    bool CameraControlService::applyMindVisionConfig(int cameraIndex,
                                                     const std::string &jsonPath,
                                                     std::string *errorOut)
    {
        (void)cameraIndex;
        (void)jsonPath;
        return reportUnsupported("applyMindVisionConfig", errorOut, "MindVision SDK is disabled at build time");
    }

#endif

    std::vector<DiscoveredCamera> CameraControlService::discoverAllCameras()
    {
        std::vector<DiscoveredCamera> results = discoverCameras();
        const auto mindVision = discoverMindVisionCameras();
        results.insert(results.end(), mindVision.begin(), mindVision.end());
        return results;
    }
} // namespace backend::services
