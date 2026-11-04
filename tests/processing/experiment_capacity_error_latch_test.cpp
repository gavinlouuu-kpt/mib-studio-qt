// Issue #828: a full experiment buffer refuses every later frame, from every
// worker. The capacity error must reach the fatal-save sink once per
// experiment, not once per refused frame (the per-frame flood stacked modal
// dialogs on the GUI thread until its stack overflowed).
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <thread>
#include <vector>

namespace backend::services {
struct ProcessingServiceCapacityLatchTestAccess {
    static bool append(ProcessingService& svc, ProcessedFrame frame) {
        svc.experimentAccounting_.admit(frame.index);
        svc.experimentAccounting_.processed.fetch_add(1);
        return svc.appendExperimentFrame(std::move(frame), true);
    }
};
} // namespace backend::services

using namespace backend::services;

int main() {
    mib::test::Watchdog watchdog(60);
    ProcessedFrame frame;
    frame.originalImage = cv::Mat(16, 64, CV_8UC1, cv::Scalar(17));
    frame.processedImage = cv::Mat(16, 64, CV_8UC1, cv::Scalar(255));

    ProcessingService svc;
    svc.setFlushInterval(100);
    std::atomic<int> reports{0};
    // No flush request callback: nothing drains, so the buffer saturates.
    svc.setFlushErrorCallback([&](const std::string&) { reports.fetch_add(1); });

    for (int run = 1; run <= 2; ++run) {
        watchdog.mark(run == 1 ? "first run" : "second run");
        svc.startExperiment();
        const size_t overflow = svc.getMaxBufferedFrames() * 3;
        // Several producers, like the processing workers on the rig.
        std::vector<std::thread> producers;
        std::atomic<uint64_t> next{1};
        for (int t = 0; t < 8; ++t) {
            producers.emplace_back([&] {
                for (uint64_t i = next.fetch_add(1); i <= overflow; i = next.fetch_add(1)) {
                    ProcessedFrame f = frame;
                    f.index = i;
                    ProcessingServiceCapacityLatchTestAccess::append(svc, std::move(f));
                }
            });
        }
        for (auto& p : producers)
            p.join();
        MIB_EXPECT(svc.getBufferedFrameCounts().total() == svc.getMaxBufferedFrames(),
                   "buffer saturated");
        MIB_EXPECT(reports.load() == run, "capacity error reported exactly once per experiment");
        MIB_REQUIRE(svc.endExperiment(), "settle experiment");
    }
    return mib::test::exitCode();
}
