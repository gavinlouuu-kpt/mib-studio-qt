// review_inspect_test (#717 B3b, R3 Inspect's backend; review ABI 3)
//
// The linked records of a track and one record reprocessed next to its
// stored values.
//  Track rows (ReviewSession::trackRows):
//  - the records of a set sharing a track id, in set order, with their set
//    positions; the invalid set apart; `max` truncates and `total` counts
//    every match; above kReviewTrackRowsMax, a negative track id, a raw
//    recording and no open file are refused.
//  Reprocess one record (ReviewJobs::reprocessRecord):
//  - recorded settings: the stored record's own frame, config, ROI and
//    background give back the stored record, matched one-to-one with equal
//    values, and the result names the settings and the kernel;
//  - a frame where the kernel finds another number of objects than the
//    stored record says: every object returned, the stored one matched by
//    position (image coordinates, with an offset recorded ROI), the
//    difference flagged;
//  - default settings: the whole image, no background, labelled so;
//  - refused while a regenerate job holds a lane (the job reads HDF5 outside
//    the session's mutex, #722) and served once it ends; refused for a
//    record whose frame has no stored image, a raw recording, an index
//    outside the set, an unknown settings value and without a kernel;
//  - nothing is written: the file's bytes are unchanged.
// Watchdog guarded.
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <hdf5.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace backend::review;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;

namespace
{
    constexpr int kW = 180, kH = 96;
    // The recorded ROI is offset, so stored coordinates are not image ones.
    const ProcessingService::Roi kRoi{10, 0, 170, 96};

    cv::Mat rings(std::initializer_list<int> xs)
    {
        cv::Mat image(kH, kW, CV_8UC1, cv::Scalar(0));
        for (const int x : xs)
        {
            cv::circle(image, cv::Point(x, 48), 20, cv::Scalar(255), cv::FILLED);
            cv::circle(image, cv::Point(x, 48), 9, cv::Scalar(0), cv::FILLED);
        }
        return image;
    }

    ProcessingConfig permissive()
    {
        ProcessingConfig config;
        config.gaussian_blur_size = 1;
        config.bg_subtract_threshold = 127;
        config.morph_kernel_size = 1;
        config.morph_iterations = 1;
        config.enable_area_range_check = false;
        config.enable_ring_ratio_check = false;
        config.enable_deformability_range_check = false;
        config.enable_area_ratio_check = false;
        config.enable_border_check = true;
        config.require_single_inner_contour = true;
        return config;
    }

    // The stored record of `analysed`, as the kernel finds it with the
    // recorded inputs, stored with `image` (which may hold more than that).
    ProcessedFrame record(ProcessingService &kernel, const cv::Mat &analysed, const cv::Mat &image, uint64_t index,
                          int trackId)
    {
        const cv::Mat black(kH, kW, CV_8UC1, cv::Scalar(0));
        auto found = kernel.processBatch({analysed}, permissive(), black, kRoi);
        MIB_REQUIRE(found.size() == 1, "the fixture's kernel finds one cell, found " + std::to_string(found.size()));
        ProcessedFrame f = found.front();
        f.index = index;
        f.timestampNs = 1'000'000 + index * 1000;
        f.originalImage = image;
        f.processedImage = cv::Mat::zeros(kH, kW, CV_8UC1);
        f.validation.trackId = trackId;
        f.validation.isValid = true;
        return f;
    }

