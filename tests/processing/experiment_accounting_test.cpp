// experiment_accounting_test (issue #367)
//
// ProcessingService experiment accounting on the realtime inline path: frames
// examined during an experiment are admitted and end in exactly one of
// Empty / Processed / RejectedByScientificFilter / ProcessingFailed /
// StoreOverwritten; persistence admissions reconcile with committed +
// cancelled + pending; a kernel failure is never counted as an empty frame;
// frame counts stay separate from object counts.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace rec = backend::recording;
using backend::services::ProcessingService;

namespace {
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
} // namespace

int main()
{
    mib::test::Watchdog wd(40);
    mib::test::TempDir td("experiment_accounting");

    auto store = std::make_shared<backend::playback::FrameStore>(4096);
    ProcessingService svc;
    svc.start(2);
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string err;
    MIB_REQUIRE(svc.activateProcessingKernel(kernel, &err), "activate fault kernel: " + err);
    {
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
    }
    svc.setRealtimeRoi(ProcessingService::Roi{0, 0, 96, 96});
    svc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    svc.setInvalidFrameSamplingRate(1);
    svc.startRealtime(store);
    MIB_REQUIRE(svc.isRealtimeRunning(), "realtime running");

    // Warm-up frame outside the experiment so the loop is caught up.
    pushMat(*store, mib::test::ringFrame(96, 96, 0), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Experiment 1: mixed frames, every 4th empty check fails -------------
    wd.mark("experiment 1");
    kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::FailEmptyCheck);
    kernel->failPeriod = 4;
    kernel->emptyCalls = 0;
    kernel->injectedFailures = 0;
    svc.setExperimentAccountingContext(/*generation=*/3, /*policyAllowsDrops=*/false);
    svc.startExperiment();
    const uint64_t callsAtStart = kernel->emptyCalls.load();
    constexpr int kFrames = 60;
    uint64_t emptyPushed = 0, ringPushed = 0;
    for (int i = 0; i < kFrames; ++i) {
        const bool empty = (i % 3 == 1);
        cv::Mat m = empty ? cv::Mat(96, 96, CV_8UC1, cv::Scalar(0)) : mib::test::ringFrame(96, 96, i);
        if (empty) ++emptyPushed; else ++ringPushed;
        pushMat(*store, m, 100 + i);
        // Pace so the 64-slot ring never wraps (deterministic: no store loss).
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() - callsAtStart >= kFrames; },
                        std::chrono::seconds(10)),
                "all frames examined");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    svc.endExperiment();
    kernel->failPeriod = 0;

    {
        const auto a = svc.experimentAccountingSnapshot();
        std::fprintf(stderr, "exp1: admitted=%llu empty=%llu processed=%llu rejected=%llu failed=%llu "
                     "ow=%llu objects=%llu pAdmitted=%llu pPending=%llu completion=%s (%s)\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.empty,
                     (unsigned long long)a.processed, (unsigned long long)a.scientificallyRejected,
                     (unsigned long long)a.processingFailed, (unsigned long long)a.storeOverwritten,
                     (unsigned long long)a.objectsDetected, (unsigned long long)a.persistenceAdmitted,
                     (unsigned long long)a.persistencePendingAtStop, rec::toString(a.completion),
                     a.completionReason.c_str());
        MIB_EXPECT(a.admitted == kFrames, "every experiment frame admitted");
        MIB_EXPECT(a.processingFailed == kernel->injectedFailures.load() && a.processingFailed > 0,
                   "injected failures counted as ProcessingFailed");
        MIB_EXPECT(a.empty + a.processed + a.scientificallyRejected + a.processingFailed == a.admitted,
                   "frame terms exact (no store loss)");
        MIB_EXPECT(a.empty < emptyPushed, "some failures landed on empty frames -> empty count is not inflated");
        MIB_EXPECT(a.empty + a.processingFailed >= emptyPushed || a.processed + a.scientificallyRejected + a.processingFailed >= ringPushed,
                   "classification consistent with pushed mix");
        MIB_EXPECT(a.persistenceAdmitted == a.processed + a.scientificallyRejected,
                   "each non-empty, successfully processed frame was appended once");
        MIB_EXPECT(a.persistencePendingAtStop == a.persistenceAdmitted, "nothing flushed yet -> all pending");
        MIB_EXPECT(a.reconciled, "experiment accounting reconciles");
        MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "processing failures -> IncompleteLoss");
        MIB_EXPECT(a.sessionGeneration == 3, "generation carried");
        MIB_EXPECT(a.objectsDetected >= a.processed, "object count tracked separately (>= frames with a valid object)");
        MIB_EXPECT(svc.getProcessingFailureCount() >= a.processingFailed, "lifetime failure counter");
    }

    // ---- Flush to HDF5: pending -> committed ----------------------------------
    wd.mark("flush");
    {
        backend::services::Hdf5Service h;
        const std::string path = (td.path() / "exp.h5").string();
        MIB_REQUIRE(h.openFile(path), "open");
        const size_t n = svc.flushBufferedFrames(h);
        MIB_EXPECT(n > 0, "flushed frames");
        MIB_REQUIRE(svc.finishFlush(), "finish flush");
        const auto a = svc.experimentAccountingSnapshot();
        MIB_EXPECT(a.persistenceCommitted == a.persistenceAdmitted && a.persistencePendingAtStop == 0,
                   "all admissions committed after flush");
        MIB_EXPECT(a.reconciled, "reconciles after flush");
        // Persist and re-read on the experiment file.
        MIB_REQUIRE(h.writeExperimentInfo(1, 2, a.processed, a.scientificallyRejected,
                                          svc.getProcessingConfig(), svc.getRealtimeRoi()),
                    "experiment info");
        MIB_REQUIRE(h.writeRunAccounting(a), "write accounting on experiment file");
        h.closeFile();
        backend::services::Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "reload");
        rec::RecordingAccountingSnapshot back;
        MIB_REQUIRE(r.readRunAccounting(back), "read accounting");
        MIB_EXPECT(back.processingFailed == a.processingFailed && back.completion == a.completion &&
                       back.persistenceCommitted == a.persistenceCommitted,
                   "experiment accounting round-trips");
        r.closeFile();
    }

    // ---- Experiment 2: clean run + buffer cap policy ----------------------------
    wd.mark("experiment 2");
    kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::Passthrough);
    // The buffer cap derives from the flush interval (defaultMaxBufferedFrames).
    svc.setFlushInterval(1);
    svc.setExperimentAccountingContext(4, false);
    svc.startExperiment();
    const uint64_t calls2 = kernel->emptyCalls.load();
    // The bounded experiment buffer floors at 1000 frames (defaultMaxBufferedFrames):
    // exceed it so the declared eviction policy is exercised.
    constexpr int kFrames2 = 1100;
    for (int i = 0; i < kFrames2; ++i) {
        pushMat(*store, mib::test::ringFrame(96, 96, i), 1000 + i);
        if (i % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    MIB_REQUIRE(waitFor([&] { return kernel->emptyCalls.load() - calls2 >= kFrames2; }, std::chrono::seconds(30)), "examined");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    svc.endExperiment();
    {
        const auto a = svc.experimentAccountingSnapshot();
        std::fprintf(stderr, "exp2: admitted=%llu processed=%llu rejected=%llu pAdmitted=%llu cancelled=%llu pending=%llu completion=%s\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.processed,
                     (unsigned long long)a.scientificallyRejected, (unsigned long long)a.persistenceAdmitted,
                     (unsigned long long)a.persistenceCancelledByPolicy, (unsigned long long)a.persistencePendingAtStop,
                     rec::toString(a.completion));
        MIB_EXPECT(a.admitted == kFrames2 && a.processingFailed == 0 && a.empty == 0 && a.storeOverwritten == 0, "clean frames");
        MIB_EXPECT(a.persistenceCancelledByPolicy > 0, "buffer-cap evictions counted as CancelledByPolicy");
        MIB_EXPECT(a.persistenceCancelledByPolicy + a.persistencePendingAtStop == a.persistenceAdmitted,
                   "persistence terms exact under the cap");
        MIB_EXPECT(a.reconciled && a.completion == rec::RunCompletionState::IntentionallyPartial,
                   "declared buffer policy -> IntentionallyPartial, not Complete");
    }

    // ---- Experiment 3: start/stop boundaries under a continuous stream ----------
    // A frame admitted just before startExperiment() must not be counted as
    // an outcome of the run, and a frame admitted before endExperiment() must
    // be counted even if its outcome lands after it. Either skew makes a
    // clean run "Failed: accounting does not reconcile" (bench + CI lane,
    // 2026-09-08). Many short runs against a free-running pusher hit the
    // boundaries with high probability.
    wd.mark("experiment 3");
    svc.setFlushInterval(1000);
    {
        std::atomic<bool> pushing{true};
        std::atomic<uint64_t> pushed{0};
        std::thread pusher([&] {
            uint64_t ts = 10'000'000;
            int i = 0;
            while (pushing.load(std::memory_order_relaxed)) {
                pushMat(*store, mib::test::ringFrame(96, 96, i++ % 7), ++ts);
                pushed.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        int mismatches = 0;
        constexpr int kRuns = 40;
        for (int run = 0; run < kRuns; ++run) {
            wd.mark("experiment 3 run");
            svc.setExperimentAccountingContext(100 + run, false);
            svc.startExperiment();
            std::this_thread::sleep_for(std::chrono::milliseconds(15 + (run % 5)));
            svc.endExperiment();
            const auto a = svc.experimentAccountingSnapshot();
            const bool frameTermsOk = a.admitted == a.empty + a.processed + a.scientificallyRejected +
                                                    a.processingFailed + a.storeOverwritten +
                                                    a.storeNotCommitted + a.storeMalformed;
            if (!frameTermsOk || !a.reconciled) {
                ++mismatches;
                std::fprintf(stderr, "exp3 run %d: admitted=%llu empty=%llu processed=%llu rejected=%llu "
                             "failed=%llu ow=%llu pAdmitted=%llu pending=%llu completion=%s (%s)\n",
                             run, (unsigned long long)a.admitted, (unsigned long long)a.empty,
                             (unsigned long long)a.processed, (unsigned long long)a.scientificallyRejected,
                             (unsigned long long)a.processingFailed, (unsigned long long)a.storeOverwritten,
                             (unsigned long long)a.persistenceAdmitted,
                             (unsigned long long)a.persistencePendingAtStop, rec::toString(a.completion),
                             a.completionReason.c_str());
            }
            // Drain the buffer the way the coordinator does between runs.
            svc.clearAccumulatedFrames();
        }
        pushing.store(false);
        pusher.join();
        std::fprintf(stderr, "exp3: %d runs, %d boundary mismatches, %llu frames pushed\n", kRuns, mismatches,
                     (unsigned long long)pushed.load());
        MIB_EXPECT(mismatches == 0, "no start/stop boundary accounting skew across " + std::to_string(kRuns) + " runs");
    }

    svc.stopRealtime();
    svc.stop();
    return mib::test::exitCode();
}
