// memory_budget_test (issue #370)
//
// Deterministic ownership/retention policy checks:
//  - ByteAccountant current/peak/evicted semantics;
//  - ExperimentFrameBuffer: byte budget AND frame cap, sampled-invalid-first
//    eviction, valid never evicts valid to enter, every eviction reported,
//    takeAll transfers without copies, series bytes counted once;
//  - FrameStore: measured retained bytes after reserve, a plateau across
//    overwrites (ring policy), re-accounting after resize;
//  - async batch queue: byte budget drops are counted separately from the
//    frame-cap drops and the queue bytes never exceed the budget;
//  - per-object results of one frame share the source/mask allocation (no
//    clone for lifetime) and scientific results are unchanged by sharing;
//  - realtime path: monitoring rings reach a bounded byte plateau behind a
//    consumer that never reads them; the experiment byte budget evicts under
//    the declared policy and the run accounting still reconciles;
//    presentation (snapshot) retention is one frame.

#include "backend/diagnostics/MemoryBudget.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ExperimentFrameBuffer.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <functional>
#include <set>
#include <thread>

using namespace backend::services;
namespace diag = backend::diagnostics;

namespace {
constexpr int kW = 512;
constexpr int kH = 96;
constexpr uint64_t kMatBytes = static_cast<uint64_t>(kW) * kH; // Mono8

ProcessedFrame makeFrame(uint64_t idx, bool valid = true)
{
    ProcessedFrame f;
    f.index = idx;
    f.originalImage = mib::test::ringFrame(kW, kH, idx);
    f.processedImage = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(0));
    f.validation.isValid = valid;
    return f;
}

