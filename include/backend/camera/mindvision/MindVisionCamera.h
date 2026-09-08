#pragma once

#include "backend/camera/common/ICamera.h"
#include "backend/camera/mindvision/MindVisionFrameGeometry.h"
#include "backend/camera/mindvision/MindVisionSdk.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace camera::common
{

// MindVision SDK-backed ICamera. All SDK access goes through an injectable
// backend::camera::mindvision::SdkOps table (issue #366) so the class is
// compiled and unit-testable in every build; without the real SDK start()
// fails closed with code "mindvision.sdk_unavailable".
class MindVisionCamera : public ICamera
{
public:
    using SdkOps = backend::camera::mindvision::SdkOps;

    explicit MindVisionCamera(int cameraIndex, std::string configPath = {},
                              std::shared_ptr<const SdkOps> sdk = nullptr);
    ~MindVisionCamera() override;

    void applyConfig(const CameraConfig &config) override;
    bool start() override;
    void stop() override;
    bool isRunning() const override
    {
        return running_.load(std::memory_order_acquire);
    }

    bool grabFrame(Frame &out) override;
    bool pollStats(CameraStats &out) const override;
    bool checkDeviceHealth() const override;

    FrameDeliveryCapabilities deliveryCapabilities() const override;
    FrameDeliveryMode activeDeliveryMode() const override;
    bool pollAcquisitionQueueStats(AcquisitionQueueStats &out) const override;

    void configureTriggerOutput(const std::string &lineSelector) override;
    bool setTriggerOutput(bool high) override;

    // Software acquisition trigger (CameraSoftTrigger). Distinct from
    // setTriggerOutput, which drives the sort-output pulse.
    bool softTrigger() override;

    CameraFailure lastFailure() const override;
    // Frame::timestamp = uiTimeStamp (0.1 ms device ticks) x 100000 -> ns;
    // rawDeviceTicks keeps the native counter. Device clock, not host.
    TimestampDescriptor timestampDescriptor() const override;

    // Validated session geometry (valid while running). Exposed for
    // lifecycle/preflight reporting and tests.
    backend::camera::mindvision::SessionGeometry sessionGeometry() const;

    // Frames rejected before conversion because their header did not match
    // the session allocation (issue #366). Each rejection also faults the
    // stream (grabFrame returns false, isRunning() -> false).
    uint64_t geometryRejectedFrames() const
    {
        return geometryRejectedFrames_.load(std::memory_order_relaxed);
    }

    // Bounded wait for in-flight SDK operations at stop(). Exposed so tests
    // can shorten it; production default is generous because SDK calls are
    // themselves bounded by their 100 ms retrieval timeout.
    void setInFlightDrainTimeout(std::chrono::milliseconds t) { inFlightDrainTimeout_ = t; }

private:
    bool applyJsonConfig(int hCamera);
    void recordFailure(const std::string &code, const std::string &message);
    // Tear down an open handle (must hold stateMutex_). Waits for in-flight
    // SDK operations first; if they do not drain within the bounded timeout
    // the handle is abandoned (leaked) rather than uninitialized underneath
    // an in-flight call.
    void closeHandleLocked(std::unique_lock<std::mutex> &lock);
    void faultStreamLocked(const std::string &code, const std::string &message);

    // RAII marker for an SDK operation using the current handle. Taken under
    // stateMutex_ only while running_; stop() waits for the count to reach
    // zero before CameraUnInit. This is the documented synchronization
    // boundary between grabFrame's blocking SDK calls and handle teardown.
    class InFlightOp
    {
    public:
        explicit InFlightOp(MindVisionCamera &owner);
        ~InFlightOp();
        bool valid() const { return handle_ >= 0; }
        int handle() const { return handle_; }
        FrameDeliveryMode mode() const { return mode_; }
        int maxDrain() const { return maxDrain_; }

    private:
        MindVisionCamera &owner_;
        int handle_{-1};
        FrameDeliveryMode mode_{FrameDeliveryMode::EveryFrame};
        int maxDrain_{0};
    };
    friend class InFlightOp;

    std::shared_ptr<const SdkOps> sdk_;
    int cameraIndex_{-1};
    std::string configPath_;
    CameraConfig config_{};

    int hCamera_{-1};
    uint8_t *outBuffer_{nullptr};
    std::size_t outBufferBytes_{0};
    backend::camera::mindvision::SessionGeometry sessionGeometry_{};
    int triggerOutputIndex_{-1};
    // trigger_mode from the applied JSON config (0 continuous, 1 software,
    // 2 external) — checkDeviceHealth keys off it.
    int configuredTriggerMode_{0};

    std::atomic<bool> running_{false};
    mutable std::mutex stateMutex_;
    std::condition_variable inFlightCv_;
    int inFlightOps_{0};
    std::chrono::milliseconds inFlightDrainTimeout_{5000};

    CameraFailure lastFailure_{};

    // Delivery mode confirmed at the most recent successful start(). Until a
    // start() succeeds, activeDeliveryMode() reports config_.deliveryMode.
    FrameDeliveryMode confirmedDeliveryMode_{FrameDeliveryMode::EveryFrame};
    bool deliveryModeConfirmed_{false};

    // Stale completed SDK buffers dropped by the LatestFrame policy (exact
    // count on the drain path; the SDK priority path cannot observe skips).
    std::atomic<uint64_t> intentionalDiscards_{0};
    std::atomic<uint64_t> geometryRejectedFrames_{0};

    mutable uint64_t frameCount_{0};
    mutable std::chrono::steady_clock::time_point startTime_{};
    mutable CameraStats lastStats_{};
};

} // namespace camera::common
