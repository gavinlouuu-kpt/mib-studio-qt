// Injectable seam over the subset of the MindVision (MVSDK) C API that
// MindVisionCamera uses (issues #365/#366).
//
// Every SDK entry point the camera lifecycle, frame retrieval, format
// configuration, and conversion path touch goes through this table, so:
//   - tests inject a scripted fake (format-set failure, RGB readback, bad
//     geometry, mid-session geometry change, delayed grab) without hardware;
//   - the camera class itself compiles in every build, including builds
//     without the SDK (`MIB_HAS_MINDVISION=0`), where the default table
//     reports "unavailable" for every call.
//
// The types here deliberately mirror only the SDK fields we consume; they are
// SDK-header-free so tests and the Qt-free backend never include CameraApi.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace backend::camera::mindvision {

struct Config; // MindVisionConfig.h

// Mirrors the SDK's CameraSdkStatus: 0 == success, negative == error.
using SdkStatus = int;
constexpr SdkStatus kSdkSuccess = 0;
// CAMERA_STATUS_TIME_OUT in CameraStatus.h.
constexpr SdkStatus kSdkTimeout = -12;
// Sentinel for "the SDK is not compiled in / function unavailable".
constexpr SdkStatus kSdkUnavailable = -10001;

// CAMERA_MEDIA_TYPE_* constants we reason about (CameraDefine.h). Values are
// the SDK's; other formats are simply "not Mono8" for our purposes.
constexpr std::uint32_t kMediaTypeMono8 = 0x01080001u;

// Bits of tSdkFrameHead needed for validation + conversion.
struct SdkFrameInfo {
    std::uint32_t mediaType{0}; // raw sensor format of this frame (uiMediaType)
    std::uint32_t bytes{0};     // uBytes: payload size the SDK reports
    int width{0};               // iWidth
    int height{0};              // iHeight
    std::uint32_t timeStamp{0}; // uiTimeStamp: 0.1 ms device ticks
    std::uint32_t exposureUs{0};// uiExpTime
    bool isTrigger{false};      // bIsTrigger
};

struct SdkCapability {
    bool monoSensor{false};
};

struct SdkFrameStatistic {
    int total{0};
    int captured{0};
    int lost{0};
};

// One function per SDK call. `handle` is CameraHandle (int in the MVSDK).
// All are std::function so a test fake can capture state; production binds
// the real symbols once (see realMindVisionSdk()).
struct SdkOps {
    std::function<SdkStatus()> sdkInit;                        // CameraSdkInit(0)
    std::function<SdkStatus(int& count)> enumerate;            // CameraEnumerateDevice
    std::function<SdkStatus(int index, int& handle)> init;     // CameraInit(&dev[index])
    std::function<SdkStatus(int handle, SdkCapability&)> getCapability;
    std::function<bool(int handle, const Config&)> applyConfig; // applyConfigToHandle
    std::function<SdkStatus(int handle, int& width, int& height)> getImageResolution;
    std::function<SdkStatus(int handle, std::uint32_t format)> setIspOutFormat;
    std::function<SdkStatus(int handle, std::uint32_t& format)> getIspOutFormat;
    std::function<std::uint8_t*(std::size_t bytes, int align)> alignMalloc;
    std::function<void(std::uint8_t*)> alignFree;
    std::function<SdkStatus(int handle)> play;
    std::function<SdkStatus(int handle)> stop;
    std::function<SdkStatus(int handle)> unInit;
    // Ordered retrieval (CameraGetImageBuffer).
    std::function<SdkStatus(int handle, SdkFrameInfo&, std::uint8_t*& buffer, unsigned timeoutMs)>
        getImageBuffer;
    // Newest-frame retrieval (CameraGetImageBufferPriority, NEWEST). May be
    // empty when the SDK variant lacks it -> drain fallback is used.
    std::function<SdkStatus(int handle, SdkFrameInfo&, std::uint8_t*& buffer, unsigned timeoutMs)>
        getImageBufferNewest;
    std::function<SdkStatus(int handle, std::uint8_t* buffer)> releaseImageBuffer;
    // CameraImageProcess(handle, in, out, &head). `out` must hold the
    // validated destination size; the seam never resizes it.
    std::function<SdkStatus(int handle, std::uint8_t* in, std::uint8_t* out, SdkFrameInfo&)>
        imageProcess;
    std::function<SdkStatus(int handle, SdkFrameStatistic&)> getFrameStatistic;
    std::function<SdkStatus(int handle, int ioIndex, int mode)> setOutputIoMode;
    std::function<SdkStatus(int handle, int ioIndex, unsigned state)> setIoStateEx;
    std::function<SdkStatus(int handle)> softTrigger;
};

// The production table bound to the real SDK. When the SDK is not compiled
// in, every call returns kSdkUnavailable (and start() fails closed with a
// structured "sdk_unavailable" failure). Safe to call repeatedly; returns a
// shared immutable table.
std::shared_ptr<const SdkOps> realMindVisionSdk();

// True when the real SDK is linked into this build.
bool mindVisionSdkAvailable();

} // namespace backend::camera::mindvision
