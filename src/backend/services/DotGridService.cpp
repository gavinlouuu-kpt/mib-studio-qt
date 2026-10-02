#include "backend/services/DotGridService.h"

#include "backend/playback/FrameStore.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace backend::services {

namespace {

bool useRegistry(const DotGridService::Config& c) {
    return c.codebookPath.empty() && c.registry && !c.registry->empty();
}

bool sameCodebookSource(const DotGridService::Config& a, const DotGridService::Config& b) {
    if (a.codebookPath != b.codebookPath) return false;
    if (!a.codebookPath.empty()) return true;
    if (useRegistry(a) != useRegistry(b)) return false;
    if (useRegistry(a)) return a.registry->fingerprint() == b.registry->fingerprint();
    const auto& p = a.codebook;
    const auto& q = b.codebook;
    return p.seed == q.seed && p.columns == q.columns && p.rows == q.rows &&
           p.pitchUm == q.pitchUm && p.dotDiameterUm == q.dotDiameterUm &&
           p.displacementUm == q.displacementUm && p.originXUm == q.originXUm &&
           p.originYUm == q.originYUm;
}

} // namespace

DotGridService::DotGridService() = default;

DotGridService::~DotGridService() {
    stop();
}

void DotGridService::setFrameStore(std::shared_ptr<playback::FrameStore> store) {
    std::lock_guard<std::mutex> lock(frameStoreMutex_);
    frameStore_ = std::move(store);
}

bool DotGridService::setConfig(const Config& config, std::string* errorOut) {
    std::shared_ptr<const dotgrid::Registry> registry;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        if (registry_ && sameCodebookSource(config_, config)) registry = registry_;
    }
    if (!registry) {
        try {
            if (!config.codebookPath.empty()) {
                dotgrid::Codebook cb;
                std::string err;
                if (!dotgrid::Codebook::loadJson(config.codebookPath, cb, &err)) {
                    SPDLOG_WARN("DotGridService: cannot load codebook '{}': {}",
                                config.codebookPath, err);
                    if (errorOut) *errorOut = err;
                    return false;
                }
                registry = std::make_shared<const dotgrid::Registry>(dotgrid::Registry::single(
                    std::make_shared<const dotgrid::Codebook>(std::move(cb))));
            } else if (useRegistry(config)) {
                registry = config.registry;
            } else {
                registry = std::make_shared<const dotgrid::Registry>(dotgrid::Registry::single(
                    std::make_shared<const dotgrid::Codebook>(
                        dotgrid::Codebook::generate(config.codebook))));
            }
        } catch (const std::exception& e) {
            SPDLOG_WARN("DotGridService: invalid codebook parameters: {}", e.what());
            if (errorOut) *errorOut = e.what();
            return false;
        }
        for (const auto& d : registry->designs()) {
            const auto& p = d.codebook->params();
            SPDLOG_INFO("DotGridService: design '{}' ready (seed={}, {}x{} nodes, pitch={}um, "
                        "dot={}um, shift={}um, chips={})",
                        d.id.empty() ? d.codebook->designName() : d.id, p.seed, p.columns, p.rows,
                        p.pitchUm, p.dotDiameterUm, p.displacementUm, d.codebook->chips().size());
        }
    }
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
        if (registry_ != registry) {
            registry_ = registry;
            decoder_ = std::make_shared<const dotgrid::Decoder>(registry_);
        }
        enabled_.store(config.enabled, std::memory_order_release);
    }
    wake();
    return true;
}

DotGridService::Config DotGridService::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

bool DotGridService::hasCodebook() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return registry_ && !registry_->empty();
}

std::shared_ptr<const dotgrid::Registry> DotGridService::activeRegistry() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return registry_;
}

void DotGridService::setPaused(bool paused) {
    if (paused_.exchange(paused, std::memory_order_acq_rel) == paused) return;
    SPDLOG_INFO("DotGridService: {}", paused ? "paused (no view)" : "resumed");
    if (!paused) wake(); // decode the current frame now, not one interval later
}

void DotGridService::wake() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        wakeRequested_ = true;
    }
    wakeCv_.notify_all();
}

void DotGridService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::thread([this] { loop(); });
    SPDLOG_INFO("DotGridService: started");
}

void DotGridService::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    wake();
    if (thread_.joinable()) thread_.join();
    SPDLOG_INFO("DotGridService: stopped (attempts={}, successes={})", attempts_.load(),
                successes_.load());
}

bool DotGridService::getLatestPose(Pose& out) const {
    std::lock_guard<std::mutex> lock(poseMutex_);
    if (!hasPose_) return false;
    out = latestPose_;
    return true;
}

