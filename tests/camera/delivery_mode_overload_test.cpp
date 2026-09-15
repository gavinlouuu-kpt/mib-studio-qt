// Overload and mode-switch behavior of the FrameDeliveryMode contract using
// the simulated SDK-queue camera (support/queue_camera.h).
//
// EveryFrame under overload: ordered delivery, zero intentional discards,
// backlog growth to capacity followed by unavoidable underruns.
// LatestFrame under overload: sequence gaps counted as intentional discards,
// bounded staleness (delivered frame stays close to the newest produced one),
// bounded frame age.
// Mode switches: safe restart in both directions, no stale frames crossing the
// transition, no buffer-pool shrinkage over repeated cycles.
//
// Timing gates use logical sequence distances and cross-mode ratios, not
// absolute milliseconds, so the test is machine-independent.

#include "support/assert.h"
#include "support/queue_camera.h"
#include "support/watchdog.h"

#include "backend/app/Tools.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using camera::common::AcquisitionQueueStats;
using camera::common::CameraConfig;
using camera::common::Frame;
using camera::common::FrameDeliveryMode;
using mib::test::QueueBackedTestCamera;

namespace {

constexpr int kQueueCapacity = 8;
constexpr auto kProduceInterval = std::chrono::microseconds(500);
// Keep the consumer pause above the coarse Windows timer quantum. Assertions
// remain based on queue depth, sequence distance, and cross-mode age ratios.
constexpr auto kSlowConsumerDelay = std::chrono::milliseconds(50);
constexpr int kOverloadGrabs = 30;

struct OverloadResult {
    std::vector<uint64_t> sequences;
    std::vector<uint64_t> lagAtGrab; // newest completed sequence - delivered sequence
    uint64_t maxFrameAgeUs = 0;
};

OverloadResult runOverload(QueueBackedTestCamera& camera, FrameDeliveryMode mode,
                           mib::test::Watchdog& dog)
{
    CameraConfig cfg;
    cfg.numBuffers = kQueueCapacity;
    cfg.deliveryMode = mode;
    camera.applyConfig(cfg);
    MIB_REQUIRE(camera.start(), "camera must start");
    MIB_REQUIRE(camera.activeDeliveryMode() == mode, "backend must confirm the requested mode");

    OverloadResult result;
    for (int i = 0; i < kOverloadGrabs; ++i) {
        dog.mark("overload grab");
        Frame frame;
        MIB_REQUIRE(camera.grabFrame(frame), "producer outruns consumer; grab must succeed");
        const uint64_t seq = QueueBackedTestCamera::sequenceOf(frame);
        result.sequences.push_back(seq);
        // Compare with the newest frame that had reached the completed-buffer
        // queue at the moment of the grab (sampled under the grab lock).
        // Sequence numbers rejected by a full queue are underruns, not frames
        // that LatestFrame could have delivered -- and because they still
        // consume sequence numbers, reading the head after the grab would
        // race with the next push and inflate the lag by every underrun.
        result.lagAtGrab.push_back(camera.newestCompletedSequenceAtLastGrab() - seq);
        const uint64_t now = backend::Tools::getTimestamp();
        if (now > frame.timestamp) {
            result.maxFrameAgeUs = std::max(result.maxFrameAgeUs, now - frame.timestamp);
        }
        std::this_thread::sleep_for(kSlowConsumerDelay); // the overload: consumer is ~10x slower
    }
    camera.stop();
    return result;
}

} // namespace

