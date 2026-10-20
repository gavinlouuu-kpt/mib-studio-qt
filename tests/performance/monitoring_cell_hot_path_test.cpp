// monitoring_cell_hot_path_test (#716 A6a, bridge ABI 41) — what live cell
// images cost the acquisition/processing path.
//
// The change on the hot path itself is one 8-byte store per monitoring append
// (the run tag, into a preallocated array, under the lock the append already
// holds). What remains is contention: readers of the monitoring ring hold the
// same lock. This measures it three ways, alternating windows with
//   off    no reader;
//   store  the live cell store at 500 Hz (refresh the strip, list it, copy
//          every image and mask; a pin and an unpin every 10 ms) — about 200x
//          the rate a UI polls at;
//   legacy the full-ring copy Qt's Monitoring tab makes (and
//          fetch_monitoring_snapshot made before ABI 41), at the same 500 Hz.
// 1. Throughput: rows the real realtime loop appends per second. Gate: the
//    store's windows keep >= 85% of the off windows (median of rounds).
// 2. Lock hold: the source calls the store makes (newest 24 valid rows; a
//    full-ring find for a pin; giving the shares back, also in the worst case
//    where the reader frees them) against the legacy full copy. Gate: the strip
//    read holds the ring's lock for less time than the legacy copy.
// 3. Append latency (printed): the monitoring append critical section timed
//    on a dedicated ring, p50/p99/max per mode.
// Ratio gates only; label `performance`, so the sanitizer lanes skip it.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using backend::playback::FrameStore;
using backend::services::MonitoringCellStore;
using backend::services::ProcessedFrame;
using backend::services::ProcessingService;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kW = 512; // the 512x96 production ROI
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

enum class Mode { Off, Store, Legacy };
const char* name(Mode m) { return m == Mode::Off ? "off" : m == Mode::Store ? "store" : "legacy"; }

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
}

// One reader at `mode`, 500 Hz, until stopped.
class Reader {
public:
    Reader(ProcessingService& proc, MonitoringCellStore& cells) : proc_(proc), cells_(cells)
    {
        thread_ = std::thread([this] { loop(); });
    }
    ~Reader()
    {
        stop_ = true;
        thread_.join();
    }
    void setMode(Mode m) { mode_.store(m); }

private:
    void loop()
    {
        uint64_t tick = 0;
        while (!stop_) {
            const Mode m = mode_.load();
            if (m == Mode::Store) {
                cells_.refreshStrip();
                const auto l = cells_.listing();
                for (const auto& c : l.strip) {
                    for (bool mask : {false, true}) {
                        cv::Mat image;
                        if (cells_.image(c.key, mask, image)) {
                            std::vector<uint8_t> copy(static_cast<size_t>(image.total()));
                            for (int r = 0; r < image.rows; ++r)
                                std::copy(image.ptr(r), image.ptr(r) + image.cols, copy.begin() + r * image.cols);
                        }
                    }
                }
                if ((++tick % 5) == 0 && !l.strip.empty()) {
                    const auto& k = l.strip.back().key;
                    cells_.pin(k);
                    cells_.unpin(k);
                }
            } else if (m == Mode::Legacy) {
                auto valid = proc_.getMonitoringValidFrames();
                auto invalid = proc_.getMonitoringInvalidFrames();
                (void)valid;
                (void)invalid;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ProcessingService& proc_;
    MonitoringCellStore& cells_;
    std::atomic<Mode> mode_{Mode::Off};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// ---- 1. throughput of the real realtime loop ------------------------------------------
void throughput()
{
    std::printf("throughput: realtime loop rows/s per reader mode\n");
    auto store = std::make_shared<FrameStore>(512);
    ProcessingService proc;
    proc.setProcessingConfig(lenient());
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setPixelToMicronFactor(0.5);
    proc.setRealtimeDropFrames(true);
    proc.setMonitoringActive(true);
    proc.start(1);
    proc.startRealtime(store);
    proc.setRealtimeEnabled(true);
    auto source = backend::services::makeProcessingCellSource(proc);
    MonitoringCellStore cells(*source);

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        uint64_t i = 0;
        cv::Mat m(kH, kW, CV_8UC1);
        while (!stop.load(std::memory_order_relaxed)) {
            m.setTo(0);
            cv::circle(m, cv::Point(40 + static_cast<int>(i % 400), kH / 2), 16, cv::Scalar(220), -1);
            store->pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH, static_cast<size_t>(kW), kMono8, 0);
            ++i;
            if ((i & 15) == 0) std::this_thread::yield();
        }
    });
    auto rows = [&] {
        const auto v = proc.visitMonitoringNewest(0, false, {});
        return v.validAppended + v.invalidAppended;
    };
    std::vector<double> rate[3];
    {
        Reader reader(proc, cells);
        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // warm-up
        for (int round = 0; round < 6; ++round) {
            for (Mode m : {Mode::Off, Mode::Store, Mode::Legacy}) {
                reader.setMode(m);
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); // settle
                const uint64_t r0 = rows();
                const auto t0 = Clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
                rate[static_cast<int>(m)].push_back(static_cast<double>(rows() - r0) / secs);
            }
        }
        reader.setMode(Mode::Off);
    }
    stop = true;
    producer.join();
    proc.stopRealtime();
    proc.stop();
    const double off = median(rate[0]), st = median(rate[1]), legacy = median(rate[2]);
    std::printf("  rows/s median: off %.0f, store %.0f (ratio %.3f), legacy %.0f (ratio %.3f); "
                "strip refreshes %llu, last refresh %llu us (source %llu us)\n",
                off, st, off > 0 ? st / off : 0.0, legacy, off > 0 ? legacy / off : 0.0,
                static_cast<unsigned long long>(cells.stats().refreshes),
                static_cast<unsigned long long>(cells.stats().lastRefreshUs),
                static_cast<unsigned long long>(cells.stats().lastSourceUs));
    MIB_REQUIRE(off > 0, "the realtime loop appended rows");
    MIB_EXPECT(st >= 0.85 * off, "the live cell store keeps >= 85% of the realtime rows/s");
}