void DotGridService::setPoseCallback(PoseCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback_ = std::move(callback);
}

bool DotGridService::frameToGray(const playback::Frame& frame, cv::Mat& out) {
    if (frame.width == 0 || frame.height == 0 || frame.data.empty()) return false;
    const int w = static_cast<int>(frame.width), h = static_cast<int>(frame.height);
    const int bits = static_cast<int>((frame.pixelFormat >> 16) & 0xFF); // PFNC bits-per-pixel byte
    const size_t bytesPerPx = bits > 8 ? 2 : 1;
    const size_t pitch = frame.linePitch == 0 ? frame.width * bytesPerPx : frame.linePitch;
    if (frame.data.size() < pitch * (frame.height - 1) + frame.width * bytesPerPx) return false;
    const cv::Mat view(h, w, bytesPerPx == 2 ? CV_16UC1 : CV_8UC1,
                       const_cast<uint8_t*>(frame.data.data()), pitch);
    if (bytesPerPx == 2)
        cv::normalize(view, out, 0, 255, cv::NORM_MINMAX, CV_8U);
    else
        out = view.clone();
    return true;
}

DotGridService::Pose DotGridService::decodeImage(const cv::Mat& gray, uint64_t frameIndex,
                                                 uint64_t timestampNs) const {
    std::shared_ptr<const dotgrid::Decoder> decoder;
    dotgrid::DecoderConfig dc;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        decoder = decoder_;
        dc.umPerPxHint = config_.umPerPxHint;
        dc.minVotes = config_.minVotes;
        dc.minAgreement = config_.minAgreement;
    }
    Pose pose;
    pose.frameIndex = frameIndex;
    pose.timestampNs = timestampNs;
    pose.imageWidth = gray.cols;
    pose.imageHeight = gray.rows;
    if (!decoder) {
        pose.reason = "no codebook";
        return pose;
    }
    dotgrid::DecodeResult r = decoder->decode(gray, dc);
    pose.valid = r.ok;
    pose.reason = r.reason;
    pose.centreXUm = r.centreXUm;
    pose.centreYUm = r.centreYUm;
    pose.thetaDeg = r.thetaDeg;
    pose.umPerPx = r.umPerPx;
    pose.mirrored = r.mirrored;
    pose.chip = r.chip;
    pose.designId = r.designId;
    pose.designName = r.designName;
    pose.votes = r.votes;
    pose.dots = r.dots;
    pose.agreement = r.agreement;
    pose.residualPx = r.residualPx;
    pose.decodeMs = r.decodeMs;
    std::memcpy(pose.pixelToWafer, r.pixelToWafer, sizeof(pose.pixelToWafer));
    pose.dotsPx = std::move(r.dotsPx);
    return pose;
}

void DotGridService::publish(const Pose& pose) {
    {
        std::lock_guard<std::mutex> lock(poseMutex_);
        latestPose_ = pose;
        hasPose_ = true;
    }
    PoseCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cb = callback_;
    }
    if (cb) cb(pose);
}

void DotGridService::loop() {
    uint64_t lastIndex = 0;
    bool haveLast = false;
    while (running_.load(std::memory_order_acquire)) {
        int intervalMs = 250;
        bool enabled = false;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            intervalMs = std::max(10, config_.intervalMs);
            enabled = config_.enabled && static_cast<bool>(decoder_) &&
                      !paused_.load(std::memory_order_acquire);
        }
        if (enabled) {
            std::shared_ptr<playback::FrameStore> store;
            {
                std::lock_guard<std::mutex> lock(frameStoreMutex_);
                store = frameStore_;
            }
            if (store && store->committedCount() > 0) {
                const uint64_t latest = store->latestCommittedIndex();
                if (!haveLast || latest != lastIndex) {
                    playback::Frame frame;
                    if (store->getByWriteIndex(latest, frame)) {
                        cv::Mat gray;
                        if (frameToGray(frame, gray)) {
                            attempts_.fetch_add(1, std::memory_order_relaxed);
                            Pose pose = decodeImage(gray, latest, frame.timestamp);
                            lastDecodeMs_.store(pose.decodeMs, std::memory_order_relaxed);
                            if (pose.valid) successes_.fetch_add(1, std::memory_order_relaxed);
                            publish(pose);
                        }
                    }
                    lastIndex = latest;
                    haveLast = true;
                }
            }
        }
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wakeCv_.wait_for(lock, std::chrono::milliseconds(intervalMs), [this] {
            return wakeRequested_ || !running_.load(std::memory_order_acquire);
        });
        wakeRequested_ = false;
    }
}

} // namespace backend::services
