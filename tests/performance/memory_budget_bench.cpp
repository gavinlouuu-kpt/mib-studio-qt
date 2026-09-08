// memory_budget_bench (issue #370)
//
// Measures the host-path memory ownership policies with synthetic frames and
// asserts the release gates:
//  A. per-object copies: objects of one frame share the source + mask (0
//     copies per object) versus the former clone-per-object cost (computed);
//  B. slow persistence consumer: the experiment buffer plateaus at its byte
//     budget (no growth with input), every eviction counted;
//  C. larger frames (2048x1088): the byte budget, not the frame cap, bounds
//     the backlog;
//  D. long monitoring session behind a consumer that never reads: rings
//     plateau (bytes at 2N == bytes at N);
//  E. FrameStore ring: retained bytes constant across 20k pushes; an
//     EveryFrame consumer and a LatestFrame-style consumer both leave the
//     store bounded;
//  F. 1M-frame metadata stress: accounting only, bounded and fast;
//  G. multi-image series retention is bounded by multi_image_count.
// Writes a JSON report to $MIB_MEMORY_BENCH_REPORT when set.

#include "backend/diagnostics/MemoryBudget.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ExperimentFrameBuffer.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/RecordingAccounting.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace backend::services;
namespace diag = backend::diagnostics;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kW = 512;
constexpr int kH = 96;
constexpr uint64_t kMatBytes = static_cast<uint64_t>(kW) * kH;

double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Linux: /proc/self/status (VmRSS / VmHWM). Elsewhere: Tools (Windows) or 0.
double rssMB(bool peak = false)
{
    std::ifstream f("/proc/self/status");
    std::string line;
    const std::string key = peak ? "VmHWM:" : "VmRSS:";
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
            std::istringstream is(line.substr(key.size()));
            double kb = 0;
            is >> kb;
            return kb / 1024.0;
        }
    }
    return 0.0;
}

cv::Mat objectsFrame(int w, int h, int objects, uint64_t i)
{
    // Objects jump between frames so batch tracking yields one result per
    // object per frame (a static object would be matched to its track and
    // emitted once).
    cv::Mat m(h, w, CV_8UC1, cv::Scalar(0));
    const int pitch = std::max(30, (w - 80) / std::max(1, objects));
    for (int k = 0; k < objects; ++k) {
        const int cx = 30 + ((k * pitch + static_cast<int>((i * 37) % 200)) % (w - 60));
        if (cx + 15 >= w) break;
        cv::circle(m, cv::Point(cx, h / 2), 14, cv::Scalar(220), -1);
        cv::circle(m, cv::Point(cx, h / 2), 7, cv::Scalar(0), -1);
    }
    return m;
}

ProcessedFrame syntheticFrame(int w, int h, uint64_t idx)
{
    ProcessedFrame f;
    f.index = idx;
    f.originalImage = cv::Mat(h, w, CV_8UC1, cv::Scalar(static_cast<int>(idx % 255)));
    f.processedImage = cv::Mat(h, w, CV_8UC1, cv::Scalar(0));
    f.validation.isValid = true;
    return f;
}

ProcessingConfig lenientConfig(ProcessingConfig cfg)
{
    cfg.empty_frame_pixel_threshold = 1;
    cfg.bg_subtract_threshold = 100;
    cfg.enable_border_check = false;
    cfg.enable_area_range_check = false;
    cfg.enable_deformability_range_check = false;
    cfg.enable_ring_ratio_check = false;
    cfg.enable_area_ratio_check = false;
    cfg.require_single_inner_contour = false;
    cfg.auto_background_enabled = false;
    return cfg;
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

void pushMat(backend::playback::FrameStore& s, const cv::Mat& m, uint64_t ts)
{
    s.pushFrame(m.data, m.total(), m.cols, m.rows, m.cols, 0x01080001, ts, ts);
}

struct Report {
    std::vector<std::string> entries;
    void add(const std::string& name, const std::string& jsonBody)
    {
        entries.push_back("  \"" + name + "\": {" + jsonBody + "}");
        std::printf("[bench] %s: {%s}\n", name.c_str(), jsonBody.c_str());
    }
    std::string json() const
    {
        std::string out = "{\n";
        for (size_t i = 0; i < entries.size(); ++i) out += entries[i] + (i + 1 < entries.size() ? ",\n" : "\n");
        return out + "}\n";
    }
};
std::string kv(const char* k, double v) { char b[64]; std::snprintf(b, sizeof(b), "\"%s\": %.3f", k, v); return b; }
std::string kv(const char* k, uint64_t v) { return std::string("\"") + k + "\": " + std::to_string(v); }
std::string kv(const char* k, bool v) { return std::string("\"") + k + "\": " + (v ? "true" : "false"); }
} // namespace

