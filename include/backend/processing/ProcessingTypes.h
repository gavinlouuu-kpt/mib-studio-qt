#pragma once

// Plain scientific data contracts shared by the desktop service, the
// processing kernels (IProcessingKernel), and the portable science
// implementation (ProcessingScience). Qt-free by design.

#include <cstdint>
#include <memory>
#include <vector>

#include <opencv2/core.hpp>

namespace backend::services {

struct BrightnessQuantiles {
    double q1{0.0}; // 25th percentile
    double q2{0.0}; // 50th percentile (median)
    double q3{0.0}; // 75th percentile
    double q4{0.0}; // 100th percentile (max)
};

struct ProcessingConfig {
    int gaussian_blur_size{3};
    int bg_subtract_threshold{8};
    int morph_kernel_size{3};
    int morph_iterations{1};
    int area_threshold_min{60};    // μm²
    int area_threshold_max{290};   // μm²
    double deformability_threshold_min{0.0};
    double deformability_threshold_max{1.0};
    bool enable_border_check{true};
    bool enable_area_range_check{true};
    bool enable_deformability_range_check{false};
    double area_ratio_threshold_max{1.5};
    bool enable_area_ratio_check{false};
    double ring_ratio_min{15.0};
    double ring_ratio_max{25.0};
    bool enable_ring_ratio_check{true};
    bool require_single_inner_contour{true};
    int empty_frame_pixel_threshold{100};
    bool auto_background_enabled{false};
    int auto_background_empty_frames{30};
    int auto_background_cooldown_frames{1000};
    // Target group sort trigger (second gate within valid frames)
    bool enable_target_group{false};
    int target_group_area_min{72};   // μm²
    int target_group_area_max{191};  // μm²
    double target_group_deformability_min{0.0};
    double target_group_deformability_max{0.3};
    // Young's modulus gating (uses LUT lookup from area + deformability)
    bool enable_target_group_emodulus{false};
    double target_group_emodulus_min{0.0};
    double target_group_emodulus_max{10.0};
    // Multi-image recording: capture a series of N consecutive frames per valid detection
    // Metrics are computed only from the first (trigger) frame
    bool multi_image_enabled{false};
    int multi_image_count{1}; // Number of images per series (1 = disabled, >1 = series)
};

struct FilterResult {
    bool isValid{false};
    bool touchesBorder{false};
    bool hasSingleInnerContour{false};
    bool inRange{false};
    int innerContourCount{0};
    int objectId{-1};
    int objectCount{0};
    int trackId{-1};
    uint64_t trackFirstFrame{0};
    uint64_t trackLastFrame{0};
    int trackObservationCount{0};
    double bboxX{0.0};
    double bboxY{0.0};
    double bboxWidth{0.0};
    double bboxHeight{0.0};
    double centroidX{0.0};
    double centroidY{0.0};
    double deformability{0.0};
    double area{0.0};
    double areaRatio{0.0};
    double ringRatio{0.0};
    double youngsModulus{0.0}; // Young's modulus (kPa) from LUT lookup
    BrightnessQuantiles brightness;
    bool isTargetGroup{false}; // True if valid AND matches target group criteria
    // Contours found during processing (for snapshot/display), in the same
    // coordinate space as the processedImage mask. Shared (not deep-copied) so
    // that the per-object FilterResults of a frame, plus the monitoring /
    // experiment copies, all reference one allocation instead of duplicating
    // every contour point N times. Null when no contours were extracted.
    std::shared_ptr<const std::vector<std::vector<cv::Point>>> allContours;
};

// One analysed frame (or one object of a frame — several ProcessedFrames can
// share the same source index). Image members are read-only after
// publication (frozen-Mats invariant): every consumer shares them by
// refcount and never clones merely for lifetime (issue #370).
struct ProcessedFrame {
    uint64_t index{0};
    uint64_t timestampNs{0};
    // Host monotonic acquisition stamp carried from playback::Frame (0 if unknown).
    uint64_t hostTimestampUs{0};
    cv::Mat originalImage;
    cv::Mat processedImage; // mask
    FilterResult validation;
    // Multi-image series: additional images captured after the trigger frame.
    // seriesImages[0] is the trigger image (same as originalImage), followed by subsequent frames.
    // Empty when multi-image mode is disabled.
    std::vector<cv::Mat> seriesImages;
};

struct BufferedFrameCounts {
    size_t valid{0};
    size_t invalid{0};

    size_t total() const { return valid + invalid; }
};

// Host-owned batch tracking state. The lifecycle (creation, per-frame
// bookkeeping) belongs to the caller; the matching DECISION is
// version-sensitive science owned by the selected processing kernel.
struct BatchTrack {
    int id{-1};
    uint64_t firstFrame{0};
    uint64_t lastFrame{0};
    int observations{0};
    cv::Rect2d lastBbox;
    cv::Point2d lastCentroid;
    size_t outputIndex{0};
};

} // namespace backend::services