cv::Mat multiObjectFrame(int objects, uint64_t i)
{
    // Objects jump between frames so batch tracking sees new tracks (one
    // result per object per frame) instead of matching the previous frame.
    cv::Mat m(kH, kW, CV_8UC1, cv::Scalar(0));
    for (int k = 0; k < objects; ++k) {
        const int cx = 30 + ((k * 60 + static_cast<int>((i * 37) % 200)) % (kW - 60));
        cv::circle(m, cv::Point(cx, kH / 2), 14, cv::Scalar(220), -1);
        cv::circle(m, cv::Point(cx, kH / 2), 7, cv::Scalar(0), -1);
    }
    return m;
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

void pushMat(backend::playback::FrameStore& s, const cv::Mat& m, uint64_t ts)
{
    s.pushFrame(m.data, m.total(), m.cols, m.rows, m.cols, 0x01080001, ts, ts);
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
} // namespace

int main()
{
    mib::test::Watchdog wd(150);
    mib::test::TempDir td("memory_budget");

    // ---- 1. ByteAccountant ----------------------------------------------------
    {
        diag::ByteAccountant a;
        a.add(100, 1);
        a.add(50, 1);
        MIB_EXPECT(a.bytes() == 150 && a.count() == 2 && a.peakBytes() == 150 && a.peakCount() == 2, "add tracks current+peak");
        a.remove(100, 1);
        MIB_EXPECT(a.bytes() == 50 && a.count() == 1 && a.peakBytes() == 150, "remove keeps the peak");
        a.remove(999, 5);
        MIB_EXPECT(a.bytes() == 0 && a.count() == 0, "remove saturates at zero");
        a.set(70, 3);
        a.noteEvicted(2);
        const auto s = a.snapshot("x", diag::MemoryKnowledge::Measured, 60, 10, "n");
        MIB_EXPECT(s.currentBytes == 70 && s.peakBytes == 150 && s.capacityBytes == 60 && s.overBudget() && s.evictedByBudget == 2,
                   "snapshot carries budget + over-budget flag");
        a.reset();
        MIB_EXPECT(a.peakBytes() == 0 && a.evicted() == 2, "reset clears bytes/peaks, not the eviction tally");
    }

    // ---- 2. ExperimentFrameBuffer ----------------------------------------------
    wd.mark("experiment buffer");
    {
        MIB_EXPECT(processedFrameBytes(makeFrame(0)) == 2 * kMatBytes, "frame bytes = original + mask");
        ProcessedFrame series = makeFrame(1);
        series.seriesImages.push_back(series.originalImage); // trigger image shared
        series.seriesImages.push_back(mib::test::ringFrame(kW, kH, 2));
        series.seriesImages.push_back(mib::test::ringFrame(kW, kH, 3));
        MIB_EXPECT(processedFrameBytes(series) == 4 * kMatBytes, "shared trigger image counted once; extra series images counted");

        // Byte budget with an all-valid backlog: the newcomer is refused, every
        // refusal reported, bytes never exceed the budget.
        const uint64_t budget = 50 * 2 * kMatBytes;
        ExperimentFrameBuffer buf({1000, budget});
        uint64_t reportedDrops = 0, maxBytes = 0;
        for (uint64_t i = 0; i < 200; ++i) {
            const auto r = buf.append(makeFrame(i), true);
            reportedDrops += r.dropped();
            maxBytes = std::max(maxBytes, r.bytesAfter);
            MIB_EXPECT(r.bytesAfter <= budget, "bytes never exceed the budget");
        }
        MIB_EXPECT(buf.counts().valid == 50 && buf.counts().invalid == 0, "50 valid retained under the byte budget");
        MIB_EXPECT(reportedDrops == 150 && buf.totalDroppedValid() == 150, "150 refused valid frames all reported");
        const auto ms = buf.memoryStats();
        MIB_EXPECT(ms.currentBytes == budget && ms.peakBytes == budget && ms.capacityBytes == budget && !ms.overBudget()
                       && ms.evictedByBudget == 150 && ms.currentCount == 50 && ms.capacityCount == 1000,
                   "memory stats: current/peak/capacity/evicted");

        // Frame cap: invalid evicted first; a valid newcomer displaces invalid; an
        // invalid newcomer is refused when full.
        ExperimentFrameBuffer small({10, 0});
        for (uint64_t i = 0; i < 5; ++i) small.append(makeFrame(i, false), false);
        for (uint64_t i = 5; i < 10; ++i) small.append(makeFrame(i, true), true);
        auto r = small.append(makeFrame(10, true), true);
        MIB_EXPECT(r.stored && r.droppedInvalid == 1 && r.droppedValid == 0 && small.counts().valid == 6 && small.counts().invalid == 4,
                   "valid newcomer evicts the oldest invalid");
        r = small.append(makeFrame(11, false), false);
        MIB_EXPECT(!r.stored && r.droppedInvalid == 1 && small.counts().total() == 10, "invalid newcomer refused when full");
        for (uint64_t i = 12; i < 20; ++i) small.append(makeFrame(i, true), true);
        MIB_EXPECT(small.counts().invalid == 0 && small.counts().valid == 10, "all invalid evicted before any valid");
        r = small.append(makeFrame(20, true), true);
        MIB_EXPECT(!r.stored && r.droppedValid == 1 && small.counts().valid == 10, "valid never evicts valid to enter");

        // takeAll moves (refcount transfer), leaves the buffer empty, keeps totals.
        std::vector<ProcessedFrame> v, inv;
        const uint64_t droppedBefore = small.totalDroppedValid();
        small.takeAll(v, inv);
        MIB_EXPECT(v.size() == 10 && inv.empty() && small.empty() && small.bytes() == 0 && small.totalDroppedValid() == droppedBefore,
                   "takeAll empties without losing accounting");
        MIB_EXPECT(v[0].originalImage.u != nullptr && v[0].originalImage.u->refcount == 1, "moved-out image has a single owner (no copy)");
        // Tightening the policy trims immediately on the next append.
        buf.setPolicy({5, 0});
        r = buf.append(makeFrame(500, false), false);
        MIB_EXPECT(buf.counts().total() <= 5 && r.dropped() >= 45, "policy change trims to the new bound and reports it");
    }

    // ---- 3. FrameStore retained bytes ----------------------------------------
    wd.mark("frame store");
    {
        backend::playback::FrameStore store(64);
        MIB_EXPECT(store.memoryStats().currentBytes == 0 && store.memoryStats().knowledge == diag::MemoryKnowledge::Measured, "empty store: 0 measured");
        store.reserveFrameBytes(kMatBytes);
        auto ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes >= 64 * kMatBytes && ms.capacityBytes == 64 * kMatBytes && ms.currentCount == 0,
                   "reserve makes the retained allocation explicit before any frame");
        const uint64_t afterReserve = ms.currentBytes;
        for (uint64_t i = 0; i < 200; ++i) pushMat(store, mib::test::ringFrame(kW, kH, i), i + 1);
        ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes == afterReserve && ms.peakBytes == afterReserve, "pushing through the ring does not grow retained bytes (plateau)");
        MIB_EXPECT(ms.currentCount == 64 && ms.peakCount == 64 && ms.capacityCount == 64 && ms.evictedByBudget == 136, "counts + overwrites");
        MIB_EXPECT(store.resize(16), "resize");
        ms = store.memoryStats();
        MIB_EXPECT(ms.currentBytes <= 16 * kMatBytes + 16 * 64 && ms.currentCount <= 16, "resize re-accounts the smaller ring");
    }

    // ---- 4. batch queue byte budget ----------------------------------------------
    wd.mark("batch queue");
    {
        ProcessingService svc;
        svc.start(1);
        std::string err;
        MIB_REQUIRE(svc.activateBundledProcessingKernel(&err), "bundled kernel: " + err);
        ProcessingService::BatchPipelineConfig cfg;
        cfg.batchSize = 64;           // workers wait for 64 frames ...
        cfg.maxBatchDelayMs = 2000;   // ... or 2 s, so the queue holds what we enqueue
        cfg.maxQueuedFrames = 1000;
        cfg.maxQueuedBytes = 3 * kMatBytes;
        cfg.processing = lenientConfig(svc.getProcessingConfig());
        cfg.roi = ProcessingService::Roi{0, 0, kW, kH};
        std::atomic<size_t> delivered{0};
        MIB_REQUIRE(svc.startBatchPipeline(cfg, [&](std::vector<ProcessedFrame> r) { delivered += r.size(); }), "start batch pipeline");
        int accepted = 0;
        for (uint64_t i = 0; i < 10; ++i) accepted += svc.enqueueBatchFrame(mib::test::ringFrame(kW, kH, i), i) ? 1 : 0;
        auto st = svc.getBatchPipelineStats();
        MIB_EXPECT(accepted == 3 && st.framesAccepted == 3, "three frames fit the byte budget");
        MIB_EXPECT(st.framesDropped == 7 && st.framesDroppedByByteBudget == 7, "byte-budget drops counted (and as drops)");
        MIB_EXPECT(st.currentQueueBytes == 3 * kMatBytes && st.maxQueueBytes == 3 * kMatBytes, "queue bytes = accepted frames");
        const auto pm = svc.memoryStats();
        MIB_EXPECT(pm.batchQueue.currentBytes == 3 * kMatBytes && pm.batchQueue.capacityBytes == 3 * kMatBytes && !pm.batchQueue.overBudget(),
                   "batch queue owner reports its budget");
        svc.stopBatchPipeline();
        MIB_EXPECT(svc.getBatchPipelineStats().currentQueueBytes == 0, "stop releases the queue bytes");
        MIB_EXPECT(delivered.load() == 3, "drained on stop");
        svc.stop();
    }

    // ---- 5. per-object sharing (no clone for lifetime) ----------------------------
    wd.mark("per-object sharing");
    {
        ProcessingService svc;
        svc.start(1);
        std::string err;
        MIB_REQUIRE(svc.activateBundledProcessingKernel(&err), "bundled kernel: " + err);
        const auto cfg = lenientConfig(svc.getProcessingConfig());
        std::vector<cv::Mat> inputs;
        for (uint64_t i = 0; i < 5; ++i) inputs.push_back(multiObjectFrame(6, i));
        const auto results = svc.processBatch(inputs, cfg, cv::Mat{}, ProcessingService::Roi{0, 0, kW, kH});
        MIB_REQUIRE(results.size() >= 2, "objects detected");
        std::set<uint64_t> indices;
        size_t shared = 0, perFrameObjects = 0;
        uint64_t retained = 0;
        std::set<const uchar*> uniqueOriginals;
        for (const auto& r : results) {
            indices.insert(r.index);
            retained += processedFrameBytes(r);
            uniqueOriginals.insert(r.originalImage.data);
        }
        for (const auto& r : results)
            if (r.index == results[0].index) {
                ++perFrameObjects;
                if (r.originalImage.data == results[0].originalImage.data && r.processedImage.data == results[0].processedImage.data) ++shared;
            }
        MIB_EXPECT(perFrameObjects >= 2 && shared == perFrameObjects, "objects of one frame share source + mask allocations");
        MIB_EXPECT(uniqueOriginals.size() == indices.size(), "one source allocation per frame, not per object");
        MIB_EXPECT(retained == results.size() * 2 * kMatBytes, "per-owner accounting counts the shared frame for every holder (upper bound)");
        // Science unchanged by sharing: identical metrics from a cloned re-run.
        const auto again = svc.processBatch(inputs, cfg, cv::Mat{}, ProcessingService::Roi{0, 0, kW, kH});
        MIB_REQUIRE(again.size() == results.size(), "deterministic object count");
        bool same = true;
        for (size_t i = 0; i < results.size(); ++i)
            same = same && results[i].validation.area == again[i].validation.area &&
                   results[i].validation.deformability == again[i].validation.deformability &&
                   results[i].validation.isValid == again[i].validation.isValid;
        MIB_EXPECT(same, "scientific results identical across runs");
        svc.stop();
    }

    // ---- 6. realtime path: monitoring plateau, experiment byte budget, snapshot ---
    wd.mark("realtime");
    {
        auto store = std::make_shared<backend::playback::FrameStore>(4096);
        store->reserveFrameBytes(kMatBytes);
        ProcessingService svc;
        svc.start(2);
        std::string err;
        MIB_REQUIRE(svc.activateBundledProcessingKernel(&err), "bundled kernel: " + err);
        svc.setProcessingConfig(lenientConfig(svc.getProcessingConfig()));
        svc.setRealtimeRoi(ProcessingService::Roi{0, 0, kW, kH});
        svc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
        svc.setRealtimeDropFrames(false);
        svc.setInvalidFrameSamplingRate(1);
        svc.setMonitoringActive(true);
        svc.startRealtime(store);
        MIB_REQUIRE(svc.isRealtimeRunning(), "realtime running");

        // No consumer ever reads the rings; they must still plateau.
        uint64_t ts = 1;
        auto processed = [&] { return svc.getIdentificationCounters().framesProcessed; };
        // Warm-up frame: write index 0 is the realtime loop's never-consumed
        // sentinel, so it is pushed outside the counted range.
        pushMat(*store, mib::test::ringFrame(kW, kH, 0), ts++);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Producer first (the 4096-slot store holds all of them), then the
        // consumer, so the two rates are reported separately.
        const auto tPush0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 2500; ++i) pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);
        const auto tPush1 = std::chrono::steady_clock::now();
        const bool done = waitFor([&] { return processed() >= 2500; }, std::chrono::seconds(60));
        const auto tProc1 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "realtime: push 2500 frames %.1f ms; processed %llu in %.1f ms (%.0f fps); monitoring active\n",
                     std::chrono::duration<double, std::milli>(tPush1 - tPush0).count(),
                     static_cast<unsigned long long>(processed()),
                     std::chrono::duration<double, std::milli>(tProc1 - tPush0).count(),
                     static_cast<double>(processed()) / std::chrono::duration<double>(tProc1 - tPush0).count());
        MIB_REQUIRE(done, "2500 frames processed");
        auto m1 = svc.memoryStats();
        MIB_EXPECT(m1.monitoringRings.currentCount <= 2000 && m1.monitoringRings.currentBytes > 0, "monitoring rings bounded by capacity");
        MIB_EXPECT(m1.monitoringRings.currentBytes <= 2000 * 2 * kMatBytes, "monitoring bytes bounded by capacity x frame");
        const uint64_t plateauBytes = m1.monitoringRings.currentBytes;
        for (uint64_t i = 2500; i < 4000; ++i) {
            pushMat(*store, mib::test::ringFrame(kW, kH, i), ts++);
            if ((i % 200) == 199) waitFor([&] { return processed() >= i; }, std::chrono::seconds(20));
        }
        MIB_REQUIRE(waitFor([&] { return processed() >= 4000; }, std::chrono::seconds(30)), "4000 frames processed");
        auto m2 = svc.memoryStats();
        MIB_EXPECT(m2.monitoringRings.currentBytes == plateauBytes && m2.monitoringRings.evictedByBudget > m1.monitoringRings.evictedByBudget,
                   "long session: monitoring bytes stay at the plateau; replacements counted");
        MIB_EXPECT(m2.snapshot.currentCount == 1 && m2.snapshot.currentBytes > 0, "presentation snapshot retains exactly one frame");
        MIB_EXPECT(svc.getMonitoringValidFrames().size() + svc.getMonitoringInvalidFrames().size() <= 2000, "rings readable, bounded");
        svc.setMonitoringActive(false);
        svc.clearMonitoringFrames();
        MIB_EXPECT(svc.memoryStats().monitoringRings.currentBytes == 0, "clearing the rings releases the bytes");

        // Experiment with a tight byte budget behind a persistence consumer that
        // never flushes: evictions follow the declared policy and are accounted.
        backend::services::Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.initialize(td.path().string()), "hdf5 init");
        svc.setFlushInterval(1000);
        const uint64_t budget = 20 * 2 * kMatBytes;
        svc.setMaxBufferedBytes(budget);
        MIB_EXPECT(svc.getMaxBufferedBytes() == budget, "budget stored");
        svc.setExperimentAccountingContext(7, false);
        svc.startExperiment(); // resets the identification counters
        const uint64_t expStart = ts;
        for (uint64_t i = 0; i < 300; ++i) {
            pushMat(*store, mib::test::ringFrame(kW, kH, 9000 + i), ts++);
            if ((i % 100) == 99) waitFor([&] { return processed() >= i; }, std::chrono::seconds(20));
        }
        MIB_REQUIRE(waitFor([&] { return processed() >= 300; }, std::chrono::seconds(30)), "experiment frames processed");
        auto m3 = svc.memoryStats();
        MIB_EXPECT(m3.experimentBuffer.currentBytes <= budget && m3.experimentBuffer.capacityBytes == budget && !m3.experimentBuffer.overBudget(),
                   "experiment buffer never exceeds its byte budget");
        MIB_EXPECT(m3.experimentBuffer.peakBytes <= budget && m3.experimentBuffer.evictedByBudget > 0, "peak bounded; evictions counted");
        auto acc = svc.experimentAccountingSnapshot();
        MIB_EXPECT(acc.persistenceReconciles(), "persistence terms reconcile with byte-budget evictions");
        MIB_EXPECT(acc.persistenceCancelledByPolicy == m3.experimentBuffer.evictedByBudget, "every eviction is persistenceCancelledByPolicy");
        MIB_EXPECT(acc.persistenceAdmitted == acc.persistenceCancelledByPolicy + acc.persistencePendingAtStop, "admitted = cancelled + pending (nothing lost silently)");
        (void)expStart;
        MIB_EXPECT(svc.finishFlush(), "no flush error");
        svc.endExperiment();
        svc.clearAccumulatedFrames();
        MIB_EXPECT(svc.memoryStats().experimentBuffer.currentBytes == 0, "buffer released after the run");
        svc.stopRealtime();
        svc.stop();
    }

    return mib::test::exitCode();
}
