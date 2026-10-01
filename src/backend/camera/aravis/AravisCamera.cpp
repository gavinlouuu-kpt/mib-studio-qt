#include "backend/camera/aravis/AravisCamera.h"

#include <arv.h>
#include <glib.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace camera::aravis {
namespace {

std::mutex& aravisGlobalMutex()
{
    // Aravis owns the interface registry and the fake-interface switch as
    // process-global state. Never call arv_shutdown() from an adapter.
    static std::mutex mutex;
    return mutex;
}

std::string errorText(GError* error, const char* fallback)
{
    if (error == nullptr)
        return fallback;
    const std::string result = error->message != nullptr ? error->message : fallback;
    g_error_free(error);
    return result;
}

bool isFakeDeviceId(const char* id)
{
    return id != nullptr && std::string(id).rfind("Fake", 0) == 0;
}

} // namespace

AravisCamera::AravisCamera(AravisCameraOptions options)
    : options_(std::move(options))
{
    config_.bufferPartCount = 1;
    timestampDescriptor_.validity = common::TimestampValidity::Unavailable;
}

AravisCamera::~AravisCamera()
{
    stop();
}

std::vector<std::string> AravisCamera::enumerateDeviceIds(bool includeFake)
{
    std::lock_guard<std::mutex> globalLock(aravisGlobalMutex());
    if (includeFake)
        arv_enable_interface("Fake");
    arv_update_device_list();

    std::vector<std::string> ids;
    const unsigned int count = arv_get_n_devices();
    ids.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        const char* id = arv_get_device_id(i);
        if (id != nullptr && (includeFake || !isFakeDeviceId(id)))
            ids.emplace_back(id);
    }
    return ids;
}

void AravisCamera::applyConfig(const common::CameraConfig& config)
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    config_ = config;
    if (config_.numBuffers < 2) {
        config_.numBuffers = 2;
    }
    options_.streamBuffers = static_cast<std::size_t>(config_.numBuffers);
    if (config_.bufferPartCount != 1) {
        failLocked("aravis.unsupported_parts", "Aravis consumer supports exactly one image part");
    } else if (config_.deliveryMode == common::FrameDeliveryMode::LatestFrame) {
        failLocked("aravis.unsupported_delivery_mode",
                   "Aravis consumer currently supports EveryFrame delivery only");
    } else {
        failure_ = {};
    }
}

bool AravisCamera::failLocked(const char* code, const std::string& message)
{
    failure_ = {code, message};
    SPDLOG_WARN("AravisCamera [{}]: {}", code, message);
    return false;
}

bool AravisCamera::setFailure(const char* code, const std::string& message)
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return failLocked(code, message);
}

