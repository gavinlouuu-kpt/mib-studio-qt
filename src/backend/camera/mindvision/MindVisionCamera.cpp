// MindVision ICamera on top of the injectable SDK seam (MindVisionSdk.h).
//
// Safety contracts (issues #365/#366):
//  - start() fails closed unless the ISP output format is set to Mono8 AND
//    read back as Mono8, and the session geometry passes checked validation.
//    The destination buffer is allocated to exactly the validated size.
//  - Every incoming frame header is validated against the session allocation
//    before CameraImageProcess; a mismatch faults the stream (never resizes
//    the buffer under the SDK, never converts into an undersized buffer).
//  - stop() waits (bounded) for in-flight SDK operations before CameraUnInit;
//    if they do not drain, the handle is abandoned rather than freed under a
//    live call.
#include "backend/camera/mindvision/MindVisionCamera.h"
#include "backend/camera/mindvision/MindVisionConfig.h"

#include <fstream>
#include <iterator>
#include <QFile>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <string>

namespace camera::common
{

namespace mv = backend::camera::mindvision;

namespace
{
    constexpr std::uint64_t kMono8PfncCode = 0x01080001u;
    // uiTimeStamp is documented as 0.1 ms ticks (CameraDefine.h); the shared
    // Frame::timestamp is nanoseconds for this backend.
    constexpr std::uint64_t kTickNs = 100'000ULL;
}

// ---------------------------------------------------------------------------
// InFlightOp
// ---------------------------------------------------------------------------

MindVisionCamera::InFlightOp::InFlightOp(MindVisionCamera &owner) : owner_(owner)
{
    std::lock_guard<std::mutex> lock(owner_.stateMutex_);
    if (!owner_.running_.load(std::memory_order_acquire) || owner_.hCamera_ < 0)
    {
        return;
    }
    handle_ = owner_.hCamera_;
    mode_ = owner_.confirmedDeliveryMode_;
    maxDrain_ = owner_.config_.numBuffers;
    ++owner_.inFlightOps_;
}

MindVisionCamera::InFlightOp::~InFlightOp()
{
    if (handle_ < 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(owner_.stateMutex_);
        --owner_.inFlightOps_;
    }
    owner_.inFlightCv_.notify_all();
}

// ---------------------------------------------------------------------------

MindVisionCamera::MindVisionCamera(int cameraIndex, std::string configPath,
                                   std::shared_ptr<const SdkOps> sdk)
    : sdk_(sdk ? std::move(sdk) : mv::realMindVisionSdk()),
      cameraIndex_(cameraIndex),
      configPath_(std::move(configPath))
{
}

MindVisionCamera::~MindVisionCamera()
{
    stop();
}

void MindVisionCamera::applyConfig(const CameraConfig &config)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    config_ = config;
}

void MindVisionCamera::recordFailure(const std::string &code, const std::string &message)
{
    lastFailure_.code = code;
    lastFailure_.message = message;
    SPDLOG_ERROR("MindVisionCamera: {} ({})", message, code);
}

CameraFailure MindVisionCamera::lastFailure() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastFailure_;
}

mv::SessionGeometry MindVisionCamera::sessionGeometry() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return sessionGeometry_;
}

bool MindVisionCamera::applyJsonConfig(int hCamera)
{
    if (configPath_.empty())
    {
        return true;
    }

    std::ifstream file(configPath_, std::ios::binary);
    if (!file)
    {
        recordFailure("mindvision.config_unreadable",
                      "cannot open MindVision config file " + configPath_);
        return false;
    }

    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    const auto parsed = mv::parseConfig(bytes);
    if (!parsed.ok)
    {
        recordFailure("mindvision.config_invalid", parsed.error + " in " + configPath_);
        return false;
    }
    for (const auto &warning : parsed.warnings)
    {
        SPDLOG_WARN("{} (in {})", warning, configPath_);
    }

    configuredTriggerMode_ = parsed.config.triggerMode;
    if (!sdk_->applyConfig(hCamera, parsed.config))
    {
        recordFailure("mindvision.config_apply_failed",
                      "MindVision SDK rejected the camera configuration from " + configPath_);
        return false;
    }
    return true;
}

