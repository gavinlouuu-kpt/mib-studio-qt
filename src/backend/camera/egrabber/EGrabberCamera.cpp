#include "backend/camera/egrabber/EGrabberCamera.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>

#ifndef MIB_HAS_EGRABBER
#define MIB_HAS_EGRABBER 0
#endif

#if MIB_HAS_EGRABBER
using namespace Euresys;
#endif

namespace camera::common {

#if MIB_HAS_EGRABBER
EGrabberCamera::EGrabberCamera() = default;

EGrabberCamera::EGrabberCamera(int interfaceIndex, int deviceIndex)
    : hasSelection_(true),
      selectedInterfaceIndex_(interfaceIndex),
      selectedDeviceIndex_(deviceIndex) {}

EGrabberCamera::~EGrabberCamera() {
    stop();
}

void EGrabberCamera::applyConfig(const CameraConfig& config) {
    config_ = config;
}

bool EGrabberCamera::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (running_) {
        return true;
    }

    try {
        genTL_ = std::make_unique<EGenTL>();
        {
            // Construct outside the lock (device open can be slow), publish
            // under triggerMutex_ so the trigger thread never sees a torn
            // grabber_ pointer.
            std::unique_ptr<EGrabber<CallbackOnDemand>> grabber;
            if (hasSelection_) {
                grabber = std::make_unique<EGrabber<CallbackOnDemand>>(
                    *genTL_, selectedInterfaceIndex_, selectedDeviceIndex_);
            } else {
                grabber = std::make_unique<EGrabber<CallbackOnDemand>>(*genTL_);
            }
            std::lock_guard<std::mutex> triggerLock(triggerMutex_);
            grabber_ = std::move(grabber);
            triggerLineApplied_ = false; // fresh nodemap: LineSelector unset
        }

        // CRITICAL: Verify device is accessible before proceeding
        std::string deviceModel = "Unknown";
        std::string deviceVendor = "Unknown";
        try {
            deviceModel = grabber_->getString<DeviceModule>("DeviceModelName");
            try {
                deviceVendor = grabber_->getString<DeviceModule>("DeviceVendorName");
            } catch (const std::exception&) {
                // Vendor name may not be available, continue
            }
            SPDLOG_DEBUG("Camera device: {} {}", deviceVendor, deviceModel);
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("Camera not accessible - device may be disconnected or in invalid state: {}", ex.what());
            {
                std::lock_guard<std::mutex> triggerLock(triggerMutex_);
                grabber_.reset();
            }
            genTL_.reset();
            return false;
        }

        // CRITICAL: Stop any existing acquisition first to ensure clean state
        try {
            grabber_->execute<RemoteModule>("AcquisitionStop");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            SPDLOG_DEBUG("EGrabberCamera: AcquisitionStop sent, waiting for camera to stabilize");
        } catch (const std::exception& ex) {
            // Graceful degradation: If AcquisitionStop fails, it may already be stopped
            // Log but continue - this is not a fatal error
            SPDLOG_DEBUG("EGrabberCamera AcquisitionStop before start (expected if not running): {}", ex.what());
        }

        // Follow SDK sample 310-high-frame-rate.cpp: probe resolution, then configure buffers.
        grabber_->setInteger<StreamModule>("BufferPartCount", 1);
        width_ = grabber_->getInteger<StreamModule>("Width");
        height_ = grabber_->getInteger<StreamModule>("Height");

        // LatestFrame forces single-frame buffers: with BufferPartCount > 1 a
        // buffer only completes after batching several frames, defeating the
        // freshest-frame goal with built-in latency.
        int effectivePartCount = config_.bufferPartCount;
        if (config_.deliveryMode == FrameDeliveryMode::LatestFrame && effectivePartCount != 1) {
            SPDLOG_WARN("EGrabberCamera: LatestFrame mode overrides BufferPartCount {} -> 1 "
                        "(multi-frame batching would add latency)",
                        config_.bufferPartCount);
            effectivePartCount = 1;
        }
        grabber_->setInteger<StreamModule>("BufferPartCount", effectivePartCount);
        grabber_->reallocBuffers(config_.numBuffers);

        // EGrabber::start() starts the data stream first, then executes
        // AcquisitionStart on the remote device — the documented order.
        // Never send AcquisitionStart manually before this call: remote
        // acquisition must start exactly once, after the stream is ready.
        grabber_->start();

        confirmedDeliveryMode_ = config_.deliveryMode;
        intentionallyDiscardedFrames_.store(0, std::memory_order_relaxed);
        running_ = true;
        pendingFrames_.clear();

        SPDLOG_INFO("EGrabberCamera started successfully: {}x{}, parts={}, buffers={}, mode={}, device={} {}",
                    width_, height_, effectivePartCount, config_.numBuffers,
                    toString(config_.deliveryMode), deviceVendor, deviceModel);
        return true;
    } catch (const gentl_error& e) {
        SPDLOG_ERROR("EGrabberCamera start failed with GenTL error (code={}): {}", 
                    static_cast<int>(e.gc_err), e.what());
        stop();
        return false;
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("EGrabberCamera start failed: {}", ex.what());
        stop();
        return false;
    }
}

