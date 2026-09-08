// Scriptable ICamera for lifecycle tests (issue #365).
//
// Unlike QueueBackedTestCamera (delivery-mode semantics) this fake models the
// *lifecycle* hazards: a start() that fails or blocks, a grabFrame() that
// blocks until released, a stream that ends on its own, a stop() that is
// slow (SDK teardown), and a "destroyed" flag so tests can prove no call
// lands after the object's session ended.
#pragma once

#include "backend/camera/common/ICamera.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace mib::test {

class FakeLifecycleCamera : public camera::common::ICamera {
public:
    struct Script {
        bool startSucceeds = true;
        // grabFrame blocks until releaseGrab() / stop() when true; otherwise
        // it produces a frame every produceInterval.
        bool blockGrab = false;
        std::chrono::microseconds produceInterval{200};
        // After this many frames the camera reports "stream ended" on its own
        // (isRunning() false, grabFrame false). 0 = never.
        uint64_t endStreamAfterFrames = 0;
        // Delay injected inside stop() to model slow SDK teardown.
        std::chrono::milliseconds stopDelay{0};
        // Delay injected inside start() before it reports success/failure.
        std::chrono::milliseconds startDelay{0};
        std::string failureCode;
        std::string failureMessage;
    };

    // Shared observation record that outlives the camera object (CaptureService
    // owns the camera; the test only keeps this).
    struct Observations {
        std::atomic<int> startCalls{0};
        std::atomic<int> stopCalls{0};
        std::atomic<int> grabCalls{0};
        std::atomic<uint64_t> framesDelivered{0};
        std::atomic<int> triggerCalls{0};
        // Set by the destructor; any access counted after this is a
        // use-after-lifetime.
        std::atomic<bool> destroyed{false};
        std::atomic<int> accessAfterDestroy{0};
        std::atomic<int> triggerCallsAfterUnbind{0};
        std::atomic<bool> unbound{false};
        // Rising edge count while bound (valid pulses).
        std::atomic<int> pulses{0};
    };

    FakeLifecycleCamera(Script script, Observations* obs) : script_(script), obs_(obs) {}

    ~FakeLifecycleCamera() override
    {
        stop();
        if (obs_) obs_->destroyed.store(true, std::memory_order_release);
    }

    void applyConfig(const camera::common::CameraConfig& config) override { config_ = config; }

    bool start() override
    {
        obs_->startCalls.fetch_add(1);
        if (script_.startDelay.count() > 0) std::this_thread::sleep_for(script_.startDelay);
        if (!script_.startSucceeds) {
            std::lock_guard<std::mutex> lk(mutex_);
            failure_.code = script_.failureCode.empty() ? "fake.start_failed" : script_.failureCode;
            failure_.message = script_.failureMessage.empty() ? "fake camera refused to start"
                                                              : script_.failureMessage;
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            running_ = true;
            released_ = false;
        }
        return true;
    }

    void stop() override
    {
        touch();
        obs_->stopCalls.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();
        if (script_.stopDelay.count() > 0) std::this_thread::sleep_for(script_.stopDelay);
    }

    bool isRunning() const override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return running_;
    }

    bool grabFrame(camera::common::Frame& out) override
    {
        touch();
        obs_->grabCalls.fetch_add(1);
        std::unique_lock<std::mutex> lk(mutex_);
        if (!running_) return false;
        if (script_.blockGrab) {
            grabBlocked_ = true;
            blockedCv_.notify_all();
            cv_.wait(lk, [&] { return !running_ || released_; });
            grabBlocked_ = false;
            if (!running_) return false;
            released_ = false;
        } else {
            lk.unlock();
            std::this_thread::sleep_for(script_.produceInterval);
            lk.lock();
            if (!running_) return false;
        }
        if (script_.endStreamAfterFrames > 0 &&
            obs_->framesDelivered.load() >= script_.endStreamAfterFrames) {
            running_ = false;
            failure_.code = "fake.stream_ended";
            failure_.message = "fake camera ended its stream";
            return false;
        }
        out.width = 8;
        out.height = 4;
        out.linePitch = 8;
        out.pixelFormat = 0x01080001;
        out.timestamp = ++seq_;
        out.data.assign(32, static_cast<uint8_t>(seq_ & 0xFF));
        obs_->framesDelivered.fetch_add(1);
        return true;
    }

    bool pollStats(camera::common::CameraStats& out) const override
    {
        out = {};
        return isRunning();
    }

    camera::common::FrameDeliveryCapabilities deliveryCapabilities() const override
    {
        camera::common::FrameDeliveryCapabilities caps;
        caps.supportsEveryFrame = true;
        caps.supportsLatestFrame = true;
        return caps;
    }

    camera::common::CameraFailure lastFailure() const override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return failure_;
    }

    void configureTriggerOutput(const std::string&) override { touch(); }

    bool setTriggerOutput(bool high) override
    {
        touch();
        obs_->triggerCalls.fetch_add(1);
        if (obs_->unbound.load(std::memory_order_acquire)) {
            obs_->triggerCallsAfterUnbind.fetch_add(1);
        }
        if (high) obs_->pulses.fetch_add(1);
        return true;
    }

    // --- test controls ---
    // Wait until grabFrame is parked in its blocking wait.
    bool waitUntilGrabBlocked(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return blockedCv_.wait_for(lk, timeout, [&] { return grabBlocked_; });
    }
    // Let one blocked grab through (produces one frame).
    void releaseGrab()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    void touch()
    {
        if (obs_ && obs_->destroyed.load(std::memory_order_acquire)) {
            obs_->accessAfterDestroy.fetch_add(1);
        }
    }

    Script script_;
    Observations* obs_;
    camera::common::CameraConfig config_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable blockedCv_;
    bool running_{false};
    bool released_{false};
    bool grabBlocked_{false};
    uint64_t seq_{0};
    camera::common::CameraFailure failure_{};
};

} // namespace mib::test
