// monitoring_row_latency_test (issue #857, review of PR #876) — latency budget of the per-frame
// Monitoring row.
//
// With the Monitoring tab open the realtime loop keeps an own copy of each frame's ROI crop (gray
// image and mask) in the ring, and the ring releases the pictures of its oldest rows to stay within
// its byte budget. At 5000 fps the loop has 0.2 ms per frame, so that work must stay a small part
// of it, also for a large ROI (the copy is proportional to the crop). This measures the loop
// itself, on frames pushed while the loop is held and timed from the release to the last frame:
//   hidden  Monitoring tab closed: no row, no copy;
//   shown   the tab open: crop copy + ring append + budget release, per frame.
// for an ROI of 1100x190 (a copy of 0.42 MB per frame) and for the whole 1216x208 frame, at the
// bench geometry. The whole-frame row is a copy too: sharing the loop's own buffers saved two
// memcpy of 0.25 MB (10 us) and cost 1.5x the frame, because the ring then retained the loop's
// per-frame buffers and every frame page-faulted two fresh ones (267 minor faults per frame
// against 16 with the copy).
//
// Ratio gates only (CI machines vary): per-frame time shown / hidden, medians over alternating
// rounds, the copy's cost against the frame's own cost, and on Linux the minor page faults a
// shown frame adds (a count, not a time). Absolute numbers are printed. Label `performance`,
// so the sanitizer lanes skip it.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using backend::services::ProcessingService;
using Clock = std::chrono::steady_clock;
using Roi = ProcessingService::Roi;