bool MindVisionCamera::start()
{
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (running_.load(std::memory_order_acquire))
    {
        return true;
    }
    lastFailure_ = {};
    sessionGeometry_ = {};

    mv::SdkStatus status = sdk_->sdkInit();
    if (status == mv::kSdkUnavailable)
    {
        recordFailure("mindvision.sdk_unavailable",
                      "MindVision SDK is unavailable in this build/runtime");
        return false;
    }
    if (status != mv::kSdkSuccess)
    {
        SPDLOG_WARN("MindVisionCamera: CameraSdkInit returned {}", status);
    }

    int count = 0;
    status = sdk_->enumerate(count);
    if (status != mv::kSdkSuccess || count <= 0)
    {
        recordFailure("mindvision.no_devices",
                      "CameraEnumerateDevice failed (status=" + std::to_string(status) +
                          ", count=" + std::to_string(count) + ")");
        return false;
    }

    if (cameraIndex_ < 0 || cameraIndex_ >= count)
    {
        recordFailure("mindvision.index_out_of_range",
                      "cameraIndex=" + std::to_string(cameraIndex_) + " out of range (found " +
                          std::to_string(count) + ")");
        return false;
    }

    int hCamera = -1;
    status = sdk_->init(cameraIndex_, hCamera);
    if (status != mv::kSdkSuccess || hCamera < 0)
    {
        recordFailure("mindvision.init_failed",
                      "CameraInit failed (status=" + std::to_string(status) + ")");
        return false;
    }
    hCamera_ = hCamera;

    // Everything below owns an open handle; every failure path closes it.
    auto failClosed = [&](const std::string &code, const std::string &message) {
        recordFailure(code, message);
        if (outBuffer_)
        {
            sdk_->alignFree(outBuffer_);
            outBuffer_ = nullptr;
            outBufferBytes_ = 0;
        }
        sdk_->unInit(hCamera_);
        hCamera_ = -1;
        sessionGeometry_ = {};
        return false;
    };

    mv::SdkCapability cap{};
    status = sdk_->getCapability(hCamera_, cap);
    if (status != mv::kSdkSuccess)
    {
        return failClosed("mindvision.capability_failed",
                          "CameraGetCapability failed (status=" + std::to_string(status) + ")");
    }

    if (!applyJsonConfig(hCamera_))
    {
        // applyJsonConfig recorded the specific failure.
        return failClosed(lastFailure_.code, lastFailure_.message);
    }

    int width = 0;
    int height = 0;
    status = sdk_->getImageResolution(hCamera_, width, height);
    if (status != mv::kSdkSuccess)
    {
        return failClosed("mindvision.resolution_failed",
                          "CameraGetImageResolution failed (status=" + std::to_string(status) + ")");
    }

    // Required Mono8 output (issue #366): the pipeline is mono8-only and the
    // destination buffer is sized 1 byte/px. A failure to set the format is a
    // hard start failure — never "warn and continue" into an undersized
    // buffer. The effective format is then read back and must match.
    status = sdk_->setIspOutFormat(hCamera_, mv::kMediaTypeMono8);
    if (status != mv::kSdkSuccess)
    {
        return failClosed("mindvision.isp_format_rejected",
                          "CameraSetIspOutFormat(MONO8) failed (status=" +
                              std::to_string(status) +
                              "); acquisition requires a Mono8 ISP output");
    }
    std::uint32_t effectiveFormat = 0;
    status = sdk_->getIspOutFormat(hCamera_, effectiveFormat);
    if (status != mv::kSdkSuccess)
    {
        // Relied-upon SDK guarantee: CameraGetIspOutFormat returns the format
        // CameraImageProcess will write. Without a successful readback the
        // destination size cannot be proven, so refuse to stream.
        return failClosed("mindvision.isp_format_unverified",
                          "CameraGetIspOutFormat failed (status=" + std::to_string(status) +
                              "); cannot verify the ISP output format");
    }

    const auto validation = mv::validateSessionGeometry(width, height, effectiveFormat);
    if (!validation.ok())
    {
        return failClosed(std::string("mindvision.geometry.") + mv::toString(validation.fault),
                          validation.message);
    }

    outBuffer_ = sdk_->alignMalloc(validation.geometry.requiredBytes, 16);
    if (!outBuffer_)
    {
        return failClosed("mindvision.alloc_failed",
                          "CameraAlignMalloc(" +
                              std::to_string(validation.geometry.requiredBytes) + ") failed");
    }
    outBufferBytes_ = validation.geometry.requiredBytes;
    sessionGeometry_ = validation.geometry;

    status = sdk_->play(hCamera_);
    if (status != mv::kSdkSuccess)
    {
        return failClosed("mindvision.play_failed",
                          "CameraPlay failed (status=" + std::to_string(status) + ")");
    }

    frameCount_ = 0;
    intentionalDiscards_.store(0, std::memory_order_relaxed);
    geometryRejectedFrames_.store(0, std::memory_order_relaxed);
    startTime_ = std::chrono::steady_clock::now();

    // Confirm the delivery mode for this run. Mode changes require a full
    // stop() -> applyConfig() -> start() cycle (modeChangeRequiresRestart);
    // we never clear or reorder an active SDK queue mid-run.
    confirmedDeliveryMode_ = config_.deliveryMode;
    deliveryModeConfirmed_ = true;

    running_.store(true, std::memory_order_release);
    SPDLOG_INFO("MindVisionCamera: started (index={}, {}x{} mono8 verified, monoSensor={}, "
                "deliveryMode={}, buffer={} bytes)",
                cameraIndex_, width, height, cap.monoSensor, toString(confirmedDeliveryMode_),
                outBufferBytes_);
    return true;
}