    // Valid: 0 one ring (track 7), 1 two rings stored as one record (track 7),
    // 2 one ring (track 9), 3 one ring (untracked). Invalid: 0 one ring (track 7).
    void writeInspect(ProcessingService &kernel, const fs::path &path)
    {
        std::vector<ProcessedFrame> valid{
            record(kernel, rings({60}), rings({60}), 10, 7),
            record(kernel, rings({50}), rings({50, 130}), 11, 7),
            record(kernel, rings({90}), rings({90}), 12, 9),
            record(kernel, rings({120}), rings({120}), 13, -1),
        };
        ProcessedFrame bad = record(kernel, rings({70}), rings({70}), 14, 7);
        bad.validation.isValid = false;
        std::vector<ProcessedFrame> invalid{bad};
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()) && hdf5.initializeDatasets(), "open inspect fixture");
        MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append inspect");
        const cv::Mat black(kH, kW, CV_8UC1, cv::Scalar(0));
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), permissive(), kRoi, &black),
                    "inspect info");
        MIB_REQUIRE(hdf5.writeRunSnapshotJson("{\"pixel_to_micron\":0.5}", "{}"), "snapshot");
        hdf5.closeFile();
    }

    void writeRecording(const fs::path &path)
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()) && hdf5.initializeRecordingDatasets(), "open recording");
        std::vector<cv::Mat> images{rings({60}), rings({90})};
        std::vector<Hdf5Service::RecordingFrameMeta> meta{{0, 0, kW, kH}, {1, 100, kW, kH}};
        MIB_REQUIRE(hdf5.appendRecordingFrames(images, meta) && hdf5.writeRecordingInfo(10, 20, 2, 0), "recording");
        hdf5.closeFile();
    }

    // The file's images dataset removed after writing: records without images.
    void dropImages(const fs::path &path)
    {
        hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open to drop images");
        MIB_REQUIRE(H5Ldelete(file, "/valid_frames/images", H5P_DEFAULT) >= 0, "drop /valid_frames/images");
        H5Fclose(file);
    }

    std::string bytesOf(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    bool same(const MetricRow &a, const MetricRow &b)
    {
        return a.area == b.area && a.deformability == b.deformability && a.bboxX == b.bboxX && a.bboxY == b.bboxY &&
               a.centroidX == b.centroidX && a.centroidY == b.centroidY && a.ringRatio == b.ringRatio &&
               a.areaUm2 == b.areaUm2 && a.valid == b.valid;
    }

    // A job sink that holds the job's thread at its first progress event.
    struct GatedSink
    {
        std::mutex m;
        std::condition_variable cv;
        bool held{false};
        bool release{false};
        std::vector<ReviewJobEvent> events;
        void operator()(const ReviewJobEvent &e)
        {
            std::unique_lock lock(m);
            events.push_back(e);
            if (e.state == ReviewJobState::Progress && !release)
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
            return cv.wait_for(lock, std::chrono::seconds(120), [&] {
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
    mib::test::TempDir td("review_inspect");
    ProcessingService kernel;
    const fs::path inspect = td.path() / "inspect.h5";
    const fs::path noImages = td.path() / "no_images.h5";
    const fs::path recording = td.path() / "rec.h5";
    writeInspect(kernel, inspect);
    writeInspect(kernel, noImages);
    dropImages(noImages);
    writeRecording(recording);

    // ---- track rows -------------------------------------------------------------
    {
        ReviewSession s;
        TrackRows rows;
        std::string err;
        MIB_EXPECT(!s.trackRows(true, 7, 10, rows, &err) && err == "No review file is open", "no file: " + err);
        MIB_REQUIRE(s.open(inspect.string()), "open inspect");
        MIB_REQUIRE(s.trackRows(true, 7, 10, rows, &err), "track 7: " + err);
        MIB_EXPECT(rows.positions == std::vector<uint64_t>({0, 1}) && rows.total == 2 && !rows.truncated,
                   "valid track 7 = positions 0, 1");
        MIB_EXPECT(rows.rows.size() == 2 && rows.rows[0].frameIndex == 10 && rows.rows[1].frameIndex == 11 &&
                       rows.rows[0].trackId == 7,
                   "the rows are the records, in set order");
        std::vector<MetricRow> page;
        uint64_t total = 0;
        MIB_REQUIRE(s.metricsPage(true, 1, 1, page, total), "page");
        MIB_EXPECT(same(rows.rows[1], page[0]), "a track row is the row metricsPage serves");
        MIB_REQUIRE(s.trackRows(false, 7, 10, rows, &err), "invalid track 7");
        MIB_EXPECT(rows.positions == std::vector<uint64_t>({0}) && rows.rows[0].frameIndex == 14, "the invalid set apart");
        MIB_REQUIRE(s.trackRows(true, 9, 10, rows, &err), "track 9");
        MIB_EXPECT(rows.positions == std::vector<uint64_t>({2}), "track 9 = position 2");
        MIB_REQUIRE(s.trackRows(true, 7, 1, rows, &err), "track 7, max 1");
        MIB_EXPECT(rows.positions == std::vector<uint64_t>({0}) && rows.total == 2 && rows.truncated,
                   "max truncates, total counts every match");
        MIB_REQUIRE(s.trackRows(true, 7, 0, rows, &err), "max 0");
        MIB_EXPECT(rows.rows.empty() && rows.total == 2 && rows.truncated, "max 0: only the count");
        MIB_REQUIRE(s.trackRows(true, 42, 10, rows, &err), "an unknown track");
        MIB_EXPECT(rows.rows.empty() && rows.total == 0 && !rows.truncated, "no match: empty, not refused");
        MIB_EXPECT(s.trackRows(true, 7, kReviewTrackRowsMax, rows, &err), "the cap itself is served");
        MIB_EXPECT(!s.trackRows(true, 7, kReviewTrackRowsMax + 1, rows, &err) && err.find("at most 256") != std::string::npos,
                   "above the cap: refused: " + err);
        MIB_EXPECT(!s.trackRows(true, -1, 10, rows, &err) && err.find("not tracked") != std::string::npos,
                   "an untracked record has no links: " + err);
        MIB_REQUIRE(s.open(recording.string()), "open recording");
        MIB_EXPECT(!s.trackRows(true, 0, 10, rows, &err) && err == "A raw recording has no records", "recording: " + err);
    }
    wd.mark("track rows");

    // ---- reprocess one record ------------------------------------------------------
    {
        const std::string before = bytesOf(inspect);
        ReviewSession s;
        MIB_REQUIRE(s.open(inspect.string()), "open inspect");
        GatedSink sink;
        ReviewJobs jobs(s, &kernel, [&sink](const ReviewJobEvent &e) { sink(e); });
        ReprocessResult r;
        std::string err;

        // Recorded settings, one cell: the stored record back, one to one.
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err), "reprocess 0: " + err);
        MIB_EXPECT(r.usedRecordedSettings && r.settingsLabel == "recorded settings (config, ROI, background)",
                   "recorded settings named: " + r.settingsLabel);
        MIB_EXPECT(!r.coreVersion.empty(), "the kernel that reprocessed it is named");
        MIB_EXPECT(r.reprocessed.size() == 1 && r.match == std::optional<std::size_t>(0) && !r.objectCountDiffers,
                   "one object, matched: " + std::to_string(r.reprocessed.size()));
        MIB_EXPECT(r.stored.frameIndex == 10 && r.reprocessed[0].frameIndex == 10, "both carry the record's frame");
        MIB_EXPECT(same(r.stored, r.reprocessed[0]), "same inputs, same kernel: equal values");
        std::vector<MetricRow> page;
        uint64_t total = 0;
        MIB_REQUIRE(s.metricsPage(true, 0, 1, page, total), "page 0");
        MIB_EXPECT(same(r.stored, page[0]), "the stored side is the stored row");

        // Two cells in the frame, one stored record: both returned, the stored one matched.
        MIB_REQUIRE(jobs.reprocessRecord(true, 1, ReprocessSettings::Recorded, r, &err), "reprocess 1: " + err);
        MIB_EXPECT(r.reprocessed.size() == 2 && r.objectCountDiffers, "two objects found, the difference flagged");
        MIB_REQUIRE(r.match.has_value(), "the stored cell is matched");
        const auto &m = r.reprocessed[*r.match];
        MIB_EXPECT(same(r.stored, m), "matched to the cell the record measured, not the new one");
        const auto &other = r.reprocessed[1 - *r.match];
        MIB_EXPECT(other.centroidX > 110.0, "the other is the ring at x 130");

        // Default settings: the default config, the whole image, no
        // background. The same cell is found and matched (coordinates are
        // image coordinates whatever the ROI), measured differently.
        MIB_REQUIRE(jobs.reprocessRecord(true, 0, ReprocessSettings::Default, r, &err), "reprocess default: " + err);
        MIB_EXPECT(!r.usedRecordedSettings && r.settings == ReprocessSettings::Default &&
                       r.settingsLabel == "default settings (whole image, no background)",
                   "default settings named: " + r.settingsLabel);
        MIB_EXPECT(r.stored.frameIndex == 10, "the same record");
        MIB_REQUIRE(r.reprocessed.size() == 1 && r.match == std::optional<std::size_t>(0),
                    "the default run finds and matches the cell: " + std::to_string(r.reprocessed.size()));
        MIB_EXPECT(std::abs(r.reprocessed[0].centroidX - r.stored.centroidX) < 1.0 &&
                       std::abs(r.reprocessed[0].centroidY - r.stored.centroidY) < 1.0,
                   "at the stored cell's position");
        MIB_EXPECT(!same(r.stored, r.reprocessed[0]), "other settings, other values: the settings were applied");

        // Refusals.
        MIB_EXPECT(!jobs.reprocessRecord(true, 4, ReprocessSettings::Recorded, r, &err) &&
                       err.find("No record 4 in the valid set") == 0,
                   "outside the set: " + err);
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, static_cast<ReprocessSettings>(7), r, &err) &&
                       err == "Unknown reprocess settings",
                   "unknown settings: " + err);
        ReviewJobs bare(s, nullptr, [](const ReviewJobEvent &) {});
        MIB_EXPECT(!bare.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       err.find("No processing kernel") == 0,
                   "no kernel: " + err);

        // A regenerate job holds the processing lane: refused, then served.
        RegenerateMasksRequest req;
        req.source = RegenerateSource::WholeFile;
        req.outputPath = (td.path() / "regenerated.h5").string();
        const uint64_t id = jobs.startRegenerateMasks(req, &err);
        MIB_REQUIRE(id != 0, "regenerate starts: " + err);
        MIB_REQUIRE(sink.waitHeld(), "the regenerate is held at its first progress event");
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       err.find("A review job is running") == 0,
                   "refused while the job runs: " + err);
        sink.releaseJob();
        MIB_REQUIRE(sink.terminal(id), "the regenerate ends");
        bool served = false;
        for (int i = 0; i < 200 && !served; ++i)
        {
            served = jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err);
            if (!served) std::this_thread::sleep_for(std::chrono::milliseconds(10)); // the lane frees after the terminal
        }
        MIB_EXPECT(served && r.match.has_value(), "served once the job has ended: " + err);
        s.close();
        MIB_EXPECT(bytesOf(inspect) == before, "reprocessing wrote nothing to the file");

        // A record whose frame has no stored image, and a raw recording.
        MIB_REQUIRE(s.open(noImages.string()), "open the file without images");
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       err == "Record 0 has no stored image to reprocess",
                   "no image: " + err);
        MIB_REQUIRE(s.open(recording.string()), "open recording");
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       err == "A raw recording has no stored records to reprocess",
                   "recording: " + err);
        s.close();
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) && err == "No review file is open",
                   "no file: " + err);
    }
    wd.mark("reprocess");
    return mib::test::exitCode();
}