void EGrabberCamera::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (!running_) {
        return;
    }

    bool wasRunning = running_;
    running_ = false;
    
    // Wait a brief moment for any in-flight grabFrame() calls to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    pendingFrames_.clear();

    if (grabber_) {
        try {
            // CRITICAL: Stop remote acquisition FIRST before stopping grabber stream
            try {
                grabber_->execute<RemoteModule>("AcquisitionStop");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                SPDLOG_DEBUG("EGrabberCamera: AcquisitionStop sent, waiting for camera to stop");
            } catch (const std::exception& ex) {
                // Graceful degradation: If AcquisitionStop fails, it may already be stopped
                // Log but continue - this is not a fatal error
                SPDLOG_DEBUG("EGrabberCamera AcquisitionStop error (may already be stopped): {}", ex.what());
            }

            // Now stop the grabber stream
            grabber_->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            SPDLOG_DEBUG("EGrabberCamera: grabber stream stopped");
            
        } catch (const gentl_error& e) {
            if (e.gc_err != gc::GC_ERR_ABORT) {
                SPDLOG_WARN("EGrabberCamera stop error: {}", e.what());
            } else {
                SPDLOG_DEBUG("EGrabberCamera stop aborted pending operations (expected): {}", e.what());
            }
        } catch (const std::exception& ex) {
            SPDLOG_WARN("EGrabberCamera stop error: {}", ex.what());
        }

        // Wake up any pending pop() to allow threads to exit cleanly
        try {
            grabber_->cancelPop();
        } catch (const std::exception& ex) {
            SPDLOG_DEBUG("EGrabberCamera cancelPop note: {}", ex.what());
        }

        // CRITICAL: Wait before releasing buffers to ensure they're all returned
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        try {
            grabber_->reallocBuffers(0);
            SPDLOG_DEBUG("EGrabberCamera: buffers released");
        } catch (const std::exception& ex) {
            SPDLOG_WARN("EGrabberCamera buffer release error: {}", ex.what());
        }

        // Final wait before destroying grabber
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
        // The trigger thread reads grabber_ in setTriggerOutput(); destroying
        // it without this lock is a use-after-free when a trigger fires
        // during stop (GUI-thread camera stop leaves the trigger thread live).
        std::lock_guard<std::mutex> triggerLock(triggerMutex_);
        grabber_.reset();
    }
    genTL_.reset();
    
    if (wasRunning) {
        SPDLOG_INFO("EGrabberCamera: fully stopped and cleaned up");
    }
}

bool EGrabberCamera::grabFrame(Frame& out) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (!running_) {
        return false;
    }

    try {
        if (pendingFrames_.empty()) {
            replenishPendingFrames();
        }

        if (pendingFrames_.empty()) {
            return false;
        }

        out = std::move(pendingFrames_.front());
        pendingFrames_.pop_front();
        return true;
    } catch (const gentl_error& e) {
        if (e.gc_err == gc::GC_ERR_ABORT) {
            // Normal during stop/shutdown
            SPDLOG_DEBUG("EGrabberCamera grab aborted (expected during stop): {}", e.what());
            return false;
        }
        SPDLOG_ERROR("EGrabberCamera grab failed with GenTL error (code={}): {}", 
                    static_cast<int>(e.gc_err), e.what());
        // Note: Don't call stop() here as it would try to acquire the same mutex
        running_ = false;
        return false;
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("EGrabberCamera grab failed: {}", ex.what());
        // Note: Don't call stop() here as it would try to acquire the same mutex
        running_ = false;
        return false;
    }
}

