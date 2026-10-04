#pragma once

// DotGridService: low-rate wafer localization from the live FrameStore.
//
// Owns one thread that samples the latest committed frame every intervalMs,
// runs the Qt-free dot-grid decoder on it, and publishes the resulting pose
// (wafer coordinates of the image centre, rotation, scale, mirror flag, chip
// id) through a mutex-protected snapshot and an optional callback. It never
// touches the capture or realtime processing threads: like the realtime
// drop-frames mode it jumps straight to the newest frame, so its cost is one
// frame copy plus one decode per interval. With a design registry it decodes
// against every registered design and the pose names the design it found. See
// knowledge_map/services/DotGridService.md.

#include "backend/processing/DotGridCodebook.h"
#include "backend/processing/DotGridCodec.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/processing/DotGridRegistry.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace backend::playback {
class FrameStore;
struct Frame;
} // namespace backend::playback

namespace backend::services {

class DotGridService {
public:
    struct Config {
        bool enabled{false};
        int intervalMs{250};
        double umPerPxHint{0.293};
        int minVotes{3};
        double minAgreement{0.9};
        // Codebook source, first match wins:
        //  1. codebookPath non-empty: that one codebook.json (explicit override);
        //  2. registry non-empty: every registered design (the normal case);
        //  3. otherwise one codebook generated from `codebook`.
        std::string codebookPath;
        std::shared_ptr<const dotgrid::Registry> registry;
        dotgrid::CodebookParams codebook;
    };

    struct Pose {
        bool valid{false};
        uint64_t frameIndex{0};
        uint64_t timestampNs{0};
        double centreXUm{0.0};
        double centreYUm{0.0};
        double thetaDeg{0.0};
        double umPerPx{0.0};
        bool mirrored{false};
        std::string chip;
        std::string designId; // registered design the frame belongs to
        std::string designName;
        // Core that decoded (ADR 0010): codec contract, core build, source.
        int codecContract{0};
        std::string coreVersion;
        std::string coreSource;
        int votes{0};
        int dots{0};
        double agreement{0.0};
        double residualPx{0.0};
        double decodeMs{0.0};
        std::string reason; // failure reason when !valid
        double pixelToWafer[6]{};
        std::vector<cv::Point2f> dotsPx;
        int imageWidth{0};
        int imageHeight{0};
    };

    using PoseCallback = std::function<void(const Pose&)>;

    DotGridService();
    ~DotGridService();

    DotGridService(const DotGridService&) = delete;
    DotGridService& operator=(const DotGridService&) = delete;

    void setFrameStore(std::shared_ptr<playback::FrameStore> store);

    // Applies the configuration; rebuilds the codebook when its source changed.
    // Returns false (and leaves the previous codebook) if a codebook file cannot
    // be loaded or the parameters are invalid.
    bool setConfig(const Config& config, std::string* errorOut = nullptr);
    Config getConfig() const;
    bool hasCodebook() const;
    // Designs the decoder currently tries (1 in single-codebook mode, 0 before setConfig).
    std::shared_ptr<const dotgrid::Registry> activeRegistry() const;
    // Active codec cores, one per contract (Phase 1: the bundled contract-1 core).
    std::vector<dotgrid::CodecIdentity> activeCodecs() const;
    bool isEnabled() const { return enabled_.load(std::memory_order_acquire); }

    // Runtime pause on top of Config::enabled, owned by the UI: the Overview
    // tab is the only view of the pose, so it pauses decoding whenever it is
    // not on screen (Experiment tab, other tabs, minimised window) and the
    // service spends no CPU next to an experiment. Not persisted; default off
    // (headless users and tests decode whenever enabled).
    void setPaused(bool paused);
    bool isPaused() const { return paused_.load(std::memory_order_acquire); }

    void start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Latest decode outcome (valid or not); false if nothing decoded yet.
    bool getLatestPose(Pose& out) const;
    // Bumped on every publish: a UI tick compares it before copying a Pose.
    uint64_t poseSequence() const { return poseSequence_.load(std::memory_order_acquire); }
    void setPoseCallback(PoseCallback callback);

    // Decode any image synchronously with the current codebook/config.
    Pose decodeImage(const cv::Mat& gray, uint64_t frameIndex = 0, uint64_t timestampNs = 0) const;

    // Metrics
    uint64_t decodeAttempts() const { return attempts_.load(std::memory_order_relaxed); }
    uint64_t decodeSuccesses() const { return successes_.load(std::memory_order_relaxed); }
    double lastDecodeMs() const { return lastDecodeMs_.load(std::memory_order_relaxed); }

    // Convert a FrameStore frame to a single-channel cv::Mat (copy). Public for tests.
    static bool frameToGray(const playback::Frame& frame, cv::Mat& out);

private:
    void loop();
    void publish(const Pose& pose);

    std::shared_ptr<playback::FrameStore> frameStore_;
    mutable std::mutex frameStoreMutex_;

    mutable std::mutex configMutex_;
    Config config_;
    std::shared_ptr<const dotgrid::Registry> registry_; // what decoder_ was built from
    dotgrid::CodecSet codecs_ = dotgrid::CodecSet::bundled();
    std::shared_ptr<const dotgrid::DesignDecoder> decoder_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex wakeMutex_;
    std::condition_variable wakeCv_;
    bool wakeRequested_{false}; // guarded by wakeMutex_; ends the interval wait early
    void wake();

    mutable std::mutex poseMutex_;
    Pose latestPose_;
    bool hasPose_{false};

    std::mutex callbackMutex_;
    PoseCallback callback_;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> paused_{false};
    std::atomic<uint64_t> configGeneration_{0}; // bumped by every accepted setConfig()
    std::atomic<uint64_t> poseSequence_{0};
    std::atomic<uint64_t> attempts_{0};
    std::atomic<uint64_t> successes_{0};
    std::atomic<double> lastDecodeMs_{0.0};
};

} // namespace backend::services
