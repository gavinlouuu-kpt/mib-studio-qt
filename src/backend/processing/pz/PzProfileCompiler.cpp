#include "backend/processing/pz/PzProfileCompiler.h"

#include <cmath>
#include <map>

namespace backend::processing::pz {

const std::vector<PageField>& unetCellsV2PageFields() {
    // abi/profiles/unet_cells_v2.json "page", in its order (checked by
    // processing.pz_profile_compiler against the vendored profile).
    static const std::vector<PageField> fields = {
        {"enable_area_range_check", 0, 27, 27, "bool"},
        {"enable_deformability_range_check", 0, 28, 28, "bool"},
        {"enable_area_ratio_check", 0, 29, 29, "bool"},
        {"enable_laplacian_variance_check", 0, 30, 30, "bool"},
        {"target_group_enabled", 0, 31, 31, "bool"},
        {"channel_band_y", 1, 0, 15, "u16"},
        {"channel_band_h", 1, 16, 31, "u16"},
        {"min_cell_area_px", 2, 0, 15, "u16"},
        {"laplacian_ksize", 3, 16, 19, "u4"},
        {"target_group_emodulus_enabled", 3, 31, 31, "bool"},
        {"pixel_to_micron", 4, 0, 31, "q16_16"},
        {"area_min_um2", 5, 0, 31, "q16_16"},
        {"area_max_um2", 6, 0, 31, "q16_16"},
        {"deformability_min", 7, 0, 15, "q0_16"},
        {"deformability_max", 8, 0, 15, "q0_16"},
        {"area_ratio_max", 9, 0, 31, "q16_16"},
        {"laplacian_variance_min", 10, 0, 31, "q24_8"},
        {"laplacian_variance_max", 11, 0, 31, "q24_8"},
        {"target_area_min_um2", 12, 0, 31, "q16_16"},
        {"target_area_max_um2", 13, 0, 31, "q16_16"},
        {"target_deformability_min", 14, 0, 15, "q0_16"},
        {"target_deformability_max", 15, 0, 15, "q0_16"},
        {"target_emodulus_min_kpa", 16, 0, 31, "q16_16"},
        {"target_emodulus_max_kpa", 17, 0, 31, "q16_16"},
        {"store_invalid_every_n", 18, 0, 15, "u16"},
        {"store_multi_image_frames", 18, 16, 23, "u8"},
        {"store_valid_anchored", 18, 24, 24, "bool"},
        {"store_mask", 18, 25, 25, "bool"},
        {"store_all_frames", 18, 26, 26, "bool"},
        {"lut_area_min_um2", 19, 0, 31, "q16_16"},
        {"lut_area_step_um2", 20, 0, 31, "q16_16"},
        {"lut_deformability_min", 21, 0, 15, "q0_16"},
        {"lut_deformability_step", 21, 16, 31, "q0_16"},
    };
    return fields;
}

namespace {

class PageWriter {
public:
    explicit PageWriter(CompiledProfile& out) : out_(out) {
        for (const auto& f : unetCellsV2PageFields()) fields_[f.name] = &f;
    }
    // Store an already-encoded raw value; range-checked against the field width.
    void raw(const std::string& name, uint64_t value) {
        const PageField& f = *fields_.at(name);
        const int bits = f.hi - f.lo + 1;
        const uint64_t limit = bits == 32 ? 0xFFFFFFFFull : (1ull << bits) - 1;
        if (value > limit) {
            out_.errors.push_back(name + " out of range (" + std::to_string(value) + " > " + std::to_string(limit) +
                                  ")");
            return;
        }
        out_.page[static_cast<size_t>(f.word)] |= static_cast<uint32_t>(value) << f.lo;
    }
    void flag(const std::string& name, bool v) { raw(name, v ? 1 : 0); }
    // Unsigned fixed point with `frac` fraction bits.
    void fixed(const std::string& name, double v, int frac) {
        if (!std::isfinite(v) || v < 0) {
            out_.errors.push_back(name + " must be a finite value >= 0");
            return;
        }
        raw(name, static_cast<uint64_t>(std::llround(std::ldexp(v, frac))));
    }
    // Q0.16 deformability: 1.0 is held as 65535 (the PL's deformability never exceeds it).
    void q016(const std::string& name, double v) {
        if (!std::isfinite(v) || v < 0 || v > 1.0) {
            out_.errors.push_back(name + " must be in [0, 1]");
            return;
        }
        raw(name, std::min<uint64_t>(static_cast<uint64_t>(std::llround(v * 65536.0)), 65535));
    }

private:
    CompiledProfile& out_;
    std::map<std::string, const PageField*> fields_;
};

} // namespace

std::vector<std::string> plIgnoredSettingsChanged(const ProcessingConfig& c) {
    // What the PL's unet_cells_v2 does not implement: the host mask pipeline (blur, background subtraction,
    // morphology), the host object filters (border, ring ratio, single inner contour) and automatic
    // background/ROI. (Multi-image is a profile store flag, store_multi_image_frames, so it is not listed.) A setting still at its default says nothing; one the
    // operator changed would otherwise be accepted silently.
    const ProcessingConfig d;
    std::vector<std::string> names;
    const auto changed = [&](bool differs, const char* name) { if (differs) names.emplace_back(name); };
    changed(c.gaussian_blur_size != d.gaussian_blur_size, "gaussian_blur_size");
    changed(c.bg_subtract_threshold != d.bg_subtract_threshold, "bg_subtract_threshold");
    changed(c.morph_kernel_size != d.morph_kernel_size, "morph_kernel_size");
    changed(c.morph_iterations != d.morph_iterations, "morph_iterations");
    changed(c.enable_border_check != d.enable_border_check, "enable_border_check");
    changed(c.enable_ring_ratio_check != d.enable_ring_ratio_check, "enable_ring_ratio_check");
    changed(c.ring_ratio_min != d.ring_ratio_min, "ring_ratio_min");
    changed(c.ring_ratio_max != d.ring_ratio_max, "ring_ratio_max");
    changed(c.require_single_inner_contour != d.require_single_inner_contour, "require_single_inner_contour");
    changed(c.empty_frame_pixel_threshold != d.empty_frame_pixel_threshold, "empty_frame_pixel_threshold");
    changed(c.auto_background_enabled != d.auto_background_enabled, "auto_background_enabled");
    changed(c.auto_background_empty_frames != d.auto_background_empty_frames, "auto_background_empty_frames");
    changed(c.auto_background_cooldown_frames != d.auto_background_cooldown_frames, "auto_background_cooldown_frames");
    changed(c.auto_roi_from_background != d.auto_roi_from_background, "auto_roi_from_background");
    changed(c.auto_roi_wall_gradient_ratio != d.auto_roi_wall_gradient_ratio, "auto_roi_wall_gradient_ratio");
    changed(c.auto_roi_wall_margin != d.auto_roi_wall_margin, "auto_roi_wall_margin");
    return names;
}

CompiledProfile compileUnetCellsV2(const UnetCellsProfileInputs& in) {
    CompiledProfile out;
    PageWriter w(out);
    const auto& c = in.config;

    w.flag("enable_area_range_check", c.enable_area_range_check);
    w.flag("enable_deformability_range_check", c.enable_deformability_range_check);
    w.flag("enable_area_ratio_check", c.enable_area_ratio_check);
    w.flag("enable_laplacian_variance_check", c.enable_laplacian_variance_check);
    w.flag("target_group_enabled", c.enable_target_group);
    w.flag("target_group_emodulus_enabled", c.enable_target_group_emodulus);

    if (c.channel_band_h > 0) {
        if (c.channel_band_y < 0) out.errors.push_back("channel_band_y must be >= 0");
        else w.raw("channel_band_y", static_cast<uint64_t>(c.channel_band_y));
        w.raw("channel_band_h", static_cast<uint64_t>(c.channel_band_h));
    }
    if (c.min_cell_area_px < 0) out.errors.push_back("min_cell_area_px must be >= 0");
    else w.raw("min_cell_area_px", static_cast<uint64_t>(c.min_cell_area_px));
    if (c.laplacian_kernel_size != 1 && c.laplacian_kernel_size != 3) {
        out.errors.push_back("laplacian_kernel_size must be 1 or 3");
    } else {
        w.raw("laplacian_ksize", static_cast<uint64_t>(c.laplacian_kernel_size));
    }

    if (!(in.pixelToMicron > 0.0)) out.errors.push_back("pixel_to_micron must be > 0");
    else w.fixed("pixel_to_micron", in.pixelToMicron, 16);
    w.fixed("area_min_um2", c.area_threshold_min, 16);
    w.fixed("area_max_um2", c.area_threshold_max, 16);
    w.q016("deformability_min", c.deformability_threshold_min);
    w.q016("deformability_max", c.deformability_threshold_max);
    w.fixed("area_ratio_max", c.area_ratio_threshold_max, 16);
    w.fixed("laplacian_variance_min", c.laplacian_variance_min, 8);
    w.fixed("laplacian_variance_max", c.laplacian_variance_max, 8);
    w.fixed("target_area_min_um2", c.target_group_area_min, 16);
    w.fixed("target_area_max_um2", c.target_group_area_max, 16);
    w.q016("target_deformability_min", c.target_group_deformability_min);
    w.q016("target_deformability_max", c.target_group_deformability_max);
    w.fixed("target_emodulus_min_kpa", c.target_group_emodulus_min, 16);
    w.fixed("target_emodulus_max_kpa", c.target_group_emodulus_max, 16);

    w.raw("store_invalid_every_n", in.storeInvalidEveryN);
    w.raw("store_multi_image_frames",
          c.multi_image_enabled && c.multi_image_count > 1 ? static_cast<uint64_t>(c.multi_image_count) : 0);
    w.flag("store_valid_anchored", in.storeValidAnchored);
    w.flag("store_mask", in.storeMask);
    w.flag("store_all_frames", in.storeAllFrames);

    const EModulusLut* lut = in.lut && in.lut->isLoaded() ? in.lut : nullptr;
    if (lut) {
        if (lut->gridAreaBins() != 200 || lut->gridDeformBins() != 200) {
            out.errors.push_back("E-modulus grid must be 200 x 200 (PROFILE_TABLE0)");
        } else {
            w.fixed("lut_area_min_um2", lut->gridAreaMin(), 16);
            w.fixed("lut_area_step_um2", lut->gridAreaStep(), 16);
            w.q016("lut_deformability_min", lut->gridDeformMin());
            w.q016("lut_deformability_step", lut->gridDeformStep());
            if (std::llround(lut->gridAreaStep() * 65536.0) == 0 || std::llround(lut->gridDeformStep() * 65536.0) == 0) {
                out.errors.push_back("E-modulus grid step rounds to 0 in fixed point");
            }
            out.table0.resize(200 * 200 * 2);
            const auto& g = lut->grid();
            for (size_t i = 0; i < g.size(); ++i) {
                uint32_t v = 0xFFFF; // no value
                if (std::isfinite(g[i])) {
                    v = static_cast<uint32_t>(std::min<long long>(std::max<long long>(std::llround(g[i] * 256.0), 0),
                                                                   0xFFFE));
                }
                out.table0[2 * i] = static_cast<uint8_t>(v & 0xFF);
                out.table0[2 * i + 1] = static_cast<uint8_t>(v >> 8);
            }
        }
    } else if (c.enable_target_group && c.enable_target_group_emodulus) {
        out.errors.push_back("the target group gates on E-modulus but no E-modulus LUT is loaded");
    }
    out.warnings = plIgnoredSettingsChanged(c);
    return out;
}

} // namespace backend::processing::pz