// ---- 2. ring lock hold of the readers -------------------------------------------------
void lockHold()
{
    std::printf("lock hold: reader calls on a full ring (1000 valid + 1000 invalid, 512x96 images)\n");
    ProcessingService proc;
    proc.setMonitoringRunGeneration(1);
    const cv::Mat image(kH, kW, CV_8UC1, cv::Scalar(100)), mask(kH, kW, CV_8UC1, cv::Scalar(0));
    for (uint64_t i = 0; i < 2000; ++i) {
        ProcessedFrame f;
        f.index = i;
        f.originalImage = image; // shared, as the realtime path does
        f.processedImage = mask;
        f.validation.isValid = (i % 2) == 0;
        f.validation.objectId = 0;
        f.validation.bboxX = 100;
        f.validation.bboxY = 30;
        f.validation.bboxWidth = 30;
        f.validation.bboxHeight = 30;
        proc.appendMonitoringFrameForTests(f);
    }
    auto source = backend::services::makeProcessingCellSource(proc);
    std::vector<double> strip, find, legacy, snapshot, release;
    struct Row { uint64_t gen, index; int object; double area, deformability; };
    for (int i = 0; i < 300; ++i) {
        auto t0 = Clock::now();
        auto n = source->newestValid(MonitoringCellStore::kStripMaxCells);
        strip.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        t0 = Clock::now();
        auto hit = source->find(1998 - 2 * (i % 900), 0);
        find.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        // The shares go back under the ring's lock (decrements; the ring still holds the rows).
        const bool hadShares = !n.rows.empty() && !n.rows.front().image.empty();
        t0 = Clock::now();
        source->releaseShares(n.rows);
        release.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        source->releaseShares(hit.rows);
        MIB_REQUIRE(hadShares && n.rows.front().image.empty(), "the source dropped the strip read's shares");
        t0 = Clock::now();
        auto all = proc.getMonitoringValidFrames();
        auto allInvalid = proc.getMonitoringInvalidFrames();
        legacy.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        // fetch_monitoring_snapshot since ABI 41: POD rows built under the lock (2000 rows).
        std::vector<Row> rows;
        rows.reserve(2000);
        t0 = Clock::now();
        proc.visitMonitoringNewest(2000, true, [&rows](const ProcessedFrame& fr, const ProcessingService::MonitoringRowTag& tag, bool) {
            rows.push_back({tag.runGeneration, fr.index, fr.validation.objectId, fr.validation.area,
                            fr.validation.deformability});
        });
        snapshot.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        MIB_REQUIRE(n.rows.size() == MonitoringCellStore::kStripMaxCells && hit.rows.size() == 1 && all.size() == 1000 &&
                        allInvalid.size() == 1000,
                    "readers see the ring");
    }
    // Worst case of the release: the ring evicted every row meanwhile, so the reader holds the last
    // references and the frees (24 images + 24 masks of 512x96) happen under the ring's lock.
    std::vector<double> releaseFrees;
    for (int round = 0; round < 20; ++round) {
        ProcessingService ring;
        for (uint64_t i = 0; i < 1000; ++i) {
            ProcessedFrame f;
            f.index = i;
            f.originalImage = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(100)); // one buffer per row
            f.processedImage = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(0));
            f.validation.isValid = true;
            ring.appendMonitoringFrameForTests(f);
        }
        auto ringSource = backend::services::makeProcessingCellSource(ring);
        auto n = ringSource->newestValid(MonitoringCellStore::kStripMaxCells);
        for (uint64_t i = 1000; i < 2000; ++i) { // evict every row
            ProcessedFrame f;
            f.index = i;
            f.originalImage = image;
            f.validation.isValid = true;
            ring.appendMonitoringFrameForTests(f);
        }
        const auto t0 = Clock::now();
        ringSource->releaseShares(n.rows);
        releaseFrees.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    std::printf("  us median/p99: strip read %.1f/%.1f, pin find %.1f/%.1f, share release %.1f/%.1f "
                "(reader frees 48 buffers: %.1f/%.1f), snapshot rows (2000) %.1f/%.1f, legacy full copy %.1f/%.1f\n",
                median(strip), percentile(strip, 0.99), median(find), percentile(find, 0.99), median(release),
                percentile(release, 0.99), median(releaseFrees), percentile(releaseFrees, 0.99), median(snapshot),
                percentile(snapshot, 0.99), median(legacy), percentile(legacy, 0.99));
    MIB_EXPECT(median(strip) < median(legacy), "the strip read holds the ring's lock less than the legacy copy");
    MIB_EXPECT(median(snapshot) < median(legacy), "snapshot rows hold the ring's lock less than the legacy copy");
}