void MindVisionCamera::closeHandleLocked(std::unique_lock<std::mutex> &lock)
{
    if (hCamera_ < 0)
    {
        return;
    }
    const int handle = hCamera_;
    // CameraStop first: it makes a blocked CameraGetImageBuffer return, so
    // in-flight operations drain quickly (their own timeout is 100 ms).
    sdk_->stop(handle);
    const bool drained = inFlightCv_.wait_for(lock, inFlightDrainTimeout_,
                                              [this] { return inFlightOps_ == 0; });
    if (!drained)
    {
        // Never uninitialize a handle that an SDK call is still using.
        // Abandoning (leaking) the handle and buffer is the fail-safe
        // outcome; the SDK call itself is bounded, so this indicates a
        // wedged driver and is reported loudly.
        SPDLOG_ERROR("MindVisionCamera: {} SDK operation(s) still in flight after {} ms; "
                     "abandoning handle {} instead of uninitializing under a live call",
                     inFlightOps_, inFlightDrainTimeout_.count(), handle);
        recordFailure("mindvision.inflight_drain_timeout",
                      "MindVision SDK call did not return before stop; handle abandoned");
        hCamera_ = -1;
        outBuffer_ = nullptr;
        outBufferBytes_ = 0;
        return;
    }
    if (outBuffer_)
    {
        sdk_->alignFree(outBuffer_);
        outBuffer_ = nullptr;
        outBufferBytes_ = 0;
    }
    sdk_->unInit(handle);
    hCamera_ = -1;
}

void MindVisionCamera::stop()
{
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (!running_.load(std::memory_order_acquire) && hCamera_ < 0)
    {
        return;
    }
    running_.store(false, std::memory_order_release);
    closeHandleLocked(lock);
    sessionGeometry_ = {};
    SPDLOG_INFO("MindVisionCamera: stopped");
}

void MindVisionCamera::faultStreamLocked(const std::string &code, const std::string &message)
{
    // Called with stateMutex_ held from the grab path. Marks the stream as
    // ended so CaptureService observes isRunning()==false and reports a
    // structured StreamEnded fault; the handle is closed by stop() (from the
    // capture thread's releaseCamera), which waits for this op to finish.
    recordFailure(code, message);
    running_.store(false, std::memory_order_release);
}

