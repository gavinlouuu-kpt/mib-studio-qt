// e2e_series_alignment_test — invariant test (multi-image series ↔ sort pulse)
//
// Drives the real pipeline (FrameStore → ProcessingService inline realtime →
// TriggerService on a fake camera → experiment flush → Hdf5Service) in
// multi-image mode with every frame a target, then reloads the file and
// checks the alignment invariants this feature exists for:
//
//   I1  every saved series carries one identity record per image
//       (series_meta row width == series_images row width, and members the
//       series collected are never "absent");
//   I2  a series flagged contiguous is N consecutive frame indices whose
//       camera and host stamps are strictly increasing; a flagged gap is a
//       real gap in the indices;
//   I3  series row i is the same frame as /valid_frames/metadata row i
//       (trigger frame == first member);
//   I4  every fired sort pulse names a frame that was saved as a series
//       member, so a pulse can be placed against the recorded exposures;
//   I5  pulse stamps are ordered (grab <= request <= wake <= fire <= done)
//       and the trigger frame's host stamp equals the frame's stored one.
//
// Frame production is paced so the consumer keeps up; a ring-behind skip
// (which would flag a series non-contiguous) is tolerated by the invariants
// but not required.

#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/services/TriggerService.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"

#include "support/assert.h"
#include "support/fake_lifecycle_camera.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using backend::diagnostics::PipelineTimingRecorder;
using backend::playback::FrameStore;
using backend::recording::TriggerEventRecord;
using backend::recording::TriggerOutcome;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;
using backend::services::SeriesImageInfo;
using backend::services::TargetGroupSignal;
using backend::services::TriggerService;
using mib::test::FakeLifecycleCamera;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kFrameW = 128;
constexpr int kFrameH = 128;
constexpr uint64_t kPixelFormatMono8 = 0x01080001ULL;
constexpr int kSeriesCount = 4;
constexpr uint64_t kFrames = 240;

ProcessingConfig makeConfig() {
    ProcessingConfig c;
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
    // Every valid object is a target: wide-open gates.
    c.enable_target_group = true;
    c.target_group_area_min = 0.0;
    c.target_group_area_max = 1e12;
    c.target_group_deformability_min = 0.0;
    c.target_group_deformability_max = 1.0;
    c.enable_target_group_emodulus = false;
    c.multi_image_enabled = true;
    c.multi_image_count = kSeriesCount;
    return c;
}

cv::Mat makeBlobFrame(uint64_t i) {
    cv::Mat img(kFrameH, kFrameW, CV_8UC1, cv::Scalar(0));
    const int cx = 32 + static_cast<int>(i % 32);
    cv::circle(img, cv::Point(cx, 64), 24, cv::Scalar(220), -1);
    return img;
}

bool waitUntil(mib::test::Watchdog& watchdog, const char* step, int timeoutMs,
               const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
        watchdog.mark(step);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

} // namespace

