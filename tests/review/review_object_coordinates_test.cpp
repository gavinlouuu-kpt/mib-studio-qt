// review_object_coordinates_test (#756 review 2; review ABI 3)
//
// The coordinate space of a file's stored bounding boxes and centroids,
// decided once at open (ObjectCoordinates.h), and the one-record reprocess
// match placing the stored record through it.
//  - The helpers: values and sources as the contract names them, the one
//    conversion (refused for unknown), the mask check's sample, vote and
//    verdict (at least 8 decisive records agreeing at 90 % or more). The
//    vote is by object identity: a component whose box (IoU 0.8) and hull
//    area (15 %; a hole's for a Contract 1 object, the outline's otherwise)
//    match the record; debris at the other placement (Codex's thin outline
//    next to dense debris), debris beside or merged into the cell, and a
//    cell at both placements never decide wrongly.
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
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5 moved contour geometry out of imgproc.hpp
#endif
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

    // An object's box and hull areas as the science measures them: the box
    // of its outer contour, the hull area of the outline and of its hole.
    struct Measured
    {
        cv::Rect2d box;
        double outerArea{0.0};
        double holeArea{0.0};
    };
    Measured measure(const cv::Mat &objectOnly)
    {
        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours(objectOnly.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE);
        Measured m;
        for (std::size_t i = 0; i < contours.size(); ++i)
        {
            std::vector<cv::Point> hull;
            cv::convexHull(contours[i], hull);
            if (hierarchy[i][3] < 0)
            {
                m.box = cv::Rect2d(cv::boundingRect(contours[i]));
                m.outerArea = cv::contourArea(hull);
            }
            else
            {
                m.holeArea = cv::contourArea(hull);
            }
        }
        return m;
    }

    struct Spec
    {
        std::vector<ProcessedFrame> frames;
        ProcessingService::Roi roi{kRoi};
        std::optional<std::string> attribute;
        std::string snapshot;
        bool blankMasks{false};
        std::vector<ProcessedFrame> invalid;
    };

    void write(const fs::path &path, Spec spec)
    {
        if (spec.blankMasks)
            for (auto &f : spec.frames) f.processedImage = cv::Mat::zeros(kH, kW, CV_8UC1);
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()) && hdf5.initializeDatasets(), "open " + path.string());
        MIB_REQUIRE(hdf5.appendFrames(spec.frames, spec.invalid), "append " + path.string());
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, spec.frames.size(), spec.invalid.size(), permissive(), spec.roi,
                                             &black()),
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

        // The vote is by object identity (#756 review 2, round 2): the stored
        // object is a mask component whose box and hull area match the record.
        {
            // A ring cell measured as the science does: Contract 1 boxes its
            // outline and measures its hole (a hole object); outer-contour
            // objects measure the outline.
            cv::Mat mask = cv::Mat::zeros(kH, kW, CV_8UC1);
            cv::circle(mask, cv::Point(110, 60), 12, cv::Scalar(255), cv::FILLED);
            cv::circle(mask, cv::Point(110, 60), 5, cv::Scalar(0), cv::FILLED);
            const Measured ring = measure(mask);
            MIB_REQUIRE(ring.box.width > 0 && ring.holeArea > 0, "the ring is measured");
            const StoredObject inner{ring.box, ring.holeArea, true};
            const StoredObject outer{ring.box, ring.outerArea, false};
            const StoredObject boxOnly{ring.box, 0.0, false};
            MIB_EXPECT(storedObjectAt(mask, ring.box, inner) && storedObjectAt(mask, ring.box, outer) &&
                           storedObjectAt(mask, ring.box, boxOnly),
                       "the ring is the stored object by its box and its hole's or outline's hull area");
            MIB_EXPECT(!storedObjectAt(mask, ring.box, {ring.box, ring.outerArea * 2.0, false}) &&
                           !storedObjectAt(mask, ring.box, {ring.box, ring.outerArea, true}),
                       "another area, or the outline's area for a hole object: not the stored object");
            MIB_EXPECT(!storedObjectAt(mask, cv::Rect2d(ring.box.x + 6, ring.box.y, ring.box.width, ring.box.height), boxOnly) &&
                           !storedObjectAt(mask, cv::Rect2d(ring.box.x, ring.box.y, ring.box.width * 2, ring.box.height), boxOnly),
                       "a box that does not match the component's (IoU below 0.8): not the stored object");
            cv::Mat filled = cv::Mat::zeros(kH, kW, CV_8UC1);
            cv::circle(filled, cv::Point(110, 60), 12, cv::Scalar(255), cv::FILLED);
            MIB_EXPECT(!storedObjectAt(filled, ring.box, {ring.box, 0.0, true}), "a hole object needs a hole");

            const StoredObject local{cv::Rect2d(ring.box.x - 40, ring.box.y - 24, ring.box.width, ring.box.height),
                                     ring.holeArea, true};
            MIB_EXPECT(maskPlacementVote(mask, inner, 40, 24) == ObjectSpace::Image, "the object at the box as stored: image");
            MIB_EXPECT(maskPlacementVote(mask, local, 40, 24) == ObjectSpace::RoiLocal,
                       "the object at the box shifted by the origin: roi_local");
            MIB_EXPECT(!maskPlacementVote(mask, {cv::Rect2d(0, 0, 10, 10), 50.0, true}, 40, 24), "neither placement: not decisive");
            MIB_EXPECT(!maskPlacementVote(mask, inner, 0, 0), "origin (0, 0): not decisive");
            MIB_EXPECT(!maskPlacementVote(cv::Mat(), inner, 40, 24), "no mask: not decisive");

            // A record without an area: the box alone decides, and a blob over
            // the other placement is not the object (its box differs).
            cv::Mat blob = mask.clone();
            cv::rectangle(blob, cv::Rect(static_cast<int>(local.box.x) - 8, static_cast<int>(local.box.y) - 8, 40, 30),
                          cv::Scalar(255), cv::FILLED);
            MIB_EXPECT(maskPlacementVote(blob, boxOnly, 40, 24) == ObjectSpace::Image,
                       "no area: the box decides; a blob over the other placement does not fit");

            // Two cells, one at each placement of the stored box: both fit, nothing decided.
            cv::Mat two = mask.clone();
            cv::circle(two, cv::Point(150, 84), 12, cv::Scalar(255), cv::FILLED);
            cv::circle(two, cv::Point(150, 84), 5, cv::Scalar(0), cv::FILLED);
            MIB_EXPECT(!maskPlacementVote(two, inner, 40, 24) && !maskPlacementVote(two, outer, 40, 24),
                       "a real object at both placements: not decisive");
        }
        {
            // Codex's case: an ROI-local capture, ROI at (40, 24), stored box
            // (50, 30, 20, 20); the cell is a thin outline at its image
            // placement (90, 54), 76 mask pixels; dense rejected debris sits at
            // (50, 30), 400 pixels. A pixel count would say image.
            cv::Mat mask = cv::Mat::zeros(kH, kW, CV_8UC1);
            cv::rectangle(mask, cv::Rect(90, 54, 20, 20), cv::Scalar(255), 1);
            cv::Mat outlineOnly = mask.clone();
            cv::rectangle(mask, cv::Rect(50, 30, 20, 20), cv::Scalar(255), cv::FILLED);
            MIB_REQUIRE(cv::countNonZero(outlineOnly) == 76 && cv::countNonZero(mask) == 476, "the fixture's pixels");
            const Measured cell = measure(outlineOnly);
            const cv::Rect2d stored(50, 30, 20, 20);
            MIB_REQUIRE(cell.box == cv::Rect2d(90, 54, 20, 20) && cell.holeArea > 0, "the outline's box and hole");
            const StoredObject asHole{stored, cell.holeArea, true};
            const StoredObject asOutline{stored, cell.outerArea, false};
            MIB_EXPECT(maskPlacementVote(mask, asHole, 40, 24) == ObjectSpace::RoiLocal,
                       "a Contract 1 object (its hole measured): the outline is the stored object, the debris is not");
            MIB_EXPECT(maskPlacementVote(mask, asOutline, 40, 24) != ObjectSpace::Image,
                       "an outline object: the debris's outline looks the same, so never image");
            // The outline with debris beside its box (another component).
            cv::Mat adjacent = outlineOnly.clone();
            cv::rectangle(adjacent, cv::Rect(111, 58, 14, 12), cv::Scalar(255), cv::FILLED);
            cv::rectangle(adjacent, cv::Rect(50, 30, 20, 20), cv::Scalar(255), cv::FILLED);
            MIB_EXPECT(maskPlacementVote(adjacent, asHole, 40, 24) == ObjectSpace::RoiLocal,
                       "a thin outline with debris beside it: still the outline");
            // Debris merged into the outline: the component is no longer the stored object.
            cv::Mat merged = outlineOnly.clone();
            cv::rectangle(merged, cv::Rect(105, 58, 20, 12), cv::Scalar(255), cv::FILLED);
            cv::rectangle(merged, cv::Rect(50, 30, 20, 20), cv::Scalar(255), cv::FILLED);
            MIB_EXPECT(maskPlacementVote(merged, asHole, 40, 24) != ObjectSpace::Image,
                       "an outline merged with debris: never image");
        }

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
        // Debris and look-alikes (#756 review 2, round 2): records whose stored
        // object is not the foreground at the other placement.
        const auto synthetic = [](const cv::Mat &mask, const cv::Rect2d &box, double area, uint64_t index) {
            ProcessedFrame f;
            f.index = index;
            f.timestampNs = 1'000'000 + index * 1000;
            f.originalImage = cv::Mat(kH, kW, CV_8UC1, cv::Scalar(30));
            f.processedImage = mask.clone();
            f.validation.isValid = true;
            f.validation.objectCount = 1;
            f.validation.trackId = -1;
            f.validation.bboxX = box.x;
            f.validation.bboxY = box.y;
            f.validation.bboxWidth = box.width;
            f.validation.bboxHeight = box.height;
            f.validation.centroidX = box.x + box.width / 2;
            f.validation.centroidY = box.y + box.height / 2;
            f.validation.area = area;
            return f;
        };
        // Codex's legacy capture, ten records: thin outlines at the image
        // placement, dense debris at the stored-as-is placement.
        for (const bool hole : {true, false})
        {
            std::vector<ProcessedFrame> frames;
            for (int i = 0; i < 10; ++i)
            {
                const int x = 50 + 8 * (i % 4), y = 30 + 6 * (i % 3);
                cv::Mat outline = cv::Mat::zeros(kH, kW, CV_8UC1);
                cv::rectangle(outline, cv::Rect(x + 40, y + 24, 20, 20), cv::Scalar(255), 1);
                const Measured cell = measure(outline);
                cv::Mat mask = outline.clone();
                cv::rectangle(mask, cv::Rect(x, y, 20, 20), cv::Scalar(255), cv::FILLED);
                ProcessedFrame f = synthetic(mask, cv::Rect2d(x, y, 20, 20), hole ? cell.holeArea : cell.outerArea,
                                             static_cast<uint64_t>(200 + i));
                f.validation.innerContourCount = hole ? 1 : 0;
                frames.push_back(f);
            }
            const fs::path debris = td.path() / (hole ? "debris_hole.h5" : "debris_outline.h5");
            write(debris, {frames, kRoi, std::nullopt, ""});
            const auto c = spaceOf(debris);
            MIB_EXPECT(c.space != ObjectSpace::Image, "debris never decides image: " + describe(c));
            if (hole)
                MIB_EXPECT(is(c, ObjectSpace::RoiLocal, ObjectSpaceSource::MaskCheck),
                           "Contract 1 objects (holes measured): roi_local by the outlines, got " + describe(c));
            else
                MIB_EXPECT(c.space == ObjectSpace::Unknown, "outline objects like the debris: unknown, got " + describe(c));
        }
        {
            // Two cells per record, one at each placement: nothing decided.
            std::vector<ProcessedFrame> frames;
            for (int i = 0; i < 10; ++i)
            {
                const int x = 56 + 6 * (i % 4);
                cv::Mat mask = cv::Mat::zeros(kH, kW, CV_8UC1);
                for (const cv::Point centre : {cv::Point(x, 40), cv::Point(x + 40, 64)})
                {
                    cv::circle(mask, centre, 12, cv::Scalar(255), cv::FILLED);
                    cv::circle(mask, centre, 5, cv::Scalar(0), cv::FILLED);
                }
                cv::Mat first = cv::Mat::zeros(kH, kW, CV_8UC1);
                cv::circle(first, cv::Point(x, 40), 12, cv::Scalar(255), cv::FILLED);
                cv::circle(first, cv::Point(x, 40), 5, cv::Scalar(0), cv::FILLED);
                const Measured cell = measure(first);
                ProcessedFrame f = synthetic(mask, cell.box, cell.holeArea, static_cast<uint64_t>(300 + i));
                f.validation.innerContourCount = 2;
                frames.push_back(f);
            }
            const fs::path twin = td.path() / "two_cells.h5";
            write(twin, {frames, kRoi, std::nullopt, ""});
            const auto c = spaceOf(twin);
            MIB_EXPECT(c.space == ObjectSpace::Unknown, "a real object at both placements: unknown, got " + describe(c));
            expectRefused(kernel, twin, "two cells");
        }

        // The sample takes the valid set first: invalid records (rejected
        // objects, often debris) only fill what the valid set lacks. 64 valid
        // ROI-local records decide, whatever 20 image-like invalid ones say.
        {
            auto invalid = regenerated(kernel, 20);
            for (auto &f : invalid) f.validation.isValid = false;
            const fs::path validFirst = td.path() / "valid_first.h5";
            write(validFirst, {captures(kernel, 64), kRoi, std::nullopt, "", false, invalid});
            const auto c = spaceOf(validFirst);
            MIB_EXPECT(is(c, ObjectSpace::RoiLocal, ObjectSpaceSource::MaskCheck), "valid records first: " + describe(c));
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
