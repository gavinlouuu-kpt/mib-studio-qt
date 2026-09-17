// Lifecycle + concurrency coverage for DotGridService: it samples the latest
// committed FrameStore frame at its own rate, publishes a pose snapshot and
// callback, ignores frames while disabled, survives config changes and
// repeated stop(), and never blocks the producer.
#include "backend/playback/FrameStore.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/services/DotGridService.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using backend::services::DotGridService;
using namespace backend::dotgrid;

namespace {

constexpr uint64_t kMono8 = 0x01080001;

struct PoseSink {
    std::mutex m;
    std::condition_variable cv;
    std::vector<DotGridService::Pose> poses;
    void push(const DotGridService::Pose& p) {
        std::lock_guard<std::mutex> lock(m);
        poses.push_back(p);
        cv.notify_all();
    }
    bool waitFor(size_t count, int ms) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(ms),
                           [&] { return poses.size() >= count; });
    }
};

cv::Mat frameAt(const Codebook& cb, double x, double y, double theta, uint32_t seed) {
    ViewPose pose;
    pose.centreXUm = x;
    pose.centreYUm = y;
    pose.thetaDeg = theta;
    pose.umPerPx = 0.293;
    pose.mirrored = true;
    RenderOptions opt;
    opt.seed = seed;
    return renderView(cb, pose, opt);
}

void push(backend::playback::FrameStore& store, const cv::Mat& img, uint64_t ts) {
    store.pushFrame(img.data, img.total(), static_cast<uint64_t>(img.cols),
                    static_cast<uint64_t>(img.rows), static_cast<size_t>(img.step), kMono8, ts);
}

} // namespace

