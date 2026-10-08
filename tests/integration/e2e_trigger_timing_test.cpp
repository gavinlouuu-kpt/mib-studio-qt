// e2e_trigger_timing_test
//
// Reproduces / measures user complaint: "trigger service has variable delay
// time." TriggerService runs a dedicated thread that waits on a condition
// variable, then drives the camera trigger line. The latency that matters is
// from onTargetGroupResult() (a target object was detected) to the moment the
// trigger output is actually asserted (setTriggerOutput(true)).
//
// This test wires a fake ICamera that timestamps the exact instant the trigger
// fires, then issues trigger requests at a fixed cadence while background
// threads create CPU/scheduling pressure (standing in for the capture +
// processing pipeline). It records the request->fire latency for each pulse and
// reports the distribution (mean / p50 / p99 / max / jitter). A pathological
// stall fails the test; the distribution itself is the diagnostic artifact.

#include "backend/services/TriggerService.h"
#include "backend/camera/common/ICamera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using backend::services::TriggerService;
using backend::services::TargetGroupSignal;
using Clock = std::chrono::steady_clock;

namespace {

// Fake camera that records the precise time the trigger line is asserted.
class TimestampingCamera : public camera::common::ICamera {
public:
    void applyConfig(const camera::common::CameraConfig&) override {}
    bool start() override { return true; }
    void stop() override {}
    bool isRunning() const override { return true; }
    bool grabFrame(camera::common::Frame&) override { return false; }
    bool pollStats(camera::common::CameraStats&) const override { return false; }

    bool setTriggerOutput(bool high) override {
        if (high) {
            // Record timestamp BEFORE publishing the count so the reader, which
            // reads count first (acquire), always sees a consistent timestamp.
            lastFireTime_.store(Clock::now().time_since_epoch().count(),
                                std::memory_order_relaxed);
            fireCount_.fetch_add(1, std::memory_order_release);
        }
        return true;
    }

    uint64_t fireCount() const { return fireCount_.load(std::memory_order_acquire); }
    Clock::time_point lastFireTime() const {
        return Clock::time_point(
            Clock::duration(lastFireTime_.load(std::memory_order_relaxed)));
    }

private:
    std::atomic<uint64_t> fireCount_{0};
    std::atomic<Clock::rep> lastFireTime_{0};
};

double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = (p / 100.0) * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

} // namespace

int main()
{
    constexpr int kPulses = 500;
    constexpr auto kCadence = std::chrono::milliseconds(2);  // ~500 Hz request rate
    constexpr auto kWaitTimeout = std::chrono::milliseconds(500);

    TimestampingCamera camera;
    TriggerService trigger;
    trigger.setPulseDurationUs(5);
    trigger.setCamera(&camera);
    trigger.start();

    // Background load: saturate cores to create scheduling pressure, mimicking
    // the capture + processing + UI threads competing with the trigger thread.
    std::atomic<bool> loadRunning{true};
    std::vector<std::thread> loadThreads;
    const unsigned loadCount = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < loadCount; ++i) {
        loadThreads.emplace_back([&loadRunning] {
            volatile double sink = 0.0;
            while (loadRunning.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 20000; ++k) {
                    sink += std::sqrt(static_cast<double>(k) + 1.0);
                }
            }
            (void)sink;
        });
    }

    std::vector<double> latenciesUs;
    latenciesUs.reserve(kPulses);
    uint64_t missed = 0;

    auto nextDeadline = Clock::now();
    for (int i = 0; i < kPulses; ++i) {
        nextDeadline += kCadence;
        std::this_thread::sleep_until(nextDeadline);

        const uint64_t before = camera.fireCount();
        const auto requestTime = Clock::now();
        trigger.onTargetGroupResult(TargetGroupSignal{true, i, i});

        // Wait for this pulse to fire.
        const auto deadline = Clock::now() + kWaitTimeout;
        while (camera.fireCount() <= before && Clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (camera.fireCount() <= before) {
            ++missed;
            continue;
        }
        const auto fireTime = camera.lastFireTime();
        const double us =
            std::chrono::duration<double, std::micro>(fireTime - requestTime).count();
        if (us >= 0.0) {
            latenciesUs.push_back(us);
        }
    }

    loadRunning.store(false);
    for (auto& t : loadThreads) t.join();
    trigger.stop();

    if (latenciesUs.empty()) {
        std::cerr << "no trigger pulses were measured\n";
        return 2;
    }

    double sum = 0.0, maxUs = 0.0, minUs = latenciesUs.front();
    for (double v : latenciesUs) {
        sum += v;
        maxUs = std::max(maxUs, v);
        minUs = std::min(minUs, v);
    }
    const double mean = sum / static_cast<double>(latenciesUs.size());
    double var = 0.0;
    for (double v : latenciesUs) var += (v - mean) * (v - mean);
    const double stddev = std::sqrt(var / static_cast<double>(latenciesUs.size()));
    const double p50 = percentile(latenciesUs, 50.0);
    const double p99 = percentile(latenciesUs, 99.0);
    const double jitter = maxUs - minUs;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== e2e trigger timing (request -> trigger asserted) ===\n";
    std::cout << "pulses measured: " << latenciesUs.size()
              << "  missed/timed-out: " << missed << "\n";
    std::cout << "trigger count (service): " << trigger.getTriggerCount() << "\n";
    std::cout << "latency us:  min=" << minUs << "  mean=" << mean
              << "  p50=" << p50 << "  p99=" << p99 << "  max=" << maxUs << "\n";
    std::cout << "stddev us: " << stddev << "   jitter(max-min) us: " << jitter << "\n";

    int rc = 0;

    // Any dropped/timed-out trigger is a correctness problem for a trigger
    // service expected to fire once per detection.
    if (missed > 0) {
        std::cout << "FAIL: " << missed
                  << " trigger request(s) never fired within "
                  << kWaitTimeout.count() << " ms.\n";
        rc = 1;
    }

    // A shared runner can preempt one pulse for tens of ms. Keep zero missed
    // pulses above, and reject a recurring stall rather than one scheduler gap.
    constexpr double kMaxAcceptableUs = 50000.0; // 50 ms
    constexpr int kAllowedOutliers = 2;          // < 1% of the 500 requests
    const auto outliers = std::count_if(latenciesUs.begin(), latenciesUs.end(),
                                        [](double us) { return us > kMaxAcceptableUs; });
    if (p99 > kMaxAcceptableUs || outliers > kAllowedOutliers) {
        std::cout << "FAIL: p99 trigger latency " << p99 << " us; " << outliers << " pulses exceed "
                  << kMaxAcceptableUs << " us (reproduced variable-delay stall).\n";
        rc = 1;
    }

    if (rc == 0) {
        std::cout << "Trigger latency stayed within bounds; distribution above "
                     "characterizes the variability under load.\n";
    }
    return rc;
}
