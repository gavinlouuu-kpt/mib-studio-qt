#include "backend/processing/ProcessingConfigJson.h"

#include <nlohmann/json.hpp>

namespace backend::processing::config_json
{
    namespace
    {
        // Assign only when the key is present; error (not default-substitute)
        // on a type mismatch.
        template <typename T>
        bool assignIfPresent(const nlohmann::json &obj, const char *key, T &out,
                             std::string *errorOut)
        {
            const auto it = obj.find(key);
            if (it == obj.end())
            {
                return true;
            }
            try
            {
                out = it->get<T>();
                return true;
            }
            catch (const nlohmann::json::exception &e)
            {
                if (errorOut)
                {
                    *errorOut = std::string("Invalid value for '") + key + "': " + e.what();
                }
                return false;
            }
        }
    } // namespace

    nlohmann::json toJson(const services::ProcessingConfig &c)
    {
        // Exact config.json `image_processing` layout (see
        // resources/defaults/config.json).
        return nlohmann::json{
            {"gaussian_blur_size", c.gaussian_blur_size},
            {"bg_subtract_threshold", c.bg_subtract_threshold},
            {"morph_kernel_size", c.morph_kernel_size},
            {"morph_iterations", c.morph_iterations},
            {"area_threshold_min", c.area_threshold_min},
            {"area_threshold_max", c.area_threshold_max},
            {"deformability_threshold_min", c.deformability_threshold_min},
            {"deformability_threshold_max", c.deformability_threshold_max},
            {"area_ratio_threshold_max", c.area_ratio_threshold_max},
            {"ring_ratio_min", c.ring_ratio_min},
            {"ring_ratio_max", c.ring_ratio_max},
            {"empty_frame_pixel_threshold", c.empty_frame_pixel_threshold},
            {"auto_background_enabled", c.auto_background_enabled},
            {"auto_background_empty_frames", c.auto_background_empty_frames},
            {"auto_background_cooldown_frames", c.auto_background_cooldown_frames},
            {"filters",
             {
                 {"enable_border_check", c.enable_border_check},
                 {"enable_area_range_check", c.enable_area_range_check},
                 {"enable_deformability_range_check", c.enable_deformability_range_check},
                 {"enable_area_ratio_check", c.enable_area_ratio_check},
                 {"enable_ring_ratio_check", c.enable_ring_ratio_check},
                 {"require_single_inner_contour", c.require_single_inner_contour},
             }},
            {"target_group",
             {
                 {"enabled", c.enable_target_group},
                 {"area_min", c.target_group_area_min},
                 {"area_max", c.target_group_area_max},
                 {"deformability_min", c.target_group_deformability_min},
                 {"deformability_max", c.target_group_deformability_max},
                 {"emodulus_enabled", c.enable_target_group_emodulus},
                 {"emodulus_min", c.target_group_emodulus_min},
                 {"emodulus_max", c.target_group_emodulus_max},
             }},
            {"multi_image",
             {
                 {"enabled", c.multi_image_enabled},
                 {"count", c.multi_image_count},
             }},
        };
    }

    bool fromJson(const nlohmann::json &json,
                  services::ProcessingConfig &c,
                  std::string *errorOut)
    {
        if (!json.is_object())
        {
            if (errorOut)
            {
                *errorOut = "image_processing must be a JSON object";
            }
            return false;
        }

        bool ok = true;
        ok &= assignIfPresent(json, "gaussian_blur_size", c.gaussian_blur_size, errorOut);
        ok &= assignIfPresent(json, "bg_subtract_threshold", c.bg_subtract_threshold, errorOut);
        ok &= assignIfPresent(json, "morph_kernel_size", c.morph_kernel_size, errorOut);
        ok &= assignIfPresent(json, "morph_iterations", c.morph_iterations, errorOut);
        ok &= assignIfPresent(json, "area_threshold_min", c.area_threshold_min, errorOut);
        ok &= assignIfPresent(json, "area_threshold_max", c.area_threshold_max, errorOut);
        ok &= assignIfPresent(json, "deformability_threshold_min", c.deformability_threshold_min, errorOut);
        ok &= assignIfPresent(json, "deformability_threshold_max", c.deformability_threshold_max, errorOut);
        ok &= assignIfPresent(json, "area_ratio_threshold_max", c.area_ratio_threshold_max, errorOut);
        ok &= assignIfPresent(json, "ring_ratio_min", c.ring_ratio_min, errorOut);
        ok &= assignIfPresent(json, "ring_ratio_max", c.ring_ratio_max, errorOut);
        ok &= assignIfPresent(json, "empty_frame_pixel_threshold", c.empty_frame_pixel_threshold, errorOut);
        ok &= assignIfPresent(json, "auto_background_enabled", c.auto_background_enabled, errorOut);
        ok &= assignIfPresent(json, "auto_background_empty_frames", c.auto_background_empty_frames, errorOut);
        ok &= assignIfPresent(json, "auto_background_cooldown_frames", c.auto_background_cooldown_frames, errorOut);

        if (const auto filters = json.find("filters"); filters != json.end())
        {
            ok &= assignIfPresent(*filters, "enable_border_check", c.enable_border_check, errorOut);
            ok &= assignIfPresent(*filters, "enable_area_range_check", c.enable_area_range_check, errorOut);
            ok &= assignIfPresent(*filters, "enable_deformability_range_check",
                                  c.enable_deformability_range_check, errorOut);
            ok &= assignIfPresent(*filters, "enable_area_ratio_check", c.enable_area_ratio_check, errorOut);
            ok &= assignIfPresent(*filters, "enable_ring_ratio_check", c.enable_ring_ratio_check, errorOut);
            ok &= assignIfPresent(*filters, "require_single_inner_contour",
                                  c.require_single_inner_contour, errorOut);
        }
        if (const auto target = json.find("target_group"); target != json.end())
        {
            ok &= assignIfPresent(*target, "enabled", c.enable_target_group, errorOut);
            ok &= assignIfPresent(*target, "area_min", c.target_group_area_min, errorOut);
            ok &= assignIfPresent(*target, "area_max", c.target_group_area_max, errorOut);
            ok &= assignIfPresent(*target, "deformability_min", c.target_group_deformability_min, errorOut);
            ok &= assignIfPresent(*target, "deformability_max", c.target_group_deformability_max, errorOut);
            ok &= assignIfPresent(*target, "emodulus_enabled", c.enable_target_group_emodulus, errorOut);
            ok &= assignIfPresent(*target, "emodulus_min", c.target_group_emodulus_min, errorOut);
            ok &= assignIfPresent(*target, "emodulus_max", c.target_group_emodulus_max, errorOut);
        }
        if (const auto multi = json.find("multi_image"); multi != json.end())
        {
            ok &= assignIfPresent(*multi, "enabled", c.multi_image_enabled, errorOut);
            ok &= assignIfPresent(*multi, "count", c.multi_image_count, errorOut);
        }
        return ok;
    }

} // namespace backend::processing::config_json
