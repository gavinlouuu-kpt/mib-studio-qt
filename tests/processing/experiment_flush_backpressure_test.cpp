#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace backend::services {
struct ProcessingServiceFlushTestAccess {
    static bool append(ProcessingService& svc, ProcessedFrame frame) {
        svc.experimentAccounting_.admit(frame.index);
        svc.experimentAccounting_.processed.fetch_add(1);
        return svc.appendExperimentFrame(std::move(frame), true);
    }
};
} // namespace backend::services

using namespace backend::services;
using Clock = std::chrono::steady_clock;

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir dir("experiment_backpressure");
    // Immutable shared pixels keep producer allocation out of the pacing. The
    // real HDF5 append writes every image and mask; frames are small (64x16)
    // because the regression is about queued batch COUNT during a writer
    // stall, not bytes, and a loaded CI host must not turn this into a disk
    // throughput test (512x96 at 5 kHz across parallel tests overran the buffer).
    ProcessedFrame frame;
    frame.originalImage = cv::Mat(16, 64, CV_8UC1, cv::Scalar(17));
    frame.processedImage = cv::Mat(16, 64, CV_8UC1, cv::Scalar(255));
    // Diagnostic comparison with the pre-#597 250 ms polling cadence.
    const bool polling = std::getenv("MIB_TEST_PRE597_POLLING") != nullptr;
    for (bool permanent : {false, true}) {
        if (polling && permanent) break;
        watchdog.mark(permanent ? "permanent stall" : "transient stall");
        Hdf5Service hdf;
        const auto path = (dir.path() / (permanent ? "permanent.h5" : "transient.h5")).string();
        MIB_REQUIRE(hdf.openFile(path), "open experiment file");
        ProcessingService svc;
        svc.setFlushInterval(100);
        svc.startExperiment();
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        std::atomic<size_t> batches{0};
        std::string error;
        if (!polling)
            svc.setFlushErrorCallback([&](const std::string& msg) {
                std::lock_guard<std::mutex> lock(mutex);
                error = msg;
            });
        auto getError = [&] {
            std::lock_guard<std::mutex> lock(mutex);
            return error;
        };
        if (!polling) svc.setFlushRequestCallback([&] { svc.flushBufferedFrames(hdf); });
        setHdf5PerformanceTraceHook([&](std::string_view name, std::string_view, double,
                                        std::string_view) {
            if (name != "hdf5.append_frames") return;
            const size_t batch = ++batches;
            if (permanent) {
                std::unique_lock<std::mutex> lock(mutex);
                MIB_REQUIRE(cv.wait_for(lock, std::chrono::seconds(20), [&] { return release; }),
                            "blocked writer released");
            } else {
                // Fault injection, not a timing assertion: one 150 ms stall (longer
                // than the 3-slot queue's ~60 ms of 100-frame batches at 5 kHz,
                // which overflowed before the fix) plus a small per-batch cost.
                // Kept light so a loaded CI host can't exhaust the 1000-frame
                // buffer: this checks the stall is absorbed, not raw throughput.
                std::this_thread::sleep_for(std::chrono::milliseconds(batch == 1 ? 150 : 5));
            }
        });
        const auto start = Clock::now();
        auto nextPoll = start + std::chrono::milliseconds(250);
        size_t admitted = 0;
        for (; admitted < 10000 && getError().empty(); ++admitted) {
            const auto due = start + std::chrono::microseconds(admitted * 200);
            std::this_thread::sleep_until(due);
            if (polling && Clock::now() >= nextPoll) {
                svc.flushBufferedFrames(hdf);
                nextPoll = Clock::now() + std::chrono::milliseconds(250);
            }
            frame.index = admitted + 1;
            ProcessingServiceFlushTestAccess::append(svc, frame);
        }
        std::fprintf(stderr, "%s: admitted=%zu batches=%zu elapsed=%.1f ms error=%s\n",
                     permanent ? "permanent" : "transient", admitted, batches.load(),
                     std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                     getError().c_str());
        if (permanent) {
            MIB_EXPECT(getError() ==
                           "Experiment buffer capacity exceeded: recording data was dropped",
                       "sustained overload fails through the bounded experiment buffer");
            MIB_EXPECT(svc.getBufferedFrameCounts().total() == svc.getMaxBufferedFrames(),
                       "backlog remains bounded");
        } else {
            MIB_EXPECT(getError().empty(),
                       "transient writer stall is absorbed without fatal overflow");
            MIB_EXPECT(admitted == 10000, "two seconds at 5000 fps admitted");
        }
        svc.setFlushRequestCallback({});
        MIB_REQUIRE(svc.endExperiment(), "settle experiment");
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        // Same two-stage drain as ExperimentCoordinator: a full queue can
        // defer the first flush, then the remainder is submitted after join.
        svc.flushBufferedFrames(hdf);
        const bool firstDrain = svc.finishFlush();
        svc.flushBufferedFrames(hdf);
        const bool remainderDrain = svc.finishFlush();
        MIB_EXPECT(firstDrain && remainderDrain, "Stop drains queued and buffered frames");
        setHdf5PerformanceTraceHook({});
        const auto a = svc.experimentAccountingSnapshot();
        std::fprintf(
            stderr,
            "settled: persisted=%llu/%llu cancelled=%llu failed=%llu pending=%llu batches=%zu\n",
            (unsigned long long)a.persistenceCommitted, (unsigned long long)a.persistenceAdmitted,
            (unsigned long long)a.persistenceCancelledByPolicy,
            (unsigned long long)a.persistenceFailed, (unsigned long long)a.persistencePendingAtStop,
            batches.load());
        MIB_EXPECT(a.reconciled, "frame and persistence accounting reconcile");
        MIB_EXPECT(a.persistenceFailed == 0 && a.persistencePendingAtStop == 0,
                   "no submitted frame lost or left pending");
        MIB_EXPECT(a.persistenceCommitted + a.persistenceCancelledByPolicy == a.persistenceAdmitted,
                   "all admissions persisted or explicitly cancelled at buffer capacity");
        if (!permanent) {
            if (polling) {
                MIB_EXPECT(a.persistenceCancelledByPolicy > 0,
                           "old polling cadence exceeds the 1000-frame buffer at 5000 fps");
            } else {
                MIB_EXPECT(a.completion == backend::recording::RunCompletionState::Complete,
                           "recovered run completes");
                MIB_EXPECT(a.persistenceCommitted == admitted &&
                               a.persistenceCancelledByPolicy == 0,
                           "persisted equals admitted");
            }
            hdf.closeFile();
            MIB_REQUIRE(hdf.loadFile(path), "reload persisted experiment");
            std::vector<ProcessedFrame> saved;
            MIB_REQUIRE(hdf.readValidFrames(saved), "read persisted frames");
            MIB_EXPECT(saved.size() == a.persistenceCommitted, "round-trip frame count");
            MIB_EXPECT(!saved.empty() && saved.front().originalImage.at<unsigned char>(0, 0) == 17,
                       "round-trip pixels");
        }
        hdf.closeFile();
    }
    return mib::test::exitCode();
}
