// The coordinate space of a file's stored bounding boxes and centroids
// (coordinator ruling on #756 / #757, round 2; review ABI 3).
//
// The realtime capture path with an ROI stores ROI-local boxes and centroids
// next to full-frame images and masks (the mask is pasted at the ROI); the
// regenerate / reanalysis writer (`saveMasksToHdf5`) and the whole-image
// capture path store image coordinates. Readers convert through
// `toImageSpace` only, with the space decided once per file at open:
//   1. `/experiment_info @object_coordinates` ("roi_local" | "image"),
//      written from #756 on;
//   2. a run snapshot v3 of a host-science run: an ROI with w, h > 0 under
//      the inline realtime path means roi_local; the async batch path
//      (computeProcessedFrame) or no ROI image; any other realtime mode does
//      not decide;
//   3. an ROI without area, or at origin (0, 0): both placements are the
//      same, so image (the mask check below can never decide such a file);
//   4. a bounded mask check by object identity: for up to
//      kObjectSpaceMaskCheckMaxRecords records with masks (valid records
//      first, spread over the set; invalid ones only to fill the sample),
//      which placement of the stored box lands on the stored object: a mask
//      component whose bounding box matches the placed box (IoU of at least
//      kObjectSpaceMaskCheckMinBoxIouPercent %) and, where the record has an
//      area, whose measured contour's convex-hull area matches it within
//      kObjectSpaceMaskCheckAreaTolerancePercent %: a hole of the component
//      for a Contract 1 inner-contour object (the science measures the hole
//      and boxes its outline; such an object needs a hole), its outline
//      otherwise. A record decides only
//      when exactly one placement fits; foreground at the other placement
//      (debris, another cell) is not the object. The file is decided only
//      with at least kObjectSpaceMaskCheckMinDecisive decisive records
//      agreeing at kObjectSpaceMaskCheckMinAgreementPercent or more;
//   5. otherwise unknown: whatever needs the placement refuses. Never a
//      guess.
#pragma once

