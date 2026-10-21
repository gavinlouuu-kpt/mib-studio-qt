// realtime_rates_decay_test (#763 F17)
//
// After Stop acquisition the status bar and fetch_processing_stats kept
// showing the last second's rates (96/15/93 per second, 20 s after the stop):
// the inline realtime loop closed its 1 s window only after a processed
// frame, so with no frames the last window stayed published. Now:
//   1. realtimeRateAt (fake clock): a published rate is read as is while it
//      is younger than kRealtimeRateStaleUs and as 0 after it; never
//      published reads 0; a publication that lands after "now" was read is
//      fresh.
//   2. both realtime modes, the real ProcessingService loop on a FrameStore:
//      frames until the algo rate is > 0, then no more frames while realtime
//      keeps running (the camera stopped). A window closes without a frame
//      and publishes 0: all three rates read 0 while the publication is
//      fresh, so the 0 is the loop's value and not the staleness guard's.
//   3. stopRealtime zeroes the rates at once.

#include "backend/app/Tools.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using backend::playback::FrameStore;
using backend::services::ProcessingService;
using Mode = ProcessingService::RealtimeProcessingMode;

namespace {

constexpr int kW = 320;
constexpr int kH = 96;
constexpr uint64_t kMono8 = 0x01080001ULL;

backend::services::ProcessingConfig lenient()
{
    backend::services::ProcessingConfig c;
    c.gaussian_blur_size = 3;
    c.bg_subtract_threshold = 8;
    c.morph_kernel_size = 3;
    c.morph_iterations = 1;
    c.enable_border_check = false;
    c.enable_area_range_check = false;
    c.enable_deformability_range_check = false;
    c.enable_area_ratio_check = false;
    c.enable_ring_ratio_check = false;
    c.require_single_inner_contour = false;
    c.empty_frame_pixel_threshold = 1;
    c.auto_background_enabled = false;
    c.multi_image_enabled = false;
    return c;
}

cv::Mat oneObject()
{
    cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
    m(cv::Rect(70, 36, 16, 12)).setTo(200);
    return m;
}

void push(FrameStore& store, const cv::Mat& m, uint64_t ts)
{
    store.pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH, static_cast<size_t>(kW), kMono8, ts, 0, 0);
}

void start(ProcessingService& proc, Mode mode, const std::shared_ptr<FrameStore>& store)
{
    proc.setProcessingConfig(lenient());
    proc.setRealtimeProcessingMode(mode);
    proc.setPixelToMicronFactor(0.5);
    proc.setRealtimeDropFrames(true);
    proc.setRealtimeEnabled(true);
    proc.startRealtime(store);
}

bool allZero(const ProcessingService& proc)
{
    return proc.getAlgoFps1s() == 0.0 && proc.getValidFps1s() == 0.0 && proc.getInvalidFps1s() == 0.0;
}

// ---- 1. the staleness rule, with a fake clock ---------------------------------------------
void stalenessRule()
{
    constexpr uint64_t stale = ProcessingService::kRealtimeRateStaleUs;
    const uint64_t at = 10'000'000;
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, at, at) == 96.0, "read at the publication");
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, at, at + stale) == 96.0, "still fresh at the limit");
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, at, at + stale + 1) == 0.0, "0 once older than the limit");
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, at, at + 20'000'000) == 0.0, "0 twenty seconds later");
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, 0, at) == 0.0, "never published reads 0");
    MIB_EXPECT(ProcessingService::realtimeRateAt(96.0, at + 5, at) == 96.0, "a publication after the read is fresh");
}

// ---- 2. frames stop, realtime keeps running ------------------------------------------------
void decaysWhenFramesStop(Mode mode, const char* name, mib::test::Watchdog& wd)
{
    std::printf("frames stop, %s mode\n", name);
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    start(proc, mode, store);
    const cv::Mat frame = oneObject();
    uint64_t ts = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (proc.getAlgoFps1s() <= 0.0 && std::chrono::steady_clock::now() < deadline) {
        push(*store, frame, ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    MIB_REQUIRE(proc.getAlgoFps1s() > 0.0, std::string(name) + ": the loop published a rate while frames arrived");
    wd.mark(mode == Mode::Inline ? "inline: waiting for the idle window" : "async batch: waiting for the idle window");

    // No more frames; realtime keeps running, as after Stop acquisition.
    bool decayed = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!decayed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint64_t published = proc.getAlgoAvgUs1sUpdatedUs();
        const bool zero = allZero(proc);
        const uint64_t now = backend::Tools::getTimestamp();
        decayed = zero && published != 0 && (now <= published || now - published < ProcessingService::kRealtimeRateStaleUs);
    }
    MIB_EXPECT(decayed, std::string(name) + ": a window closed without frames and published 0 (algo " +
                            std::to_string(proc.getAlgoFps1s()) + ", valid " + std::to_string(proc.getValidFps1s()) +
                            ", invalid " + std::to_string(proc.getInvalidFps1s()) + ")");

    // Frames again: the rate comes back.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (proc.getAlgoFps1s() <= 0.0 && std::chrono::steady_clock::now() < deadline) {
        push(*store, frame, ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    MIB_EXPECT(proc.getAlgoFps1s() > 0.0, std::string(name) + ": the rate returns with frames");
    proc.stopRealtime();
    proc.stop();
}

// ---- 3. stopRealtime zeroes the rates --------------------------------------------------------
void stopZeroes()
{
    std::printf("stopRealtime zeroes the rates\n");
    auto store = std::make_shared<FrameStore>(256);
    ProcessingService proc;
    proc.start(1);
    start(proc, Mode::Inline, store);
    const cv::Mat frame = oneObject();
    uint64_t ts = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (proc.getAlgoFps1s() <= 0.0 && std::chrono::steady_clock::now() < deadline) {
        push(*store, frame, ++ts);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    MIB_REQUIRE(proc.getAlgoFps1s() > 0.0, "a rate before the stop");
    proc.stopRealtime();
    MIB_EXPECT(allZero(proc), "every rate reads 0 right after stopRealtime");
    proc.stop();
}

} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(120);
    wd.mark("staleness rule");
    stalenessRule();
    wd.mark("frames stop, inline");
    decaysWhenFramesStop(Mode::Inline, "inline", wd);
    wd.mark("frames stop, async batch");
    decaysWhenFramesStop(Mode::AsyncBatch, "async batch", wd);
    wd.mark("stop zeroes");
    stopZeroes();
    std::printf("realtime_rates_decay_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
