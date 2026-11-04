// A camera that reports its acquisition like the eGrabber camera (#833):
// it delivers Mono8 frames of its window's size and returns an
// acquisitionReport() with the window, the driven rate and the camera's
// maximum rate for the window. Used by the recording capacity readiness tests.
#pragma once

#include "backend/camera/common/ICamera.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mib::test {

class AcquisitionReportCamera : public camera::common::ICamera {
public:
    struct Setup {
        camera::common::CameraAcquisitionReport report;
        std::chrono::microseconds frameInterval{2000};
        bool reports = true; // false: like a backend with no acquisition report
    };

    explicit AcquisitionReportCamera(Setup setup) : setup_(std::move(setup)) {}
    ~AcquisitionReportCamera() override { stop(); }

    void applyConfig(const camera::common::CameraConfig& config) override { config_ = config; }
    bool start() override
    {
        running_.store(true);
        next_ = std::chrono::steady_clock::now();
        return true;
    }
    void stop() override { running_.store(false); }
    bool isRunning() const override { return running_.load(); }

    bool grabFrame(camera::common::Frame& out) override
    {
        if (!running_.load()) return false;
        next_ += setup_.frameInterval;
        std::this_thread::sleep_until(next_);
        if (!running_.load()) return false;
        const auto& w = setup_.report.window;
        out.width = static_cast<uint64_t>(w.width);
        out.height = static_cast<uint64_t>(w.height);
        out.pixelFormat = 0x01080001; // Mono8
        out.linePitch = static_cast<size_t>(out.width);
        out.data.assign(static_cast<size_t>(out.width * out.height), static_cast<uint8_t>(frames_ & 0xff));
        out.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(next_.time_since_epoch()).count());
        ++frames_;
        return true;
    }
    bool pollStats(camera::common::CameraStats& out) const override
    {
        out = {};
        return false;
    }
    bool acquisitionReport(camera::common::CameraAcquisitionReport& out) const override
    {
        if (!setup_.reports) return false;
        out = setup_.report;
        return true;
    }

private:
    Setup setup_;
    camera::common::CameraConfig config_{};
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point next_{};
    uint64_t frames_{0};
};

// The camera's report for one window of the EoSens 2.0MCX12 (MC2066) table in
// issue #833 (exposure 20 us), driven at `requestedFps` by CycleMinimumPeriod.
inline camera::common::CameraAcquisitionReport mc2066Report(int width, int height, double requestedFps, double maxFps)
{
    camera::common::CameraAcquisitionReport r;
    r.vendor = "SVS-Vistek GmbH";
    r.model = "MC2066";
    r.window = {width, height, 0, 0};
    r.windowKnown = true;
    r.requestedFps = requestedFps;
    r.requestedFpsSource = "CycleMinimumPeriod " + std::to_string(static_cast<int>(1e6 / requestedFps)) + " us (RC)";
    r.maxFps = maxFps;
    return r;
}

} // namespace mib::test
