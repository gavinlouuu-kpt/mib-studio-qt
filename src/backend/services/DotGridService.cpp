#include "backend/services/DotGridService.h"

#include "backend/playback/FrameStore.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace backend::services {

namespace {

bool sameCodebookSource(const DotGridService::Config& a, const DotGridService::Config& b) {
    if (a.codebookPath != b.codebookPath) return false;
    if (!a.codebookPath.empty()) return true;
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
    std::shared_ptr<const dotgrid::Codebook> codebook;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        if (codebook_ && sameCodebookSource(config_, config)) codebook = codebook_;
    }
    if (!codebook) {
        try {
            dotgrid::Codebook cb;
            if (!config.codebookPath.empty()) {
                std::string err;
                if (!dotgrid::Codebook::loadJson(config.codebookPath, cb, &err)) {
                    SPDLOG_WARN("DotGridService: cannot load codebook '{}': {}",
                                config.codebookPath, err);
                    if (errorOut) *errorOut = err;
                    return false;
                }
            } else {
                cb = dotgrid::Codebook::generate(config.codebook);
            }
            codebook = std::make_shared<const dotgrid::Codebook>(std::move(cb));
        } catch (const std::exception& e) {
            SPDLOG_WARN("DotGridService: invalid codebook parameters: {}", e.what());
            if (errorOut) *errorOut = e.what();
            return false;
        }
        const auto& p = codebook->params();
        SPDLOG_INFO("DotGridService: codebook ready (seed={}, {}x{} nodes, pitch={}um, dot={}um, "
                    "shift={}um, chips={})",
                    p.seed, p.columns, p.rows, p.pitchUm, p.dotDiameterUm, p.displacementUm,
                    codebook->chips().size());
    }
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
        codebook_ = codebook;
        enabled_.store(config.enabled, std::memory_order_release);
        decoder_ = std::make_shared<const dotgrid::Decoder>(codebook_);
    }
    wakeCv_.notify_all();
    return true;
}

DotGridService::Config DotGridService::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

bool DotGridService::hasCodebook() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return static_cast<bool>(codebook_);
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
    wakeCv_.notify_all();
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
            enabled = config_.enabled && static_cast<bool>(decoder_);
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
        wakeCv_.wait_for(lock, std::chrono::milliseconds(intervalMs),
                         [this] { return !running_.load(std::memory_order_acquire); });
    }
}

} // namespace backend::services