#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace backend::review
{

    // Review contract `review_object_coordinates.values` (strings on the wire).
    enum class ObjectSpace
    {
        Unknown,
        Image,
        RoiLocal,
    };

    // Review contract `review_object_coordinates.sources`; None with Unknown.
    enum class ObjectSpaceSource
    {
        None,
        Attribute,
        Snapshot,
        MaskCheck,
        RoiOrigin,
    };

    // Review contract `review_object_coordinates.mask_check`.
    inline constexpr std::size_t kObjectSpaceMaskCheckMaxRecords = 64;
    inline constexpr std::size_t kObjectSpaceMaskCheckMinDecisive = 8;
    inline constexpr int kObjectSpaceMaskCheckMinAgreementPercent = 90;
    inline constexpr int kObjectSpaceMaskCheckMinBoxIouPercent = 80;
    inline constexpr int kObjectSpaceMaskCheckAreaTolerancePercent = 15;

    const char *toString(ObjectSpace space);       // "roi_local" | "image" | "unknown"
    const char *toString(ObjectSpaceSource source); // "attribute" | ... | "" for None

    // A recorded `object_coordinates` value; nullopt for anything else.
    std::optional<ObjectSpace> parseObjectSpace(std::string_view value);

    // The space of one open file and the origin it implies: the offset from
    // a stored box or centroid to image coordinates (the recorded ROI origin
    // for RoiLocal, (0, 0) otherwise, including Unknown).
    struct ObjectCoordinates
    {
        ObjectSpace space{ObjectSpace::Unknown};
        ObjectSpaceSource source{ObjectSpaceSource::None};
        int originX{0};
        int originY{0};
    };

    // The one conversion. A stored box / centroid to image coordinates;
    // false (and nothing changed) when the space is unknown.
    bool toImageSpace(const ObjectCoordinates &coordinates, cv::Rect2d &bbox, cv::Point2d &centroid);

    // What a file records about its space, read at open.
    struct ObjectSpaceEvidence
    {
        std::optional<std::string> attribute;               // /experiment_info @object_coordinates
        std::uint32_t snapshotVersion{0};                  // 0: no readable run snapshot
        std::optional<services::ProcessingService::Roi> snapshotRoi; // its `roi`, when it has one
        bool snapshotPlScience{false};                     // `science_placement` "pl": PL rows
        std::string snapshotRealtimeMode;                  // `realtime_mode`: "inline" | "async_batch"
        services::ProcessingService::Roi recordedRoi{};    // /experiment_info ROI
    };

    // A stored record's object as the mask check identifies it.
    struct StoredObject
    {
        cv::Rect2d box;      // as stored (the outline's bounding box)
        double areaPx{0.0};  // the stored hull area in px²; 0 or NaN: not checked
        bool hole{false};    // a Contract 1 inner-contour object: the area is a hole's
    };

    // Whether the stored object sits at `placedBox` in `mask` (8-connected
    // components of its non-zero pixels): some component overlapping the box
    // has a bounding box with IoU >= kObjectSpaceMaskCheckMinBoxIouPercent %,
    // a hole when `object.hole`, and, when the record has an area, the
    // convex-hull area of a hole (`hole`) or of its outline within
    // kObjectSpaceMaskCheckAreaTolerancePercent % of it.
    bool storedObjectAt(const cv::Mat &mask, const cv::Rect2d &placedBox, const StoredObject &object);

    // One record's vote in the mask check: the placement of its stored box
    // (as stored, or shifted by the origin) where the stored object is
    // (storedObjectAt). Decisive only when exactly one placement fits;
    // nullopt when both, neither, or the origin is (0, 0).
    std::optional<ObjectSpace> maskPlacementVote(const cv::Mat &mask, const StoredObject &object, int originX,
                                                 int originY);

    // The mask check's verdict over its votes (nullopt: an indecisive record):
    // a space only with enough decisive records agreeing.
    std::optional<ObjectSpace> maskCheckVerdict(const std::vector<std::optional<ObjectSpace>> &votes);

    // Up to `limit` of `candidates` positions, spread evenly over them, in
    // order.
    std::vector<std::size_t> maskCheckSample(std::size_t candidates,
                                             std::size_t limit = kObjectSpaceMaskCheckMaxRecords);

    // Steps 1–3 and 5 of the order above; `maskCheck` runs step 4 (it is
    // called only when nothing before it decides). Returns the file's space.
    template <typename MaskCheck>
    ObjectCoordinates resolveObjectCoordinates(const ObjectSpaceEvidence &evidence, MaskCheck &&maskCheck);

    ObjectCoordinates objectCoordinatesFor(ObjectSpace space, ObjectSpaceSource source,
                                           const services::ProcessingService::Roi &recordedRoi);

    template <typename MaskCheck>
    ObjectCoordinates resolveObjectCoordinates(const ObjectSpaceEvidence &evidence, MaskCheck &&maskCheck)
    {
        const auto &roi = evidence.recordedRoi;
        if (evidence.attribute)
        {
            if (const auto space = parseObjectSpace(*evidence.attribute))
                return objectCoordinatesFor(*space, ObjectSpaceSource::Attribute, roi);
        }
        // A v3 snapshot names a host-science capture (ExperimentCoordinator).
        // Its inline realtime path validates the ROI's mask (ROI-local rows);
        // its async batch path validates the full-frame mask (image rows). A
        // PL run's rows come from the PL, which this rule says nothing about.
        if (evidence.snapshotVersion >= 3 && evidence.snapshotRoi && !evidence.snapshotPlScience)
        {
            const bool withRoi = evidence.snapshotRoi->w > 0 && evidence.snapshotRoi->h > 0;
            if (!withRoi || evidence.snapshotRealtimeMode == "async_batch")
                return objectCoordinatesFor(ObjectSpace::Image, ObjectSpaceSource::Snapshot, roi);
            if (evidence.snapshotRealtimeMode == "inline")
                return objectCoordinatesFor(ObjectSpace::RoiLocal, ObjectSpaceSource::Snapshot, roi);
        }
        if (roi.w <= 0 || roi.h <= 0 || (roi.x == 0 && roi.y == 0))
            return objectCoordinatesFor(ObjectSpace::Image, ObjectSpaceSource::RoiOrigin, roi);
        if (const std::optional<ObjectSpace> checked = maskCheck())
            return objectCoordinatesFor(*checked, ObjectSpaceSource::MaskCheck, roi);
        return ObjectCoordinates{};
    }

} // namespace backend::review
