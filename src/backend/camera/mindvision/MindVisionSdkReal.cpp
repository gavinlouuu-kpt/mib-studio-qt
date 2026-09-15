// Production binding of the MindVision SDK seam (MindVisionSdk.h) to the real
// MVSDK C API. This is the only translation unit (besides MindVisionApply.cpp)
// that includes the vendor headers; on Windows it also owns the dynamic-loader
// function table via API_LOAD_MAIN.
#include "backend/camera/mindvision/MindVisionSdk.h"
#include "backend/camera/mindvision/MindVisionApply.h"
#include "backend/camera/mindvision/MindVisionConfig.h"

#ifndef MIB_HAS_MINDVISION
#define MIB_HAS_MINDVISION 0
#endif

#include <spdlog/spdlog.h>

#if MIB_HAS_MINDVISION

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#endif

#ifdef _WIN32
#define API_LOAD_MAIN
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

// Compile-time detection of the newest-frame priority retrieval API
// (CameraGetImageBufferPriority + CAMERA_GET_IMAGE_PRIORITY_NEWEST). SDK
// variants that expose the priority constant as a preprocessor #define are
// detected automatically; the pinned R2 SDKs opt in with
// -DMIB_MINDVISION_USE_PRIORITY_API=1 (see src/backend/CMakeLists.txt).
#ifndef MIB_MINDVISION_USE_PRIORITY_API
#define MIB_MINDVISION_USE_PRIORITY_API 0
#endif
#if defined(CAMERA_GET_IMAGE_PRIORITY_NEWEST) || MIB_MINDVISION_USE_PRIORITY_API
#define MIB_MINDVISION_HAS_PRIORITY_NEWEST 1
#else
#define MIB_MINDVISION_HAS_PRIORITY_NEWEST 0
#endif

namespace backend::camera::mindvision {

namespace {

// Device list from the last enumeration; CameraInit needs the tSdkCameraDevInfo
// record, so the seam keeps it here instead of exposing the vendor type.
tSdkCameraDevInfo g_devList[32];
int g_devCount = 0;

void fromHead(const tSdkFrameHead& head, SdkFrameInfo& out)
{
    out.mediaType = head.uiMediaType;
    out.bytes = head.uBytes;
    out.width = head.iWidth;
    out.height = head.iHeight;
    out.timeStamp = head.uiTimeStamp;
    out.exposureUs = head.uiExpTime;
    out.isTrigger = head.bIsTrigger != 0;
}

void toHead(const SdkFrameInfo& in, tSdkFrameHead& head)
{
    head.uiMediaType = in.mediaType;
    head.uBytes = in.bytes;
    head.iWidth = in.width;
    head.iHeight = in.height;
    head.uiTimeStamp = in.timeStamp;
    head.uiExpTime = in.exposureUs;
    head.bIsTrigger = in.isTrigger ? 1 : 0;
}

std::shared_ptr<const SdkOps> buildRealOps()
{
    auto ops = std::make_shared<SdkOps>();
    ops->sdkInit = []() -> SdkStatus {
#ifdef _WIN32
        if (LoadSdkApi() != CAMERA_STATUS_SUCCESS) {
            SPDLOG_ERROR("MindVisionCamera: failed to load MVCAMSDK DLL");
            return kSdkUnavailable;
        }
#endif
        return CameraSdkInit(0);
    };
    ops->enumerate = [](int& count) -> SdkStatus {
        INT n = 32;
        const CameraSdkStatus st = CameraEnumerateDevice(g_devList, &n);
        g_devCount = (st == CAMERA_STATUS_SUCCESS) ? n : 0;
        count = g_devCount;
        return st;
    };
    ops->init = [](int index, int& handle) -> SdkStatus {
        if (index < 0 || index >= g_devCount) return kSdkUnavailable;
        CameraHandle h = -1;
        const CameraSdkStatus st = CameraInit(&g_devList[index], -1, -1, &h);
        handle = (st == CAMERA_STATUS_SUCCESS) ? h : -1;
        return st;
    };
    ops->getCapability = [](int handle, SdkCapability& cap) -> SdkStatus {
        tSdkCameraCapbility c{};
        const CameraSdkStatus st = CameraGetCapability(handle, &c);
        if (st == CAMERA_STATUS_SUCCESS) cap.monoSensor = c.sIspCapacity.bMonoSensor != 0;
        return st;
    };
    ops->applyConfig = [](int handle, const Config& cfg) -> bool {
        return applyConfigToHandle(handle, cfg, nullptr);
    };
    ops->getImageResolution = [](int handle, int& w, int& h) -> SdkStatus {
        tSdkImageResolution res{};
        const CameraSdkStatus st = CameraGetImageResolution(handle, &res);
        if (st == CAMERA_STATUS_SUCCESS) { w = res.iWidth; h = res.iHeight; }
        return st;
    };
    ops->setIspOutFormat = [](int handle, std::uint32_t fmt) -> SdkStatus {
        return CameraSetIspOutFormat(handle, fmt);
    };
    ops->getIspOutFormat = [](int handle, std::uint32_t& fmt) -> SdkStatus {
        UINT f = 0;
        const CameraSdkStatus st = CameraGetIspOutFormat(handle, &f);
        if (st == CAMERA_STATUS_SUCCESS) fmt = f;
        return st;
    };
    ops->alignMalloc = [](std::size_t bytes, int align) -> std::uint8_t* {
        return CameraAlignMalloc(static_cast<int>(bytes), align);
    };
    ops->alignFree = [](std::uint8_t* p) { CameraAlignFree(p); };
    ops->play = [](int handle) -> SdkStatus { return CameraPlay(handle); };
    ops->stop = [](int handle) -> SdkStatus { return CameraStop(handle); };
    ops->unInit = [](int handle) -> SdkStatus { return CameraUnInit(handle); };
    ops->getImageBuffer = [](int handle, SdkFrameInfo& info, std::uint8_t*& buffer,
                             unsigned timeoutMs) -> SdkStatus {
        tSdkFrameHead head{};
        BYTE* p = nullptr;
        const CameraSdkStatus st = CameraGetImageBuffer(handle, &head, &p, timeoutMs);
        if (st == CAMERA_STATUS_SUCCESS) { fromHead(head, info); buffer = p; }
        return st;
    };
#if MIB_MINDVISION_HAS_PRIORITY_NEWEST
    ops->getImageBufferNewest = [](int handle, SdkFrameInfo& info, std::uint8_t*& buffer,
                                   unsigned timeoutMs) -> SdkStatus {
        tSdkFrameHead head{};
        BYTE* p = nullptr;
        const CameraSdkStatus st = CameraGetImageBufferPriority(
            handle, &head, &p, timeoutMs, CAMERA_GET_IMAGE_PRIORITY_NEWEST);
        if (st == CAMERA_STATUS_SUCCESS) { fromHead(head, info); buffer = p; }
        return st;
    };
#endif
    ops->releaseImageBuffer = [](int handle, std::uint8_t* buffer) -> SdkStatus {
        return CameraReleaseImageBuffer(handle, buffer);
    };
    ops->imageProcess = [](int handle, std::uint8_t* in, std::uint8_t* out,
                           SdkFrameInfo& info) -> SdkStatus {
        tSdkFrameHead head{};
        toHead(info, head);
        const CameraSdkStatus st = CameraImageProcess(handle, in, out, &head);
        fromHead(head, info);
        return st;
    };
    ops->getFrameStatistic = [](int handle, SdkFrameStatistic& s) -> SdkStatus {
        tSdkFrameStatistic fs{};
        const CameraSdkStatus st = CameraGetFrameStatistic(handle, &fs);
        if (st == CAMERA_STATUS_SUCCESS) {
            s.total = fs.iTotal; s.captured = fs.iCapture; s.lost = fs.iLost;
        }
        return st;
    };
    ops->setOutputIoMode = [](int handle, int io, int mode) -> SdkStatus {
        return CameraSetOutPutIOMode(handle, io, mode);
    };
    ops->setIoStateEx = [](int handle, int io, unsigned state) -> SdkStatus {
        return CameraSetIOStateEx(handle, io, state);
    };
    ops->softTrigger = [](int handle) -> SdkStatus { return CameraSoftTrigger(handle); };
    return ops;
}

} // namespace

std::shared_ptr<const SdkOps> realMindVisionSdk()
{
    static std::shared_ptr<const SdkOps> ops = buildRealOps();
    return ops;
}

bool mindVisionSdkAvailable() { return true; }

} // namespace backend::camera::mindvision

