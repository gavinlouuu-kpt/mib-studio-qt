#pragma once

#include "backend/camera/common/ICamera.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct _ArvCamera;
struct _ArvStream;

namespace camera::aravis {

/** Options for the optional Aravis consumer.
 *
 * `useFake` is deliberately explicit.  A missing physical device must never
 * turn into a synthetic camera merely because Aravis is enabled.
 */
struct AravisCameraOptions {
    std::string deviceId;
    bool useFake = false;
    std::size_t streamBuffers = 8;
    uint32_t popTimeoutMs = 100;
    // Optional SFNC software-trigger mode. It is primarily useful for
    // deterministic consumers/tests that need to observe an empty receive
    // queue while acquisition remains running.
    bool softwareTrigger = false;
    // Test-only lifecycle barrier. It is invoked after start has registered
    // its generation but before taking SDK ownership, allowing deterministic
    // stop-during-start coverage without timing sleeps.
    std::function<void()> beforeStartHook;
};

class AravisCamera final : public common::ICamera {
public:
    explicit AravisCamera(AravisCameraOptions options = {});
    ~AravisCamera() override;

    AravisCamera(const AravisCamera&) = delete;
    AravisCamera& operator=(const AravisCamera&) = delete;

    /** Enumerate current Aravis devices. The fake interface is opt-in. */
    static std::vector<std::string> enumerateDeviceIds(bool includeFake = false);

    void applyConfig(const common::CameraConfig& config) override;
    bool start() override;
    void stop() override;
    bool isRunning() const override;
    bool grabFrame(common::Frame& out) override;
    bool pollStats(common::CameraStats& out) const override;
    common::FrameDeliveryCapabilities deliveryCapabilities() const override;
    common::FrameDeliveryMode activeDeliveryMode() const override;
    bool pollAcquisitionQueueStats(common::AcquisitionQueueStats& out) const override;
    bool checkDeviceHealth() const override;
    bool softTrigger() override;
    common::CameraFailure lastFailure() const override;
    common::TimestampDescriptor timestampDescriptor() const override;

private:
    bool failLocked(const char* code, const std::string& message);
    bool openAndConfigureLocked();
    void releaseResourcesLocked();
    bool setFailure(const char* code, const std::string& message);

    AravisCameraOptions options_;
    mutable std::mutex lifecycleMutex_;
    // Protects Aravis object lifetime and SDK calls. grabFrame holds this only
    // across a bounded timeout pop and buffer copy; stop can therefore always
    // make progress and no object is freed while the SDK is using it.
    mutable std::mutex sdkMutex_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    // Incremented when stop() begins. A start that was already entering the
    // SDK when stop() arrived must observe the changed generation and abort;
    // otherwise it could reopen resources after the stop had completed.
    std::atomic<uint64_t> lifecycleGeneration_{0};
    bool stopInProgress_ = false; // guarded by lifecycleMutex_

    _ArvCamera* camera_ = nullptr;
    _ArvStream* stream_ = nullptr;
    common::CameraConfig config_{};
    common::CameraStats stats_{};
    common::AcquisitionQueueStats queueStats_{};
    common::CameraFailure failure_{};
    common::TimestampDescriptor timestampDescriptor_{};
    uint64_t sessionGeneration_ = 0;
    uint64_t deliveredFrames_ = 0;
};

} // namespace camera::aravis
