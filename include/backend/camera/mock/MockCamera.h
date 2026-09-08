#pragma once

#include "backend/camera/common/ICamera.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>

namespace camera::mock {

struct MockCameraOptions {
    std::filesystem::path folder;
    std::chrono::microseconds frameInterval{33'000};
    bool loopFiles{true};
};

class MockCamera : public camera::common::ICamera {
public:
    explicit MockCamera(MockCameraOptions options);
    ~MockCamera() override = default;

    void applyConfig(const camera::common::CameraConfig& config) override;
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

    bool grabFrame(camera::common::Frame& out) override;
    bool pollStats(camera::common::CameraStats& out) const override;

    // The mock source synthesizes frames on demand, so there is no SDK queue
    // to drain: both delivery modes are supported and behave identically, with
    // a genuinely-zero completed-buffer backlog.
    camera::common::FrameDeliveryCapabilities deliveryCapabilities() const override {
        camera::common::FrameDeliveryCapabilities caps;
        caps.supportsEveryFrame = true;
        caps.supportsLatestFrame = true;
        caps.modeChangeRequiresRestart = false;
        return caps;
    }
    camera::common::FrameDeliveryMode activeDeliveryMode() const override {
        return config_.deliveryMode;
    }
    bool pollAcquisitionQueueStats(camera::common::AcquisitionQueueStats& out) const override;
    // Frame::timestamp = steady_clock nanoseconds at delivery (synthetic; a
    // different unit from Tools::getTimestamp's microseconds).
    camera::common::TimestampDescriptor timestampDescriptor() const override {
        camera::common::TimestampDescriptor d;
        d.domain = camera::common::ClockDomain::HostSteadyNs;
        d.ticksPerSecond = 1'000'000'000ULL;
        d.semantic = camera::common::TimestampSemantic::Synthetic;
        d.validity = camera::common::TimestampValidity::Valid;
        return d;
    }

    // Simulated digital trigger output so TriggerService works end-to-end in
    // mock mode (headless pipeline dry-runs, latency instrumentation). The
    // "line" only records level changes; rising edges are counted.
    void configureTriggerOutput(const std::string& lineSelector) override;
    bool setTriggerOutput(bool high) override;
    uint64_t triggerPulseCount() const {
        return triggerPulseCount_.load(std::memory_order_relaxed);
    }

    void setFrameInterval(std::chrono::microseconds interval);
    void setLooping(bool loop);

private:
    void refreshFileList();
    bool loadFrameFromPath(const std::filesystem::path& path, camera::common::Frame& frame);
    bool preloadFrames();

    MockCameraOptions options_;
    camera::common::CameraConfig config_{};
    std::vector<std::filesystem::path> files_;
    std::vector<camera::common::Frame> preloadedFrames_;
    size_t nextIndex_{0};

    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point lastFrameTime_{};
    mutable camera::common::CameraStats stats_{};
    std::atomic<uint64_t> deliveredFrames_{0};

    // Simulated trigger line (written by the trigger thread).
    std::atomic<bool> triggerLineHigh_{false};
    std::atomic<uint64_t> triggerPulseCount_{0};
};

} // namespace camera::mock