bool MindVisionCamera::grabFrame(Frame &out)
{
    while (true)
    {
        // One in-flight SDK operation per iteration; stop() waits for it.
        InFlightOp op(*this);
        if (!op.valid())
        {
            return false;
        }
        const int hCamera = op.handle();
        const FrameDeliveryMode mode = op.mode();

        mv::SdkFrameInfo frameHead{};
        std::uint8_t *pBuffer = nullptr;
        mv::SdkStatus status;

        if (mode == FrameDeliveryMode::LatestFrame && sdk_->getImageBufferNewest)
        {
            // SDK-side newest-frame retrieval: the SDK discards every completed
            // buffer older than the returned one before handing it out.
            // Limitation: those SDK-internal skips have no per-skip callback,
            // so intentionalDiscards_ cannot count them exactly on this path
            // (the drain fallback below does count exactly).
            status = sdk_->getImageBufferNewest(hCamera, frameHead, pBuffer, 100);
        }
        else if (mode == FrameDeliveryMode::LatestFrame)
        {
            // Fallback for SDK variants without the priority API: block for the
            // next completed buffer, then drain any further already-completed
            // buffers non-blocking (zero timeout), keeping only the newest.
            // Bounded by numBuffers so a producer outpacing us cannot starve
            // delivery. Exactly one buffer is held at any time; each superseded
            // buffer is released immediately and counted as an intentional
            // discard.
            status = sdk_->getImageBuffer(hCamera, frameHead, pBuffer, 100);
            if (status == mv::kSdkSuccess)
            {
                for (int i = 0; i < op.maxDrain(); ++i)
                {
                    mv::SdkFrameInfo newerHead{};
                    std::uint8_t *newerBuffer = nullptr;
                    const mv::SdkStatus drainStatus =
                        sdk_->getImageBuffer(hCamera, newerHead, newerBuffer, 0);
                    if (drainStatus != mv::kSdkSuccess)
                    {
                        break; // No newer completed buffer; keep the held one.
                    }
                    sdk_->releaseImageBuffer(hCamera, pBuffer);
                    intentionalDiscards_.fetch_add(1, std::memory_order_relaxed);
                    pBuffer = newerBuffer;
                    frameHead = newerHead;
                }
            }
        }
        else
        {
            // EveryFrame: ordered retrieval, never skip. The priority API is
            // deliberately not used here.
            status = sdk_->getImageBuffer(hCamera, frameHead, pBuffer, 100);
        }

        if (status == mv::kSdkTimeout)
        {
            continue;
        }
        if (status != mv::kSdkSuccess)
        {
            SPDLOG_WARN("MindVisionCamera: frame retrieval returned {} (mode={})",
                        status, toString(mode));
            return false;
        }

        mv::SdkStatus procStatus = mv::kSdkSuccess;
        bool copied = false;
        bool faulted = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!running_.load(std::memory_order_acquire) || !outBuffer_)
            {
                sdk_->releaseImageBuffer(hCamera, pBuffer);
                return false;
            }
            // Destination-size proof (issue #366): the header must match the
            // validated session allocation. On mismatch the frame is rejected
            // BEFORE conversion and the stream is faulted for controlled
            // reconfiguration; the buffer is never resized here.
            const auto check =
                mv::validateIncomingFrame(sessionGeometry_, frameHead, outBufferBytes_);
            if (!check.ok())
            {
                geometryRejectedFrames_.fetch_add(1, std::memory_order_relaxed);
                sdk_->releaseImageBuffer(hCamera, pBuffer);
                faultStreamLocked(std::string("mindvision.frame.") + mv::toString(check.fault),
                                  check.message);
                faulted = true;
            }
            else
            {
                procStatus = sdk_->imageProcess(hCamera, pBuffer, outBuffer_, frameHead);
                if (procStatus == mv::kSdkSuccess)
                {
                    // Copy out of outBuffer_ while still holding stateMutex_ —
                    // stop() frees the buffer under the same lock, so an
                    // unlocked read here would race a concurrent stop.
                    // The post-process header may report the output size; it
                    // must still equal the session allocation.
                    if (frameHead.width != sessionGeometry_.width ||
                        frameHead.height != sessionGeometry_.height)
                    {
                        geometryRejectedFrames_.fetch_add(1, std::memory_order_relaxed);
                        sdk_->releaseImageBuffer(hCamera, pBuffer);
                        faultStreamLocked("mindvision.frame.frameGeometryMismatch",
                                          "CameraImageProcess reported output geometry " +
                                              std::to_string(frameHead.width) + "x" +
                                              std::to_string(frameHead.height) +
                                              " different from the session allocation");
                        faulted = true;
                    }
                    else
                    {
                        out.width = static_cast<std::uint64_t>(sessionGeometry_.width);
                        out.height = static_cast<std::uint64_t>(sessionGeometry_.height);
                        out.pixelFormat = kMono8PfncCode;
                        out.linePitch = static_cast<std::size_t>(sessionGeometry_.width);
                        out.timestamp = static_cast<std::uint64_t>(frameHead.timeStamp) * kTickNs;
                        out.rawDeviceTicks = frameHead.timeStamp;
                        out.data.assign(outBuffer_, outBuffer_ + sessionGeometry_.requiredBytes);
                        ++frameCount_;
                        copied = true;
                    }
                }
            }
        }

        if (faulted)
        {
            return false;
        }

        sdk_->releaseImageBuffer(hCamera, pBuffer);

        if (!copied)
        {
            SPDLOG_WARN("MindVisionCamera: CameraImageProcess returned {}", procStatus);
            continue;
        }
        return true;
    }
}

