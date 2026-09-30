// trigger_alignment_roundtrip_test
//
// Round-trip + fault-injection coverage for the datasets that place a sort
// pulse against the frame sequence:
//   - /valid_frames/series_meta (N, seriesCount) — identity of every
//     multi-image member, parallel to series_images, with absent-member
//     padding for a partial series;
//   - /valid_frames/series_contiguous (N) — the gapped-series flag;
//   - /trigger_events — TriggerService's canonical pulse records, created on
//     first append, extended across batches AND across a reopen (the on-disk
//     extent is trusted, not a cached counter).
// Fault injection: writes on a closed / read-only file fail loudly; reads on
// a file that predates the datasets return false and leave outputs empty;
// out-of-range series index returns false.

#include "backend/recording/Hdf5Service.h"
#include "backend/recording/TriggerEventRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

using backend::recording::TriggerEventRecord;
using backend::recording::TriggerOutcome;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::SeriesImageInfo;

namespace {

ProcessedFrame makeSeries(uint64_t triggerIdx, size_t members, bool contiguous, uint64_t gapAfter)
{
    ProcessedFrame f;
    f.index = triggerIdx;
    f.timestampNs = triggerIdx * 1000ULL;
    f.hostTimestampUs = triggerIdx * 10ULL;
    f.originalImage = cv::Mat(4, 6, CV_8UC1, cv::Scalar(static_cast<int>(triggerIdx)));
    f.processedImage = cv::Mat(4, 6, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = true;
    f.validation.objectId = static_cast<int>(triggerIdx);
    f.seriesContiguous = contiguous;
    uint64_t idx = triggerIdx;
    for (size_t s = 0; s < members; ++s) {
        f.seriesImages.push_back(cv::Mat(4, 6, CV_8UC1, cv::Scalar(static_cast<int>(s))));
        f.seriesInfo.push_back(SeriesImageInfo{idx, idx * 1000ULL, idx * 10ULL});
        idx += (gapAfter != 0 && s + 1 == gapAfter) ? 3 : 1; // skip two frames once
    }
    return f;
}

TriggerEventRecord makeEvent(uint64_t seq, uint64_t frame, TriggerOutcome outcome)
{
    TriggerEventRecord r;
    r.sequence = seq;
    r.frameIndex = frame;
    r.grabUs = frame * 10;
    r.objectId = static_cast<int32_t>(frame);
    r.trackId = static_cast<int32_t>(frame * 2);
    r.generation = 7;
    r.requestUs = frame * 10 + 1;
    r.wakeUs = frame * 10 + 2;
    r.fireUs = outcome == TriggerOutcome::Fired ? frame * 10 + 3 : 0;
    r.pulseDoneUs = outcome == TriggerOutcome::Fired ? frame * 10 + 4 : 0;
    r.lineEdgeTimestamp = outcome == TriggerOutcome::Fired ? frame * 1000 + 5 : 0;
    r.lineEdgeHostUs = outcome == TriggerOutcome::Fired ? frame * 10 + 5 : 0;
    r.outcome = static_cast<uint8_t>(outcome);
    return r;
}

bool sameEvent(const TriggerEventRecord& a, const TriggerEventRecord& b)
{
    return a.sequence == b.sequence && a.frameIndex == b.frameIndex && a.grabUs == b.grabUs &&
           a.objectId == b.objectId && a.trackId == b.trackId && a.generation == b.generation &&
           a.requestUs == b.requestUs && a.wakeUs == b.wakeUs && a.fireUs == b.fireUs &&
           a.pulseDoneUs == b.pulseDoneUs && a.lineEdgeTimestamp == b.lineEdgeTimestamp &&
           a.lineEdgeHostUs == b.lineEdgeHostUs && a.outcome == b.outcome;
}

} // namespace

int main()
{
    mib::test::TempDir td("mib_trigger_alignment");
    const std::string path = (td / "aligned.h5").string();
    constexpr size_t kSeries = 4;

    // Batch 1: a full contiguous series and a full gapped series.
    std::vector<ProcessedFrame> batch1{makeSeries(10, kSeries, true, 0),
                                       makeSeries(20, kSeries, false, 2)};
    // Batch 2 (append path): a partial series (experiment ended mid-series)
    // and a plain valid frame with no series (row must NOT be emitted).
    std::vector<ProcessedFrame> batch2{makeSeries(30, 2, true, 0)};
    {
        ProcessedFrame plain = makeSeries(40, 0, true, 0);
        plain.seriesImages.clear();
        plain.seriesInfo.clear();
        batch2.push_back(plain);
    }
    std::vector<TriggerEventRecord> events1{makeEvent(1, 10, TriggerOutcome::Fired),
                                            makeEvent(2, 15, TriggerOutcome::DroppedSetFailed)};
    std::vector<TriggerEventRecord> events2{makeEvent(3, 20, TriggerOutcome::Fired)};
    std::vector<TriggerEventRecord> events3{makeEvent(4, 30, TriggerOutcome::DroppedQueueFull)};

    // ---- fault: nothing writable before a file is open ----
    {
        Hdf5Service closed;
        MIB_EXPECT(!closed.appendTriggerEvents(events1), "append on a closed service fails");
        std::vector<TriggerEventRecord> out;
        MIB_EXPECT(!closed.readTriggerEvents(out) && out.empty(), "read on a closed service fails");
        std::vector<SeriesImageInfo> info;
        MIB_EXPECT(!closed.readSeriesMeta(0, info) && info.empty(), "series meta on closed fails");
    }

    // ---- write: create + append paths ----
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "openFile");
        MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets");
        std::vector<TriggerEventRecord> none;
        MIB_EXPECT(hdf5.readTriggerEvents(none) == false, "no /trigger_events before first append");
        MIB_EXPECT(hdf5.appendTriggerEvents({}), "appending nothing is a no-op success");
        MIB_REQUIRE(hdf5.appendFrames(batch1, {}), "appendFrames batch1 (creates series datasets)");
        MIB_REQUIRE(hdf5.appendTriggerEvents(events1), "appendTriggerEvents (create)");
        MIB_REQUIRE(hdf5.appendFrames(batch2, {}), "appendFrames batch2 (extends series datasets)");
        MIB_REQUIRE(hdf5.appendTriggerEvents(events2), "appendTriggerEvents (extend)");
        hdf5.closeFile();
    }

    // ---- reopen for update: the extent on disk must be trusted ----
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFileForUpdate(path), "openFileForUpdate");
        MIB_REQUIRE(hdf5.appendTriggerEvents(events3), "appendTriggerEvents after reopen");
        hdf5.closeFile();
    }

    // ---- read back ----
    {
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "loadFile");

        size_t n = 0, seriesCount = 0;
        int h = 0, w = 0;
        MIB_REQUIRE(r.getSeriesImageInfo(n, seriesCount, h, w), "series_images present");
        MIB_EXPECT(n == 3 && seriesCount == kSeries, "three series rows, width fixed by first batch");

        std::vector<SeriesImageInfo> info;
        bool contiguous = false;
        MIB_REQUIRE(r.readSeriesMeta(0, info, &contiguous), "series_meta row 0");
        MIB_EXPECT(info.size() == kSeries, "row width == seriesCount");
        MIB_EXPECT(contiguous, "row 0 contiguous");
        bool consecutive = info.size() == kSeries;
        for (size_t s = 0; s < info.size(); ++s) {
            if (info[s].frameIndex != 10 + s || info[s].timestampNs != (10 + s) * 1000ULL ||
                info[s].hostTimestampUs != (10 + s) * 10ULL) {
                consecutive = false;
            }
        }
        MIB_EXPECT(consecutive, "row 0 identity (index, camera ts, host ts) round-trips");

        MIB_REQUIRE(r.readSeriesMeta(1, info, &contiguous), "series_meta row 1");
        MIB_EXPECT(!contiguous, "row 1 flagged non-contiguous");
        MIB_EXPECT(info.size() == kSeries && info[1].frameIndex == 21 && info[2].frameIndex == 24,
                   "row 1 records the actual gap (21 -> 24)");

        MIB_REQUIRE(r.readSeriesMeta(2, info, &contiguous), "series_meta row 2 (partial, appended)");
        MIB_EXPECT(contiguous, "partial series still contiguous");
        MIB_EXPECT(info.size() == kSeries && info[0].frameIndex == 30 && info[1].frameIndex == 31,
                   "collected members present");
        MIB_EXPECT(info[2].frameIndex == Hdf5Service::kAbsentSeriesFrame &&
                       info[3].frameIndex == Hdf5Service::kAbsentSeriesFrame &&
                       info[2].timestampNs == 0 && info[2].hostTimestampUs == 0,
                   "uncollected members read back as absent, not as frame 0");

        MIB_EXPECT(!r.readSeriesMeta(3, info), "out-of-range row rejected");

        std::vector<TriggerEventRecord> ev;
        MIB_REQUIRE(r.readTriggerEvents(ev), "readTriggerEvents");
        MIB_REQUIRE(ev.size() == 4, "create + extend + reopen-extend rows all present");
        MIB_EXPECT(sameEvent(ev[0], events1[0]) && sameEvent(ev[1], events1[1]),
                   "batch 1 records round-trip field by field");
        MIB_EXPECT(sameEvent(ev[2], events2[0]), "batch 2 record round-trips");
        MIB_EXPECT(sameEvent(ev[3], events3[0]), "post-reopen record round-trips");
        MIB_EXPECT(ev[1].outcome == static_cast<uint8_t>(TriggerOutcome::DroppedSetFailed) &&
                       ev[1].fireUs == 0,
                   "a non-fired outcome keeps its zero fire stamp");

        // Fault: read-only handle must refuse writes without corrupting.
        MIB_EXPECT(!r.appendTriggerEvents(events1), "append on a read-only file fails");
        r.closeFile();
    }

    // ---- fault: a file without the datasets (pre-feature layout) ----
    {
        const std::string legacy = (td / "legacy.h5").string();
        {
            Hdf5Service hdf5;
            MIB_REQUIRE(hdf5.openFile(legacy), "openFile legacy");
            MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets legacy");
            std::vector<ProcessedFrame> plain{makeSeries(1, 0, true, 0)};
            plain[0].seriesImages.clear();
            plain[0].seriesInfo.clear();
            MIB_REQUIRE(hdf5.appendFrames(plain, {}), "appendFrames without series");
            hdf5.closeFile();
        }
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(legacy), "loadFile legacy");
        std::vector<SeriesImageInfo> info{SeriesImageInfo{1, 2, 3}};
        MIB_EXPECT(!r.readSeriesMeta(0, info) && info.empty(), "no series_meta -> false, output cleared");
        std::vector<TriggerEventRecord> ev{TriggerEventRecord{}};
        MIB_EXPECT(!r.readTriggerEvents(ev) && ev.empty(), "no trigger_events -> false, output cleared");
        r.closeFile();
    }

    return mib::test::exitCode();
}
