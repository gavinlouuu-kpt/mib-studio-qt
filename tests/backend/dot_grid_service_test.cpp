// Lifecycle + concurrency coverage for DotGridService: it samples the latest
// committed FrameStore frame at its own rate, publishes a pose snapshot and
// callback, ignores frames while disabled, survives config changes and
// repeated stop(), and never blocks the producer.
#include "backend/playback/FrameStore.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/services/DotGridService.h"
#include "support/assert.h"
#include "support/opencv_tsan.h"
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
    mib::test::serializeOpenCvUnderTsan();
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
    // never blocks the producer, and its pose follows the newest frame. The
    // gate is a ratio, not a frame count per wall-clock window (sanitizer
    // builds copy a 1920x1200 frame far slower): the burst must finish in
    // well under the time a decode of every frame would take.
    wd.mark("burst");
    constexpr int kBurst = 60;
    const double decodeMs = service.lastDecodeMs();
    const uint64_t writtenBefore = store->totalWritten();
    double burstMs = 0.0;
    {
        const cv::Mat burstFrame = frameAt(cb, 40000.0, 52000.0, 12.0, 2); // render once, push many
        const auto t0 = std::chrono::steady_clock::now();
        std::thread producer([&] {
            uint64_t ts = 2000;
            for (int k = 0; k < kBurst; ++k) {
                push(*store, burstFrame, ts++);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        producer.join();
        burstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                      .count();
    }
    const uint64_t written = store->totalWritten();
    MIB_EXPECT(written == writtenBefore + kBurst, "every burst frame was committed");
    MIB_EXPECT(burstMs < 0.5 * kBurst * decodeMs,
               "producer was never blocked by the decoder: burst " + std::to_string(burstMs) +
                   " ms vs " + std::to_string(kBurst) + " decodes of " +
                   std::to_string(decodeMs) + " ms");
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

    // Paused (no view on screen): enabled but nothing decoded; resuming decodes
    // the newest frame at once rather than after the (deliberately long) interval.
    wd.mark("pause");
    {
        DotGridService::Config slow = cfg;
        slow.intervalMs = 5000;
        MIB_REQUIRE(service.setConfig(slow, &err), "slow interval accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the wake settle
        service.setPaused(true);
        MIB_EXPECT(service.isPaused() && service.isEnabled(), "paused while enabled");
        const uint64_t pausedAttempts = service.decodeAttempts();
        push(*store, frameAt(cb, 47000.0, 33000.0, 21.0, 9), 12500);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        MIB_EXPECT(service.decodeAttempts() == pausedAttempts, "no decode while paused");
        service.setPaused(false);
        // Well under the 5 s interval: only the resume wake-up can explain a pose.
        bool resumed = false;
        for (int i = 0; i < 100 && !resumed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            DotGridService::Pose p;
            resumed = service.getLatestPose(p) && p.valid && std::abs(p.centreXUm - 47000.0) < 1.0;
        }
        MIB_EXPECT(resumed, "resume decodes the frame pushed while paused without waiting out "
                            "the interval");
        MIB_REQUIRE(service.setConfig(cfg, &err), "normal interval restored");
    }

    // Invalid codebook parameters are rejected and the old codebook stays usable.
    DotGridService::Config bad = cfg;
    bad.codebook.dotDiameterUm = 40.0;
    MIB_EXPECT(!service.setConfig(bad, &err) && !err.empty(), "bad codebook rejected: " + err);
    MIB_EXPECT(service.hasCodebook(), "previous codebook retained");
    DotGridService::Config missingFile = cfg;
    missingFile.codebookPath = "/nonexistent/dotgrid/codebook.json";
    MIB_EXPECT(!service.setConfig(missingFile, &err), "missing codebook file rejected");

    // Registry mode: the pose names the design the frame belongs to; an explicit
    // codebook_path still overrides the registry; the same registry contents do
    // not rebuild the decoder.
    wd.mark("registry");
    {
        auto registry = std::make_shared<Registry>();
        CodebookParams other = cfg.codebook;
        other.seed = 8;
        Design a, b;
        a.id = "chip-a";
        a.name = "Chip A";
        a.codebook = std::make_shared<const Codebook>(cb);
        b.id = "chip-b";
        b.name = "Chip B";
        b.codebook = std::make_shared<const Codebook>(Codebook::generate(other, {}, "chip-b"));
        MIB_REQUIRE(registry->add(a, &err) && registry->add(b, &err), "registry built: " + err);
        DotGridService::Config withRegistry = cfg;
        withRegistry.registry = registry;
        MIB_REQUIRE(service.setConfig(withRegistry, &err), "registry config accepted: " + err);
        const auto active = service.activeRegistry();
        MIB_EXPECT(active && active->size() == 2, "both designs active");
        const auto pb = service.decodeImage(frameAt(*b.codebook, 30000.0, 30000.0, 70.0, 6));
        MIB_EXPECT(pb.valid && pb.designId == "chip-b" && pb.designName == "Chip B",
                   "frame attributed to chip-b: '" + pb.designId + "' " + pb.reason);
        const auto pa = service.decodeImage(frameAt(cb, 30000.0, 30000.0, 70.0, 7));
        MIB_EXPECT(pa.valid && pa.designId == "chip-a", "frame attributed to chip-a");
        MIB_REQUIRE(service.setConfig(withRegistry, &err), "same registry re-applied");
        MIB_EXPECT(service.activeRegistry() == active, "unchanged registry keeps its decoder");

        DotGridService::Config fileOverride = withRegistry;
        fileOverride.codebookPath = "/nonexistent/dotgrid/codebook.json";
        MIB_EXPECT(!service.setConfig(fileOverride, &err), "codebook_path takes precedence");
        MIB_EXPECT(service.activeRegistry() == active, "rejected override keeps the registry");

        DotGridService::Config emptyRegistry = cfg;
        emptyRegistry.registry = std::make_shared<Registry>();
        MIB_REQUIRE(service.setConfig(emptyRegistry, &err), "empty registry falls back");
        const auto fallback = service.decodeImage(frameAt(cb, 30000.0, 30000.0, 70.0, 8));
        MIB_EXPECT(fallback.valid && fallback.designId.empty(),
                   "empty registry -> inline codebook params, no design id");
        MIB_REQUIRE(service.setConfig(cfg, &err), "back to the plain config");
    }

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
