// monitoring_density_service_test
//
// MonitoringDensityService, the backend-owned live KDE of the Monitoring
// scatter (fake input provider, fake load probe, capturing record sink):
//  - policy: settings clamped; the next wake is >= 20x the last compute time;
//    dropped frames or a quarter-full batch queue count as load;
//  - disabled: no estimate, requestUpdate is a no-op;
//  - enable: one estimate lands promptly, densities in [0, 1] parallel to the
//    frame indices, ~90% core with a contour, grid over the padded data
//    range (or the configured axes), the worker runs at the lowest priority,
//    and the provisional record reaches the sink;
//  - unchanged input is skipped, new cells or a core-fraction change
//    re-estimate, an interval change alone does not;
//  - under load the tick is skipped and the next quiet tick computes;
//  - disable drops the result; re-enable recomputes the same input;
//    an empty ring drops a stale result;
//  - stop() joins promptly while a long interval is pending;
//  - concurrent setSettings/requestUpdate/latest/stats from four threads
//    while the input changes (TSan lane).

#include "backend/processing/KdeCoreRecord.h"
#include "backend/services/MonitoringDensityService.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using backend::services::MonitoringDensityInput;
using backend::services::MonitoringDensityService;
using backend::services::MonitoringDensitySettings;
using backend::services::MonitoringPipelineLoad;

