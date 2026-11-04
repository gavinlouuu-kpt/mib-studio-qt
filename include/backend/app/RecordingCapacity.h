// Recording capacity readiness gates (#833). Pure: the coordinator collects
// the inputs (camera report, profile, buffer plan, measured write rate) and
// these functions decide the gates, so every rule is unit-tested without a
// camera or a disk.
//
//  camera.window      the profile's camera window is the one in the camera:
//                     Fail when it was refused (made for another camera), the
//                     camera rejected it, or the camera runs another window;
//                     Warn when the script's window was overridden, the window
//                     names no camera, or the camera cannot apply windows.
//  camera.rate        Fail when the requested rate exceeds the camera's
//                     AcquisitionFrameRateMax for the window; Warn when the
//                     maximum or the requested rate is unknown.
//  storage.buffer     Fail when one image series does not fit an explicit
//                     override, or the minimum hold (2 x flush interval) does
//                     not fit the RAM ceiling; Warn when the buffer holds less
//                     than kWarnBacklogSeconds if every frame were saved, or an
//                     override is below the flush interval.
//  storage.throughput Fail when a measured sustained write rate is below the
//                     floor stream (invalid-frame sampling at the requested
//                     rate with no valid cells: even an empty run could not
//                     keep up); Warn when the rate is unknown.
#pragma once

#include "backend/app/CameraWindowProfile.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/processing/ExperimentBufferSizing.h"

#include <cstdint>
#include <optional>
#include <string>

namespace backend::app {

struct CameraWindowGateInputs {
    std::optional<ProfileCameraWindow> profile;      // camera.window of the applied config
    bool cameraAppliesWindows{false};                // the selected camera takes a window (eGrabber)
    std::string cameraName;                          // "egrabber", "mock", ... (for the text)
    std::optional<CameraWindowApplyReport> apply;    // the last Experiment script apply, if any
    bool sessionWindowKnown{false};                  // the running camera reported its window
    ::camera::common::CameraWindow sessionWindow;
    bool frameKnown{false};                          // a frame has been received
    uint64_t frameWidth{0}, frameHeight{0};
};
ReadinessGate cameraWindowGate(const CameraWindowGateInputs& in);

struct RecordingCapacityInputs {
    uint64_t windowWidth{0}, windowHeight{0};        // the frames being saved; 0 = unknown
    uint32_t imagesPerFrame{2};
    double requestedFps{0.0};                        // 0 = unknown
    std::string requestedFpsSource;                  // "camera CycleMinimumPeriod 200 us (RC)", "profile target_fps"
    double maxFps{0.0};                              // camera maximum for the window; 0 = unavailable
    processing::ExperimentBufferPlan plan;
    uint64_t overrideBytes{0};                       // experiment_buffer_max_mb; 0 = automatic
    size_t invalidSamplingRate{1};                   // every Nth invalid frame is saved
    double diskBytesPerSecond{0.0};                  // measured sustained write rate; 0 = unknown
    std::string diskRateNote;                        // how it was measured, or why it is unknown
};

struct RecordingCapacity {
    ReadinessGate rate;       // camera.rate
    ReadinessGate buffer;     // storage.buffer
    ReadinessGate throughput; // storage.throughput
    uint64_t bytesPerFrame{0};
    double everyFrameBytesPerSecond{0}; // every frame saved at the requested rate
    double floorFps{0};                 // invalid sampling with no valid cells
    double floorBytesPerSecond{0};
};
RecordingCapacity evaluateRecordingCapacity(const RecordingCapacityInputs& in);

// "2529 MB/s" (decimal megabytes, as disk rates are quoted)
std::string formatRate(double bytesPerSecond);

} // namespace backend::app