// ---- 3. append critical section latency -----------------------------------------------
void appendLatency()
{
    std::printf("append latency: monitoring append on a dedicated ring, per reader mode\n");
    ProcessingService proc;
    auto source = backend::services::makeProcessingCellSource(proc);
    MonitoringCellStore cells(*source);
    const cv::Mat image(kH, kW, CV_8UC1, cv::Scalar(100)), mask(kH, kW, CV_8UC1, cv::Scalar(0));
    ProcessedFrame f;
    f.originalImage = image;
    f.processedImage = mask;
    f.validation.isValid = true;
    f.validation.bboxX = 100;
    f.validation.bboxY = 30;
    f.validation.bboxWidth = 30;
    f.validation.bboxHeight = 30;
    uint64_t index = 0;
    for (int i = 0; i < 2000; ++i) {
        f.index = index++;
        proc.appendMonitoringFrameForTests(f);
    }
    Reader reader(proc, cells);
    for (Mode m : {Mode::Off, Mode::Store, Mode::Legacy}) {
        reader.setMode(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<double> ns;
        ns.reserve(200000);
        const auto end = Clock::now() + std::chrono::milliseconds(400);
        while (Clock::now() < end) {
            f.index = index++;
            const auto t0 = Clock::now();
            proc.appendMonitoringFrameForTests(f);
            ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
            // ~5 kHz pacing would leave the readers idle; append back to back instead (worst case).
        }
        std::printf("  %-6s appends %zu, ns p50 %.0f, p99 %.0f, p99.9 %.0f, max %.0f\n", name(m), ns.size(),
                    percentile(ns, 0.5), percentile(ns, 0.99), percentile(ns, 0.999),
                    *std::max_element(ns.begin(), ns.end()));
    }
    reader.setMode(Mode::Off);
}

} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    wd.mark("lock hold");
    lockHold();
    wd.mark("append latency");
    appendLatency();
    wd.mark("throughput");
    throughput();
    std::printf("monitoring_cell_hot_path_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