int main()
{
    mib::test::Watchdog dog(30);

    // --- EveryFrame overload -------------------------------------------------
    dog.mark("everyframe overload");
    QueueBackedTestCamera::Options options;
    options.produceInterval = kProduceInterval;

    QueueBackedTestCamera efCamera(options);
    const OverloadResult ef = runOverload(efCamera, FrameDeliveryMode::EveryFrame, dog);

    for (size_t i = 1; i < ef.sequences.size(); ++i) {
        MIB_EXPECT(ef.sequences[i] > ef.sequences[i - 1],
                   "EveryFrame must preserve acquisition order");
    }
    MIB_EXPECT(efCamera.intentionalDiscardCount() == 0,
               "EveryFrame must never intentionally discard");
    MIB_EXPECT(efCamera.peakQueueDepth() == static_cast<size_t>(kQueueCapacity),
               "EveryFrame backlog must grow to capacity under overload");
    MIB_EXPECT(efCamera.underrunCount() > 0,
               "once the queue is full, further loss is an unavoidable underrun");
    // Backlog visible while overloaded: the last grabbed frame lags the newest
    // produced one by roughly the queue capacity.
    MIB_EXPECT(ef.lagAtGrab.back() >= static_cast<uint64_t>(kQueueCapacity) / 2,
               "EveryFrame delivery must fall behind the producer under overload");

    // Frame accounting is conserved: nothing silently lost.
    MIB_EXPECT(efCamera.producedCount() ==
                   efCamera.deliveredCount() + efCamera.intentionalDiscardCount() +
                       efCamera.underrunCount() + efCamera.strandedAtStopCount(),
               "EveryFrame accounting must conserve produced frames");

    // --- LatestFrame overload ------------------------------------------------
    dog.mark("latestframe overload");
    QueueBackedTestCamera lfCamera(options);
    const OverloadResult lf = runOverload(lfCamera, FrameDeliveryMode::LatestFrame, dog);

    MIB_EXPECT(lfCamera.intentionalDiscardCount() > 0,
               "LatestFrame under overload must discard stale frames");
    uint64_t gapCount = 0;
    for (size_t i = 1; i < lf.sequences.size(); ++i) {
        MIB_EXPECT(lf.sequences[i] > lf.sequences[i - 1],
                   "LatestFrame still delivers in increasing order");
        if (lf.sequences[i] != lf.sequences[i - 1] + 1) {
            ++gapCount;
        }
    }
    MIB_EXPECT(gapCount > 0, "LatestFrame under overload must show sequence gaps");
    // Gaps are fully explained by counted drops (intentional discards here;
    // underruns only if the consumer stalled past queue capacity).
    const uint64_t observedSpan = lf.sequences.back() - lf.sequences.front() + 1;
    const uint64_t missing = observedSpan - static_cast<uint64_t>(lf.sequences.size());
    MIB_EXPECT(missing <= lfCamera.intentionalDiscardCount() + lfCamera.underrunCount(),
               "every sequence gap must be accounted for by a counted drop");

    // Freshness is bounded: each delivered frame is within the queue capacity
    // of the newest produced frame (logical distance, machine-independent).
    for (const uint64_t lag : lf.lagAtGrab) {
        MIB_EXPECT(lag <= static_cast<uint64_t>(kQueueCapacity),
                   "LatestFrame delivery must stay near the producer head");
    }
    // Frame age stays bounded instead of growing with the backlog: the stalest
    // LatestFrame delivery must be meaningfully fresher than EveryFrame's.
    MIB_EXPECT(lf.maxFrameAgeUs * 2 < ef.maxFrameAgeUs,
               "LatestFrame max frame age must be well under EveryFrame's");

    MIB_EXPECT(lfCamera.producedCount() ==
                   lfCamera.deliveredCount() + lfCamera.intentionalDiscardCount() +
                       lfCamera.underrunCount() + lfCamera.strandedAtStopCount(),
               "LatestFrame accounting must conserve produced frames");

    // --- Mode switches with a controlled restart -----------------------------
    dog.mark("mode switch cycles");
    QueueBackedTestCamera switchCamera(options);
    CameraConfig cfg;
    cfg.numBuffers = kQueueCapacity;
    FrameDeliveryMode mode = FrameDeliveryMode::EveryFrame;
    for (int cycle = 0; cycle < 6; ++cycle) {
        cfg.deliveryMode = mode;
        switchCamera.applyConfig(cfg);
        MIB_REQUIRE(switchCamera.start(), "restart after mode change must succeed");
        MIB_EXPECT(switchCamera.activeDeliveryMode() == mode,
                   "confirmed mode must match the requested mode after restart");

        const uint64_t boundary = switchCamera.newestProducedSequence();
        Frame frame;
        MIB_REQUIRE(switchCamera.grabFrame(frame), "grab after restart must succeed");
        // stop() clears the completed queue, so nothing produced before the
        // restart may cross the transition boundary.
        MIB_EXPECT(QueueBackedTestCamera::sequenceOf(frame) > boundary,
                   "no stale frame may cross a mode-switch boundary");

        AcquisitionQueueStats stats{};
        MIB_REQUIRE(switchCamera.pollAcquisitionQueueStats(stats), "queue stats must poll");
        MIB_EXPECT(stats.sdkCompletedQueueDepth + stats.sdkInputBufferCount ==
                       static_cast<size_t>(kQueueCapacity),
                   "repeated mode switches must not shrink the buffer pool");

        switchCamera.stop();
        mode = (mode == FrameDeliveryMode::EveryFrame) ? FrameDeliveryMode::LatestFrame
                                                       : FrameDeliveryMode::EveryFrame;
    }
    MIB_EXPECT(switchCamera.producedCount() ==
                   switchCamera.deliveredCount() + switchCamera.intentionalDiscardCount() +
                       switchCamera.underrunCount() + switchCamera.strandedAtStopCount(),
               "mode-switch cycles must conserve frame accounting");

    // --- Stopping a blocked grab cancels cleanly -----------------------------
    dog.mark("blocked grab cancel");
    QueueBackedTestCamera::Options slowOptions;
    slowOptions.produceInterval = std::chrono::seconds(60); // effectively never produces
    QueueBackedTestCamera blockedCamera(slowOptions);
    cfg.deliveryMode = FrameDeliveryMode::EveryFrame;
    blockedCamera.applyConfig(cfg);
    MIB_REQUIRE(blockedCamera.start(), "camera must start");
    std::thread grabber([&blockedCamera] {
        Frame frame;
        // Must return false once stop() lands instead of hanging.
        MIB_EXPECT(!blockedCamera.grabFrame(frame), "stopped grab must not deliver");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    blockedCamera.stop();
    grabber.join();

    return mib::test::exitCode();
}
