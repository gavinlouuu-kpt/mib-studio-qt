#pragma once

// Align live view on the PZ7035 (#501 P1, images results8 on): a camera whose frames are the
// whole 816x624 sensor frames the results bridge writes to its DDR preview slots
// (IExecutionProvider::startPreview/fetchPreview). The GenTL producer applied the Align timing and
// has been stopped; this replaces its banded grabber preview, which tore moving cells at band edges.
// It runs behind CaptureService like any camera, so the UI's live view is unchanged.

#include "backend/camera/common/ICamera.h"
#include "backend/processing/IExecutionProvider.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace backend::pz {

class PzBridgePreviewCamera final : public ::camera::common::ICamera {
public:
    PzBridgePreviewCamera(processing::IExecutionProvider& provider, processing::pz::BridgePreviewConfig config,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(500),
                          unsigned maxConsecutiveTimeouts = 6);
    ~PzBridgePreviewCamera() override;

    void applyConfig(const ::camera::common::CameraConfig&) override {}
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_.load(); }
    // One new whole frame, waiting up to the timeout. A few timeouts in a row are tolerated (the
    // bridge publishes every decimation-th frame); after `maxConsecutiveTimeouts`, or when the
    // bridge leaves ARMED/RUNNING, the camera stops with the reason in lastFailure().
    bool grabFrame(::camera::common::Frame& out) override;
    bool pollStats(::camera::common::CameraStats& out) const override;
    ::camera::common::CameraFailure lastFailure() const override;

private:
    void fail(const std::string& code, const std::string& message);

    processing::IExecutionProvider& provider_;
    processing::pz::BridgePreviewConfig config_;
    std::chrono::milliseconds timeout_;
    unsigned maxTimeouts_;
    std::atomic<bool> running_{false};
    uint64_t lastFrameId_{0};
    unsigned timeouts_{0};
    std::atomic<uint64_t> frames_{0};
    std::chrono::steady_clock::time_point startedAt_{};
    mutable std::mutex failureMutex_;
    ::camera::common::CameraFailure failure_;
};

} // namespace backend::pz
