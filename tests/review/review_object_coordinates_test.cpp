// review_object_coordinates_test (#756 review 2; review ABI 3)
//
// The coordinate space of a file's stored bounding boxes and centroids,
// decided once at open (ObjectCoordinates.h), and the one-record reprocess
// match placing the stored record through it.
//  - The helpers: values and sources as the contract names them, the one
//    conversion (refused for unknown), the mask check's sample, vote and
//    verdict (at least 8 decisive records agreeing at 90 % or more).
//  - A capture-style file (ROI-local rows next to full-frame images and
//    masks, the ROI at (40, 24)) is roi_local by its attribute, by a v3
//    host snapshot with the inline realtime path, and, with neither, by the
//    mask check; its record matches one to one.
//  - A regenerate-style file (image rows) is image by the attribute that
//    saveMasksToHdf5 writes, and, without it, by the mask check; a v3
//    snapshot of the async batch path is image too; it matches one to one.
//  - An ROI at (0, 0): image by the ROI origin.
//  - Ambiguous files are unknown and the reprocess match is refused with a
//    plain reason: blank masks, fewer than 8 decisive records, 70 % agreement,
//    a PL-science snapshot (its rows come from the PL), an unrecognised
//    attribute value with no other evidence.
// Watchdog guarded.
#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ObjectCoordinates.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
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
    constexpr int kW = 200, kH = 120;
    // An ROI away from the image origin: ROI-local and image placements of a
    // box differ by (40, 24), more than a cell's size.
    const ProcessingService::Roi kRoi{40, 24, 140, 80};
    const cv::Rect kCvRoi(kRoi.x, kRoi.y, kRoi.w, kRoi.h);

    cv::Mat ringAt(int x, int y)
    {
        cv::Mat image(kH, kW, CV_8UC1, cv::Scalar(0));
        cv::circle(image, cv::Point(x, y), 15, cv::Scalar(255), cv::FILLED);
        cv::circle(image, cv::Point(x, y), 6, cv::Scalar(0), cv::FILLED);
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

    const cv::Mat &black()
    {
        static const cv::Mat image(kH, kW, CV_8UC1, cv::Scalar(0));
        return image;
    }

    // Cell centres inside the ROI, spread over it.
    const std::vector<cv::Point> kCells{{60, 46}, {75, 64}, {90, 82}, {105, 46}, {120, 64},
                                        {135, 82}, {150, 46}, {160, 64}, {70, 82}, {140, 60}};

    // As the realtime inline path with an ROI stores a record: the ROI's mask
    // validated (ROI-local box and centroid), the full image, and the ROI's
    // mask pasted into a full-frame mask.
    ProcessedFrame capture(ProcessingService &kernel, cv::Point cell, uint64_t index)
    {
        const cv::Mat full = ringAt(cell.x, cell.y);
        const cv::Mat crop = full(kCvRoi).clone();
        auto found = kernel.processBatch({crop}, permissive(), cv::Mat::zeros(crop.size(), CV_8UC1),
                                         ProcessingService::Roi{});
        MIB_REQUIRE(found.size() == 1 && found.front().validation.bboxWidth > 0, "the capture fixture finds one cell");
        ProcessedFrame f = found.front();
        cv::Mat mask = cv::Mat::zeros(kH, kW, CV_8UC1);
        f.processedImage.copyTo(mask(kCvRoi));
        f.originalImage = full;
        f.processedImage = mask;
        f.index = index;
        f.timestampNs = 1'000'000 + index * 1000;
        f.validation.isValid = true;
        f.validation.trackId = -1;
        return f;
    }

    // As regenerate stores a record: computeProcessedFrame over the full
    // image with the ROI (image coordinates, full-frame mask).
    ProcessedFrame regenerate(ProcessingService &kernel, cv::Point cell, uint64_t index)
    {
        auto found = kernel.processBatch({ringAt(cell.x, cell.y)}, permissive(), black(), kRoi);
        MIB_REQUIRE(found.size() == 1 && found.front().validation.bboxWidth > 0, "the regenerate fixture finds one cell");
        ProcessedFrame f = found.front();
        f.index = index;
        f.timestampNs = 1'000'000 + index * 1000;
        f.validation.isValid = true;
        f.validation.trackId = -1;
        return f;
    }

    struct Spec
    {
        std::vector<ProcessedFrame> frames;
        ProcessingService::Roi roi{kRoi};
        std::optional<std::string> attribute;
        std::string snapshot;
        bool blankMasks{false};
    };

    void write(const fs::path &path, Spec spec)
    {
        if (spec.blankMasks)
            for (auto &f : spec.frames) f.processedImage = cv::Mat::zeros(kH, kW, CV_8UC1);
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()) && hdf5.initializeDatasets(), "open " + path.string());
        MIB_REQUIRE(hdf5.appendFrames(spec.frames, {}), "append " + path.string());
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, spec.frames.size(), 0, permissive(), spec.roi, &black()),
                    "info " + path.string());
        if (spec.attribute) MIB_REQUIRE(hdf5.writeObjectCoordinates(*spec.attribute), "attribute " + path.string());
        if (!spec.snapshot.empty()) MIB_REQUIRE(hdf5.writeRunSnapshotJson(spec.snapshot, "{}"), "snapshot " + path.string());
        hdf5.closeFile();
    }

    std::string snapshotV3(const char *realtimeMode, const char *science = "host")
    {
        return std::string("{\"schema_version\":3,\"roi\":{\"x\":40,\"y\":24,\"w\":140,\"h\":80},\"realtime_mode\":\"") +
               realtimeMode + "\",\"science_placement\":\"" + science + "\"}";
    }

    std::vector<ProcessedFrame> captures(ProcessingService &kernel, std::size_t n)
    {
        std::vector<ProcessedFrame> out;
        for (std::size_t i = 0; i < n; ++i) out.push_back(capture(kernel, kCells[i % kCells.size()], 100 + i));
        return out;
    }

    std::vector<ProcessedFrame> regenerated(ProcessingService &kernel, std::size_t n)
    {
        std::vector<ProcessedFrame> out;
        for (std::size_t i = 0; i < n; ++i) out.push_back(regenerate(kernel, kCells[i % kCells.size()], 100 + i));
        return out;
    }

    ObjectCoordinates spaceOf(const fs::path &path)
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(path.string()), "open " + path.string());
        return s.metadata().objectCoordinates;
    }

    std::string describe(const ObjectCoordinates &c)
    {
        return std::string(toString(c.space)) + "/" + toString(c.source) + " (" + std::to_string(c.originX) + ", " +
               std::to_string(c.originY) + ")";
    }

    bool is(const ObjectCoordinates &c, ObjectSpace space, ObjectSpaceSource source)
    {
        const bool roiLocal = space == ObjectSpace::RoiLocal;
        return c.space == space && c.source == source && c.originX == (roiLocal ? kRoi.x : 0) &&
               c.originY == (roiLocal ? kRoi.y : 0);
    }

    // Reprocess record 3 with the recorded settings: matched one to one, the
    // stored record placed exactly on the reprocessed one.
    void expectMatched(ProcessingService &kernel, const fs::path &path, const std::string &what)
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(path.string()), "open " + what);
        ReviewJobs jobs(s, &kernel, [](const ReviewJobEvent &) {});
        ReprocessResult r;
        std::string err;
        MIB_REQUIRE(jobs.reprocessRecord(true, 3, ReprocessSettings::Recorded, r, &err), what + ": " + err);
        MIB_REQUIRE(r.reprocessed.size() == 1 && r.match == std::optional<std::size_t>(0),
                    what + ": one object, matched (" + std::to_string(r.reprocessed.size()) + " found)");
        cv::Rect2d box(r.stored.bboxX, r.stored.bboxY, r.stored.bboxWidth, r.stored.bboxHeight);
        cv::Point2d centre(r.stored.centroidX, r.stored.centroidY);
        MIB_REQUIRE(toImageSpace(s.metadata().objectCoordinates, box, centre), what + ": placed");
        const auto &m = r.reprocessed[0];
        MIB_EXPECT(std::abs(centre.x - m.centroidX) < 1e-6 && std::abs(centre.y - m.centroidY) < 1e-6 &&
                       box.x == m.bboxX && box.y == m.bboxY && box.width == m.bboxWidth && box.height == m.bboxHeight,
                   what + ": the stored record placed on the reprocessed one: stored (" + std::to_string(centre.x) +
                       ", " + std::to_string(centre.y) + ") vs (" + std::to_string(m.centroidX) + ", " +
                       std::to_string(m.centroidY) + ")");
        MIB_EXPECT(r.stored.area == m.area && r.stored.valid == m.valid, what + ": the same cell, measured alike");
    }

    void expectRefused(ProcessingService &kernel, const fs::path &path, const std::string &what)
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(path.string()), "open " + what);
        ReviewJobs jobs(s, &kernel, [](const ReviewJobEvent &) {});
        ReprocessResult r;
        std::string err;
        MIB_EXPECT(!jobs.reprocessRecord(true, 0, ReprocessSettings::Recorded, r, &err) &&
                       err == "Record 0 cannot be matched: the file does not record whether its bounding boxes are "
                              "relative to its ROI (which starts at 40, 24) or to the image, and its masks do not tell" &&
                       r.reprocessed.empty() && !r.match,
                   what + ": the match is refused with the reason: " + err);
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(180);
    mib::test::TempDir td("review_object_coordinates");
    ProcessingService kernel;

    // ---- the helpers -------------------------------------------------------------
    {
        MIB_EXPECT(std::string(toString(ObjectSpace::RoiLocal)) == "roi_local" &&
                       std::string(toString(ObjectSpace::Image)) == "image" &&
                       std::string(toString(ObjectSpace::Unknown)) == "unknown",
                   "values as the contract names them");
        MIB_EXPECT(std::string(toString(ObjectSpaceSource::Attribute)) == "attribute" &&
                       std::string(toString(ObjectSpaceSource::Snapshot)) == "snapshot" &&
                       std::string(toString(ObjectSpaceSource::MaskCheck)) == "mask_check" &&
                       std::string(toString(ObjectSpaceSource::RoiOrigin)) == "roi_origin" &&
                       std::string(toString(ObjectSpaceSource::None)).empty(),
                   "sources as the contract names them");
        MIB_EXPECT(parseObjectSpace("roi_local") == ObjectSpace::RoiLocal && parseObjectSpace("image") == ObjectSpace::Image &&
                       !parseObjectSpace("unknown") && !parseObjectSpace("ROI_LOCAL") && !parseObjectSpace(""),
                   "only the two written values parse");

        cv::Rect2d box(4, 6, 20, 30);
        cv::Point2d centre(14, 21);
        MIB_EXPECT(!toImageSpace(ObjectCoordinates{}, box, centre) && box == cv::Rect2d(4, 6, 20, 30) &&
                       centre == cv::Point2d(14, 21),
                   "unknown: refused, nothing changed");
        MIB_EXPECT(toImageSpace({ObjectSpace::RoiLocal, ObjectSpaceSource::Attribute, 40, 24}, box, centre) &&
                       box == cv::Rect2d(44, 30, 20, 30) && centre == cv::Point2d(54, 45),
                   "roi_local: shifted by the origin");

        MIB_EXPECT(maskCheckSample(0).empty(), "no candidates, no sample");
        MIB_EXPECT(maskCheckSample(5) == std::vector<std::size_t>({0, 1, 2, 3, 4}), "few: all of them");
        const auto spread = maskCheckSample(1000);
        MIB_EXPECT(spread.size() == kObjectSpaceMaskCheckMaxRecords && spread.front() == 0 && spread.back() > 900 &&
                       spread[1] > spread[0] + 10,
                   "many: 64, spread over the file");

        cv::Mat mask = cv::Mat::zeros(kH, kW, CV_8UC1);
        cv::rectangle(mask, cv::Rect(100, 50, 20, 20), cv::Scalar(255), cv::FILLED);
        MIB_EXPECT(maskPlacementVote(mask, cv::Rect2d(100, 50, 20, 20), 40, 24) == ObjectSpace::Image,
                   "the box as stored lands on the mask: image");
        MIB_EXPECT(maskPlacementVote(mask, cv::Rect2d(60, 26, 20, 20), 40, 24) == ObjectSpace::RoiLocal,
                   "the box shifted by the origin lands on the mask: roi_local");
        MIB_EXPECT(!maskPlacementVote(mask, cv::Rect2d(0, 0, 10, 10), 40, 24), "neither lands: not decisive");
        MIB_EXPECT(!maskPlacementVote(mask, cv::Rect2d(100, 50, 20, 20), 0, 0), "origin (0, 0): not decisive");
        MIB_EXPECT(!maskPlacementVote(mask, cv::Rect2d(100, 50, 20, 20), 4, 0), "both land about alike: not decisive");
        MIB_EXPECT(!maskPlacementVote(cv::Mat(), cv::Rect2d(100, 50, 20, 20), 40, 24), "no mask: not decisive");

        using Votes = std::vector<std::optional<ObjectSpace>>;
        const Votes eightLocal(8, ObjectSpace::RoiLocal);
        MIB_EXPECT(maskCheckVerdict(eightLocal) == ObjectSpace::RoiLocal, "8 decisive, all agree");
        MIB_EXPECT(!maskCheckVerdict(Votes(7, ObjectSpace::RoiLocal)), "7 decisive: too few");
        Votes withSilence = Votes(7, ObjectSpace::Image);
        withSilence.resize(20, std::nullopt);
        MIB_EXPECT(!maskCheckVerdict(withSilence), "indecisive records do not count");
        Votes nine(9, ObjectSpace::Image);
        nine.emplace_back(ObjectSpace::RoiLocal);
        MIB_EXPECT(maskCheckVerdict(nine) == ObjectSpace::Image, "90 % agreement decides");
        Votes eight(8, ObjectSpace::Image);
        eight.insert(eight.end(), 2, ObjectSpace::RoiLocal);
        MIB_EXPECT(!maskCheckVerdict(eight), "80 % agreement does not");
    }

    // ---- capture-style files: ROI-local rows ---------------------------------------------
    const fs::path captureAttr = td.path() / "capture_attribute.h5";
    const fs::path captureSnap = td.path() / "capture_snapshot.h5";
    const fs::path captureLegacy = td.path() / "capture_legacy.h5";
    write(captureAttr, {captures(kernel, 10), kRoi, std::string(Hdf5Service::kObjectCoordinatesRoiLocal), ""});
    write(captureSnap, {captures(kernel, 10), kRoi, std::nullopt, snapshotV3("inline")});
    write(captureLegacy, {captures(kernel, 10), kRoi, std::nullopt, ""});
    {
        const auto attr = spaceOf(captureAttr);
        MIB_EXPECT(is(attr, ObjectSpace::RoiLocal, ObjectSpaceSource::Attribute), "attribute: " + describe(attr));
        const auto snap = spaceOf(captureSnap);
        MIB_EXPECT(is(snap, ObjectSpace::RoiLocal, ObjectSpaceSource::Snapshot), "v3 inline snapshot: " + describe(snap));
        const auto legacy = spaceOf(captureLegacy);
        MIB_EXPECT(is(legacy, ObjectSpace::RoiLocal, ObjectSpaceSource::MaskCheck), "legacy capture: " + describe(legacy));
        expectMatched(kernel, captureAttr, "capture (attribute)");
        expectMatched(kernel, captureSnap, "capture (snapshot)");
        expectMatched(kernel, captureLegacy, "capture (mask check)");
        // The stored record is as stored: ROI-local, the cell at (105, 46) is (65, 22) in the ROI.
        ReviewSession s;
        MIB_REQUIRE(s.open(captureAttr.string()), "open capture");
        std::vector<MetricRow> page;
        uint64_t total = 0;
        MIB_REQUIRE(s.metricsPage(true, 3, 1, page, total), "page");
        MIB_EXPECT(std::abs(page[0].centroidX - 65.0) < 1.0 && std::abs(page[0].centroidY - 22.0) < 1.0,
                   "the fixture's rows are ROI-local: (" + std::to_string(page[0].centroidX) + ", " +
                       std::to_string(page[0].centroidY) + ")");
    }

    // ---- regenerate-style files: image rows ----------------------------------------------
    const fs::path regenWritten = td.path() / "regenerated.h5";
    const fs::path regenLegacy = td.path() / "regen_legacy.h5";
    const fs::path asyncBatch = td.path() / "async_batch.h5";
    {
        // saveMasksToHdf5 (regenerate, reanalysis, the Qt batch dialog) records image coordinates.
        auto frames = regenerated(kernel, 10);
        MIB_REQUIRE(backend::services::batch_masks::saveMasksToHdf5(frames, regenWritten.string(), permissive(), kRoi.x,
                                                                    kRoi.y, kRoi.w, kRoi.h, black(), true, nullptr),
                    "saveMasksToHdf5");
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(regenWritten.string()), "reload regenerated");
        MIB_EXPECT(reader.readScalarStringAttribute("/experiment_info", Hdf5Service::kObjectCoordinatesAttribute) ==
                       std::optional<std::string>("image"),
                   "saveMasksToHdf5 writes object_coordinates = image");
        reader.closeFile();
    }
    write(regenLegacy, {regenerated(kernel, 10), kRoi, std::nullopt, ""});
    write(asyncBatch, {regenerated(kernel, 10), kRoi, std::nullopt, snapshotV3("async_batch")});
    {
        const auto written = spaceOf(regenWritten);
        MIB_EXPECT(is(written, ObjectSpace::Image, ObjectSpaceSource::Attribute), "regenerated: " + describe(written));
        const auto legacy = spaceOf(regenLegacy);
        MIB_EXPECT(is(legacy, ObjectSpace::Image, ObjectSpaceSource::MaskCheck), "legacy regenerate: " + describe(legacy));
        const auto batch = spaceOf(asyncBatch);
        MIB_EXPECT(is(batch, ObjectSpace::Image, ObjectSpaceSource::Snapshot), "v3 async batch snapshot: " + describe(batch));
        expectMatched(kernel, regenWritten, "regenerated (attribute)");
        expectMatched(kernel, regenLegacy, "regenerate (mask check)");
        expectMatched(kernel, asyncBatch, "async batch (snapshot)");
    }

    // ---- an ROI at the origin --------------------------------------------------------------
    {
        const fs::path origin = td.path() / "origin.h5";
        auto frames = regenerated(kernel, 4);
        write(origin, {frames, ProcessingService::Roi{0, 0, 180, 110}, std::nullopt, ""});
        const auto c = spaceOf(origin);
        MIB_EXPECT(is(c, ObjectSpace::Image, ObjectSpaceSource::RoiOrigin), "origin (0, 0): " + describe(c));
        const fs::path noRoi = td.path() / "no_roi.h5";
        write(noRoi, {frames, ProcessingService::Roi{40, 24, 0, 0}, std::nullopt, ""});
        const auto n = spaceOf(noRoi);
        MIB_EXPECT(is(n, ObjectSpace::Image, ObjectSpaceSource::RoiOrigin), "an ROI without area: " + describe(n));
    }

    // ---- ambiguous files: unknown, the match refused ------------------------------------------
    {
        struct Case
        {
            const char *name;
            Spec spec;
        };
        auto mixed = captures(kernel, 7);
        for (auto &f : regenerated(kernel, 3)) mixed.push_back(f);
        std::vector<Case> cases;
        cases.push_back({"blank masks", {captures(kernel, 10), kRoi, std::nullopt, "", true}});
        cases.push_back({"5 decisive records", {captures(kernel, 5), kRoi, std::nullopt, ""}});
        cases.push_back({"70 % agreement", {mixed, kRoi, std::nullopt, ""}});
        cases.push_back({"a PL-science snapshot, blank masks", {captures(kernel, 10), kRoi, std::nullopt, snapshotV3("inline", "pl"), true}});
        cases.push_back({"an unrecognised attribute, blank masks", {captures(kernel, 10), kRoi, std::string("pixels"), "", true}});
        cases.push_back({"a snapshot of another realtime mode, blank masks",
                         {captures(kernel, 10), kRoi, std::nullopt, snapshotV3("kin6"), true}});
        for (auto &c : cases)
        {
            const fs::path path = td.path() / (std::string("ambiguous_") + std::to_string(&c - cases.data()) + ".h5");
            write(path, c.spec);
            const auto space = spaceOf(path);
            MIB_EXPECT(space.space == ObjectSpace::Unknown && space.source == ObjectSpaceSource::None &&
                           space.originX == 0 && space.originY == 0,
                       std::string(c.name) + ": unknown, got " + describe(space));
            expectRefused(kernel, path, c.name);
        }
        // With its masks, the PL snapshot's file is decided by the mask check, not the snapshot.
        const fs::path plMasks = td.path() / "pl_masks.h5";
        write(plMasks, {captures(kernel, 10), kRoi, std::nullopt, snapshotV3("async_batch", "pl")});
        const auto pl = spaceOf(plMasks);
        MIB_EXPECT(is(pl, ObjectSpace::RoiLocal, ObjectSpaceSource::MaskCheck), "a PL snapshot does not decide: " + describe(pl));
    }

    // ---- a raw recording, no file -------------------------------------------------------------
    {
        ReviewSession s;
        const auto closed = s.metadata().objectCoordinates;
        MIB_EXPECT(closed.space == ObjectSpace::Unknown && closed.source == ObjectSpaceSource::None, "no file: unknown");
        const fs::path recording = td.path() / "rec.h5";
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(recording.string()) && hdf5.initializeRecordingDatasets(), "open recording");
        std::vector<cv::Mat> images{ringAt(60, 46)};
        std::vector<Hdf5Service::RecordingFrameMeta> meta{{0, 0, kW, kH}};
        MIB_REQUIRE(hdf5.appendRecordingFrames(images, meta) && hdf5.writeRecordingInfo(10, 20, 1, 0), "recording");
        hdf5.closeFile();
        const auto rec = spaceOf(recording);
        MIB_EXPECT(rec.space == ObjectSpace::Unknown && rec.source == ObjectSpaceSource::None,
                   "a raw recording stores no objects: unknown");
    }

    return mib::test::exitCode();
}