namespace {
constexpr int kW = 1216; // the bench geometry (MC2066 window)
constexpr int kH = 208;
constexpr int kBurst = 300;
constexpr int kRounds = 7;

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// Minor page faults of this process so far; 0 where not measured.
double minorFaults()
{
#if defined(__linux__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<double>(ru.ru_minflt);
#endif
    return 0.0;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

struct Result {
    double hiddenUs{0.0}; // per frame, median
    double shownUs{0.0};
    double cloneUs{0.0};  // copying the crop's gray image and mask, per frame
    double hiddenFaults{0.0}; // minor page faults per frame, median (Linux)
    double shownFaults{0.0};
};

Result measure(const char* label, const Roi& roi, int cropW, int cropH)
{
    auto store = std::make_shared<backend::playback::FrameStore>(kBurst + 8);
    ProcessingService svc;
    svc.start(1);
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string err;
    MIB_REQUIRE(svc.activateProcessingKernel(kernel, &err), "activate test kernel: " + err);
    auto cfg = svc.getProcessingConfig();
    cfg.empty_frame_pixel_threshold = 1;
    cfg.bg_subtract_threshold = 100;
    cfg.enable_border_check = false;
    cfg.enable_area_range_check = false;
    cfg.enable_deformability_range_check = false;
    cfg.enable_ring_ratio_check = false;
    cfg.enable_area_ratio_check = false;
    cfg.require_single_inner_contour = false;
    cfg.auto_background_enabled = false;
    svc.setProcessingConfig(cfg);
    svc.setRealtimeRoi(roi);
    svc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    svc.setRealtimeDropFrames(false); // a run: every frame
    svc.startRealtime(store);
    MIB_REQUIRE(svc.isRealtimeRunning(), "realtime running");

    std::vector<cv::Mat> frames;
    for (int i = 0; i < 16; ++i) frames.push_back(mib::test::ringFrame(kW, kH, static_cast<uint64_t>(i) * 7));
    uint64_t ts = 1;
    auto push = [&](int i) {
        const cv::Mat& m = frames[static_cast<size_t>(i) % frames.size()];
        store->pushFrame(m.data, m.total(), kW, kH, kW, 0x01080001, ts, ts);
        ++ts;
    };
    auto appended = [&] { return svc.getMonitoringValidAppended() + svc.getMonitoringInvalidAppended(); };

    push(0);
    push(0); // write index 0 is the loop's never-consumed sentinel
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() == 1; }, std::chrono::seconds(10)), "warm-up");

    double lastFaults = 0.0;
    // One timed burst: hold the loop on its first frame, queue the rest, time the release to the end.
    auto burst = [&](bool shown) {
        svc.setMonitoringActive(shown);
        svc.clearMonitoringFrames();
        const uint64_t calls = kernel->emptyCalls.load();
        const uint64_t rows = appended();
        kernel->holdCall.store(calls + 1);
        push(1);
        MIB_REQUIRE(waitFor([&] { return kernel->heldCall.load() == calls + 1; }, std::chrono::seconds(10)),
                    "loop held on the first frame of the burst");
        for (int i = 2; i <= kBurst; ++i) push(i);
        kernel->holdCall.store(0);
        const double faults0 = minorFaults();
        const auto t0 = Clock::now();
        kernel->release(calls + 1);
        MIB_REQUIRE(waitFor([&] {
                        return kernel->emptyCalls.load() >= calls + kBurst &&
                               (!shown || appended() >= rows + kBurst);
                    }, std::chrono::seconds(30)),
                    "burst processed");
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / kBurst;
        lastFaults = (minorFaults() - faults0) / kBurst;
        // Let the loop finish its last frame before the next burst.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return us;
    };

    burst(false); // warm caches, allocator and threads
    burst(true);
    std::vector<double> hidden, shown, hiddenFaults, shownFaults;
    for (int r = 0; r < kRounds; ++r) {
        hidden.push_back(burst(false));
        hiddenFaults.push_back(lastFaults);
        shown.push_back(burst(true));
        shownFaults.push_back(lastFaults);
    }

    // The copy itself: a crop's gray image and mask, as the loop makes them, in isolation.
    const cv::Rect crop(roi.w > 0 ? roi.x : 0, roi.w > 0 ? roi.y : 0, cropW, cropH);
    cv::Mat gray = frames[0].clone();
    cv::Mat mask(kH, kW, CV_8UC1, cv::Scalar(0));
    std::vector<double> clones;
    for (int round = 0; round < kRounds; ++round) {
        const auto c0 = Clock::now();
        size_t sink = 0;
        for (int i = 0; i < kBurst; ++i) {
            cv::Mat g = gray(crop).clone();
            cv::Mat m = mask(crop).clone();
            sink += static_cast<size_t>(g.data[0]) + m.data[0];
        }
        clones.push_back(std::chrono::duration<double, std::micro>(Clock::now() - c0).count() / kBurst +
                         static_cast<double>(sink) * 0.0);
    }

    svc.setMonitoringActive(false);
    svc.stopRealtime();
    svc.stop();
    Result r;
    r.hiddenUs = median(hidden);
    r.shownUs = median(shown);
    r.cloneUs = median(clones);
    r.hiddenFaults = median(hiddenFaults);
    r.shownFaults = median(shownFaults);
    std::printf("%-30s per frame: hidden %.1f us, shown %.1f us (x%.2f); crop copy alone %.1f us (%.0f%% of a frame); "
                "minor faults per frame %.1f hidden, %.1f shown\n",
                label, r.hiddenUs, r.shownUs, r.hiddenUs > 0 ? r.shownUs / r.hiddenUs : 0.0, r.cloneUs,
                r.hiddenUs > 0 ? 100.0 * r.cloneUs / r.hiddenUs : 0.0, r.hiddenFaults, r.shownFaults);
    return r;
}
} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(120);

    wd.mark("large ROI (copy)");
    const Result big = measure("ROI 1100x190 (copied)", Roi{40, 8, 1100, 190}, 1100, 190);
    wd.mark("whole frame (copied)");
    const Result whole = measure("whole frame 1216x208 (copied)", Roi{0, 0, kW, kH}, kW, kH);

    // The gates are ratios. Showing the tab adds the copy, the ring append and the budget release; a
    // copy per object, a copy under the ring's lock or a share of the loop's buffers would each
    // cost another 20-50% of the frame. 1.5 leaves room for a loaded machine (1.12 measured).
    MIB_EXPECT(big.shownUs <= 1.5 * big.hiddenUs, "Monitoring shown, large ROI: a frame costs at most 1.5x the hidden one");
    MIB_EXPECT(whole.shownUs <= 1.5 * whole.hiddenUs, "Monitoring shown, whole frame: a frame costs at most 1.5x the hidden one");
    // The copy is the new per-frame work: it must be a minor part of what a frame costs anyway.
    MIB_EXPECT(big.cloneUs <= 0.6 * big.hiddenUs, "the crop copy is under 60% of a frame's own cost");
    MIB_EXPECT(whole.cloneUs <= 0.3 * whole.hiddenUs, "the whole-frame copy is under 30% of a frame's own cost");
#if defined(__linux__)
    // A shown frame retains one crop of two images; the allocator recycles what the ring releases
    // (16 minor faults per frame measured). The same crops taken from fresh memory every frame
    // fault 60 pages each (267 per frame measured when rows shared the loop's own buffers).
    MIB_EXPECT(big.shownFaults <= 128.0, "Monitoring shown, large ROI: the ring recycles its memory (page faults per frame)");
    MIB_EXPECT(whole.shownFaults <= 128.0, "Monitoring shown, whole frame: the ring recycles its memory (page faults per frame)");
#endif
    MIB_EXPECT(big.hiddenUs > 0.0 && whole.hiddenUs > 0.0, "the loop processed the bursts");
    return mib::test::exitCode();
}
