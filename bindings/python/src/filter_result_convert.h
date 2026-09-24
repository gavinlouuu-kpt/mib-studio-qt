#pragma once

// Converts a backend::services::ProcessedFrame (one detected object
// candidate) to a dict matching docs/gold_standard_metrics.schema.json --
// the same field names process_batch's caller can feed straight into the A5
// conformance harness (compare_metrics.py) or the A2 schema validator,
// mirroring scripts/export_hdf5.py's _frame_to_gold_standard_dict.

#include <cmath>
#include <limits>

#include <pybind11/pybind11.h>

#include "backend/processing/ProcessingService.h"
#include "config_convert.h"

namespace mib_processing_bindings {

namespace py = pybind11;
using backend::services::FilterResult;
using backend::services::ProcessedFrame;

inline py::dict processedFrameToDict(const ProcessedFrame& frame, double pixelToMicron) {
    const auto& v = frame.validation;
    py::dict d;
    d["frame_type"] = v.isValid ? "valid" : "invalid";
    d["index"] = frame.index;
    d["timestamp_ns"] = frame.timestampNs;
    d["object_id"] = v.objectId;
    d["object_count"] = v.objectCount;
    d["track_id"] = v.trackId;
    d["track_first_frame"] = v.trackFirstFrame;
    d["track_last_frame"] = v.trackLastFrame;
    d["track_observation_count"] = v.trackObservationCount;
    d["deformability"] = v.deformability;
    d["area"] = v.area;
    d["area_um2"] = v.area * pixelToMicron * pixelToMicron;
    d["area_ratio"] = v.areaRatio;
    // Contract 1 focus metric (ring width). NaN under Contract 2 -> omitted.
    if (!std::isnan(v.ringRatio)) {
        d["ring_ratio"] = v.ringRatio;
    }
    // Contract 2 focus metric (per-object Laplacian variance); NaN when not
    // computed (no detection) so readers can tell "unusable" from a value.
    d["laplacian_variance"] = v.laplacianVariance;
    if (!std::isnan(v.youngsModulus)) {
        d["youngs_modulus"] = v.youngsModulus;
    }
    d["is_valid"] = v.isValid;
    d["touches_border"] = v.touchesBorder;
    d["has_single_inner_contour"] = v.hasSingleInnerContour;
    d["in_range"] = v.inRange;
    d["is_target_group"] = v.isTargetGroup;
    d["inner_contour_count"] = v.innerContourCount;
    d["brightness_q1"] = v.brightness.q1;
    d["brightness_q2"] = v.brightness.q2;
    d["brightness_q3"] = v.brightness.q3;
    d["brightness_q4"] = v.brightness.q4;
    return d;
}

// Reverse of processedFrameToDict, used by save_masks_to_hdf5: reconstructs
// a ProcessedFrame's metadata (not its image) from a gold-standard-shaped
// dict, e.g. one previously returned by process_batch(...). Fields not part
// of the gold-standard contract (bbox/centroid/track/allContours) are left
// at their FilterResult defaults -- callers needing those should keep the
// image around and call compute_processed_frame directly instead of
// round-tripping through this dict shape.
inline ProcessedFrame processedFrameFromDict(const py::dict& d) {
    ProcessedFrame f;
    f.index = dictGet<uint64_t>(d, "index", 0);
    f.timestampNs = dictGet<uint64_t>(d, "timestamp_ns", 0);

    FilterResult& v = f.validation;
    v.isValid = dictGet(d, "is_valid", false);
    v.touchesBorder = dictGet(d, "touches_border", false);
    v.hasSingleInnerContour = dictGet(d, "has_single_inner_contour", false);
    v.inRange = dictGet(d, "in_range", false);
    v.innerContourCount = dictGet(d, "inner_contour_count", 0);
    v.objectId = dictGet(d, "object_id", -1);
    v.objectCount = dictGet(d, "object_count", 0);
    v.trackId = dictGet(d, "track_id", -1);
    v.trackFirstFrame = dictGet<uint64_t>(d, "track_first_frame", 0);
    v.trackLastFrame = dictGet<uint64_t>(d, "track_last_frame", 0);
    v.trackObservationCount = dictGet(d, "track_observation_count", 0);
    v.deformability = dictGet(d, "deformability", 0.0);
    v.area = dictGet(d, "area", 0.0);
    v.areaRatio = dictGet(d, "area_ratio", 0.0);
    v.ringRatio = dictGet(d, "ring_ratio", 0.0);
    v.laplacianVariance =
        dictGet(d, "laplacian_variance", std::numeric_limits<double>::quiet_NaN());
    v.isTargetGroup = dictGet(d, "is_target_group", false);
    v.youngsModulus = dictGet(d, "youngs_modulus", std::numeric_limits<double>::quiet_NaN());
    v.brightness.q1 = dictGet(d, "brightness_q1", 0.0);
    v.brightness.q2 = dictGet(d, "brightness_q2", 0.0);
    v.brightness.q3 = dictGet(d, "brightness_q3", 0.0);
    v.brightness.q4 = dictGet(d, "brightness_q4", 0.0);
    return f;
}

} // namespace mib_processing_bindings