int main() {
    mib::test::Watchdog wd(30);
    wd.mark("setup");

    DotGridService::Config cfg;
    cfg.enabled = true;
    cfg.intervalMs = 20;
    cfg.umPerPxHint = 0.3;
    cfg.codebook.seed = 7;
    cfg.codebook.columns = 3700;
    cfg.codebook.rows = 3700;
    cfg.codebook.pitchUm = 30.0;
    cfg.codebook.dotDiameterUm = 12.0;
    cfg.codebook.displacementUm = 5.0;
    const Codebook cb = Codebook::generate(cfg.codebook);

    auto store = std::make_shared<backend::playback::FrameStore>(8);
    DotGridService service;
    service.setFrameStore(store);
    std::string err;
    MIB_REQUIRE(service.setConfig(cfg, &err), "config accepted: " + err);
    MIB_REQUIRE(service.hasCodebook(), "codebook built");

    PoseSink sink;
    service.setPoseCallback([&](const DotGridService::Pose& p) { sink.push(p); });

    // Nothing decoded before start / without frames.
    DotGridService::Pose snapshot;
    MIB_EXPECT(!service.getLatestPose(snapshot), "no pose before any frame");

    wd.mark("start");
    service.start();
    service.start(); // idempotent
    MIB_EXPECT(service.isRunning(), "running after start");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    MIB_EXPECT(service.decodeAttempts() == 0, "no attempts on an empty store");

    // One frame -> exactly one decode of that frame, then idle.
    wd.mark("first frame");
    push(*store, frameAt(cb, 40000.0, 52000.0, 12.0, 1), 1000);
    MIB_REQUIRE(sink.waitFor(1, 5000), "pose callback after the first frame");
    {
        std::lock_guard<std::mutex> lock(sink.m);
        const auto& p = sink.poses[0];
        MIB_EXPECT(p.valid, "first pose valid: " + p.reason);
        MIB_EXPECT(std::abs(p.centreXUm - 40000.0) < 1.0 && std::abs(p.centreYUm - 52000.0) < 1.0,
                   "first pose position");
        MIB_EXPECT(p.mirrored, "mirror flag");
        MIB_EXPECT(p.frameIndex == 0, "frame index recorded");
        MIB_EXPECT(p.timestampNs == 1000, "timestamp recorded");
        MIB_EXPECT(p.imageWidth == 1920 && p.imageHeight == 1200, "image size recorded");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    MIB_EXPECT(service.decodeAttempts() == 1, "the same frame is not decoded twice");
    MIB_EXPECT(service.getLatestPose(snapshot) && snapshot.valid, "snapshot mirrors the callback");
    MIB_EXPECT(service.decodeSuccesses() == 1, "success counted");
    MIB_EXPECT(service.lastDecodeMs() > 0.0, "decode time recorded");

    // Burst of frames from a producer thread: the service samples the latest,
    // never blocks the producer, and its pose follows the newest frame.
    wd.mark("burst");
    {
        std::atomic<bool> stopProducer{false};
        const cv::Mat burstFrame = frameAt(cb, 40000.0, 52000.0, 12.0, 2); // render once, push many
        std::thread producer([&] {
            uint64_t ts = 2000;
            while (!stopProducer.load()) {
                push(*store, burstFrame, ts++);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        stopProducer.store(true);
        producer.join();
    }
    const uint64_t written = store->totalWritten();
    MIB_EXPECT(written > 20, "producer was never blocked by the decoder");
    push(*store, frameAt(cb, 61000.0, 22000.0, -33.0, 3), 9999);
    const size_t before = [&] {
        std::lock_guard<std::mutex> lock(sink.m);
        return sink.poses.size();
    }();
    bool sawNew = false;
    for (int i = 0; i < 100 && !sawNew; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(sink.m);
        for (size_t k = before > 0 ? before - 1 : 0; k < sink.poses.size(); ++k) {
            const auto& p = sink.poses[k];
            if (p.valid && std::abs(p.centreXUm - 61000.0) < 1.0 &&
                std::abs(p.centreYUm - 22000.0) < 1.0)
                sawNew = true;
        }
    }
    MIB_EXPECT(sawNew, "pose follows the newest frame after a burst");
    MIB_EXPECT(service.decodeAttempts() < written,
               "decoder sampled rather than processed every frame");

    // Disabled config: new frames are ignored; re-enabling resumes.
    wd.mark("disable");
    DotGridService::Config off = cfg;
    off.enabled = false;
    MIB_REQUIRE(service.setConfig(off, &err), "disable accepted");
    const uint64_t attemptsBefore = service.decodeAttempts();
    push(*store, frameAt(cb, 45000.0, 45000.0, 0.0, 4), 12000);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    MIB_EXPECT(service.decodeAttempts() == attemptsBefore, "no decode while disabled");
    MIB_REQUIRE(service.setConfig(cfg, &err), "re-enable accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    MIB_EXPECT(service.decodeAttempts() > attemptsBefore, "decoding resumes when enabled");

    // Invalid codebook parameters are rejected and the old codebook stays usable.
    DotGridService::Config bad = cfg;
    bad.codebook.dotDiameterUm = 40.0;
    MIB_EXPECT(!service.setConfig(bad, &err) && !err.empty(), "bad codebook rejected: " + err);
    MIB_EXPECT(service.hasCodebook(), "previous codebook retained");
    DotGridService::Config missingFile = cfg;
    missingFile.codebookPath = "/nonexistent/dotgrid/codebook.json";
    MIB_EXPECT(!service.setConfig(missingFile, &err), "missing codebook file rejected");

    // A frame without a pattern yields an invalid pose with a reason, not a stale valid one.
    wd.mark("blank frame");
    {
        const cv::Mat blank(1200, 1920, CV_8UC1, cv::Scalar(180));
        const auto p = service.decodeImage(blank);
        MIB_EXPECT(!p.valid && p.reason == "too few dots", "blank frame reason: " + p.reason);
    }

    wd.mark("stop");
    service.stop();
    service.stop(); // idempotent
    MIB_EXPECT(!service.isRunning(), "stopped");
    // Frames pushed after stop are ignored.
    const uint64_t attemptsAtStop = service.decodeAttempts();
    push(*store, frameAt(cb, 45000.0, 45000.0, 0.0, 5), 13000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    MIB_EXPECT(service.decodeAttempts() == attemptsAtStop, "no decode after stop");

    return mib::test::exitCode();
}
