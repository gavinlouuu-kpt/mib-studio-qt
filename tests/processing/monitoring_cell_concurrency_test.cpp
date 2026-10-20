// monitoring_cell_concurrency_test (#716 A6a, bridge ABI 41) — concurrency stress, TSan lane.
//
// Producer: the real ProcessingService realtime loop (inline mode) on blob
// frames pushed into a FrameStore flat out, monitoring active, so rows enter
// the monitoring ring through the real hot path (tagged under its lock).
// Consumers, all at once, against one MonitoringCellStore over the real ring:
//   - two clients refreshing the strip, listing it and copying every listed
//     image and mask (what fetch_monitoring_cells + the packet pulls do);
//   - a snapshot reader (visitMonitoringNewest over both rings + noteRows);
//   - a pinner pinning the newest rows by their identity, unpinning and
//     clearing when full;
//   - a run switcher: setMonitoringRunGeneration + beginRun, as the
//     coordinator does at Start and at the end of accumulation.
// Invariants checked on every listing: the caps hold (count and bytes), every
// pin belongs to the scope's run or to none, every cell has exactly one of an
// image and a reason, and a fetched image has the listed size. The run must
// make progress on every side (rows appended, strip copies, pins taken).

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/MonitoringCellStore.h"

#include "support/assert.h"
#include "support/opencv_tsan.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using backend::playback::FrameStore;
using backend::services::MonitoringCellKey;
using backend::services::MonitoringCellMetrics;
using backend::services::MonitoringCellStore;
using backend::services::ProcessedFrame;
using backend::services::ProcessingService;
using Outcome = MonitoringCellStore::PinOutcome;

namespace {

constexpr int kW = 160;
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

// Failures from worker threads, reported on the main thread.
std::mutex gFailMutex;
std::vector<std::string> gFailures;
void fail(const std::string& what)
{
    std::scoped_lock lk(gFailMutex);
    if (gFailures.size() < 20) gFailures.push_back(what);
}

void checkListing(const MonitoringCellStore::Listing& l)
{
    const auto& p = l.policy;
    if (l.strip.size() > p.stripMaxCells) fail("strip over its count cap");
    if (l.stats.stripBytes > p.stripMaxBytes) fail("strip over its byte cap");
    if (l.pins.size() > p.maxPins) fail("pins over their count cap");
    if (l.stats.pinBytes > p.pinMaxBytes) fail("pins over their byte cap");
    std::uint64_t stripBytes = 0, pinBytes = 0;
    for (const auto& c : l.strip) {
        stripBytes += c.bytes;
        if (c.hasImage == !c.noImageReason.empty()) fail("strip cell: image xor reason");
    }
    for (const auto& c : l.pins) {
        pinBytes += c.bytes;
        if (c.key.startGeneration != 0 && c.key.startGeneration != l.scopeGeneration)
            fail("a pin of run " + std::to_string(c.key.startGeneration) + " under scope " +
                 std::to_string(l.scopeGeneration));
        if (c.hasImage == !c.noImageReason.empty()) fail("pin: image xor reason");
    }
    if (stripBytes != l.stats.stripBytes) fail("strip byte accounting drifted");
    if (pinBytes != l.stats.pinBytes) fail("pin byte accounting drifted");
}

} // namespace

