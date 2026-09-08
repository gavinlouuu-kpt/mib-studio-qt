#pragma once

#include "backend/camera/common/ICamera.h"

#ifndef MIB_HAS_EGRABBER
#define MIB_HAS_EGRABBER 0
#endif

#if MIB_HAS_EGRABBER
#include <EGrabber.h>
#endif

#include <atomic>
#include <deque>
#include <optional>
#include <memory>
#include <mutex>

namespace camera::common {

/**
 * Concrete camera implementation backed by Euresys EGrabber.
 * The code path mirrors the official SDK sample
 * `egrabber-snippets/samples/310-high-frame-rate.cpp`.
 */
class EGrabberCamera : public ICamera {
public:
    EGrabberCamera();
    EGrabberCamera(int interfaceIndex, int deviceIndex);
    ~EGrabberCamera() override;

    void applyConfig(const CameraConfig& config) override;
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

    bool grabFrame(Frame& out) override;
    bool pollStats(CameraStats& out) const override;
    // Frame::timestamp = Coaxlink BUFFER_INFO_TIMESTAMP / per-part custom
    // timestamps: microseconds since host boot (QPC domain on Windows).
    TimestampDescriptor timestampDescriptor() const override;
    FrameDeliveryCapabilities deliveryCapabilities() const override;
    FrameDeliveryMode activeDeliveryMode() const override;
    bool pollAcquisitionQueueStats(AcquisitionQueueStats& out) const override;
    bool checkDeviceHealth() const;

    void configureTriggerOutput(const std::string& lineSelector) override;
    bool setTriggerOutput(bool high) override;

private:
    void replenishPendingFrames();

    CameraConfig config_{};
#if MIB_HAS_EGRABBER
    mutable std::unique_ptr<Euresys::EGrabber<Euresys::CallbackOnDemand>> grabber_;
    std::unique_ptr<Euresys::EGenTL> genTL_;
#endif

    uint64_t width_ = 0;
    uint64_t height_ = 0;

    // Optional target selection (interface/device indices)
    bool hasSelection_ = false;
    int selectedInterfaceIndex_ = -1;
    int selectedDeviceIndex_ = -1;

    mutable CameraStats lastStats_{};
    std::deque<Frame> pendingFrames_;
    // Mode confirmed at the most recent successful start(); before any start
    // activeDeliveryMode() falls back to config_.deliveryMode.
    std::optional<FrameDeliveryMode> confirmedDeliveryMode_;
    // Stale completed buffers dropped by the LatestFrame drain. Atomic: read
    // by pollAcquisitionQueueStats() while the capture thread increments it.
    std::atomic<uint64_t> intentionallyDiscardedFrames_{0};
    // Atomic: read lock-free by the trigger thread (setTriggerOutput) and by
    // isRunning() while the capture thread writes it under stateMutex_.
    std::atomic<bool> running_{false};
    mutable std::mutex stateMutex_;
    // Guards grabber_ pointer lifetime against the trigger thread. Held by
    // setTriggerOutput() for the whole grabber_ use and by start()/stop()
    // around every grabber_ assignment/reset. Deliberately separate from
    // stateMutex_, which stop() holds across ~360 ms of teardown sleeps.
    mutable std::mutex triggerMutex_;

    // Trigger output state. triggerLineApplied_ tracks whether LineSelector
    // has been written on the current grabber_'s nodemap: the selection is
    // GenApi client state owned by that EGrabber instance, so it survives
    // between pulses and only needs (re)applying after start() or a trigger
    // reconfiguration. Guarded by triggerMutex_ alongside grabber_.
    std::string triggerLineSelector_;
    bool triggerConfigured_{false};
    bool triggerLineApplied_{false};
};

} // namespace camera::common