bool AravisCamera::openAndConfigureLocked()
{
    // The global lock covers discovery and fake-interface configuration only;
    // the adapter's SDK lock covers the newly opened objects afterwards.
    std::lock_guard<std::mutex> globalLock(aravisGlobalMutex());
    if (options_.useFake)
        arv_enable_interface("Fake");

    const std::string requestedId = options_.useFake && options_.deviceId.empty()
                                        ? std::string("Fake_1")
                                        : options_.deviceId;
    if (!options_.useFake && isFakeDeviceId(requestedId.c_str()))
        return failLocked("aravis.fake_not_enabled", "Fake Aravis interface requires MIB_ARAVIS_FAKE=1");

    std::string selectedId = requestedId;
    if (selectedId.empty()) {
        arv_update_device_list();
        for (unsigned int i = 0; i < arv_get_n_devices(); ++i) {
            const char* id = arv_get_device_id(i);
            if (id != nullptr && !isFakeDeviceId(id)) {
                selectedId = id;
                break;
            }
        }
        if (selectedId.empty())
            return failLocked("aravis.no_device", "No Aravis camera was discovered");
    }

    GError* error = nullptr;
    camera_ = arv_camera_new(selectedId.c_str(), &error);
    if (camera_ == nullptr) {
        return failLocked("aravis.device_not_found",
                          errorText(error, "Aravis camera open failed"));
    }

    // This first slice has a deliberately narrow, lossless contract. The
    // future PZ7035 producer can add negotiated formats without weakening it.
    arv_camera_set_pixel_format(camera_, ARV_PIXEL_FORMAT_MONO_8, &error);
    if (error != nullptr)
        return failLocked("aravis.pixel_format", errorText(error, "Cannot select Mono8"));

    if (options_.softwareTrigger) {
        arv_camera_set_trigger(camera_, "Software", &error);
        if (error != nullptr)
            return failLocked("aravis.software_trigger",
                              errorText(error, "Cannot configure Aravis software trigger"));
    }

    gint x = 0, y = 0, width = 0, height = 0;
    arv_camera_get_region(camera_, &x, &y, &width, &height, &error);
    if (error != nullptr || width <= 0 || height <= 0)
        return failLocked("aravis.geometry", errorText(error, "Invalid Aravis image geometry"));

    const guint payload = arv_camera_get_payload(camera_, &error);
    if (error != nullptr || payload == 0)
        return failLocked("aravis.payload", errorText(error, "Invalid Aravis payload size"));

    stream_ = arv_camera_create_stream(camera_, nullptr, nullptr, nullptr, &error);
    if (stream_ == nullptr)
        return failLocked("aravis.stream_create", errorText(error, "Cannot create Aravis stream"));

    const std::size_t bufferCount = std::clamp<std::size_t>(options_.streamBuffers, 2, 64);
    for (std::size_t i = 0; i < bufferCount; ++i) {
        ArvBuffer* buffer = arv_buffer_new_allocate(payload);
        if (buffer == nullptr)
            return failLocked("aravis.buffer_allocation", "Cannot allocate an Aravis stream buffer");
        arv_stream_push_buffer(stream_, buffer);
    }

    stats_ = {};
    queueStats_ = {};
    queueStats_.sdkInputBufferCount = bufferCount;
    queueStats_.inputBufferCountValid = true;
    queueStats_.completedQueueDepthValid = true;
    ++sessionGeneration_;
    timestampDescriptor_ = {};
    // Aravis exposes a camera timestamp, but its tick mapping is transport-
    // specific. Preserve it as opaque device ticks until a producer contract
    // proves its rate and host relationship.
    timestampDescriptor_.domain = common::ClockDomain::DeviceTicks;
    timestampDescriptor_.semantic = common::TimestampSemantic::DeviceCapture;
    timestampDescriptor_.validity = common::TimestampValidity::Unsupported;
    timestampDescriptor_.sessionGeneration = sessionGeneration_;
    return true;
}

bool AravisCamera::start()
{
    // SDK calls (including the bounded grab path) take sdkMutex_ first and
    // only then publish lifecycle state. Keep the same order here to avoid a
    // stop/start deadlock when a buffer completes concurrently.
    const uint64_t startGeneration = lifecycleGeneration_.load(std::memory_order_acquire);
    if (options_.beforeStartHook)
        options_.beforeStartHook();
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (stopInProgress_ || lifecycleGeneration_.load(std::memory_order_relaxed) != startGeneration)
        return false;
    if (running_.load(std::memory_order_acquire))
        return true;
    if (config_.bufferPartCount != 1)
        return failLocked("aravis.unsupported_parts", "Aravis consumer supports exactly one image part");
    if (config_.deliveryMode == common::FrameDeliveryMode::LatestFrame)
        return failLocked("aravis.unsupported_delivery_mode",
                          "Aravis consumer currently supports EveryFrame delivery only");

    releaseResourcesLocked();
    // A completed stop leaves cancellation set. A new start owns the new
    // lifecycle and clears it only after it has acquired both locks.
    stopRequested_.store(false, std::memory_order_release);
    if (!openAndConfigureLocked()) {
        releaseResourcesLocked();
        return false;
    }

    GError* error = nullptr;
    if (!arv_camera_start_acquisition(camera_, &error)) {
        const bool result = failLocked("aravis.acquisition_start",
                                       errorText(error, "Cannot start Aravis acquisition"));
        releaseResourcesLocked();
        return result;
    }
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    deliveredFrames_ = 0;
    failure_ = {};
    return true;
}

