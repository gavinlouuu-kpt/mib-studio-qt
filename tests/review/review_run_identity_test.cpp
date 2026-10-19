// review_run_identity_test (#717 B1a, R1 Open's backend)
//
// The run identity a review file carries (run snapshot v3, #712, and the
// planned /experiment_annotation) as ReviewSession reads it, and the
// experiment library's header-only identity probe.
//  Round trip:
//  - a v3 file: snapshot version, run id, experiment id, run index, sample
//    ref, calibration record id and the annotation (fixed members + free
//    key/values, sorted) read back as written, through open() and the probe;
//  - the probe's header facts: start/end time, counts, run accounting, the
//    processing core version; a recording file's counts and times.
//  Legacy read (absent, never fabricated):
//  - a file without a snapshot or annotation reads every member absent: no
//    zero ids, no empty strings, no version; a v1 snapshot is a snapshot
//    with no identity; a v3 snapshot that declares nothing reads absent;
//  - a file without times or counts probes them as absent.
//  Fault injection:
//  - a snapshot or annotation that is not a JSON object reads as unreadable,
//    not as legacy; a corrupt HDF5 file, a missing path, a directory and an
//    empty path are per-file errors and never fail the call;
//  - more than review_probe.max_paths (256) paths are refused as a whole.
//  Concurrency (HDF5 is not thread-safe, #722):
//  - a probe while a file is open leaves the session's file, metadata,
//    images and rows as they were, also when it probes the open file, and
//    leaks no HDF5 handle;
//  - probes and interactive reads from two threads both succeed (the probe
//    takes the session's mutex per file);
//  - ReviewJobs::probeRuns is refused while a job runs and works after it.
// Watchdog guarded.

