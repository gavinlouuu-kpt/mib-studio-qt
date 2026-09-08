// pipeline_timing_recorder_test
//
// Unit coverage for backend::diagnostics::PipelineTimingRecorder, the
// lock-free per-frame latency recorder behind the pipeline-delay diagnosis
// workflow (docs/howto/pipeline-latency-diagnosis.md):
//   * disabled recorder is a no-op (hot paths stay clean by default)
//   * records round-trip and snapshot in oldest -> newest order
//   * ring wrap keeps the newest records and the monotonic total
//   * one frame-writer + one trigger-writer + multi-thread skip counters run
//     concurrently without losing counts (the intended threading model)
//   * dumpCsv produces parseable files with matching row counts

#include "backend/diagnostics/PipelineTimingRecorder.h"

#include "support/watchdog.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using backend::diagnostics::FrameTimingRecord;
using backend::diagnostics::PipelineSkipReason;
using backend::diagnostics::PipelineTimingRecorder;
using backend::diagnostics::TriggerTimingRecord;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond << "\n";           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

size_t countCsvDataRows(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return static_cast<size_t>(-1);
    std::string line;
    size_t rows = 0;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false; // header
            continue;
        }
        if (!line.empty()) ++rows;
    }
    return rows;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(20);
    auto& rec = PipelineTimingRecorder::instance();

    // --- disabled recorder is a no-op ---
    watchdog.mark("disabled-noop");
    rec.setEnabled(false);
    rec.clear();
    rec.recordFrame(FrameTimingRecord{});
    rec.recordTrigger(TriggerTimingRecord{});
    rec.countSkipped(PipelineSkipReason::DroppedToLatest, 5);
    CHECK(rec.frameRecordCount() == 0);
    CHECK(rec.triggerRecordCount() == 0);
    CHECK(rec.skippedCount(PipelineSkipReason::DroppedToLatest) == 0);

    // --- basic round-trip, oldest -> newest ---
    watchdog.mark("round-trip");
    rec.setEnabled(true);
    for (uint64_t i = 0; i < 10; ++i) {
        FrameTimingRecord r;
        r.frameIndex = i;
        r.grabUs = 100 + i;
        r.algoStartUs = 200 + i;
        r.algoEndUs = 300 + i;
        r.callbacksDoneUs = 400 + i;
        r.validCount = 1;
        r.isTargetGroup = (i % 2 == 0) ? 1 : 0;
        rec.recordFrame(r);
    }
    TriggerTimingRecord t;
    t.frameIndex = 4;
    t.grabUs = 104;
    t.requestUs = 500;
    t.wakeUs = 510;
    t.fireUs = 520;
    t.pulseDoneUs = 530;
    t.coalesced = 2;
    rec.recordTrigger(t);
    rec.countSkipped(PipelineSkipReason::EmptyFrame, 3);

    CHECK(rec.frameRecordCount() == 10);
    CHECK(rec.triggerRecordCount() == 1);
    CHECK(rec.skippedCount(PipelineSkipReason::EmptyFrame) == 3);
    {
        const auto frames = rec.frameRecords();
        CHECK(frames.size() == 10);
        for (size_t i = 0; i < frames.size(); ++i) {
            CHECK(frames[i].frameIndex == i);
            CHECK(frames[i].grabUs == 100 + i);
        }
        const auto triggers = rec.triggerRecords();
        CHECK(triggers.size() == 1);
        CHECK(triggers[0].frameIndex == 4);
        CHECK(triggers[0].coalesced == 2);
        CHECK(triggers[0].pulseDoneUs == 530);
    }

    // --- ring wrap keeps the newest kFrameCapacity records ---
    watchdog.mark("ring-wrap");
    rec.clear();
    const uint64_t overshoot = 100;
    const uint64_t total = PipelineTimingRecorder::kFrameCapacity + overshoot;
    for (uint64_t i = 0; i < total; ++i) {
        FrameTimingRecord r;
        r.frameIndex = i;
        rec.recordFrame(r);
    }
    CHECK(rec.frameRecordCount() == total);
    {
        const auto frames = rec.frameRecords();
        CHECK(frames.size() == PipelineTimingRecorder::kFrameCapacity);
        CHECK(frames.front().frameIndex == overshoot);
        CHECK(frames.back().frameIndex == total - 1);
    }

    // --- concurrent writers per the threading model ---
    watchdog.mark("concurrency");
    rec.clear();
    constexpr uint64_t kPerThread = 20000;
    std::thread frameWriter([&] {
        for (uint64_t i = 0; i < kPerThread; ++i) {
            FrameTimingRecord r;
            r.frameIndex = i;
            rec.recordFrame(r);
        }
    });
    std::thread triggerWriter([&] {
        for (uint64_t i = 0; i < kPerThread; ++i) {
            TriggerTimingRecord r;
            r.frameIndex = i;
            rec.recordTrigger(r);
        }
    });
    std::thread skipA([&] {
        for (uint64_t i = 0; i < kPerThread; ++i) {
            rec.countSkipped(PipelineSkipReason::DroppedToLatest);
        }
    });
    std::thread skipB([&] {
        for (uint64_t i = 0; i < kPerThread; ++i) {
            rec.countSkipped(PipelineSkipReason::DroppedToLatest);
        }
    });
    frameWriter.join();
    triggerWriter.join();
    skipA.join();
    skipB.join();
    CHECK(rec.frameRecordCount() == kPerThread);
    CHECK(rec.triggerRecordCount() == kPerThread);
    CHECK(rec.skippedCount(PipelineSkipReason::DroppedToLatest) == 2 * kPerThread);

    // --- CSV dump ---
    watchdog.mark("csv-dump");
    const auto dumpDir =
        std::filesystem::temp_directory_path() / "mib_pipeline_timing_recorder_test";
    std::filesystem::remove_all(dumpDir);
    std::string error;
    CHECK(rec.dumpCsv(dumpDir.string(), &error));
    if (!error.empty()) std::cerr << "dump error: " << error << "\n";
    const size_t expectedFrames =
        std::min<uint64_t>(kPerThread, PipelineTimingRecorder::kFrameCapacity);
    const size_t expectedTriggers =
        std::min<uint64_t>(kPerThread, PipelineTimingRecorder::kTriggerCapacity);
    CHECK(countCsvDataRows(dumpDir / "pipeline_frames.csv") == expectedFrames);
    CHECK(countCsvDataRows(dumpDir / "pipeline_triggers.csv") == expectedTriggers);
    // Skips file: one row per reason plus the two record-count summary rows.
    CHECK(countCsvDataRows(dumpDir / "pipeline_skips.csv") ==
          static_cast<size_t>(PipelineSkipReason::Count) + 2);
    std::filesystem::remove_all(dumpDir);

    rec.setEnabled(false);
    rec.clear();

    if (failures == 0) {
        std::cout << "pipeline_timing_recorder_test: OK\n";
        return 0;
    }
    std::cerr << "pipeline_timing_recorder_test: " << failures << " failure(s)\n";
    return 1;
}