void AravisCamera::releaseResourcesLocked()
{
    if (stream_ != nullptr) {
        // Stop the device first so AcquisitionStop is not omitted when the
        // adapter is stopped outside CaptureService. The stream stop then
        // cancels any bounded receive wait before host resources are released.
        bool cameraStopped = false;
        if (camera_ != nullptr) {
            GError* stopError = nullptr;
            cameraStopped = arv_camera_stop_acquisition(camera_, &stopError);
            if (!cameraStopped) {
                // releaseResourcesLocked() is called with lifecycleMutex_
                // held by every lifecycle path. Do not call failLocked()
                // here: it is intentionally a lock-free helper and taking
                // the mutex again would deadlock exactly on this error path.
                failure_ = {"aravis.acquisition_stop",
                            errorText(stopError,
                                      "Aravis AcquisitionStop failed; host stream teardown follows")};
                SPDLOG_WARN("AravisCamera [{}]: {}", failure_.code, failure_.message);
            }
        }
        if (!cameraStopped)
            arv_stream_stop_acquisition(stream_, nullptr);
    }
    if (camera_ != nullptr) {
        g_object_unref(camera_);
        camera_ = nullptr;
    }
    if (stream_ != nullptr) {
        g_object_unref(stream_);
        stream_ = nullptr;
    }
}

void AravisCamera::stop()
{
    stopRequested_.store(true, std::memory_order_release);
    // Register cancellation before waiting for SDK ownership. A concurrent
    // start that has not yet acquired the lifecycle lock will see this flag
    // and cannot reopen resources behind this stop call.
    lifecycleGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        stopInProgress_ = true;
    }
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    // Reassert cancellation after taking the resource locks. A concurrent
    // start may have published running=true while this stop waited for SDK
    // ownership; stop must still win before releasing those resources.
    stopRequested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    releaseResourcesLocked();
    // Close the cancellation interval as a second generation edge. A start
    // that entered after the first edge but was waiting for SDK ownership must
    // still be rejected even if it observes stopInProgress_ after this stop
    // has already released its resources.
    lifecycleGeneration_.fetch_add(1, std::memory_order_acq_rel);
    stopInProgress_ = false;
}

bool AravisCamera::isRunning() const
{
    return running_.load(std::memory_order_acquire);
}

bool AravisCamera::grabFrame(common::Frame& out)
{
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    if (!running_.load(std::memory_order_acquire) || stopRequested_.load(std::memory_order_acquire) ||
        stream_ == nullptr)
        return false;

    const uint32_t timeoutMs = std::clamp(options_.popTimeoutMs, 1u, 1000u);
    ArvBuffer* buffer = arv_stream_timeout_pop_buffer(stream_,
                                                       static_cast<guint64>(timeoutMs) * 1000ULL);
    if (buffer == nullptr)
        return false;

    bool valid = false;
    if (arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS) {
        setFailure("aravis.incomplete_buffer", "Aravis returned an incomplete buffer");
    } else if (arv_buffer_get_payload_type(buffer) != ARV_BUFFER_PAYLOAD_TYPE_IMAGE ||
               arv_buffer_get_n_parts(buffer) != 1) {
        setFailure("aravis.unsupported_payload", "Aravis returned a non-single-part image payload");
    } else {
        gint width = 0, height = 0;
        arv_buffer_get_image_region(buffer, nullptr, nullptr, &width, &height);
        const ArvPixelFormat pixelFormat = arv_buffer_get_image_pixel_format(buffer);
        size_t byteSize = 0;
        const void* data = arv_buffer_get_image_data(buffer, &byteSize);
        gint xPadding = 0;
        arv_buffer_get_image_padding(buffer, &xPadding, nullptr);
        const std::size_t expectedPitch = static_cast<std::size_t>(width) +
                                          static_cast<std::size_t>(std::max(0, xPadding));
        const std::size_t expectedBytes = expectedPitch * static_cast<std::size_t>(height);
        if (pixelFormat != ARV_PIXEL_FORMAT_MONO_8 || width <= 0 || height <= 0 || data == nullptr ||
            expectedBytes > byteSize || expectedPitch < static_cast<std::size_t>(width)) {
            setFailure("aravis.geometry_mismatch", "Aravis buffer is not a valid Mono8 image");
        } else {
            out.width = static_cast<uint64_t>(width);
            out.height = static_cast<uint64_t>(height);
            out.pixelFormat = static_cast<uint64_t>(pixelFormat);
            out.linePitch = expectedPitch;
            out.timestamp = arv_buffer_get_timestamp(buffer);
            out.rawDeviceTicks = out.timestamp;
            out.data.resize(expectedBytes);
            std::memcpy(out.data.data(), data, expectedBytes);
            ++deliveredFrames_;
            queueStats_.deliveredFrames = deliveredFrames_;
            valid = true;
        }
    }
    // Every popped buffer is returned, including incomplete or rejected ones.
    arv_stream_push_buffer(stream_, buffer);
    return valid;
}