namespace {

bool spinUntil(const std::function<bool()>& pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Thread-safe fake monitoring ring: a cluster plus sparse outliers.
struct FakeRing {
    std::mutex m;
    MonitoringDensityInput data;
    std::mt19937 rng{20260926};
    std::atomic<int> calls{0};

    void append(std::size_t n, uint64_t firstIndex) {
        std::normal_distribution<double> ax(300.0, 15.0), ay(0.05, 0.008);
        std::uniform_real_distribution<double> fx(700.0, 1000.0), fy(0.5, 0.9);
        std::scoped_lock lk(m);
        for (std::size_t i = 0; i < n; ++i) {
            const bool outlier = i % 10 == 9;
            data.frameIndices.push_back(firstIndex + i);
            data.points.push_back(outlier ? backend::monitoring::DensityPoint{fx(rng), fy(rng)}
                                          : backend::monitoring::DensityPoint{ax(rng), ay(rng)});
        }
        data.pixelToMicron = 0.5;
        if (data.points.size() > 1000) { // the monitoring ring's capacity
            const auto excess = static_cast<std::ptrdiff_t>(data.points.size() - 1000);
            data.points.erase(data.points.begin(), data.points.begin() + excess);
            data.frameIndices.erase(data.frameIndices.begin(), data.frameIndices.begin() + excess);
        }
    }
    void clear() {
        std::scoped_lock lk(m);
        data.frameIndices.clear();
        data.points.clear();
    }
    MonitoringDensityInput snapshot() {
        ++calls;
        std::scoped_lock lk(m);
        return data;
    }
};

struct FakeLoad {
    std::atomic<uint64_t> dropped{0};
    std::atomic<std::size_t> depth{0};
    MonitoringPipelineLoad sample() const { return {dropped.load(), depth.load(), 4096}; }
};

struct Sink {
    std::mutex m;
    std::vector<std::string> records;
    void push(std::string j) {
        std::scoped_lock lk(m);
        records.push_back(std::move(j));
    }
    std::size_t size() {
        std::scoped_lock lk(m);
        return records.size();
    }
    std::string last() {
        std::scoped_lock lk(m);
        return records.empty() ? std::string() : records.back();
    }
};

MonitoringDensitySettings enabledSettings() {
    MonitoringDensitySettings s;
    s.enabled = true;
    s.intervalMs = 60000; // ticks only on request: deterministic
    return s;
}

// Wait until the worker has consumed the request and gone idle.
bool settle(const MonitoringDensityService& svc) {
    return spinUntil([&] { return !svc.busy(); }, 10000);
}

} // namespace

int main() {
    mib::test::Watchdog wd(60);

    // ---- policy -----------------------------------------------------------------
    wd.mark("policy");
    MIB_EXPECT(MonitoringDensityService::nextIntervalMs(2000, 10) == 2000,
               "cheap estimate keeps the interval");
    MIB_EXPECT(MonitoringDensityService::nextIntervalMs(500, 100) == 2000,
               "100 ms estimate -> 2 s (20x budget)");
    MIB_EXPECT(MonitoringDensityService::nextIntervalMs(10, 0) ==
                   MonitoringDensitySettings::kIntervalMsMin,
               "interval floor");
    {
        MonitoringPipelineLoad before{5, 0, 4096};
        MIB_EXPECT(MonitoringDensityService::underLoad({6, 0, 4096}, before),
                   "a dropped frame is load");
        MIB_EXPECT(MonitoringDensityService::underLoad({5, 1024, 4096}, before),
                   "quarter-full queue is load");
        MIB_EXPECT(!MonitoringDensityService::underLoad({5, 1023, 4096}, before),
                   "shallow queue is not load");
        MIB_EXPECT(!MonitoringDensityService::underLoad({5, 999, 0}, before),
                   "unknown capacity: depth ignored");
    }
    {
        MonitoringDensitySettings s;
        s.intervalMs = 1;
        s.bandwidthFactor = 99.0;
        s.coreFraction = std::nan("");
        s.x1 = std::nan("");
        const auto c = s.clamped();
        MIB_EXPECT(c.intervalMs == MonitoringDensitySettings::kIntervalMsMin &&
                       c.bandwidthFactor == MonitoringDensitySettings::kBandwidthFactorMax &&
                       c.coreFraction == 0.9 && c.x0 == 0.0 && c.x1 == 0.0,
                   "settings clamped; non-finite values fall back");
    }

    FakeRing ring;
    FakeLoad load;
    Sink sink;
    ring.append(400, 0);
    {
        MonitoringDensityService svc([&] { return ring.snapshot(); }, [&] { return load.sample(); },
                                     [&](std::string j) { sink.push(std::move(j)); });

        // ---- disabled -------------------------------------------------------------
        wd.mark("disabled");
        svc.requestUpdate();
        MIB_EXPECT(!svc.busy(), "requestUpdate is a no-op while disabled");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MIB_EXPECT(svc.generation() == 0 && !svc.latest() && ring.calls == 0,
                   "disabled: nothing is read or computed");

        // ---- enable: first estimate ------------------------------------------------
        wd.mark("enable");
        svc.setSettings(enabledSettings());
        MIB_REQUIRE(spinUntil([&] { return svc.generation() >= 1; }, 10000),
                    "first estimate lands on enable");
        const auto r1 = svc.latest();
        MIB_REQUIRE(r1 != nullptr, "result published");
        MIB_EXPECT(r1->frameIndices.size() == 400 && r1->density.size() == 400,
                   "one density per cell");
        double maxD = 0.0, minD = 1.0;
        for (double d : r1->density) {
            maxD = std::max(maxD, d);
            minD = std::min(minD, d);
        }
        MIB_EXPECT(maxD == 1.0 && minD >= 0.0, "densities normalised to [0, 1]");
        MIB_EXPECT(r1->coreCount >= 340 && r1->coreCount <= 380, "90% core of 400 cells");
        MIB_EXPECT(!r1->contours.empty(), "core contour traced");
        MIB_EXPECT(r1->x0 < 280.0 && r1->x1 > 950.0 &&
                       r1->gridNx == MonitoringDensityService::kGridNx,
                   "grid spans the padded data range when no axes are configured");
        MIB_EXPECT(svc.stats().priorityLowered, "worker runs at the lowest OS priority");
        MIB_REQUIRE(spinUntil([&] { return sink.size() >= 1; }, 2000), "record reaches the sink");
        {
            std::string why;
            const auto rec = backend::monitoring::fromJson(sink.last(), &why);
            MIB_REQUIRE(rec.has_value(), "sink record parses: " + why);
            MIB_EXPECT(rec->provisional && rec->source == "live-buffer" &&
                           rec->populationCount == 400 && rec->cellCount == r1->coreCount &&
                           rec->contours.size() == r1->contours.size() && rec->pixelToMicron == 0.5,
                       "record mirrors the published estimate");
        }

        // ---- fingerprint -----------------------------------------------------------
        wd.mark("fingerprint");
        MIB_REQUIRE(settle(svc), "idle after the first estimate");
        const auto skippedBefore = svc.stats().skippedUnchanged;
        svc.requestUpdate();
        MIB_REQUIRE(settle(svc), "idle after an unchanged request");
        MIB_EXPECT(svc.generation() == 1 && svc.stats().skippedUnchanged == skippedBefore + 1,
                   "unchanged input is skipped");
        auto s = enabledSettings();
        s.intervalMs = 50000;
        svc.setSettings(s);
        MIB_REQUIRE(settle(svc), "idle after an interval change");
        MIB_EXPECT(svc.generation() == 1, "an interval change alone does not re-estimate");
        ring.append(20, 400);
        svc.requestUpdate();
        MIB_REQUIRE(spinUntil([&] { return svc.generation() == 2; }, 10000),
                    "new cells re-estimate");
        MIB_EXPECT(svc.latest()->frameIndices.size() == 420, "new cells included");
        const auto core90 = svc.latest()->coreCount;
        s.coreFraction = 0.5;
        svc.setSettings(s);
        MIB_REQUIRE(spinUntil([&] { return svc.generation() == 3; }, 10000),
                    "fraction change re-estimates");
        MIB_EXPECT(svc.latest()->coreCount < core90 && svc.latest()->coreFraction == 0.5,
                   "50% core is smaller");
        s.x0 = 0.0;
        s.x1 = 1000.0;
        s.y0 = 0.0;
        s.y1 = 1.0;
        svc.setSettings(s);
        MIB_REQUIRE(spinUntil([&] { return svc.generation() == 4; }, 10000),
                    "axes change re-estimates");
        MIB_EXPECT(svc.latest()->x0 == 0.0 && svc.latest()->x1 == 1000.0 && svc.latest()->y1 == 1.0,
                   "configured axes are the grid range");

        // ---- load back-off ---------------------------------------------------------
        wd.mark("load");
        MIB_REQUIRE(settle(svc), "idle before load");
        ring.append(20, 420);
        load.dropped += 3;
        const auto loadSkips = svc.stats().skippedUnderLoad;
        svc.requestUpdate();
        MIB_REQUIRE(settle(svc), "idle after a loaded tick");
        MIB_EXPECT(svc.generation() == 4 && svc.stats().skippedUnderLoad == loadSkips + 1,
                   "dropped frames since the last tick: estimate skipped");
        load.depth = 2048;
        svc.requestUpdate();
        MIB_REQUIRE(settle(svc), "idle after a backlog tick");
        MIB_EXPECT(svc.generation() == 4 && svc.stats().skippedUnderLoad == loadSkips + 2,
                   "batch queue backlog: estimate skipped");
        load.depth = 0;
        svc.requestUpdate();
        MIB_REQUIRE(spinUntil([&] { return svc.generation() == 5; }, 10000),
                    "quiet pipeline: estimate runs");
        MIB_EXPECT(svc.latest()->frameIndices.size() == 440, "the skipped cells are included");

        // ---- disable / re-enable / empty -------------------------------------------
        wd.mark("disable");
        s.enabled = false;
        svc.setSettings(s);
        MIB_EXPECT(!svc.latest() && svc.generation() == 6,
                   "disable drops the result and bumps the generation");
        s.enabled = true;
        svc.setSettings(s);
        MIB_REQUIRE(spinUntil([&] { return svc.latest() != nullptr; }, 10000),
                    "re-enable recomputes the unchanged input");
        MIB_REQUIRE(settle(svc), "idle after re-enable");
        const auto genBeforeEmpty = svc.generation();
        ring.clear();
        svc.requestUpdate();
        MIB_REQUIRE(settle(svc), "idle after an empty ring");
        MIB_EXPECT(!svc.latest() && svc.generation() == genBeforeEmpty + 1,
                   "an empty ring drops the stale result");
        MIB_EXPECT(svc.stats().nextIntervalMs >= 50000, "interval honoured (>= configured)");

        // ---- stop is prompt --------------------------------------------------------
        wd.mark("stop");
        const auto t0 = std::chrono::steady_clock::now();
        svc.stop();
        const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
        MIB_EXPECT(stopMs < 1000, "stop joins promptly during a 50 s wait");
        svc.stop();                         // idempotent
        svc.setSettings(enabledSettings()); // after stop: harmless
    }

    // ---- concurrent callers ---------------------------------------------------------
    wd.mark("concurrency");
    {
        FakeRing r;
        r.append(300, 0);
        std::atomic<uint64_t> sunk{0};
        MonitoringDensityService svc([&] { return r.snapshot(); }, [&] { return load.sample(); },
                                     [&](std::string) { ++sunk; });
        load.dropped = 0;
        load.depth = 0;
        std::atomic<bool> go{true};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                uint64_t i = 1000 + static_cast<uint64_t>(t) * 100000;
                MonitoringDensitySettings st;
                st.enabled = true;
                st.intervalMs = 500;
                while (go.load()) {
                    switch (i % 5) {
                    case 0:
                        st.coreFraction = 0.5 + 0.1 * static_cast<double>(i % 4);
                        svc.setSettings(st);
                        break;
                    case 1:
                        svc.requestUpdate();
                        break;
                    case 2: {
                        auto l = svc.latest();
                        if (l) (void)l->density.size();
                        break;
                    }
                    case 3:
                        (void)svc.stats();
                        (void)svc.busy();
                        break;
                    default:
                        r.append(3, i * 3);
                        break;
                    }
                    ++i;
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        go = false;
        for (auto& th : threads)
            th.join();
        wd.mark("concurrency stop");
        svc.stop();
        MIB_EXPECT(svc.stats().estimates >= 1 && sunk.load() >= 1,
                   "estimates kept landing under concurrent callers");
    }

    return mib::test::exitCode();
}
