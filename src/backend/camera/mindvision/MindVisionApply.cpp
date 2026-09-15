#include "backend/camera/mindvision/MindVisionApply.h"

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
// NOTE: no API_LOAD_MAIN here — MindVisionCamera.cpp owns the Windows
// function-pointer table; this translation unit only gets extern declarations.
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

namespace backend::camera::mindvision {

bool applyConfigToHandle(int hCamera, const Config& cfg, std::string* firstError)
{
    SPDLOG_INFO("MindVision config: applying w={} h={} ox={} oy={} exp={} trig={} gain={}",
                cfg.width, cfg.height, cfg.offsetX, cfg.offsetY,
                cfg.exposureUs, cfg.triggerMode, cfg.analogGain);

    tSdkImageResolution res{};
    res.iIndex = 0xFF;
    res.iHOffsetFOV = cfg.offsetX;
    res.iVOffsetFOV = cfg.offsetY;
    res.iWidthFOV = cfg.width;
    res.iHeightFOV = cfg.height;
    res.iWidth = cfg.width;
    res.iHeight = cfg.height;

    CameraSdkStatus status = CameraSetImageResolution(hCamera, &res);
    if (status != CAMERA_STATUS_SUCCESS)
    {
        SPDLOG_WARN("MindVision config: CameraSetImageResolution returned {}", status);
        if (firstError && firstError->empty())
        {
            *firstError = "CameraSetImageResolution failed (status=" + std::to_string(status) + ")";
        }
    }

    auto warnOnFail = [](const char* what, CameraSdkStatus st)
    {
        if (st != CAMERA_STATUS_SUCCESS)
        {
            SPDLOG_WARN("MindVision config: {} returned {}", what, st);
        }
    };

    warnOnFail("CameraSetExposureTime", CameraSetExposureTime(hCamera, cfg.exposureUs));
    warnOnFail("CameraSetTriggerMode", CameraSetTriggerMode(hCamera, cfg.triggerMode));

    // Acquisition-trigger extras. Applied unconditionally (harmless in
    // free-run mode 0) so switching trigger_mode never needs a re-apply of
    // other fields to pick them up.
    warnOnFail("CameraSetExtTrigSignalType",
               CameraSetExtTrigSignalType(hCamera, cfg.extTrigSignalType));
    warnOnFail("CameraSetExtTrigJitterTime",
               CameraSetExtTrigJitterTime(hCamera, static_cast<UINT>(cfg.extTrigJitterUs)));
    warnOnFail("CameraSetTriggerDelayTime",
               CameraSetTriggerDelayTime(hCamera, static_cast<UINT>(cfg.acqTriggerDelayUs)));
    warnOnFail("CameraSetTriggerCount", CameraSetTriggerCount(hCamera, cfg.triggerCount));

    warnOnFail("CameraSetAnalogGain", CameraSetAnalogGain(hCamera, cfg.analogGain));
    warnOnFail("CameraSetAeState", CameraSetAeState(hCamera, cfg.aeEnabled ? TRUE : FALSE));
    warnOnFail("CameraSetAeTarget", CameraSetAeTarget(hCamera, cfg.aeTarget));
    warnOnFail("CameraSetGamma", CameraSetGamma(hCamera, cfg.gamma));
    warnOnFail("CameraSetContrast", CameraSetContrast(hCamera, cfg.contrast));
    warnOnFail("CameraSetSharpness", CameraSetSharpness(hCamera, cfg.sharpness));
    warnOnFail("CameraSetFrameSpeed", CameraSetFrameSpeed(hCamera, cfg.frameSpeed));
    warnOnFail("CameraSetMirror(H)", CameraSetMirror(hCamera, 0, cfg.flipHorizontal ? TRUE : FALSE));
    warnOnFail("CameraSetMirror(V)", CameraSetMirror(hCamera, 1, cfg.flipVertical ? TRUE : FALSE));

    warnOnFail("CameraSetStrobeMode", CameraSetStrobeMode(hCamera, cfg.strobeMode));
    warnOnFail("CameraSetStrobePulseWidth",
               CameraSetStrobePulseWidth(hCamera, static_cast<UINT>(cfg.strobePulseUs)));
    warnOnFail("CameraSetStrobeDelayTime",
               CameraSetStrobeDelayTime(hCamera, static_cast<UINT>(cfg.strobeDelayUs)));
    warnOnFail("CameraSetStrobePolarity", CameraSetStrobePolarity(hCamera, cfg.strobePolarity));

    return true;
}

} // namespace backend::camera::mindvision

#else // !MIB_HAS_MINDVISION

namespace backend::camera::mindvision {

bool applyConfigToHandle(int hCamera, const Config& cfg, std::string* firstError)
{
    (void)hCamera;
    (void)cfg;
    if (firstError && firstError->empty())
    {
        *firstError = "MindVision SDK disabled at build time";
    }
    SPDLOG_WARN("MindVision config: applyConfigToHandle unavailable (SDK disabled at build time)");
    return false;
}

} // namespace backend::camera::mindvision

#endif