int main()
{
    mib::test::Watchdog wd(560);
    Report report;
    const double rss0 = rssMB();
    report.add("environment", kv("rss_start_mb", rss0) + ", " + kv("frame_w", static_cast<uint64_t>(kW)) + ", " + kv("frame_h", static_cast<uint64_t>(kH)) +
                                  ", " + kv("threads", static_cast<uint64_t>(std::thread::hardware_concurrency())));

    // ---- A. per-object copies -------------------------------------------------------
    wd.mark("A");
    {
        ProcessingService svc;
        svc.start(1);
        std::string err;
        MIB_REQUIRE(svc.activateBundledProcessingKernel(&err), "bundled kernel: " + err);
        const auto cfg = lenientConfig(svc.getProcessingConfig());
        for (int objects : {0, 1, 8}) {
            std::vector<cv::Mat> inputs;
            for (uint64_t i = 0; i < 200; ++i) inputs.push_back(objectsFrame(kW, kH, objects, i));
            const double r0 = rssMB();
            const auto t0 = Clock::now();
            const auto results = svc.processBatch(inputs, cfg, cv::Mat{}, ProcessingService::Roi{0, 0, kW, kH});
            const auto t1 = Clock::now();
            std::set<const uchar*> uniqueOriginals, uniqueMasks;
            uint64_t retained = 0;
            for (const auto& r : results) {
                uniqueOriginals.insert(r.originalImage.data);
                uniqueMasks.insert(r.processedImage.data);
                retained += processedFrameBytes(r);
            }
            const uint64_t uniqueBytes = (uniqueOriginals.size() + uniqueMasks.size()) * kMatBytes;
            const uint64_t cloneCost = static_cast<uint64_t>(results.size()) * 2 * kMatBytes; // former clone-per-object
            const double copiesPerObject = results.empty() ? 0.0 : static_cast<double>(uniqueBytes) / (2.0 * kMatBytes) / static_cast<double>(results.size());
            MIB_EXPECT(uniqueOriginals.size() <= inputs.size() && uniqueMasks.size() <= inputs.size(), "at most one source + one mask allocation per frame");
            const std::string name = "A_objects_" + std::to_string(objects);
            report.add(name, kv("frames", static_cast<uint64_t>(inputs.size())) + ", " + kv("results", static_cast<uint64_t>(results.size())) + ", " +
                                 kv("ms_per_frame", ms(t0, t1) / static_cast<double>(inputs.size())) + ", " +
                                 kv("unique_allocation_bytes", uniqueBytes) + ", " + kv("retained_bytes_per_owner_sum", retained) + ", " +
                                 kv("former_clone_per_object_bytes", cloneCost) + ", " + kv("allocations_per_result", copiesPerObject) + ", " +
                                 kv("rss_delta_mb", rssMB() - r0));
        }
        svc.stop();
    }

    // ---- B. slow persistence consumer: buffer plateau at its byte budget ----------
    wd.mark("B");
    {
        const uint64_t budget = 64ULL * 1024 * 1024;
        ExperimentFrameBuffer buf({5000, budget});
        const double r0 = rssMB();
        uint64_t peak = 0, evicted = 0;
        const auto t0 = Clock::now();
        const uint64_t frames = 20000; // ~2 GB of input if unbounded
        for (uint64_t i = 0; i < frames; ++i) {
            const auto r = buf.append(syntheticFrame(kW, kH, i), (i % 7) != 0);
            peak = std::max(peak, r.bytesAfter);
            evicted += r.dropped();
        }
        const auto t1 = Clock::now();
        const auto st = buf.memoryStats();
        MIB_EXPECT(peak <= budget && st.peakBytes <= budget, "never above the byte budget");
        MIB_EXPECT(st.currentCount + evicted == frames, "every frame stored or reported evicted");
        MIB_EXPECT(rssMB() - r0 < (budget / (1024.0 * 1024.0)) * 1.5 + 64.0, "RSS growth stays near the budget");
        report.add("B_slow_persistence", kv("frames", frames) + ", " + kv("budget_bytes", budget) + ", " + kv("peak_bytes", peak) + ", " +
                                             kv("retained_frames", st.currentCount) + ", " + kv("evicted", evicted) + ", " +
                                             kv("us_per_append", ms(t0, t1) * 1000.0 / static_cast<double>(frames)) + ", " + kv("rss_delta_mb", rssMB() - r0));
    }

    // ---- C. larger frames: byte budget bounds before the frame cap ----------------
    wd.mark("C");
    {
        const int w = 2048, h = 1088; // ~2.2 MB per image, ~4.4 MB per frame
        const uint64_t budget = 256ULL * 1024 * 1024;
        ExperimentFrameBuffer buf({1000, budget});
        const double r0 = rssMB();
        uint64_t evicted = 0;
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < 300; ++i) evicted += buf.append(syntheticFrame(w, h, i), true).dropped();
        const auto st = buf.memoryStats();
        const uint64_t perFrame = 2ULL * w * h;
        MIB_EXPECT(st.currentBytes <= budget && st.currentCount == budget / perFrame && st.currentCount < 1000, "byte budget bounds large frames below the frame cap");
        report.add("C_large_frames", kv("frame_bytes", perFrame) + ", " + kv("budget_bytes", budget) + ", " + kv("retained_frames", st.currentCount) + ", " +
                                         kv("frame_cap", static_cast<uint64_t>(1000)) + ", " + kv("evicted", evicted) + ", " +
                                         kv("ms_total", ms(t0, Clock::now())) + ", " + kv("rss_delta_mb", rssMB() - r0));
        buf.clear();
    }

    // ---- D/E/G. realtime pipeline: monitoring plateau, store plateau, series ------
    wd.mark("D");
    {
        auto store = std::make_shared<backend::playback::FrameStore>(512);
        store->reserveFrameBytes(kMatBytes);
        ProcessingService svc;
        svc.start(2);
        std::string err;
        MIB_REQUIRE(svc.activateBundledProcessingKernel(&err), "bundled kernel: " + err);
        svc.setProcessingConfig(lenientConfig(svc.getProcessingConfig()));
        svc.setRealtimeRoi(ProcessingService::Roi{0, 0, kW, kH});
        svc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
        svc.setRealtimeDropFrames(false); // EveryFrame consumer
        svc.setMonitoringActive(true);
        svc.startRealtime(store);
        auto processed = [&] { return svc.getIdentificationCounters().framesProcessed; };
        const double r0 = rssMB();
        uint64_t ts = 1;
        // Warm-up: write index 0 is the realtime loop's never-consumed sentinel.
        pushMat(*store, mib::test::ringFrame(kW, kH, 0), ts++);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto pushN = [&](uint64_t from, uint64_t to) {
            for (uint64_t i = from; i < to; ++i) {
                pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);
                if ((i % 256) == 255) waitFor([&] { return processed() >= i - 200; }, std::chrono::seconds(30));
            }
            waitFor([&] { return processed() >= to; }, std::chrono::seconds(60));
        };
        const auto t0 = Clock::now();
        pushN(0, 3000);
        const auto m1 = svc.memoryStats();
        const auto s1 = store->memoryStats();
        pushN(3000, 6000);
        const auto t1 = Clock::now();
        const auto m2 = svc.memoryStats();
        const auto s2 = store->memoryStats();
        MIB_EXPECT(m2.monitoringRings.currentBytes == m1.monitoringRings.currentBytes && m1.monitoringRings.currentBytes > 0,
                   "monitoring rings at a plateau behind a consumer that never reads");
        MIB_EXPECT(s2.currentBytes == s1.currentBytes && s1.currentBytes >= 512 * kMatBytes, "FrameStore retained bytes constant (EveryFrame)");
        MIB_EXPECT(m2.snapshot.currentCount == 1, "one presentation snapshot");
        report.add("D_long_monitoring", kv("frames", static_cast<uint64_t>(6000)) + ", " + kv("processed", processed()) + ", " +
                                            kv("ring_bytes_at_3k", m1.monitoringRings.currentBytes) + ", " + kv("ring_bytes_at_6k", m2.monitoringRings.currentBytes) + ", " +
                                            kv("ring_entries", m2.monitoringRings.currentCount) + ", " + kv("ring_replaced", m2.monitoringRings.evictedByBudget) + ", " +
                                            kv("frames_per_s", 6000.0 / (ms(t0, t1) / 1000.0)) + ", " + kv("rss_delta_mb", rssMB() - r0));
        report.add("E_frame_store_everyframe", kv("capacity", s2.capacityCount) + ", " + kv("retained_bytes", s2.currentBytes) + ", " +
                                                   kv("peak_bytes", s2.peakBytes) + ", " + kv("overwrites", s2.evictedByBudget) + ", " + kv("plateau", s2.currentBytes == s1.currentBytes));
        // LatestFrame-style consumer: drop-to-latest, same bounded store.
        svc.setRealtimeDropFrames(true);
        const auto t2 = Clock::now();
        for (uint64_t i = 6000; i < 16000; ++i) pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the drop-to-latest loop settle
        const auto s3 = store->memoryStats();
        const auto m3 = svc.memoryStats();
        MIB_EXPECT(s3.currentBytes == s1.currentBytes, "FrameStore retained bytes constant (LatestFrame-style consumer)");
        MIB_EXPECT(m3.monitoringRings.currentBytes <= m1.monitoringRings.currentBytes, "rings still bounded under drop-to-latest");
        report.add("E_frame_store_latestframe", kv("pushed", static_cast<uint64_t>(10000)) + ", " + kv("processed_delta", processed()) + ", " +
                                                    kv("retained_bytes", s3.currentBytes) + ", " + kv("ms_push_loop", ms(t2, Clock::now())));
        svc.stopRealtime();

        // G. multi-image series retention (batch path, bounded by count).
        {
            auto cfg = lenientConfig(svc.getProcessingConfig());
            cfg.multi_image_enabled = true;
            cfg.multi_image_count = 4;
            std::vector<cv::Mat> inputs;
            for (uint64_t i = 0; i < 60; ++i) inputs.push_back(objectsFrame(kW, kH, 1, i));
            const auto results = svc.processBatch(inputs, cfg, cv::Mat{}, ProcessingService::Roi{0, 0, kW, kH});
            size_t maxSeries = 0;
            uint64_t retained = 0;
            for (const auto& r : results) {
                maxSeries = std::max(maxSeries, r.seriesImages.size());
                retained += processedFrameBytes(r);
            }
            MIB_EXPECT(maxSeries <= 4, "series retention bounded by multi_image_count");
            report.add("G_series_retention", kv("results", static_cast<uint64_t>(results.size())) + ", " + kv("max_series_images", static_cast<uint64_t>(maxSeries)) + ", " +
                                                 kv("retained_bytes", retained));
        }
        svc.stop();
    }

    // ---- F. 1M-frame metadata stress (accounting only) ------------------------------
    wd.mark("F");
    {
        backend::recording::RecordingAccounting acc;
        acc.reset(1, false);
        const double r0 = rssMB();
        const auto t0 = Clock::now();
        const uint64_t n = 1'000'000;
        for (uint64_t i = 0; i < n; ++i) {
            acc.admit(i);
            acc.count((i % 10) == 0 ? backend::recording::FrameOutcome::Empty : backend::recording::FrameOutcome::Processed);
        }
        auto s = acc.snapshot();
        s.persistenceAdmitted = s.processed;
        s.persistenceCommitted = s.processed;
        s = backend::recording::reconcile(s);
        const auto t1 = Clock::now();
        MIB_EXPECT(s.admitted == n && s.empty + s.processed == n && s.sequenceGaps == 0, "1M frames accounted exactly");
        MIB_EXPECT(rssMB() - r0 < 16.0, "metadata stress adds no per-frame retention");
        report.add("F_metadata_1M", kv("frames", n) + ", " + kv("ns_per_frame", ms(t0, t1) * 1e6 / static_cast<double>(n)) + ", " +
                                        kv("rss_delta_mb", rssMB() - r0) + ", " + kv("completion", static_cast<uint64_t>(s.completion)));
    }

    report.add("summary", kv("rss_end_mb", rssMB()) + ", " + kv("rss_peak_mb", rssMB(true)) + ", " + kv("rss_delta_total_mb", rssMB() - rss0));
    if (const char* path = std::getenv("MIB_MEMORY_BENCH_REPORT")) {
        std::ofstream out(path);
        out << report.json();
        std::printf("[bench] report written to %s\n", path);
    }
    return mib::test::exitCode();
}