bool AravisCamera::pollStats(common::CameraStats& out) const
{
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    if (!running_.load(std::memory_order_acquire) || camera_ == nullptr)
        return false;
    GError* error = nullptr;
    const double fps = arv_camera_get_frame_rate(camera_, &error);
    if (error != nullptr) {
        g_error_free(error);
        return false;
    }
    out = stats_;
    out.frameRate = fps > 0.0 ? static_cast<uint64_t>(fps) : 0;
    out.dataRateMBps = 0;
    return true;
}

common::FrameDeliveryCapabilities AravisCamera::deliveryCapabilities() const
{
    common::FrameDeliveryCapabilities caps;
    caps.supportsEveryFrame = true;
    caps.supportsLatestFrame = false;
    caps.modeChangeRequiresRestart = true;
    caps.timestampsHostComparable = false;
    return caps;
}

common::FrameDeliveryMode AravisCamera::activeDeliveryMode() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return config_.deliveryMode;
}

bool AravisCamera::pollAcquisitionQueueStats(common::AcquisitionQueueStats& out) const
{
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    if (stream_ == nullptr)
        return false;
    out = queueStats_;
    gint input = 0, output = 0, filling = 0;
    arv_stream_get_n_owned_buffers(stream_, &input, &output, &filling);
    out.sdkInputBufferCount = input > 0 ? static_cast<size_t>(input) : 0;
    out.sdkCompletedQueueDepth = output > 0 ? static_cast<size_t>(output) : 0;
    out.inputBufferCountValid = true;
    out.completedQueueDepthValid = true;
    guint64 failures = 0, underruns = 0;
    arv_stream_get_statistics(stream_, nullptr, &failures, &underruns);
    out.transportLostFrames = failures;
    out.bufferUnderruns = underruns;
    out.transportLossValid = true;
    out.underrunsValid = true;
    return true;
}

bool AravisCamera::checkDeviceHealth() const
{
    return isRunning();
}

bool AravisCamera::softTrigger()
{
    std::lock_guard<std::mutex> sdkLock(sdkMutex_);
    if (!running_.load(std::memory_order_acquire) || camera_ == nullptr)
        return false;
    GError* error = nullptr;
    if (!arv_camera_is_software_trigger_supported(camera_, &error)) {
        if (error != nullptr)
            g_error_free(error);
        return false;
    }
    arv_camera_software_trigger(camera_, &error);
    if (error != nullptr) {
        g_error_free(error);
        return false;
    }
    return true;
}

common::CameraFailure AravisCamera::lastFailure() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return failure_;
}

common::TimestampDescriptor AravisCamera::timestampDescriptor() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return timestampDescriptor_;
}

} // namespace camera::aravis
