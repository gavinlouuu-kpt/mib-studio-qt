#pragma once

#include "backend/camera/common/ICamera.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct _ArvCamera;
struct _ArvStream;

namespace camera::aravis {

struct AravisRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/** Settings read back from the device after start(), with the PZ7035
 *  producer's rate model when the device exposes it. The frame rate is the
 *  sensor rate; a producer that reads full-field images out in bands delivers
 *  fewer images per second (deliveredFrameRateHz), and the UI must say so. */
struct AravisSessionInfo {
    std::string deviceId;
    std::string vendor;
    std::string model;
    AravisRegion region;
    int sensorWidth = 0;   // 0 = not reported
    int sensorHeight = 0;
    int widthIncrement = 1;
    int heightIncrement = 1;
    int offsetXIncrement = 1;
    int offsetYIncrement = 1;
    double requestedFrameRateHz = 0.0; // 0 = not requested
    double frameRateHz = 0.0;
    double frameRateMinHz = 0.0;
    double frameRateMaxHz = 0.0;
    bool frameRateClamped = false;     // requested rate differs from the applied one
    double requestedExposureUs = 0.0;  // 0 = not requested
    double exposureUs = 0.0;
    double exposureMaxUs = 0.0;
    bool exposureClamped = false;
    // PZ7035 extension features (PzBandCount, PzDeliveredFrameRate, ...).
    bool pzFeatures = false;
    int64_t bandCount = 1;
    double deliveredFrameRateHz = 0.0;
    std::string deliveredFrameRateLimit; // "SensorRate", "BandReadout" or "PreviewRate"
    double previewRateHz = 0.0;          // applied PzPreviewRate (0 = every frame / no feature)
    std::string frameRateLimitReason;    // "SensorGeometry", "Profile", "StoreBandwidth"
};

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
    // Auto-selection (empty deviceId) prefers a device of this vendor; "YOFO"
    // is the PZ7035 GenTL producer. Other devices remain selectable by id.
    std::string preferredVendor = "YOFO";
    // GigE Vision discovery resolves device names with getaddrinfo, which on
    // the isolated PZ7035 network costs seconds per open. Off unless a GigE
    // camera is actually wanted.
    bool enableGigEVision = false;
    // Optional settings applied at start in this order (the frame-rate and
    // exposure maxima depend on the region). Every value is read back:
    // devices clamp silently, so sessionInfo() reports what was applied.
    std::optional<AravisRegion> region;
    std::optional<double> frameRateHz;
    std::optional<double> exposureUs;
    // Images per second the device should deliver (PzPreviewRate on the PZ7035 producer; the
    // PL processes every frame, the PS only displays). 0 = every frame. Ignored by devices
    // without the feature.
    std::optional<double> previewRateHz;
    // Overview: the whole sensor (SensorWidth x SensorHeight at offset 0), overriding `region`.
    bool fullSensor = false;
    // Called with the read-back after every successful start (from start()'s thread), so an
    // owner that does not hold the camera (AppBackend, behind CaptureService) can show the
    // applied geometry and the producer's rate model.
    std::function<void(const AravisSessionInfo&)> onSession;
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

    /** Read-back of the running (or last) session; empty before the first start. */
    AravisSessionInfo sessionInfo() const;

private:
    bool failLocked(const char* code, const std::string& message);
    bool openAndConfigureLocked();
    bool applySettingsLocked();
    void readSessionInfoLocked(const std::string& deviceId);
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
    // Mode of the running session; written by start() under both locks and
    // read by grabFrame() under sdkMutex_.
    common::FrameDeliveryMode activeMode_ = common::FrameDeliveryMode::EveryFrame;
    AravisSessionInfo sessionInfo_{};
};

} // namespace camera::aravis
