#include "backend/camera/mock/MockCamera.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <thread>

namespace camera::mock
{

    namespace
    {
        constexpr uint64_t kPfncMono8 = 0x01080001;

        bool hasSupportedExtension(const std::filesystem::path &path)
        {
            static const std::vector<std::string> exts = {
                ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"};
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return std::find(exts.begin(), exts.end(), ext) != exts.end();
        }
    } // namespace

    MockCamera::MockCamera(MockCameraOptions options)
        : options_(std::move(options)) {}

    void MockCamera::applyConfig(const camera::common::CameraConfig &config)
    {
        config_ = config;
    }

    bool MockCamera::start()
    {
        refreshFileList();
        if (files_.empty())
        {
            SPDLOG_WARN("MockCamera: no images found in {}", options_.folder.string());
            running_ = false;
            return false;
        }

        // Preload all frames into memory for high-FPS playback
        preloadedFrames_.clear();
        if (!preloadFrames())
        {
            SPDLOG_ERROR("MockCamera: failed to preload frames from {}", options_.folder.string());
            running_ = false;
            return false;
        }
        if (preloadedFrames_.empty())
        {
            SPDLOG_WARN("MockCamera: no valid frames could be preloaded from {}", options_.folder.string());
            running_ = false;
            return false;
        }

        nextIndex_ = 0;
        running_ = true;
        lastFrameTime_ = std::chrono::steady_clock::now();
        stats_ = {};
        deliveredFrames_.store(0, std::memory_order_relaxed);

        SPDLOG_INFO("MockCamera started with {} files from {} (preloaded {} frames)",
                    files_.size(), options_.folder.string(), preloadedFrames_.size());
        return true;
    }

    void MockCamera::stop()
    {
        // Only flip the (atomic) running_ flag. The capture thread observes it in
        // grabFrame()/isRunning() and then stops touching the other members;
        // start() resets nextIndex_ before the next capture thread runs.
        // Resetting nextIndex_ here would race with the still-draining capture
        // thread (flagged by ThreadSanitizer).
        running_.store(false, std::memory_order_release);
    }

    bool MockCamera::grabFrame(camera::common::Frame &out)
    {
        if (!running_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (preloadedFrames_.empty())
        {
            SPDLOG_WARN("MockCamera: no preloaded frames available to stream");
            running_ = false;
            return false;
        }

        const auto interval = options_.frameInterval;
        if (interval > std::chrono::microseconds::zero())
        {
            // Hybrid wait: coarse sleep for long intervals, busy-wait for sub-millisecond precision
            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - lastFrameTime_;
            if (elapsed < interval)
            {
                auto remaining = interval - elapsed;
                // For >=2 ms, do a coarse sleep first to reduce CPU
                constexpr auto kCoarseThreshold = std::chrono::microseconds(2000);
                if (remaining >= kCoarseThreshold)
                {
                    // Leave ~1 ms for the fine wait to avoid oversleep on Windows
                    const auto coarse = remaining - std::chrono::milliseconds(1);
                    std::this_thread::sleep_for(coarse);
                }
                // Fine wait: busy-spin until target time
                const auto target = lastFrameTime_ + interval;
                do
                {
                    // Only yield when far from target; yielding too close can jitter
                    if ((target - std::chrono::steady_clock::now()) > std::chrono::microseconds(1000))
                    {
                        std::this_thread::yield();
                    }
                } while (std::chrono::steady_clock::now() < target);
            }
        }

        const camera::common::Frame &cached = preloadedFrames_[nextIndex_];

        const auto delivered = std::chrono::steady_clock::now();
        camera::common::Frame frameOut = cached;
        frameOut.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(delivered.time_since_epoch()).count());

        out = std::move(frameOut);

        nextIndex_ += 1;
        if (nextIndex_ >= preloadedFrames_.size())
        {
            if (options_.loopFiles)
            {
                nextIndex_ = 0;
            }
            else
            {
                running_ = false;
            }
        }

        const auto delta = delivered - lastFrameTime_;
        lastFrameTime_ = delivered;

        double fps = 0.0;
        const double seconds = std::chrono::duration<double>(delta).count();
        if (seconds > 0.0)
        {
            fps = 1.0 / seconds;
        }
        else if (interval > std::chrono::microseconds::zero())
        {
            fps = 1'000'000.0 / static_cast<double>(interval.count());
        }
        deliveredFrames_.fetch_add(1, std::memory_order_relaxed);
        stats_.frameRate = fps > 0.0 ? static_cast<uint64_t>(std::llround(fps)) : 0;
        stats_.dataRateMBps = (fps > 0.0 && !out.data.empty())
                                  ? static_cast<uint64_t>(std::llround(
                                        (static_cast<double>(out.data.size()) * fps) / 1'000'000.0))
                                  : 0;

        return true;
    }

