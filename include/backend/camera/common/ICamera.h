#pragma once

#include "backend/camera/common/Frame.h"
#include "backend/camera/common/TimestampValue.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace camera::common
{

    /**
     * User-facing frame delivery policy, applied at the earliest controllable
     * SDK queue (not just downstream application queues).
     *
     * - EveryFrame: completed frames are returned in acquisition order and
     *   never intentionally skipped. Backlog (and therefore latency) can grow
     *   when the consumer is slower than the camera.
     * - LatestFrame: stale completed SDK buffers are discarded before the
     *   expensive copy so grabFrame() returns the freshest complete image.
     *   Every deliberate discard is counted; sequences may have gaps.
     */
    enum class FrameDeliveryMode
    {
        EveryFrame,
        LatestFrame,
    };

    inline const char *toString(FrameDeliveryMode mode)
    {
        return mode == FrameDeliveryMode::LatestFrame ? "latestFrame" : "everyFrame";
    }

    // Deterministic migration default: anything unrecognized maps to EveryFrame
    // so pre-existing profiles keep today's ordered behavior.
    inline FrameDeliveryMode frameDeliveryModeFromString(const std::string &text)
    {
        return text == "latestFrame" ? FrameDeliveryMode::LatestFrame
                                     : FrameDeliveryMode::EveryFrame;
    }

    /**
     * Per-backend delivery-mode support. Not every SDK exposes identical queue
     * controls, so unsupported modes are represented here instead of failing
     * deep inside a backend.
     */
    struct FrameDeliveryCapabilities
    {
        bool supportsEveryFrame = true;
        bool supportsLatestFrame = false;
        bool modeChangeRequiresRestart = true;
        // True when Frame::timestamp lives in a clock domain comparable to
        // Tools::getTimestamp() (host monotonic microseconds), so frame age
        // may be estimated. Only set after the mapping has been verified for
        // the specific transport (e.g. Coaxlink buffer timestamps are
        // microseconds since computer startup).
        bool timestampsHostComparable = false;
    };

    /**
     * Acquisition-queue telemetry. Intentional discards (LatestFrame policy),
     * transport loss/underrun, and downstream processing drops are distinct
     * failure modes and must never be folded into one counter.
     *
     * Backends that cannot observe a field leave its *Valid flag false so
     * consumers can distinguish "zero" from "unknown".
     */
    struct AcquisitionQueueStats
    {
        uint64_t intentionallyDiscardedFrames = 0; // stale frames dropped by LatestFrame policy
        uint64_t transportLostFrames = 0;          // frames the transport/SDK reported lost or incomplete
        uint64_t bufferUnderruns = 0;              // acquisition engine had no input buffer available
        uint64_t deliveredFrames = 0;              // completed frames handed to the application
        size_t sdkCompletedQueueDepth = 0;         // completed buffers waiting in the SDK output FIFO
        size_t sdkInputBufferCount = 0;            // buffers available to the acquisition engine
        bool completedQueueDepthValid = false;
        bool inputBufferCountValid = false;
        bool underrunsValid = false;
        bool transportLossValid = false;
    };

    struct CameraConfig
    {
        int bufferPartCount = 1;
        int numBuffers = 20;
        FrameDeliveryMode deliveryMode = FrameDeliveryMode::EveryFrame;
    };

    struct CameraStats
    {
        uint64_t frameRate = 0;    // Frames per second as reported by the device.
        uint64_t dataRateMBps = 0; // Throughput in MB/s.
    };

    /**
     * Structured description of the most recent start/stream failure of a
     * backend (issues #365/#366). `code` is a stable machine-readable token
     * (e.g. "mindvision.isp_format_unverified"); `message` is operator text.
     * Empty code means no failure has been recorded.
     */
    struct CameraFailure
    {
        std::string code;
        std::string message;
        bool empty() const { return code.empty(); }
    };

    class ICamera
    {
    public:
        virtual ~ICamera() = default;

        virtual void applyConfig(const CameraConfig &config) = 0;
        virtual bool start() = 0;
        virtual void stop() = 0;
        virtual bool isRunning() const = 0;

        /**
         * Blocks until a frame is available or the camera is stopped.
         * Returns false when no further frames can be delivered (e.g. after stop()).
         */
        virtual bool grabFrame(Frame &out) = 0;

        /**
         * Poll device statistics. Returns false if stats are unavailable.
         */
        virtual bool pollStats(CameraStats &out) const = 0;

        /**
         * Delivery-mode support of this backend. Callers must consult this
         * before requesting LatestFrame; backends reject unsupported modes in
         * start() with an actionable error.
         */
        virtual FrameDeliveryCapabilities deliveryCapabilities() const { return {}; }

        /**
         * The mode acquisition is actually running in (backend-confirmed, not
         * merely requested). Valid after a successful start(); before that it
         * reports the mode the current config would select.
         */
        virtual FrameDeliveryMode activeDeliveryMode() const { return FrameDeliveryMode::EveryFrame; }

        /**
         * Poll acquisition-queue telemetry (queue depths, intentional discards,
         * transport loss, underruns). Returns false when the backend exposes no
         * queue observability at all.
         */
        virtual bool pollAcquisitionQueueStats(AcquisitionQueueStats &out) const
        {
            (void)out;
            return false;
        }

        /**
         * Check if device is healthy and responsive. Returns false if device is unresponsive.
         * Can be called periodically to detect device failures.
         */
        virtual bool checkDeviceHealth() const { return true; }

        /**
         * Configure a digital output line for trigger output.
         * No-op for cameras that don't support hardware trigger.
         */
        virtual void configureTriggerOutput(const std::string& lineSelector) { (void)lineSelector; }

        /**
         * Set trigger output line to High or Low. Returns false if not supported.
         */
        virtual bool setTriggerOutput(bool high) { (void)high; return false; }

        /**
         * Fire one software acquisition trigger (starts an exposure when the
         * camera is in software-trigger mode). This is the acquisition-side
         * trigger — NOT the sort-output pulse driven by setTriggerOutput.
         * Returns false when unsupported, not running, or rejected.
         */
        virtual bool softTrigger() { return false; }

        /**
         * Most recent structured failure (start rejected, stream faulted).
         * Backends that fail closed on configuration/geometry mismatches
         * report the reason here so the capture lifecycle and UI can show it
         * instead of a generic "failed to start".
         */
        virtual CameraFailure lastFailure() const { return {}; }

        /**
         * Describes the `Frame::timestamp` values this backend produces
         * (clock domain, tick rate after the adapter's documented
         * normalization, native tick rate, semantic, validity). Issue #368.
         * Default: undeclared/unsupported — consumers must not assume a unit.
         */
        virtual TimestampDescriptor timestampDescriptor() const { return {}; }
    };

} // namespace camera::common