bool MindVisionCamera::pollStats(CameraStats &out) const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!running_.load(std::memory_order_acquire))
    {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - startTime_).count();
    if (elapsed > 0.0)
    {
        lastStats_.frameRate = static_cast<std::uint64_t>(static_cast<double>(frameCount_) / elapsed);
        const std::uint64_t bytesPerFrame = sessionGeometry_.requiredBytes;
        lastStats_.dataRateMBps = (lastStats_.frameRate * bytesPerFrame) / (1024ULL * 1024ULL);
    }
    out = lastStats_;
    return true;
}

TimestampDescriptor MindVisionCamera::timestampDescriptor() const
{
    TimestampDescriptor d;
    d.domain = ClockDomain::DeviceTicks;
    d.ticksPerSecond = 1'000'000'000ULL; // normalized to ns at the grab boundary
    d.nativeTicksPerSecond = 1'000'000'000ULL / kTickNs; // 10 kHz (0.1 ms ticks)
    d.semantic = TimestampSemantic::DeviceCapture; // tSdkFrameHead::uiTimeStamp
    d.validity = TimestampValidity::Valid;
    d.counterBits = 32; // UINT uiTimeStamp
    return d;
}

FrameDeliveryCapabilities MindVisionCamera::deliveryCapabilities() const
{
    FrameDeliveryCapabilities caps;
    caps.supportsEveryFrame = true;
    caps.supportsLatestFrame = true;
    caps.modeChangeRequiresRestart = true;
    // frameHead.uiTimeStamp is a device tick counter, not the host monotonic
    // clock, so frame age cannot be estimated against Tools::getTimestamp().
    caps.timestampsHostComparable = false;
    return caps;
}

FrameDeliveryMode MindVisionCamera::activeDeliveryMode() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return deliveryModeConfirmed_ ? confirmedDeliveryMode_ : config_.deliveryMode;
}