    bool MockCamera::pollStats(camera::common::CameraStats &out) const
    {
        if (!running_.load(std::memory_order_acquire))
        {
            return false;
        }
        out = stats_;
        return true;
    }

    bool MockCamera::pollAcquisitionQueueStats(camera::common::AcquisitionQueueStats &out) const
    {
        out = {};
        out.deliveredFrames = deliveredFrames_.load(std::memory_order_relaxed);
        out.completedQueueDepthValid = true; // genuinely zero: frames are made on demand
        out.inputBufferCountValid = false;
        out.underrunsValid = false;
        out.transportLossValid = false;
        return true;
    }

    void MockCamera::configureTriggerOutput(const std::string &lineSelector)
    {
        SPDLOG_INFO("MockCamera: simulated trigger output configured on line '{}'", lineSelector);
    }

    bool MockCamera::setTriggerOutput(bool high)
    {
        const bool wasHigh = triggerLineHigh_.exchange(high, std::memory_order_acq_rel);
        if (high && !wasHigh)
        {
            triggerPulseCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    void MockCamera::setFrameInterval(std::chrono::microseconds interval)
    {
        options_.frameInterval = interval;
    }

    void MockCamera::setLooping(bool loop)
    {
        options_.loopFiles = loop;
    }

    void MockCamera::refreshFileList()
    {
        files_.clear();
        if (!std::filesystem::exists(options_.folder) || !std::filesystem::is_directory(options_.folder))
        {
            SPDLOG_WARN("MockCamera: folder {} does not exist or is not a directory", options_.folder.string());
            return;
        }

        // error_code overload: the folder can vanish between the exists()
        // check above and iteration; the throwing overload would propagate
        // std::filesystem_error out of start().
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(options_.folder, ec))
        {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc)
            {
                continue;
            }
            if (!hasSupportedExtension(entry.path()))
            {
                continue;
            }
            files_.push_back(entry.path());
        }
        if (ec)
        {
            SPDLOG_WARN("MockCamera: failed to enumerate {}: {}", options_.folder.string(), ec.message());
        }

        std::sort(files_.begin(), files_.end());
    }

    bool MockCamera::loadFrameFromPath(const std::filesystem::path &path, camera::common::Frame &frame)
    {
        // Decode with OpenCV (imgcodecs, already linked). Covers every extension
        // hasSupportedExtension() accepts (PNG/JPEG/BMP/TIFF). Force single-channel
        // 8-bit so the output is always PFNC Mono8, matching the real camera paths.
        // (Qt-free — part of epic #246 backend decoupling.)
        const cv::Mat img = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        if (img.empty() || img.data == nullptr)
        {
            SPDLOG_WARN("MockCamera: failed to decode {}", path.string());
            return false;
        }

        // Convert cv::Mat -> Frame. Rows are packed tightly (linePitch = width),
        // handling the non-continuous Mat case row-by-row via step[0].
        frame.width = static_cast<uint64_t>(img.cols);
        frame.height = static_cast<uint64_t>(img.rows);
        frame.pixelFormat = kPfncMono8;

        const size_t width = static_cast<size_t>(img.cols);
        const size_t height = static_cast<size_t>(img.rows);
        frame.linePitch = width;
        frame.data.resize(width * height);

        if (img.isContinuous())
        {
            std::memcpy(frame.data.data(), img.data, frame.data.size());
        }
        else
        {
            uint8_t *dst = frame.data.data();
            const uint8_t *src = img.data;
            for (int y = 0; y < img.rows; ++y)
            {
                std::memcpy(dst + static_cast<size_t>(y) * width,
                            src + static_cast<size_t>(y) * img.step[0], width);
            }
        }

        SPDLOG_DEBUG("MockCamera: loaded {} ({}x{}) via OpenCV", path.string(), img.cols, img.rows);
        return true;
    }

    bool MockCamera::preloadFrames()
    {
        // Load and convert all images once to maximize grab throughput
        preloadedFrames_.reserve(files_.size());

        size_t loadedCount = 0;
        for (const auto &path : files_)
        {
            camera::common::Frame f;
            if (loadFrameFromPath(path, f))
            {
                preloadedFrames_.push_back(std::move(f));
                ++loadedCount;
            }
            else
            {
                // Warning already emitted by loadFrameFromPath; continue
            }
        }
        return loadedCount > 0;
    }

} // namespace camera::mock
