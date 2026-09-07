// Simulated SDK-queue camera for delivery-mode tests.
//
// Models the part of a vendor SDK that the FrameDeliveryMode contract governs:
// a producer (the "acquisition engine") fills a bounded completed-buffer FIFO
// (capacity = CameraConfig::numBuffers) faster than the consumer drains it.
// grabFrame() applies the queue policy exactly as a real backend must:
//   EveryFrame  -> pop the oldest completed buffer, never skip.
//   LatestFrame -> drain to the newest completed buffer, counting every
//                  deliberately discarded stale frame.
// When the FIFO is full the producer drops the new frame and counts a buffer
// underrun (no input buffer available) — the unavoidable-loss path.
//
// Every produced frame carries a monotonically increasing sequence number in
// the first 8 bytes of Frame::data (little-endian via memcpy) and a
// Tools::getTimestamp() production stamp in Frame::timestamp, so tests can
// assert ordering, gap accounting, and frame age. The sequence counter is NOT
// reset by stop()/start(): frames that cross a mode-switch boundary are
// detectable because pre-restart sequence numbers are all smaller.
#pragma once

#include "backend/app/Tools.h"
#include "backend/camera/common/ICamera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace mib::test {

class QueueBackedTestCamera : public camera::common::ICamera {
public:
    struct Options {
        std::chrono::microseconds produceInterval{500};
        uint64_t width = 16;
        uint64_t height = 8;
        bool supportsLatestFrame = true; // false: exercise the unsupported-mode error path
    };

    explicit QueueBackedTestCamera(Options options) : options_(options) {}

    ~QueueBackedTestCamera() override { stop(); }

    void applyConfig(const camera::common::CameraConfig& config) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }

    bool start() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        if (config_.deliveryMode == camera::common::FrameDeliveryMode::LatestFrame &&
            !options_.supportsLatestFrame) {
            return false;
        }
        activeMode_ = config_.deliveryMode;
        queue_.clear();
        running_.store(true, std::memory_order_release);
        producer_ = std::thread([this] { produceLoop(); });
        return true;
    }

    void stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            running_.store(false, std::memory_order_release);
        }
        cv_.notify_all();
        if (producer_.joinable()) {
            producer_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        strandedAtStop_.fetch_add(queue_.size(), std::memory_order_relaxed);
        queue_.clear();
    }

    bool isRunning() const override { return running_.load(std::memory_order_acquire); }

    bool grabFrame(camera::common::Frame& out) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || !running_.load(std::memory_order_acquire);
        });
        if (queue_.empty()) {
            return false; // stopped: a blocked grab cancels cleanly
        }
        if (activeMode_ == camera::common::FrameDeliveryMode::LatestFrame) {
            while (queue_.size() > 1) {
                queue_.pop_front();
                intentionalDiscards_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        delivered_.fetch_add(1, std::memory_order_relaxed);
        // Sample the producer head under the same lock as the grab so lag
        // measurements are a logical distance at grab time, not a race with
        // the next push (underruns consume sequence numbers, so one push
        // landing between the grab and a later read can inflate the apparent
        // lag by every frame rejected while the queue was full).
        headAtLastGrab_.store(newestCompletedSequence_.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        return true;
    }

    bool pollStats(camera::common::CameraStats& out) const override
    {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        out = {};
        return true;
    }

    camera::common::FrameDeliveryCapabilities deliveryCapabilities() const override
    {
        camera::common::FrameDeliveryCapabilities caps;
        caps.supportsEveryFrame = true;
        caps.supportsLatestFrame = options_.supportsLatestFrame;
        caps.modeChangeRequiresRestart = true;
        caps.timestampsHostComparable = true; // production stamp uses Tools::getTimestamp
        return caps;
    }

    camera::common::FrameDeliveryMode activeDeliveryMode() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeMode_;
    }

    bool pollAcquisitionQueueStats(camera::common::AcquisitionQueueStats& out) const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = {};
        out.intentionallyDiscardedFrames = intentionalDiscards_.load(std::memory_order_relaxed);
        out.bufferUnderruns = underruns_.load(std::memory_order_relaxed);
        out.deliveredFrames = delivered_.load(std::memory_order_relaxed);
        out.sdkCompletedQueueDepth = queue_.size();
        out.sdkInputBufferCount =
            static_cast<size_t>(config_.numBuffers) > queue_.size()
                ? static_cast<size_t>(config_.numBuffers) - queue_.size()
                : 0;
        out.completedQueueDepthValid = true;
        out.inputBufferCountValid = true;
        out.underrunsValid = true;
        out.transportLossValid = true;
        return true;
    }

    // Test-side accounting accessors (counters survive stop()).
    uint64_t producedCount() const { return produced_.load(std::memory_order_relaxed); }
    uint64_t deliveredCount() const { return delivered_.load(std::memory_order_relaxed); }
    uint64_t intentionalDiscardCount() const
    {
        return intentionalDiscards_.load(std::memory_order_relaxed);
    }
    uint64_t underrunCount() const { return underruns_.load(std::memory_order_relaxed); }
    uint64_t strandedAtStopCount() const { return strandedAtStop_.load(std::memory_order_relaxed); }
    size_t peakQueueDepth() const { return peakDepth_.load(std::memory_order_relaxed); }
    uint64_t newestProducedSequence() const { return nextSequence_.load(std::memory_order_relaxed); }
    uint64_t newestCompletedSequence() const
    {
        return newestCompletedSequence_.load(std::memory_order_relaxed);
    }
    // Newest completed sequence as observed atomically with the last grab.
    uint64_t newestCompletedSequenceAtLastGrab() const
    {
        return headAtLastGrab_.load(std::memory_order_relaxed);
    }

    static uint64_t sequenceOf(const camera::common::Frame& frame)
    {
        uint64_t seq = 0;
        std::memcpy(&seq, frame.data.data(), sizeof(seq));
        return seq;
    }