bool EGrabberCamera::pollStats(CameraStats& out) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (!running_ || !grabber_) {
        return false;
    }

    try {
        lastStats_.frameRate = grabber_->getInteger<StreamModule>("StatisticsFrameRate");
        lastStats_.dataRateMBps = grabber_->getInteger<StreamModule>("StatisticsDataRate");
        out = lastStats_;
        return true;
    } catch (const std::exception& ex) {
        SPDLOG_WARN("EGrabberCamera stats polling failed: {}", ex.what());
        return false;
    }
}

TimestampDescriptor EGrabberCamera::timestampDescriptor() const {
    // Coaxlink BUFFER_INFO_TIMESTAMP / BUFFER_INFO_CUSTOM_PART_TIMESTAMPS are
    // microseconds since host boot. On Windows that is the QPC domain used by
    // Tools::getTimestamp(); elsewhere the mapping is unverified.
    TimestampDescriptor d;
    d.ticksPerSecond = 1'000'000ULL;
    d.semantic = TimestampSemantic::TransportReceipt;
#ifdef _WIN32
    d.domain = ClockDomain::HostMonotonicUs;
    d.validity = TimestampValidity::Valid;
#else
    d.domain = ClockDomain::Unknown;
    d.validity = TimestampValidity::Unsupported;
#endif
    d.counterBits = 64;
    return d;
}

FrameDeliveryCapabilities EGrabberCamera::deliveryCapabilities() const {
    FrameDeliveryCapabilities caps;
    caps.supportsEveryFrame = true;
    caps.supportsLatestFrame = true;
    caps.modeChangeRequiresRestart = true; // BufferPartCount and drain policy are fixed at start()
#ifdef _WIN32
    // Coaxlink buffer timestamps are microseconds since computer startup —
    // the same domain as Tools::getTimestamp() (QPC µs) on Windows only.
    caps.timestampsHostComparable = true;
#endif
    return caps;
}

FrameDeliveryMode EGrabberCamera::activeDeliveryMode() const {
    return confirmedDeliveryMode_.value_or(config_.deliveryMode);
}

bool EGrabberCamera::pollAcquisitionQueueStats(AcquisitionQueueStats& out) const {
    std::lock_guard<std::mutex> lock(stateMutex_);

    if (!running_ || !grabber_) {
        return false;
    }

    try {
        out = {};
        // GenTL datatypes: NUM_AWAIT_DELIVERY/NUM_QUEUED are SIZET,
        // NUM_UNDERRUN/NUM_DELIVERED are UINT64.
        out.sdkCompletedQueueDepth =
            grabber_->getInfo<StreamModule, size_t>(gc::STREAM_INFO_NUM_AWAIT_DELIVERY);
        out.completedQueueDepthValid = true;
        out.sdkInputBufferCount =
            grabber_->getInfo<StreamModule, size_t>(gc::STREAM_INFO_NUM_QUEUED);
        out.inputBufferCountValid = true;
        out.bufferUnderruns =
            grabber_->getInfo<StreamModule, uint64_t>(gc::STREAM_INFO_NUM_UNDERRUN);
        out.underrunsValid = true;
        out.deliveredFrames =
            grabber_->getInfo<StreamModule, uint64_t>(gc::STREAM_INFO_NUM_DELIVERED);
        out.intentionallyDiscardedFrames =
            intentionallyDiscardedFrames_.load(std::memory_order_relaxed);
        // No documented GenTL/Coaxlink stream-info counter maps cleanly to
        // transport loss, so transportLossValid stays false.
        return true;
    } catch (const std::exception& ex) {
        SPDLOG_WARN("EGrabberCamera acquisition-queue stats polling failed: {}", ex.what());
        return false;
    }
}