#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RunProvenanceRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <hdf5.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace rec = backend::recording;
using backend::review::OverlayMode;
using backend::review::ReviewDataset;
using backend::review::ReviewImage;
using backend::review::ReviewJobEvent;
using backend::review::ReviewJobs;
using backend::review::ReviewJobState;
using backend::review::ReviewSession;
using backend::review::RunIdentity;
using backend::review::RunProbe;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace
{
    constexpr int kH = 24, kW = 32;
    const char *kExperimentId = "5b1d6f8e-2c4a-4e7b-9a3d-0f1e2d3c4b5a";
    const char *kRunId = "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f";

    cv::Mat pattern(uint64_t index)
    {
        cv::Mat m(kH, kW, CV_8UC1);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7) % 200);
        return m;
    }

    ProcessedFrame makeFrame(uint64_t idx, bool valid)
    {
        ProcessedFrame f;
        f.index = idx;
        f.timestampNs = (idx + 1) * 1000ULL;
        f.originalImage = pattern(idx);
        f.processedImage = cv::Mat::zeros(kH, kW, CV_8UC1);
        cv::circle(f.processedImage, cv::Point(kW / 2, kH / 2), 8, cv::Scalar(255), -1);
        f.validation.isValid = valid;
        f.validation.objectCount = 1;
        f.validation.area = 100.0 + static_cast<double>(idx);
        f.validation.deformability = 0.05;
        return f;
    }

    // What the file's /run_provenance and /experiment_annotation hold.
    enum class Snapshot
    {
        None,          // legacy: no /run_provenance
        V1,            // a snapshot from before schema_version
        V3Declared,    // v3 with the declared identity
        V3Undeclared,  // v3 whose identity members are all null
        NotJson,       // present, not a JSON object
    };
    enum class Annotation
    {
        None,
        Planned,
        NotJson,
    };

    rec::ExperimentAnnotation planned()
    {
        rec::ExperimentAnnotation a;
        a.sample = "Donor 4 · CytoD 0.1";
        a.condition = "CytoD 0.1";
        a.experimentalUnit = "donor-4";
        a.concentration = rec::ConcentrationRecord{0.1, "uM"};
        a.channel = "30 um";
        a.medium = "PBS";
        a.temperatureC = 22.5;
        a.notes = "first run of the day";
        a.free = {{"plate", "B"}, {"batch", "2026-10-19"}}; // written unsorted
        return a;
    }

    void writeExperiment(const fs::path &path, Snapshot snapshot, Annotation annotation, bool withInfo = true,
                         bool withAccounting = true)
    {
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < 10; ++i) valid.push_back(makeFrame(static_cast<uint64_t>(i * 2), true));
        for (int i = 0; i < 4; ++i) invalid.push_back(makeFrame(static_cast<uint64_t>(i * 2 + 1), false));
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()), "open fixture");
        MIB_REQUIRE(hdf5.initializeDatasets(), "init datasets");
        MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append frames");
        if (withInfo)
        {
            backend::services::ProcessingConfig cfg;
            backend::services::ProcessingService::Roi roi{0, 0, kW, kH};
            backend::processing::ProcessingCoreIdentity core;
            core.version = "2.1.0";
            MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi, nullptr, &core),
                        "experiment info");
        }
        if (withInfo && withAccounting)
        {
            rec::RecordingAccountingSnapshot a;
            a.admitted = 14;
            a.processed = 10;
            a.scientificallyRejected = 4;
            a.persistenceAdmitted = 14;
            a.persistenceCommitted = 14;
            a.completion = rec::RunCompletionState::Complete;
            a.reconciled = true;
            MIB_REQUIRE(hdf5.writeRunAccounting(a), "accounting");
        }
        std::string doc;
        switch (snapshot)
        {
        case Snapshot::None:
            break;
        case Snapshot::V1:
            doc = "{\"pixel_to_micron\":0.25,\"profile_id\":\"p\"}";
            break;
        case Snapshot::V3Declared:
        {
            rec::RunProvenanceV3 v3;
            v3.runId = kRunId;
            v3.identity.experimentId = kExperimentId;
            v3.identity.runIndex = 2;
            v3.identity.sampleRef = "S-03";
            v3.identity.calibrationRecordId = "cal-0042";
            v3.identity.selectedOperator = rec::OperatorRecord{std::nullopt, std::string("Gavin")};
            doc = "{\"schema_version\":3,\"pixel_to_micron\":0.25,\"profile_id\":\"p\"," + rec::runProvenanceV3Members(v3) + "}";
            break;
        }
        case Snapshot::V3Undeclared:
            doc = "{\"schema_version\":3,\"pixel_to_micron\":0.25,\"profile_id\":\"p\"," +
                  rec::runProvenanceV3Members(rec::RunProvenanceV3{}) + "}";
            break;
        case Snapshot::NotJson:
            doc = "not json {";
            break;
        }
        if (snapshot != Snapshot::None) MIB_REQUIRE(hdf5.writeRunSnapshotJson(doc, "{}"), "run snapshot");
        if (annotation == Annotation::Planned)
        {
            MIB_REQUIRE(hdf5.writeExperimentAnnotationJson(rec::experimentAnnotationToJson(planned())), "annotation");
        }
        else if (annotation == Annotation::NotJson)
        {
            MIB_REQUIRE(hdf5.writeExperimentAnnotationJson("[1, 2]"), "annotation (not an object)");
        }
        hdf5.closeFile();
    }

    void writeRecording(const fs::path &path, int frames)
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()), "open recording fixture");
        MIB_REQUIRE(hdf5.initializeRecordingDatasets(), "init recording datasets");
        std::vector<cv::Mat> images;
        std::vector<Hdf5Service::RecordingFrameMeta> meta;
        for (int i = 0; i < frames; ++i)
        {
            images.push_back(pattern(static_cast<uint64_t>(i)));
            meta.push_back({static_cast<uint64_t>(i), static_cast<uint64_t>(i) * 100, kW, kH});
        }
        MIB_REQUIRE(hdf5.appendRecordingFrames(images, meta), "append recording frames");
        MIB_REQUIRE(hdf5.writeRecordingInfo(10, 20, frames, 2), "recording info");
        hdf5.closeFile();
    }

    // Every member of a legacy identity is absent: nothing stands in for it.
    bool allAbsent(const RunIdentity &id)
    {
        const auto &d = id.declared;
        return !id.hasSnapshot && !id.snapshotUnreadable && id.snapshotVersion == 0 && !id.runId && d.empty() &&
               !id.annotation && !id.annotationUnreadable && !id.annotationSchemaVersion;
    }

    bool declaredAsWritten(const RunIdentity &id)
    {
        const auto &d = id.declared;
        return id.hasSnapshot && !id.snapshotUnreadable && id.snapshotVersion == 3 && id.runId == std::string(kRunId) &&
               d.experimentId == std::string(kExperimentId) && d.runIndex == 2u && d.sampleRef == std::string("S-03") &&
               d.calibrationRecordId == std::string("cal-0042") && d.selectedOperator &&
               d.selectedOperator->name == std::string("Gavin") && !d.selectedOperator->id;
    }

    bool annotationAsWritten(const RunIdentity &id)
    {
        if (!id.annotation || id.annotationUnreadable || id.annotationSchemaVersion != 1u) return false;
        const auto &a = *id.annotation;
        const std::vector<std::pair<std::string, std::string>> sortedFree{{"batch", "2026-10-19"}, {"plate", "B"}};
        return a.sample == std::string("Donor 4 · CytoD 0.1") && a.condition == std::string("CytoD 0.1") &&
               a.experimentalUnit == std::string("donor-4") && a.concentration && a.concentration->value == 0.1 &&
               a.concentration->unit == "uM" && a.channel == std::string("30 um") && a.medium == std::string("PBS") &&
               a.temperatureC == 22.5 && a.notes == std::string("first run of the day") && a.free == sortedFree;
    }

    long long openFiles() { return H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE); }

    // A job sink that can hold the job's thread at its first progress event.
    struct GatedSink
    {
        std::mutex m;
        std::condition_variable cv;
        bool hold{false};
        bool held{false};
        bool release{false};
        std::vector<ReviewJobEvent> events;
        void operator()(const ReviewJobEvent &e)
        {
            std::unique_lock lock(m);
            events.push_back(e);
            if (e.state == ReviewJobState::Progress && hold && !release)
            {
                held = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
            }
            cv.notify_all();
        }
        bool waitHeld()
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(60), [&] { return held; });
        }
        void releaseJob()
        {
            std::scoped_lock lock(m);
            release = true;
            cv.notify_all();
        }
        bool terminal(uint64_t id)
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(60), [&] {
                for (const auto &e : events)
                    if (e.operationId == id && (e.state == ReviewJobState::Completed || e.state == ReviewJobState::Failed ||
                                                e.state == ReviewJobState::Cancelled))
                        return true;
                return false;
            });
        }
    };
} // namespace