bool MindVisionCamera::pollAcquisitionQueueStats(AcquisitionQueueStats &out) const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!running_.load(std::memory_order_acquire) || hCamera_ < 0)
    {
        return false;
    }

    out = AcquisitionQueueStats{};
    out.deliveredFrames = frameCount_;
    out.intentionallyDiscardedFrames = intentionalDiscards_.load(std::memory_order_relaxed);

    // The MindVision SDK exposes no completed-queue depth, input-buffer count,
    // or underrun counter, so those fields stay 0 with their valid flags false
    // ("unknown", not "zero") as required by issue #333.

    // Transport loss is observable through the SDK's cumulative frame
    // statistics (tSdkFrameStatistic: iTotal/iCapture/iLost).
    mv::SdkFrameStatistic frameStat{};
    if (sdk_->getFrameStatistic(hCamera_, frameStat) == mv::kSdkSuccess)
    {
        out.transportLostFrames = frameStat.lost > 0 ? static_cast<uint64_t>(frameStat.lost) : 0;
        out.transportLossValid = true;
    }

    return true;
}

bool MindVisionCamera::checkDeviceHealth() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!running_.load(std::memory_order_acquire) || hCamera_ < 0)
    {
        return false;
    }

    if (configuredTriggerMode_ != 0)
    {
        // Triggered acquisition: the frame probe below would consume a real
        // triggered frame (data loss), and "no frame within 100ms" is the
        // normal idle state, so the probe carries no health signal anyway.
        // A valid running handle is the best available check.
        return true;
    }

    mv::SdkFrameInfo frameHead{};
    std::uint8_t *pBuffer = nullptr;
    const mv::SdkStatus status = sdk_->getImageBuffer(hCamera_, frameHead, pBuffer, 100);
    if (status == mv::kSdkSuccess)
    {
        sdk_->releaseImageBuffer(hCamera_, pBuffer);
        return true;
    }

    return status == mv::kSdkTimeout;
}

void MindVisionCamera::configureTriggerOutput(const std::string &lineSelector)
{
    (void)lineSelector;
    constexpr int kSortOutputIndex = 1;
    constexpr int kIoModeGpOutput = 3; // IOMODE_GP_OUTPUT (CameraDefine.h)

    std::lock_guard<std::mutex> lock(stateMutex_);
    if (hCamera_ < 0)
    {
        triggerOutputIndex_ = kSortOutputIndex;
        SPDLOG_WARN("MindVisionCamera: configureTriggerOutput called before camera open");
        return;
    }

    mv::SdkStatus status = sdk_->setOutputIoMode(hCamera_, kSortOutputIndex, kIoModeGpOutput);
    if (status != mv::kSdkSuccess)
    {
        SPDLOG_WARN("MindVisionCamera: CameraSetOutPutIOMode(OUT2, GP_OUTPUT) returned {}", status);
    }

    status = sdk_->setIoStateEx(hCamera_, kSortOutputIndex, 0);
    if (status != mv::kSdkSuccess)
    {
        SPDLOG_WARN("MindVisionCamera: CameraSetIOStateEx(OUT2, LOW) returned {}", status);
    }

    triggerOutputIndex_ = kSortOutputIndex;
    SPDLOG_INFO("MindVisionCamera: trigger output configured on OUT2 (index {})", kSortOutputIndex);
}

bool MindVisionCamera::setTriggerOutput(bool high)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (triggerOutputIndex_ < 0 || !running_.load(std::memory_order_acquire) || hCamera_ < 0)
    {
        return false;
    }

    const mv::SdkStatus status = sdk_->setIoStateEx(hCamera_, triggerOutputIndex_, high ? 1u : 0u);
    if (status != mv::kSdkSuccess)
    {
        SPDLOG_WARN("MindVisionCamera: CameraSetIOStateEx({}, {}) returned {}",
                    triggerOutputIndex_, high ? "HIGH" : "LOW", status);
        return false;
    }
    return true;
}

bool MindVisionCamera::softTrigger()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!running_.load(std::memory_order_acquire) || hCamera_ < 0)
    {
        return false;
    }

    const mv::SdkStatus status = sdk_->softTrigger(hCamera_);
    if (status != mv::kSdkSuccess)
    {
        SPDLOG_WARN("MindVisionCamera: CameraSoftTrigger returned {}", status);
        return false;
    }
    return true;
}

} // namespace camera::common