private:
    void produceLoop()
    {
        // Deadline-based pacing with catch-up. A coarse OS timer (Windows'
        // default ~15.6 ms tick) turns a 500 us sleep into ~15 ms, silently
        // dropping the nominal rate to ~64 fps -- the same rate a "slow" 5 ms
        // consumer runs at once it is quantized too -- so overload never
        // materialises and LatestFrame never discards. Producing every frame
        // the elapsed time owes (bounded to one queue's worth per wake, like a
        // DMA engine completing several buffers at once) keeps the nominal
        // rate honest on any timer resolution.
        // First frame lands one interval after start (a 60 s interval must not
        // produce immediately -- the blocked-grab cancel test relies on it).
        auto nextDue = std::chrono::steady_clock::now() +
                       std::max(options_.produceInterval, std::chrono::microseconds(1));
        while (running_.load(std::memory_order_acquire)) {
            // Sleep in slices so stop() never waits out a long produce interval.
            for (;;) {
                if (!running_.load(std::memory_order_acquire)) {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= nextDue) {
                    break;
                }
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::microseconds>(nextDue - now);
                std::this_thread::sleep_for(std::min(remaining, std::chrono::microseconds(1000)));
            }
            const auto now = std::chrono::steady_clock::now();
            const auto interval = std::max(options_.produceInterval, std::chrono::microseconds(1));
            const int64_t owed = 1 + (now - nextDue) / interval;
            const int64_t cap = std::max<int64_t>(1, config_.numBuffers);
            const int64_t burst = std::min(owed, cap);
            nextDue += interval * owed; // lands within one interval of now
            for (int64_t i = 0; i < burst && running_.load(std::memory_order_acquire); ++i) {
                produceOne();
            }
        }
    }

    void produceOne()
    {
        camera::common::Frame frame;
        frame.width = options_.width;
        frame.height = options_.height;
        frame.linePitch = options_.width;
        frame.pixelFormat = 0x01080001; // Mono8
        frame.timestamp = backend::Tools::getTimestamp();
        frame.data.assign(static_cast<size_t>(options_.width * options_.height), 0);
        const uint64_t seq = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        std::memcpy(frame.data.data(), &seq, sizeof(seq));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            if (queue_.size() >= static_cast<size_t>(config_.numBuffers)) {
                // No input buffer available: the transport must drop.
                underruns_.fetch_add(1, std::memory_order_relaxed);
            } else {
                queue_.push_back(std::move(frame));
                newestCompletedSequence_.store(seq, std::memory_order_relaxed);
                size_t depth = queue_.size();
                size_t peak = peakDepth_.load(std::memory_order_relaxed);
                while (depth > peak &&
                       !peakDepth_.compare_exchange_weak(peak, depth,
                                                         std::memory_order_relaxed)) {
                }
            }
            produced_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    Options options_;
    camera::common::CameraConfig config_{};
    camera::common::FrameDeliveryMode activeMode_{camera::common::FrameDeliveryMode::EveryFrame};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<camera::common::Frame> queue_;
    std::thread producer_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> nextSequence_{0};
    std::atomic<uint64_t> newestCompletedSequence_{0};
    std::atomic<uint64_t> headAtLastGrab_{0};
    std::atomic<uint64_t> produced_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> intentionalDiscards_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> strandedAtStop_{0};
    std::atomic<size_t> peakDepth_{0};
};

} // namespace mib::test
