#pragma once

// Converts between backend::services::ProcessingConfig and a flat Python
// dict using the exact C++ struct field names (see
// include/backend/processing/ProcessingService.h and
// docs/gold_standard_metrics.md "Portable Processing Contract" -> "
// ProcessingConfig contract"). This dict is FLAT: it does not replicate the
// nested image_processing/filters/target_group/multi_image grouping used by
// the desktop app's resources/defaults/config.json. A caller starting from
// that nested JSON (e.g. Biowork's config sync client) must flatten it
// before calling process_batch/compute_processed_frame.

#include <pybind11/pybind11.h>

#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingService.h"

#include <stdexcept>
#include <string>

namespace mib_processing_bindings {

namespace py = pybind11;
using backend::services::ProcessingConfig;

template <class T>
inline T dictGet(const py::dict& d, const char* key, T fallback) {
    if (d.contains(key)) {
        return py::cast<T>(d[key]);
    }
    return fallback;
}

inline ProcessingConfig configFromDict(const py::dict& d) {
    ProcessingConfig c;  // start from struct defaults; dict only overrides what it sets
    // Contract selection (ADR 0006). Fail closed on anything but 1 or 2.
    c.processing_contract_version =
        dictGet(d, "processing_contract_version", c.processing_contract_version);
    if (!backend::processing::contract::isSupportedProcessingContract(c.processing_contract_version)) {
        throw std::invalid_argument("unsupported processing_contract_version " +
                                    std::to_string(c.processing_contract_version) +
                                    " (supported: 1, 2)");
    }
    c.gaussian_blur_size = dictGet(d, "gaussian_blur_size", c.gaussian_blur_size);
    // v2 canonical `difference_threshold` wins over the legacy v1 key.
    c.bg_subtract_threshold = dictGet(d, "bg_subtract_threshold", c.bg_subtract_threshold);
    c.bg_subtract_threshold = dictGet(d, "difference_threshold", c.bg_subtract_threshold);
    c.morph_kernel_size = dictGet(d, "morph_kernel_size", c.morph_kernel_size);
    c.morph_iterations = dictGet(d, "morph_iterations", c.morph_iterations);
    c.area_threshold_min = dictGet(d, "area_threshold_min", c.area_threshold_min);
    c.area_threshold_max = dictGet(d, "area_threshold_max", c.area_threshold_max);
    c.deformability_threshold_min = dictGet(d, "deformability_threshold_min", c.deformability_threshold_min);
    c.deformability_threshold_max = dictGet(d, "deformability_threshold_max", c.deformability_threshold_max);
    c.enable_border_check = dictGet(d, "enable_border_check", c.enable_border_check);
    c.enable_area_range_check = dictGet(d, "enable_area_range_check", c.enable_area_range_check);
    c.enable_deformability_range_check = dictGet(d, "enable_deformability_range_check", c.enable_deformability_range_check);
    c.area_ratio_threshold_max = dictGet(d, "area_ratio_threshold_max", c.area_ratio_threshold_max);
    c.enable_area_ratio_check = dictGet(d, "enable_area_ratio_check", c.enable_area_ratio_check);
    c.ring_ratio_min = dictGet(d, "ring_ratio_min", c.ring_ratio_min);
    c.ring_ratio_max = dictGet(d, "ring_ratio_max", c.ring_ratio_max);
    c.enable_ring_ratio_check = dictGet(d, "enable_ring_ratio_check", c.enable_ring_ratio_check);
    c.laplacian_variance_min = dictGet(d, "laplacian_variance_min", c.laplacian_variance_min);
    c.laplacian_variance_max = dictGet(d, "laplacian_variance_max", c.laplacian_variance_max);
    c.enable_laplacian_variance_check =
        dictGet(d, "enable_laplacian_variance_check", c.enable_laplacian_variance_check);
    c.require_single_inner_contour = dictGet(d, "require_single_inner_contour", c.require_single_inner_contour);
    c.empty_frame_pixel_threshold = dictGet(d, "empty_frame_pixel_threshold", c.empty_frame_pixel_threshold);
    c.auto_background_enabled = dictGet(d, "auto_background_enabled", c.auto_background_enabled);
    c.auto_background_empty_frames = dictGet(d, "auto_background_empty_frames", c.auto_background_empty_frames);
    c.auto_background_cooldown_frames = dictGet(d, "auto_background_cooldown_frames", c.auto_background_cooldown_frames);
    c.channel_band_y = dictGet(d, "channel_band_y", c.channel_band_y);
    c.channel_band_h = dictGet(d, "channel_band_h", c.channel_band_h);
    c.enable_target_group = dictGet(d, "enable_target_group", c.enable_target_group);
    c.target_group_area_min = dictGet(d, "target_group_area_min", c.target_group_area_min);
    c.target_group_area_max = dictGet(d, "target_group_area_max", c.target_group_area_max);
    c.target_group_deformability_min = dictGet(d, "target_group_deformability_min", c.target_group_deformability_min);
    c.target_group_deformability_max = dictGet(d, "target_group_deformability_max", c.target_group_deformability_max);
    c.enable_target_group_emodulus = dictGet(d, "enable_target_group_emodulus", c.enable_target_group_emodulus);
    c.target_group_emodulus_min = dictGet(d, "target_group_emodulus_min", c.target_group_emodulus_min);
    c.target_group_emodulus_max = dictGet(d, "target_group_emodulus_max", c.target_group_emodulus_max);
    c.multi_image_enabled = dictGet(d, "multi_image_enabled", c.multi_image_enabled);
    c.multi_image_count = dictGet(d, "multi_image_count", c.multi_image_count);
    return c;
}

inline py::dict configToDict(const ProcessingConfig& c) {
    py::dict d;
    d["gaussian_blur_size"] = c.gaussian_blur_size;
    d["processing_contract_version"] = c.processing_contract_version;
    if (backend::processing::contract::contractHasRingWidth(c.processing_contract_version)) {
        d["bg_subtract_threshold"] = c.bg_subtract_threshold;
    } else {
        d["difference_threshold"] = c.bg_subtract_threshold;
    }
    d["morph_kernel_size"] = c.morph_kernel_size;
    d["morph_iterations"] = c.morph_iterations;
    d["area_threshold_min"] = c.area_threshold_min;
    d["area_threshold_max"] = c.area_threshold_max;
    d["deformability_threshold_min"] = c.deformability_threshold_min;
    d["deformability_threshold_max"] = c.deformability_threshold_max;
    d["enable_border_check"] = c.enable_border_check;
    d["enable_area_range_check"] = c.enable_area_range_check;
    d["enable_deformability_range_check"] = c.enable_deformability_range_check;
    d["area_ratio_threshold_max"] = c.area_ratio_threshold_max;
    d["enable_area_ratio_check"] = c.enable_area_ratio_check;
    d["ring_ratio_min"] = c.ring_ratio_min;
    d["ring_ratio_max"] = c.ring_ratio_max;
    d["enable_ring_ratio_check"] = c.enable_ring_ratio_check;
    d["laplacian_variance_min"] = c.laplacian_variance_min;
    d["laplacian_variance_max"] = c.laplacian_variance_max;
    d["enable_laplacian_variance_check"] = c.enable_laplacian_variance_check;
    d["require_single_inner_contour"] = c.require_single_inner_contour;
    d["empty_frame_pixel_threshold"] = c.empty_frame_pixel_threshold;
    d["auto_background_enabled"] = c.auto_background_enabled;
    d["auto_background_empty_frames"] = c.auto_background_empty_frames;
    d["auto_background_cooldown_frames"] = c.auto_background_cooldown_frames;
    d["channel_band_y"] = c.channel_band_y;
    d["channel_band_h"] = c.channel_band_h;
    d["enable_target_group"] = c.enable_target_group;
    d["target_group_area_min"] = c.target_group_area_min;
    d["target_group_area_max"] = c.target_group_area_max;
    d["target_group_deformability_min"] = c.target_group_deformability_min;
    d["target_group_deformability_max"] = c.target_group_deformability_max;
    d["enable_target_group_emodulus"] = c.enable_target_group_emodulus;
    d["target_group_emodulus_min"] = c.target_group_emodulus_min;
    d["target_group_emodulus_max"] = c.target_group_emodulus_max;
    d["multi_image_enabled"] = c.multi_image_enabled;
    d["multi_image_count"] = c.multi_image_count;
    return d;
}

}  // namespace mib_processing_bindings