void EGrabberCamera::replenishPendingFrames() {
    // Note: This is called from grabFrame() which already holds the mutex
    // So we don't need to lock again here
    
    if (!grabber_) {
        return;
    }

    try {
        if (confirmedDeliveryMode_ == FrameDeliveryMode::LatestFrame) {
            // Latest-buffer pattern: while more than one completed buffer is
            // pending, pop each stale one as a ScopedBuffer and let it destruct
            // immediately (requeues to the input FIFO). Never
            // flushEvent<NewBufferData>() here — that would discard events
            // without releasing the buffers they own, desynchronizing the
            // event queue from buffer ownership.
            while (grabber_->getPendingEventCount<NewBufferData>() > 1) {
                ScopedBuffer stale(*grabber_);
                intentionallyDiscardedFrames_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        ScopedBuffer buffer(*grabber_);
        uint8_t* basePtr = buffer.getInfo<uint8_t*>(gc::BUFFER_INFO_BASE);
        const size_t imageSize = buffer.getInfo<size_t>(ge::BUFFER_INFO_CUSTOM_PART_SIZE);
        const size_t linePitch = buffer.getInfo<size_t>(ge::BUFFER_INFO_CUSTOM_LINE_PITCH);
        const uint64_t pixelFormat = buffer.getInfo<uint64_t>(gc::BUFFER_INFO_PIXELFORMAT);
        const uint64_t bufferTimestamp = buffer.getInfo<uint64_t>(gc::BUFFER_INFO_TIMESTAMP);
        const size_t delivered = buffer.getInfo<size_t>(ge::BUFFER_INFO_CUSTOM_NUM_DELIVERED_PARTS);

        std::vector<char> rawTimestamps = buffer.getInfo<std::vector<char>>(ge::BUFFER_INFO_CUSTOM_PART_TIMESTAMPS);
        const size_t tsCount = rawTimestamps.size() / sizeof(uint64_t);
        const uint64_t* timestamps = tsCount > 0 ? reinterpret_cast<const uint64_t*>(rawTimestamps.data()) : nullptr;

        // Validate the SDK-reported buffer against the geometry every frame
        // downstream will trust: on unplug/partial delivery the base pointer
        // can be null or the part size garbage, and copy_n below would
        // segfault (or resize() would throw a huge bad_alloc). Consumers
        // build strided views reading (height-1)*pitch + width bytes.
        const size_t effPitch = (linePitch == 0 ? static_cast<size_t>(width_) : linePitch);
        const size_t requiredBytes = (height_ > 0 && width_ > 0)
            ? (static_cast<size_t>(height_) - 1) * effPitch + static_cast<size_t>(width_)
            : 0;
        if (basePtr == nullptr || imageSize == 0 || requiredBytes == 0 ||
            imageSize < requiredBytes) {
            SPDLOG_WARN("EGrabberCamera: rejected delivered buffer (base={}, partSize={}, "
                        "geometry {}x{} pitch={} needs {} bytes)",
                        static_cast<const void*>(basePtr), imageSize, width_, height_,
                        effPitch, requiredBytes);
            return;
        }

        for (size_t idx = 0; idx < delivered; ++idx) {
            const uint8_t* partPtr = basePtr + idx * imageSize;

            Frame frame;
            frame.width = width_;
            frame.height = height_;
            frame.pixelFormat = pixelFormat;
            frame.linePitch = linePitch == 0 ? static_cast<size_t>(width_) : linePitch;
            frame.timestamp = (timestamps && idx < tsCount) ? timestamps[idx] : bufferTimestamp;
            frame.data.resize(imageSize);
            std::copy_n(partPtr, imageSize, frame.data.begin());

            pendingFrames_.push_back(std::move(frame));
        }
    } catch (const gentl_error& e) {
        if (e.gc_err == gc::GC_ERR_ABORT) {
            // Expected during stop; do nothing
            SPDLOG_DEBUG("EGrabberCamera replenish aborted (expected during stop): {}", e.what());
            return;
        }
        throw;
    }
}

bool EGrabberCamera::checkDeviceHealth() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (!grabber_) {
        SPDLOG_DEBUG("Device health check: grabber not initialized");
        return false;
    }
    
    try {
        // Try to read a simple property to verify device is responsive
        std::string model = grabber_->getString<DeviceModule>("DeviceModelName");
        SPDLOG_TRACE("Device health check passed: {}", model);
        return true;
    } catch (const gentl_error& e) {
        SPDLOG_WARN("Device health check failed with GenTL error (code={}): {}", 
                   static_cast<int>(e.gc_err), e.what());
        return false;
    } catch (const std::exception& ex) {
        SPDLOG_WARN("Device health check failed: {}", ex.what());
        return false;
    }
}

void EGrabberCamera::configureTriggerOutput(const std::string& lineSelector) {
    std::lock_guard<std::mutex> triggerLock(triggerMutex_);
    triggerLineSelector_ = lineSelector;
    triggerConfigured_ = true;
    triggerLineApplied_ = false; // re-select on the next pulse
    SPDLOG_INFO("EGrabberCamera: trigger output configured on {}", lineSelector);
}

bool EGrabberCamera::setTriggerOutput(bool high) {
    // triggerMutex_ (not stateMutex_) pins grabber_ alive for the duration of
    // the call: stop() resets grabber_ under the same lock, so a trigger pulse
    // racing a camera stop fails cleanly instead of touching a destroyed
    // grabber. InterfaceModule operations themselves are thread-safe vs
    // StreamModule (frame grabbing on the capture thread), and stateMutex_ is
    // deliberately avoided — stop() holds it across ~360 ms of teardown sleeps.
    std::lock_guard<std::mutex> triggerLock(triggerMutex_);
    if (!triggerConfigured_ || !running_.load(std::memory_order_acquire) || !grabber_) return false;
    try {
        // LineSelector is GenApi nodemap state on this grabber_ instance —
        // select once after start()/reconfigure, then each pulse edge is a
        // single LineSource register write (halves PCIe transactions per
        // pulse, which dominate onset latency and pulse width).
        if (!triggerLineApplied_) {
            grabber_->setString<Euresys::InterfaceModule>("LineSelector", triggerLineSelector_);
            triggerLineApplied_ = true;
        }
        grabber_->setString<Euresys::InterfaceModule>("LineSource", high ? "High" : "Low");
        return true;
    } catch (const std::exception& ex) {
        triggerLineApplied_ = false; // unknown selector state: re-select next pulse
        SPDLOG_WARN("EGrabberCamera trigger output failed: {}", ex.what());
        return false;
    }
}

#else

EGrabberCamera::EGrabberCamera() = default;

EGrabberCamera::EGrabberCamera(int interfaceIndex, int deviceIndex)
    : hasSelection_(true),
      selectedInterfaceIndex_(interfaceIndex),
      selectedDeviceIndex_(deviceIndex) {}

EGrabberCamera::~EGrabberCamera() {
    stop();
}

void EGrabberCamera::applyConfig(const CameraConfig& config) {
    config_ = config;
}

bool EGrabberCamera::start() {
    SPDLOG_WARN("EGrabberCamera::start unavailable on this platform (EGrabber SDK is Windows-only)");
    running_ = false;
    return false;
}

void EGrabberCamera::stop() {
    running_ = false;
    pendingFrames_.clear();
}

bool EGrabberCamera::grabFrame(Frame& out) {
    (void)out;
    return false;
}

bool EGrabberCamera::pollStats(CameraStats& out) const {
    (void)out;
    return false;
}

// Stub camera cannot start, so it reports the defaults (LatestFrame
// unsupported, no queue observability) rather than mirroring the SDK path.
FrameDeliveryCapabilities EGrabberCamera::deliveryCapabilities() const {
    return {};
}

TimestampDescriptor EGrabberCamera::timestampDescriptor() const {
    return {}; // stub: no frames, undeclared
}

FrameDeliveryMode EGrabberCamera::activeDeliveryMode() const {
    return FrameDeliveryMode::EveryFrame;
}

bool EGrabberCamera::pollAcquisitionQueueStats(AcquisitionQueueStats& out) const {
    (void)out;
    return false;
}

void EGrabberCamera::replenishPendingFrames() {}

bool EGrabberCamera::checkDeviceHealth() const {
    return false;
}

void EGrabberCamera::configureTriggerOutput(const std::string& lineSelector) {
    (void)lineSelector;
}

bool EGrabberCamera::setTriggerOutput(bool high) {
    (void)high;
    return false;
}

#endif

} // namespace camera::common


