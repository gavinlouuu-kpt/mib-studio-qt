#include "backend/pz/PzBridgePreviewCamera.h"

#include <spdlog/spdlog.h>

namespace backend::pz {

namespace {
constexpr uint64_t kMono8 = 0x01080001u; // PFNC Mono8
}

PzBridgePreviewCamera::PzBridgePreviewCamera(processing::IExecutionProvider& provider,
                                             processing::pz::BridgePreviewConfig config,
                                             std::chrono::milliseconds timeout, unsigned maxConsecutiveTimeouts,
                                             unsigned maxStartupTimeouts)
    : provider_(provider), config_(config), timeout_(timeout), maxTimeouts_(maxConsecutiveTimeouts),
      maxStartupTimeouts_(maxStartupTimeouts) {}

PzBridgePreviewCamera::~PzBridgePreviewCamera() { stop(); }

void PzBridgePreviewCamera::fail(const std::string& code, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        failure_ = {code, message};
    }
    running_.store(false);
    SPDLOG_WARN("PzBridgePreviewCamera: {}", message);
}

bool PzBridgePreviewCamera::start() {
    std::string error;
    if (!provider_.startPreview(config_, &error)) {
        fail("pz.preview_start", "Align preview did not start: " + error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        failure_ = {};
    }
    lastFrameId_ = 0;
    timeouts_ = 0;
    frames_.store(0);
    startedAt_ = std::chrono::steady_clock::now();
    running_.store(true);
    return true;
}

void PzBridgePreviewCamera::stop() {
    if (running_.exchange(false)) provider_.stopPreview();
}

bool PzBridgePreviewCamera::grabFrame(::camera::common::Frame& out) {
    if (!running_.load()) return false;
    processing::pz::BridgePreviewImage image;
    std::string error;
    if (!provider_.fetchPreview(lastFrameId_, timeout_, image, &error)) {
        const bool lost = error.find("left ARMED/RUNNING") != std::string::npos ||
                          error.find("not configured") != std::string::npos;
        const unsigned limit = frames_.load() == 0 ? maxStartupTimeouts_ : maxTimeouts_;
        if (lost || ++timeouts_ >= limit) {
            fail(lost ? "pz.preview_lost" : "pz.preview_timeout", "Align preview: " + error);
            provider_.stopPreview();
        }
        return false;
    }
    timeouts_ = 0;
    lastFrameId_ = image.frameId;
    out.width = image.width;
    out.height = image.height;
    out.pixelFormat = kMono8;
    out.linePitch = image.width;
    out.timestamp = image.frameId; // the sensor frame index; no host-comparable clock
    out.rawDeviceTicks = image.frameId;
    out.data = std::move(image.pixels);
    frames_.fetch_add(1);
    return true;
}

bool PzBridgePreviewCamera::pollStats(::camera::common::CameraStats& out) const {
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count();
    out.frameRate = seconds > 0 ? static_cast<uint64_t>(frames_.load() / seconds + 0.5) : 0;
    out.dataRateMBps = out.frameRate * config_.slotBytes() / 1000000;
    return true;
}

::camera::common::CameraFailure PzBridgePreviewCamera::lastFailure() const {
    std::lock_guard<std::mutex> lock(failureMutex_);
    return failure_;
}

} // namespace backend::pz