int main()
{
    mib::test::serializeOpenCvUnderTsan();
    mib::test::Watchdog wd(120);

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
    std::atomic<uint64_t> pushed{0}, listings{0}, imageCopies{0}, pinsTaken{0}, pinRefusals{0}, runSwitches{0},
        snapshots{0};
    std::vector<std::thread> threads;

    // Producer: blob frames, the cell moving along the channel.
    threads.emplace_back([&] {
        uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
            const int cx = 30 + static_cast<int>(i % 100);
            cv::circle(m, cv::Point(cx, kH / 2), 14, cv::Scalar(220), -1);
            store->pushFrame(m.data, static_cast<size_t>(m.total()), kW, kH, static_cast<size_t>(kW), kMono8, 0);
            pushed.fetch_add(1, std::memory_order_relaxed);
            ++i;
            if ((i & 63) == 0) std::this_thread::yield();
        }
    });

    // Two strip clients.
    for (int client = 0; client < 2; ++client) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                cells.refreshStrip();
                const auto l = cells.listing();
                checkListing(l);
                listings.fetch_add(1, std::memory_order_relaxed);
                for (const auto* group : {&l.strip, &l.pins}) {
                    for (const auto& c : *group) {
                        for (bool mask : {false, true}) {
                            cv::Mat image;
                            backend::services::MonitoringCellInfo info;
                            if (!cells.image(c.key, mask, image, &info)) continue; // evicted or unpinned meanwhile
                            if (image.cols != info.width || image.rows != info.height || image.type() != CV_8UC1)
                                fail("an image does not match its listing");
                            // Read every pixel of the private copy, as the facade does.
                            std::vector<std::uint8_t> bytes(static_cast<size_t>(image.total()));
                            for (int r = 0; r < image.rows; ++r)
                                std::copy(image.ptr(r), image.ptr(r) + image.cols, bytes.begin() + r * image.cols);
                            imageCopies.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }

    // Snapshot reader: what fetchMonitoringSnapshot does (POD rows under the ring's lock + noteRows).
    std::mutex newestMutex;
    std::vector<MonitoringCellKey> newestRows;
    threads.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<std::pair<MonitoringCellKey, MonitoringCellMetrics>> rows;
            rows.reserve(2000);
            proc.visitMonitoringNewest(2000, true, [&](const ProcessedFrame& f, const ProcessingService::MonitoringRowTag& tag, bool) {
                rows.push_back({{tag.runGeneration, f.index, f.validation.objectId, tag.captureEpoch},
                                backend::services::monitoringCellMetrics(f.validation, f.timestampNs,
                                                                         {tag.geometryOriginX, tag.geometryOriginY})});
            });
            cells.noteRows(rows);
            {
                std::scoped_lock lk(newestMutex);
                newestRows.clear();
                for (std::size_t k = rows.size() > 8 ? rows.size() - 8 : 0; k < rows.size(); ++k)
                    newestRows.push_back(rows[k].first);
            }
            snapshots.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Pinner: pins the newest rows by identity; frees room when full.
    threads.emplace_back([&] {
        uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<MonitoringCellKey> keys;
            {
                std::scoped_lock lk(newestMutex);
                keys = newestRows;
            }
            for (const auto& k : keys) {
                const auto r = cells.pin(k);
                if (r.outcome == Outcome::Pinned || r.outcome == Outcome::PinnedWithoutImage)
                    pinsTaken.fetch_add(1, std::memory_order_relaxed);
                else if (r.outcome != Outcome::AlreadyPinned)
                    pinRefusals.fetch_add(1, std::memory_order_relaxed);
                if (r.outcome == Outcome::LimitReached) {
                    const auto l = cells.listing();
                    if (!l.pins.empty()) cells.unpin(l.pins.front().key);
                }
                if (r.cell && r.cell->key != k) fail("a pin reply names another cell");
            }
            if ((++n % 50) == 0) cells.clearPins();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Run switcher: Start (tag, then beginRun) and the end of accumulation (tag 0).
    threads.emplace_back([&] {
        uint64_t generation = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++generation;
            proc.setMonitoringRunGeneration(generation);
            cells.beginRun(generation);
            runSwitches.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            proc.setMonitoringRunGeneration(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        wd.mark("stress");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stop.store(true);
    for (auto& t : threads) t.join();
    wd.mark("joined");
    proc.stopRealtime();
    proc.stop();

    const auto view = proc.visitMonitoringNewest(0, false, {});
    checkListing(cells.listing());
    std::printf("pushed %llu frames, ring appended %llu valid / %llu invalid, %llu listings, %llu image copies, "
                "%llu pins taken, %llu refused, %llu run switches, %llu snapshots; strip admitted %llu, evicted %llu\n",
                static_cast<unsigned long long>(pushed.load()),
                static_cast<unsigned long long>(view.validAppended),
                static_cast<unsigned long long>(view.invalidAppended),
                static_cast<unsigned long long>(listings.load()),
                static_cast<unsigned long long>(imageCopies.load()),
                static_cast<unsigned long long>(pinsTaken.load()),
                static_cast<unsigned long long>(pinRefusals.load()),
                static_cast<unsigned long long>(runSwitches.load()),
                static_cast<unsigned long long>(snapshots.load()),
                static_cast<unsigned long long>(cells.listing().stats.stripAdmitted),
                static_cast<unsigned long long>(cells.listing().stats.stripEvicted));
    for (const auto& f : gFailures) MIB_EXPECT(false, f);
    MIB_EXPECT(view.validAppended > 0, "the realtime path appended valid rows");
    MIB_EXPECT(cells.listing().stats.stripAdmitted > 0, "the strip took copies under load");
    MIB_EXPECT(imageCopies.load() > 0, "images were copied out under load");
    MIB_EXPECT(pinsTaken.load() > 0, "pins were taken under load");
    MIB_EXPECT(runSwitches.load() > 2, "runs switched under load");
    std::printf("monitoring_cell_concurrency_test: %s\n", mib::test::exitCode() == 0 ? "PASS" : "FAIL");
    return mib::test::exitCode();
}