#else // !MIB_HAS_MINDVISION

namespace backend::camera::mindvision {

namespace {
std::shared_ptr<const SdkOps> buildUnavailableOps()
{
    // Every entry fails closed; start() reports "mindvision.sdk_unavailable".
    auto ops = std::make_shared<SdkOps>();
    ops->sdkInit = []() -> SdkStatus {
        SPDLOG_WARN("MindVisionCamera: MindVision SDK disabled at build time");
        return kSdkUnavailable;
    };
    ops->enumerate = [](int& count) -> SdkStatus { count = 0; return kSdkUnavailable; };
    ops->init = [](int, int& handle) -> SdkStatus { handle = -1; return kSdkUnavailable; };
    ops->getCapability = [](int, SdkCapability&) -> SdkStatus { return kSdkUnavailable; };
    ops->applyConfig = [](int, const Config&) -> bool { return false; };
    ops->getImageResolution = [](int, int&, int&) -> SdkStatus { return kSdkUnavailable; };
    ops->setIspOutFormat = [](int, std::uint32_t) -> SdkStatus { return kSdkUnavailable; };
    ops->getIspOutFormat = [](int, std::uint32_t&) -> SdkStatus { return kSdkUnavailable; };
    ops->alignMalloc = [](std::size_t, int) -> std::uint8_t* { return nullptr; };
    ops->alignFree = [](std::uint8_t*) {};
    ops->play = [](int) -> SdkStatus { return kSdkUnavailable; };
    ops->stop = [](int) -> SdkStatus { return kSdkUnavailable; };
    ops->unInit = [](int) -> SdkStatus { return kSdkUnavailable; };
    ops->getImageBuffer = [](int, SdkFrameInfo&, std::uint8_t*&, unsigned) -> SdkStatus {
        return kSdkUnavailable;
    };
    ops->releaseImageBuffer = [](int, std::uint8_t*) -> SdkStatus { return kSdkUnavailable; };
    ops->imageProcess = [](int, std::uint8_t*, std::uint8_t*, SdkFrameInfo&) -> SdkStatus {
        return kSdkUnavailable;
    };
    ops->getFrameStatistic = [](int, SdkFrameStatistic&) -> SdkStatus { return kSdkUnavailable; };
    ops->setOutputIoMode = [](int, int, int) -> SdkStatus { return kSdkUnavailable; };
    ops->setIoStateEx = [](int, int, unsigned) -> SdkStatus { return kSdkUnavailable; };
    ops->softTrigger = [](int) -> SdkStatus { return kSdkUnavailable; };
    return ops;
}
} // namespace

std::shared_ptr<const SdkOps> realMindVisionSdk()
{
    static std::shared_ptr<const SdkOps> ops = buildUnavailableOps();
    return ops;
}

bool mindVisionSdkAvailable() { return false; }

} // namespace backend::camera::mindvision

#endif