int main() {
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("mib_series_alignment");
    const std::string path = (td / "aligned_run.h5").string();

    // ---- pipeline wiring (mirrors AppBackend) ----
    auto store = std::make_shared<FrameStore>(4096);
    ProcessingService proc;
    proc.setProcessingConfig(makeConfig());
    proc.setRealtimeProcessingMode(ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeDropFrames(false);
    proc.setFlushInterval(1000000); // flush manually below

    auto obs = std::make_shared<FakeLifecycleCamera::Observations>();
    FakeLifecycleCamera cam({}, obs.get());
    MIB_REQUIRE(cam.start(), "fake camera starts");
    TriggerService trig;
    trig.setCamera(&cam, 1);
    trig.start();
    proc.setTargetGroupCallback([&](const backend::services::TargetGroupEvent& ev) {
        TargetGroupSignal s;
        s.isTargetGroup = ev.isTargetGroup;
        s.objectId = ev.objectId;
        s.trackId = ev.trackId;
        s.frameIndex = ev.frameIndex;
        s.hostTimestampUs = ev.hostTimestampUs;
        trig.onTargetGroupResult(s);
    });
    proc.setTriggerEventSource([&] { return trig.drainEvents(); });

    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "openFile");
    MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets");

    proc.startRealtime(store);
    proc.setRealtimeEnabled(true);
    (void)trig.drainEvents(); // nothing from before the run belongs to it
    proc.startExperiment();

    // ---- produce frames like CaptureService (device tick + host stamp) ----
    // Paced so the inline consumer keeps up: every frame must be admitted
    // for the row-alignment invariant (I3) to be checkable frame by frame.
    uint64_t lastPushed = 0;
    // FrameStore write indices start at 0; the device stamp of index i is
    // (i + 1) * 1000 so a stamp is never 0.
    auto push = [&](uint64_t i) {
        const cv::Mat f = makeBlobFrame(i);
        store->pushFrame(f.data, static_cast<size_t>(f.total()), kFrameW, kFrameH, kFrameW,
                         kPixelFormatMono8, /*deviceTimestamp=*/(i + 1) * 1000ULL,
                         /*hostTimestampUs=*/PipelineTimingRecorder::nowUs());
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        wd.mark("produce");
    };
    for (uint64_t i = 0; i < kFrames; ++i) {
        push(i);
        lastPushed = i;
    }
    const bool settled = waitUntil(wd, "settle", 20000, [&] {
        // The last full series completes at the frame that closes it; the
        // consumer index reaching the last push means nothing is in flight.
        return proc.getBufferedFrameCounts().valid >= (kFrames / kSeriesCount) - 1 &&
               trig.getTriggerCount() >= kFrames / 2;
    });
    MIB_EXPECT(settled, "pipeline settled: series buffered and pulses fired");
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let the tail pulse land

    // ---- flush + stop, the way ExperimentCoordinator does ----
    const size_t submitted = proc.flushBufferedFrames(hdf5);
    MIB_EXPECT(proc.finishFlush(), "finishFlush ok");
    // Pulses for frames after the run are not the run's: stop routing them
    // (the coordinator's stop path unbinds the camera at capture stop).
    proc.setTargetGroupCallback({});
    proc.endExperiment();
    // A series pending at endExperiment() is only closed out when the loop
    // sees the next frame (existing behaviour: the camera keeps running in
    // production). Feed a couple so the partial series reaches the buffer.
    const auto bufferedAtEnd = proc.getBufferedFrameCounts().total();
    for (uint64_t i = kFrames; i < kFrames + 3; ++i) push(i);
    waitUntil(wd, "partial-series", 5000,
              [&] { return proc.getBufferedFrameCounts().total() > bufferedAtEnd; });
    proc.setRealtimeEnabled(false);
    proc.stopRealtime();
    const auto remainder = proc.getBufferedFrameCounts();
    if (remainder.total() > 0) {
        proc.flushBufferedFrames(hdf5);
        MIB_EXPECT(proc.finishFlush(), "remainder finishFlush ok");
    }
    {
        auto tail = trig.drainEvents();
        if (!tail.empty()) MIB_EXPECT(hdf5.appendTriggerEvents(tail), "trailing trigger events appended");
    }
    trig.setCamera(nullptr);
    trig.stop();
    hdf5.closeFile();
    std::cout << "[series-alignment] pushed=" << lastPushed << " submitted=" << submitted
              << " pulses=" << trig.getTriggerCount()
              << " dropped_requests=" << trig.getDroppedRequestCount() << "\n";
    MIB_REQUIRE(submitted > 0, "at least one series flushed");

    // ---- reload and check invariants ----
    Hdf5Service r;
    MIB_REQUIRE(r.loadFile(path), "loadFile");
    size_t rows = 0, seriesCount = 0;
    int h = 0, w = 0;
    MIB_REQUIRE(r.getSeriesImageInfo(rows, seriesCount, h, w), "series_images present");
    MIB_EXPECT(seriesCount == static_cast<size_t>(kSeriesCount), "series width == multi_image_count");
    MIB_EXPECT(h == kFrameH && w == kFrameW, "series image geometry");
    std::vector<ProcessedFrame> meta;
    MIB_REQUIRE(r.readValidMetadata(meta), "valid metadata");
    MIB_EXPECT(meta.size() == rows, "one metadata row per series (multi-image: every valid frame is a series)");

    std::set<uint64_t> memberFrames;
    std::vector<uint64_t> triggerFrames;
    std::vector<uint64_t> triggerHostUs; // series_meta[0].hostTimestampUs per row
    size_t gapped = 0;
    for (size_t i = 0; i < rows; ++i) {
        std::vector<SeriesImageInfo> info;
        bool contiguous = true;
        MIB_REQUIRE(r.readSeriesMeta(i, info, &contiguous), "series_meta row " + std::to_string(i));
        MIB_EXPECT(info.size() == seriesCount, "I1: identity row width == image row width");
        std::vector<cv::Mat> imgs;
        MIB_REQUIRE(r.readSeriesImagesByIndex(i, imgs), "series images row " + std::to_string(i));
        MIB_EXPECT(imgs.size() == info.size(), "I1: images and identities parallel");
        size_t present = 0;
        for (const auto& m : info) {
            if (m.frameIndex != Hdf5Service::kAbsentSeriesFrame) ++present;
        }
        // Only the final series may be partial (experiment ended mid-series).
        if (i + 1 < rows) MIB_EXPECT(present == seriesCount, "I1: a completed series has every member");
        MIB_EXPECT(present >= 1, "I1: a series has at least its trigger frame");

        bool consecutive = true, increasing = true;
        for (size_t s = 1; s < present; ++s) {
            if (info[s].frameIndex != info[s - 1].frameIndex + 1) consecutive = false;
            if (info[s].timestampNs <= info[s - 1].timestampNs ||
                info[s].hostTimestampUs < info[s - 1].hostTimestampUs) {
                increasing = false;
            }
        }
        if (contiguous) {
            MIB_EXPECT(consecutive, "I2: contiguous series has consecutive frame indices (row " +
                                        std::to_string(i) + ")");
        } else {
            MIB_EXPECT(!consecutive, "I2: a flagged gap is a real gap (row " + std::to_string(i) + ")");
            ++gapped;
        }
        MIB_EXPECT(increasing, "I2: camera and host stamps increase along the series");
        for (size_t s = 0; s < present; ++s) {
            MIB_EXPECT(info[s].timestampNs == (info[s].frameIndex + 1) * 1000ULL,
                       "member camera stamp is the one pushed for that frame");
            MIB_EXPECT(info[s].hostTimestampUs != 0, "member host stamp recorded");
            memberFrames.insert(info[s].frameIndex);
        }
        if (i < meta.size()) {
            MIB_EXPECT(meta[i].index == info[0].frameIndex,
                       "I3: series row aligns with metadata row (trigger frame == first member)");
            MIB_EXPECT(meta[i].timestampNs == info[0].timestampNs, "I3: trigger stamp matches");
        }
        triggerFrames.push_back(info[0].frameIndex);
        triggerHostUs.push_back(info[0].hostTimestampUs);
    }
    std::cout << "[series-alignment] series_rows=" << rows << " gapped=" << gapped
              << " member_frames=" << memberFrames.size() << "\n";

    std::vector<TriggerEventRecord> events;
    MIB_REQUIRE(r.readTriggerEvents(events), "/trigger_events present");
    MIB_EXPECT(!events.empty(), "pulses were recorded");
    size_t fired = 0, aligned = 0, ordered = 0, stamped = 0;
    uint64_t lastSeq = 0;
    bool seqIncreasing = true;
    for (const auto& e : events) {
        if (e.sequence <= lastSeq) seqIncreasing = false;
        lastSeq = e.sequence;
        if (e.outcome != static_cast<uint8_t>(TriggerOutcome::Fired)) continue;
        ++fired;
        if (memberFrames.count(e.frameIndex)) ++aligned;
        if (e.grabUs <= e.requestUs && e.requestUs <= e.wakeUs && e.wakeUs <= e.fireUs &&
            e.fireUs <= e.pulseDoneUs) {
            ++ordered;
        }
        // The pulse's grab stamp must equal the stored host stamp of that
        // frame (same source frame, same stamp), read from series_meta.
        for (size_t i = 0; i < rows; ++i) {
            if (triggerFrames[i] == e.frameIndex && triggerHostUs[i] == e.grabUs) {
                ++stamped;
                break;
            }
        }
    }
    MIB_EXPECT(seqIncreasing, "trigger event sequence increases across batches");
    MIB_EXPECT(fired > 0, "fired pulses present");
    MIB_EXPECT(fired == trig.getTriggerCount(), "every fired pulse was persisted (" +
                                                    std::to_string(fired) + "/" +
                                                    std::to_string(trig.getTriggerCount()) + ")");
    MIB_EXPECT(aligned == fired, "I4: every fired pulse names a saved series member (" +
                                     std::to_string(aligned) + "/" + std::to_string(fired) + ")");
    MIB_EXPECT(ordered == fired, "I5: pulse stamps ordered grab<=request<=wake<=fire<=done");
    MIB_EXPECT(stamped >= 1, "I5: a trigger-frame pulse carries the frame's stored host stamp");
    std::cout << "[series-alignment] events=" << events.size() << " fired=" << fired
              << " aligned=" << aligned << " ordered=" << ordered << "\n";
    r.closeFile();
    return mib::test::exitCode();
}