int main()
{
    mib::test::Watchdog wd(180);
    mib::test::TempDir td("review_run_identity");
    const fs::path v3 = td.path() / "run_v3.h5";
    const fs::path legacy = td.path() / "legacy.h5";
    const fs::path v1 = td.path() / "run_v1.h5";
    const fs::path undeclared = td.path() / "run_undeclared.h5";
    const fs::path badSnapshot = td.path() / "bad_snapshot.h5";
    const fs::path noInfo = td.path() / "no_info.h5";
    const fs::path recording = td.path() / "rec.h5";
    const fs::path corrupt = td.path() / "corrupt.h5";
    writeExperiment(v3, Snapshot::V3Declared, Annotation::Planned);
    writeExperiment(legacy, Snapshot::None, Annotation::None, /*withInfo=*/true, /*withAccounting=*/false);
    writeExperiment(v1, Snapshot::V1, Annotation::None);
    writeExperiment(undeclared, Snapshot::V3Undeclared, Annotation::None);
    writeExperiment(badSnapshot, Snapshot::NotJson, Annotation::NotJson);
    writeExperiment(noInfo, Snapshot::None, Annotation::None, /*withInfo=*/false);
    writeRecording(recording, 9);
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "this is not an HDF5 file, only bytes that end in .h5";
    }

    // ---- identity through open() ----------------------------------------------
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(v3.string()), "open v3");
        const auto id = s.metadata().runIdentity;
        MIB_EXPECT(declaredAsWritten(id), "v3 identity reads back as written");
        MIB_EXPECT(annotationAsWritten(id), "the annotation reads back as written, free keys sorted");

        MIB_REQUIRE(s.open(legacy.string()), "open legacy");
        MIB_EXPECT(allAbsent(s.metadata().runIdentity), "a legacy file reads every identity member absent");

        MIB_REQUIRE(s.open(v1.string()), "open v1");
        const auto old = s.metadata().runIdentity;
        MIB_EXPECT(old.hasSnapshot && old.snapshotVersion == 1 && !old.runId && old.declared.empty() && !old.annotation,
                   "a v1 snapshot is a snapshot without an identity");

        MIB_REQUIRE(s.open(undeclared.string()), "open undeclared v3");
        const auto none = s.metadata().runIdentity;
        MIB_EXPECT(none.hasSnapshot && none.snapshotVersion == 3 && !none.runId && none.declared.empty(),
                   "a v3 snapshot that declares nothing reads absent, not as zero or empty");

        MIB_REQUIRE(s.open(badSnapshot.string()), "a file with an unreadable snapshot still opens");
        const auto bad = s.metadata().runIdentity;
        MIB_EXPECT(!bad.hasSnapshot && bad.snapshotUnreadable && bad.declared.empty() && !bad.runId,
                   "a snapshot that is not JSON is unreadable, not legacy");
        MIB_EXPECT(!bad.annotation && bad.annotationUnreadable && !bad.annotationSchemaVersion,
                   "an annotation that is not an object is unreadable");
        s.close();
        MIB_EXPECT(allAbsent(s.metadata().runIdentity), "closed: nothing");
    }
    wd.mark("open");

    // ---- the probe: header facts, per-file errors -----------------------------
    {
        ReviewSession s;
        const std::vector<std::string> paths{v3.string(),        legacy.string(), recording.string(), corrupt.string(),
                                             (td.path() / "missing.h5").string(), td.path().string(), "",
                                             badSnapshot.string(), noInfo.string()};
        std::vector<RunProbe> probes;
        std::string err;
        MIB_REQUIRE(s.probeRuns(paths, probes, &err), "probe: " + err);
        MIB_REQUIRE(probes.size() == paths.size(), "one result per path");
        for (std::size_t i = 0; i < paths.size(); ++i) MIB_EXPECT(probes[i].path == paths[i], "results keep the order");

        const auto &a = probes[0];
        MIB_EXPECT(a.ok && a.message.empty() && !a.recordingFile, "v3 probes");
        MIB_EXPECT(declaredAsWritten(a.identity) && annotationAsWritten(a.identity), "v3 identity in the probe");
        MIB_EXPECT(a.startTimeNs == 1000u && a.endTimeNs == 5000u && a.totalValid == 10u && a.totalInvalid == 4u,
                   "v3 times and counts");
        MIB_EXPECT(a.completion == rec::RunCompletionState::Complete && a.coreVersion == std::string("2.1.0"),
                   "v3 accounting and core");

        const auto &l = probes[1];
        MIB_EXPECT(l.ok && allAbsent(l.identity), "legacy probes with every identity member absent");
        MIB_EXPECT(l.totalValid == 10u && !l.completion, "legacy: counts recorded, accounting absent");

        const auto &r = probes[2];
        MIB_EXPECT(r.ok && r.recordingFile && r.totalValid == 9u && !r.totalInvalid, "recording counts");
        MIB_EXPECT(r.startTimeNs == 10u && r.endTimeNs == 20u && allAbsent(r.identity), "recording times, no identity");

        for (std::size_t i : {3u, 4u, 5u, 6u})
        {
            const auto &e = probes[i];
            MIB_EXPECT(!e.ok && !e.message.empty(), "per-file error " + std::to_string(i) + ": " + e.message);
            MIB_EXPECT(allAbsent(e.identity) && !e.startTimeNs && !e.totalValid && !e.completion && !e.coreVersion,
                       "an error carries nothing else " + std::to_string(i));
        }
        MIB_EXPECT(probes[3].message.find("Not an HDF5 file") == 0, "corrupt: " + probes[3].message);
        MIB_EXPECT(probes[4].message.find("No such file") == 0, "missing: " + probes[4].message);
        MIB_EXPECT(probes[5].message.find("Not a file") == 0, "directory: " + probes[5].message);
        MIB_EXPECT(probes[6].message == "No file path", "empty path");

        const auto &b = probes[7];
        MIB_EXPECT(b.ok && b.identity.snapshotUnreadable && !b.identity.hasSnapshot && b.identity.annotationUnreadable,
                   "unreadable snapshot and annotation in the probe");

        const auto &n = probes[8];
        MIB_EXPECT(n.ok && !n.startTimeNs && !n.endTimeNs && !n.totalValid && !n.totalInvalid && !n.completion &&
                       !n.coreVersion,
                   "a file without /experiment_info probes its times and counts as absent");

        // The cap: refused as a whole above it, served at it.
        std::vector<std::string> many(backend::review::kReviewProbeMaxPaths + 1, legacy.string());
        probes.assign(3, RunProbe{});
        err.clear();
        MIB_EXPECT(!s.probeRuns(many, probes, &err) && probes.empty(), "more than the cap is refused");
        MIB_EXPECT(err.find("Too many files") == 0 && err.find("257") != std::string::npos && err.find("256") != std::string::npos,
                   "the refusal names the counts: " + err);
        many.pop_back();
        MIB_EXPECT(s.probeRuns(many, probes, &err) && probes.size() == 256 && probes.back().ok, "the cap itself is served");
        MIB_EXPECT(s.probeRuns({}, probes, &err) && probes.empty(), "an empty list is an empty answer");
    }
    wd.mark("probe");

    // ---- a probe while a file is open ------------------------------------------
    {
        const auto baseline = openFiles();
        ReviewSession s;
        MIB_REQUIRE(s.open(v3.string()), "open v3");
        const auto before = s.metadata();
        ReviewImage imageBefore;
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 3, OverlayMode::AllMask, true, imageBefore), "image before");
        std::vector<RunProbe> probes;
        std::string err;
        // The open file itself, a legacy file and a corrupt one.
        MIB_REQUIRE(s.probeRuns({v3.string(), legacy.string(), corrupt.string(), v3.string()}, probes, &err), err);
        MIB_EXPECT(probes[0].ok && declaredAsWritten(probes[0].identity) && probes[3].ok, "the open file probes");
        MIB_EXPECT(probes[1].ok && !probes[2].ok, "the others probe as before");
        MIB_EXPECT(s.isOpen() && s.filePath() == v3.string(), "the session still holds its file");
        const auto after = s.metadata();
        MIB_EXPECT(after.fileOpen && after.totalValid == before.totalValid && declaredAsWritten(after.runIdentity),
                   "metadata unchanged");
        ReviewImage imageAfter;
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 3, OverlayMode::AllMask, true, imageAfter), "image after");
        MIB_EXPECT(imageAfter.data == imageBefore.data && imageAfter.channels == imageBefore.channels, "the same pixels");
        std::vector<backend::review::MetricRow> rows;
        uint64_t total = 0;
        MIB_EXPECT(s.metricsPage(false, 0, 10, rows, total) && total == 4 && rows.size() == 4, "rows still served");
        MIB_EXPECT(openFiles() == baseline + 1, "only the session's own handle stays open");
        s.close();
        MIB_EXPECT(openFiles() == baseline, "closed: no handle left");
    }
    wd.mark("probe while open");

    // ---- probes and reads from two threads -------------------------------------
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(v3.string()), "open v3");
        std::atomic<int> readFailures{0};
        std::atomic<int> probeFailures{0};
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            for (int i = 0; i < 200 && !stop.load(); ++i)
            {
                ReviewImage img;
                std::vector<backend::review::MetricRow> rows;
                uint64_t total = 0;
                if (!s.fetchImage(ReviewDataset::ValidImage, static_cast<uint64_t>(i % 10), OverlayMode::AllContour, false, img) ||
                    !s.metricsPage(true, 0, 10, rows, total) || total != 10)
                {
                    readFailures.fetch_add(1);
                }
            }
        });
        for (int i = 0; i < 40; ++i)
        {
            std::vector<RunProbe> probes;
            if (!s.probeRuns({v3.string(), legacy.string(), recording.string()}, probes) || probes.size() != 3 ||
                !probes[0].ok || !declaredAsWritten(probes[0].identity) || !probes[1].ok || !probes[2].ok)
            {
                probeFailures.fetch_add(1);
            }
        }
        stop.store(true);
        reader.join();
        MIB_EXPECT(readFailures.load() == 0, "reads beside the probe: " + std::to_string(readFailures.load()) + " failed");
        MIB_EXPECT(probeFailures.load() == 0, "probes beside the reads: " + std::to_string(probeFailures.load()) + " failed");
    }
    wd.mark("two threads");

    // ---- the job lane gate -------------------------------------------------------
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(v3.string()), "open v3");
        GatedSink sink;
        sink.hold = true;
        ReviewJobs jobs(s, nullptr, [&sink](const ReviewJobEvent &e) { sink(e); });
        std::string err;
        const uint64_t id = jobs.startExportMetrics((td.path() / "metrics.csv").string(), &err);
        MIB_REQUIRE(id != 0, "export starts: " + err);
        MIB_REQUIRE(sink.waitHeld(), "the export is held at its first progress event");
        std::vector<RunProbe> probes(2);
        err.clear();
        MIB_EXPECT(!jobs.probeRuns({legacy.string()}, probes, &err) && probes.empty(), "refused while a job runs");
        MIB_EXPECT(err.find("A review job is running") == 0, "the refusal says why: " + err);
        sink.releaseJob();
        MIB_REQUIRE(sink.terminal(id), "the export ends");
        for (int i = 0; i < 6000 && jobs.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        MIB_REQUIRE(!jobs.busy(), "the lane is free");
        err.clear();
        MIB_EXPECT(jobs.probeRuns({legacy.string(), v3.string()}, probes, &err) && probes.size() == 2 && probes[1].ok &&
                       declaredAsWritten(probes[1].identity),
                   "served once the lane is free: " + err);
        MIB_EXPECT(!jobs.probeRuns(std::vector<std::string>(257, legacy.string()), probes, &err), "the cap holds on the lane");
        jobs.shutdown();
    }
    wd.mark("job lane");

    return mib::test::exitCode();
}
