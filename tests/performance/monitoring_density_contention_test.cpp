// monitoring_density_contention_test
//
// The live density estimate must never cost acquisition throughput.
//  - Budget (uncontended): with a heavy input (2500 cells, fresh every tick)
//    and the shortest interval, the worker's busy time stays within the
//    compute budget (next wake >= 20x the last compute, so <= ~5% of one
//    core) and estimates keep landing.
//  - Contention: one thread per core runs the real processing path
//    (ProcessingService::computeProcessedFrame on 512x96 ring frames) flat
//    out. Alternating windows with the density service off and on; the
//    median processed-frame throughput with it on must stay >= 90% of off.
// Ratio gates only (no absolute timings); label `performance`, so the
// sanitizer lanes skip it.

#include "backend/processing/ProcessingService.h"
#include "backend/services/MonitoringDensityService.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/watchdog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using backend::services::MonitoringDensityInput;
using backend::services::MonitoringDensityService;
using backend::services::MonitoringDensitySettings;
using backend::services::ProcessingService;

namespace {

backend::services::ProcessingConfig lenientConfig() {
    backend::services::ProcessingConfig config;
    config.gaussian_blur_size = 3;
    config.bg_subtract_threshold = 127;
    config.morph_kernel_size = 3;
    config.morph_iterations = 1;
    config.enable_area_range_check = false;
    config.enable_deformability_range_check = false;
    config.enable_ring_ratio_check = false;
    config.enable_area_ratio_check = false;
    config.enable_border_check = false;
    config.require_single_inner_contour = false;
    config.empty_frame_pixel_threshold = 1;
    return config;
}

// A fresh 2500-cell population on every call: never "unchanged".
MonitoringDensityInput heavyInput(uint64_t call) {
    std::mt19937 rng(static_cast<uint32_t>(call));
    std::normal_distribution<double> ax(300.0, 20.0), ay(0.06, 0.01), bx(600.0, 40.0),
        by(0.2, 0.03);
    MonitoringDensityInput in;
    in.pixelToMicron = 0.5;
    for (uint64_t i = 0; i < 2500; ++i) {
        in.frameIndices.push_back(call * 10000 + i);
        in.points.push_back(i % 3 ? backend::monitoring::DensityPoint{ax(rng), ay(rng)}
                                  : backend::monitoring::DensityPoint{bx(rng), by(rng)});
    }
    return in;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

} // namespace

int main() {
    mib::test::Watchdog wd(90);
    std::atomic<uint64_t> calls{0};
    MonitoringDensityService svc([&] { return heavyInput(++calls); });
    MonitoringDensitySettings on;
    on.enabled = true;
    on.intervalMs = MonitoringDensitySettings::kIntervalMsMin;
    MonitoringDensitySettings off = on;
    off.enabled = false;

    // ---- budget, uncontended --------------------------------------------------------
    // Measured from the landing of the 1st estimate (which runs at once on
    // enable, before any budget exists) to the landing of the 3rd: two
    // budgeted waits and two computes, so the window scales with the host.
    wd.mark("budget");
    auto landed = [&](uint64_t n) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            if (svc.stats().estimates >= n && !svc.busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    svc.setSettings(on);
    MIB_REQUIRE(landed(1), "first estimate lands");
    const auto busy1 = svc.stats().busyMs;
    const auto t1 = std::chrono::steady_clock::now();
    wd.mark("budget window");
    MIB_REQUIRE(landed(3), "estimates keep landing when the host is idle");
    const auto st = svc.stats();
    const double wallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const double duty = (st.busyMs - busy1) / wallMs;
    std::printf("budget: %llu estimates, last %d ms, next wake %d ms, duty %.2f%% of one core over "
                "%.0f ms, "
                "priority lowered %d\n",
                static_cast<unsigned long long>(st.estimates), st.lastComputeMs, st.nextIntervalMs,
                duty * 100.0, wallMs, st.priorityLowered ? 1 : 0);
    MIB_EXPECT(st.nextIntervalMs >=
                   MonitoringDensityService::kComputeBudgetFactor * st.lastComputeMs,
               "next wake respects the 20x compute budget");
    // 1/21 for equal computes; the margin absorbs compute-time jitter.
    MIB_EXPECT(duty <= 0.07, "worker duty cycle within the ~5% budget");
    MIB_EXPECT(st.priorityLowered, "worker runs at the lowest OS priority");
    svc.setSettings(off);

    // ---- contention -----------------------------------------------------------------
    wd.mark("contention");
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    std::vector<cv::Mat> frames;
    for (uint64_t i = 0; i < 16; ++i)
        frames.push_back(mib::test::ringFrame(512, 96, i));
    const auto config = lenientConfig();
    std::atomic<bool> run{true};
    std::atomic<uint64_t> processed{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < cores; ++t) {
        workers.emplace_back([&, t] {
            auto processing = std::make_unique<ProcessingService>();
            uint64_t i = t;
            while (run.load(std::memory_order_relaxed)) {
                const auto f =
                    processing->computeProcessedFrame(frames[i % frames.size()], cv::Mat{}, config,
                                                      ProcessingService::Roi{0, 0, 0, 0}, i);
                if (!f.processedImage.empty()) processed.fetch_add(1, std::memory_order_relaxed);
                ++i;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // warm-up
    std::vector<double> offRates, onRates;
    const uint64_t estimatesBefore = svc.stats().estimates;
    for (int round = 0; round < 6; ++round) {
        wd.mark("contention round");
        for (bool density : {false, true}) {
            svc.setSettings(density ? on : off);
            const uint64_t p0 = processed.load();
            const auto w0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
            (density ? onRates : offRates)
                .push_back(static_cast<double>(processed.load() - p0) / secs);
        }
    }
    run = false;
    for (auto& w : workers)
        w.join();
    svc.setSettings(off);
    svc.stop();
    const double offMed = median(offRates), onMed = median(onRates);
    std::printf(
        "contention: %u workers, processing %.0f frames/s density off vs %.0f on (ratio %.3f), "
        "%llu estimates ran during the on windows\n",
        cores, offMed, onMed, offMed > 0 ? onMed / offMed : 0.0,
        static_cast<unsigned long long>(svc.stats().estimates - estimatesBefore));
    MIB_REQUIRE(offMed > 0.0, "the processing workload runs");
    MIB_EXPECT(onMed >= 0.90 * offMed, "density estimation does not cost processing throughput");

    return mib::test::exitCode();
}
