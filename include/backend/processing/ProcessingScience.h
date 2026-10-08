#pragma once

// The version-sensitive scientific pipeline that runs after mask generation:
// contour extraction and hierarchy classification, per-object metrics
// (hull area, deformability, area ratio, ring ratio), brightness quantiles,
// range/target-group gating, deterministic object ordering, and batch track
// matching. This is the single implementation behind
// IProcessingKernel::analyzeObjects/matchTrack — the bundled kernel and the
// ABI v1 dynamic-kernel fallback both execute it. Do not call it from
// pipeline code directly; route through the selected kernel so a future
// ABI v2 core can replace it. Qt-free by design.

#include "backend/processing/ProcessingTypes.h"

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

namespace backend {
class EModulusLut;
} // namespace backend

namespace backend::processing::science {

struct ContourAnalysis {
    std::vector<std::vector<cv::Point>> filteredContours;
    std::vector<std::vector<cv::Point>> innerContours;
    std::vector<int> parentIndices;
    std::vector<int> innerFilteredIndices;
    std::vector<std::vector<cv::Point>> allContours;
    std::vector<cv::Vec4i> hierarchy;
    std::vector<size_t> originalIndices;
};

ContourAnalysis findContours(const cv::Mat& processedImage);

// region restricts the scan to a sub-rectangle (e.g. an object's bounding
// box); an empty rect scans the whole image. Mask pixels outside an object's
// bbox are zero, so restricting the scan yields an identical brightness set.
services::BrightnessQuantiles calculateBrightnessQuantiles(const cv::Mat& originalImage,
                                                           const cv::Mat& mask,
                                                           const cv::Rect& region = cv::Rect());

double calculateRingRatio(const std::vector<cv::Point>& innerContour,
                          const std::vector<cv::Point>& outerContour);

// Contract v2 object focus metric. Variance of the Laplacian computed over the
// detected object only:
//   - crop the original Gray8 image to the contour bounding box plus
//     laplacianKernelSize context, clipped to image bounds;
//   - run the Laplacian on the UNMASKED crop (masking before convolution would
//     create an artificial high-contrast object boundary);
//   - take the variance with meanStdDev over a filled object mask, so pixels
//     outside the detected object do not contribute.
// Returns NaN for an unusable sample (empty image/contour, degenerate crop, or
// an empty mask). `objectContour` is the same contour used for area /
// deformability: the inner contour for nested candidates, the selected
// top-level contour for outer-only mode.
double calculateLaplacianVariance(const cv::Mat& originalImage,
                                  const std::vector<cv::Point>& objectContour,
                                  int laplacianKernelSize = 3);

// Contract 3 brightness: mean and population variance of the raw Gray8 image
// over the filled contour (the cell including any hole). False when the image
// or contour is unusable; the outputs are then NaN.
bool calculateBrightnessMoments(const cv::Mat& originalImage,
                                const std::vector<cv::Point>& contour,
                                double& mean,
                                double& variance);

cv::Mat makeObjectMask(const cv::Size& size,
                       const std::vector<std::vector<cv::Point>>& contours,
                       int contourIdx,
                       int parentIdx,
                       bool nested);

bool contourTouchesRoiBorder(const std::vector<cv::Point>& contour, const cv::Rect& roi);

// Why a detection failed validation, in the same priority order and with the
// same early-exit semantics as the gating in filterProcessedObjects (and the
// human-readable ExperimentMonitoringTab tooltip). NoContour and Border are
// early exits: when either applies, metric-range reasons are not reported
// because those metrics were not computed. This is the single source of truth
// for both the live invalid-reason histogram and the UI tooltip.
enum class InvalidReasonCode : uint8_t {
    NoContour = 0,
    Border,
    Area,
    Ring,
    Deform,
    AreaRatio,
    Laplacian, // Contract v2 object focus gate (disabled by default)
    Channel,   // centroid outside the channel band (auto channel band)
};
inline constexpr int kInvalidReasonCount = 8;

// True when no channel band is configured, or the centroid row lies inside it.
bool centroidInChannelBand(const services::FilterResult& result,
                           const services::ProcessingConfig& config);

// Returns the reasons `result` is invalid. Empty for a valid detection.
// pixelToMicronFactor converts result.area (pixels) to μm² to compare against
// the μm² area gates, matching the gating and the tooltip exactly.
std::vector<InvalidReasonCode> classifyInvalidReasons(const services::FilterResult& result,
                                                      const services::ProcessingConfig& config,
                                                      double pixelToMicronFactor = 1.0);

cv::Rect2d resultBbox(const services::FilterResult& result);

// Full per-frame object analysis over an already-generated mask.
// eModulusLut may be null (Young's modulus stays 0 and emodulus target
// gating treats the lookup as unavailable, matching an unloaded LUT).
std::vector<services::FilterResult> filterProcessedObjects(
    const cv::Mat& processedImage,
    const cv::Rect& roi,
    const services::ProcessingConfig& config,
    const cv::Mat& originalImage,
    double pixelToMicronFactor,
    const backend::EModulusLut* eModulusLut);

// Contract 3 (U-Net cells) object analysis over a U-Net foreground mask, the
// rules of the PZ7035 PL cell stage (profile unet_cells_v2):
//   - an object is a top-level 8-connected foreground component (components
//     inside a hole are ignored); it is a cell when it has at least
//     min_cell_area_px pixels, otherwise a blemish (counted in blemishCount);
//   - cells are ordered by bounding box (x, then y) and numbered from 1;
//   - cut-off: a component pixel on the ROI edge (1 px rule, always checked);
//     brightness, Laplacian and centroid are still reported;
//   - a contour enclosing no area is degenerate (reason NoContour, centroid at
//     the bounding-box centre);
//   - otherwise the outer-contour metrics and gates of Contract 2, without ring
//     width. Brightness is the mean and variance over the filled contour.
// Called by filterProcessedObjects for processing_contract_version 3.
std::vector<services::FilterResult> filterUnetCellObjects(
    const cv::Mat& processedImage,
    const cv::Rect& roi,
    const services::ProcessingConfig& config,
    const cv::Mat& originalImage,
    double pixelToMicronFactor,
    const backend::EModulusLut* eModulusLut);

services::FilterResult filterProcessedImage(const cv::Mat& processedImage,
                                            const cv::Rect& roi,
                                            const services::ProcessingConfig& config,
                                            const cv::Mat& originalImage,
                                            double pixelToMicronFactor,
                                            const backend::EModulusLut* eModulusLut);

// Batch track matching decision (IoU + directional/vertical gating). Returns
// the index of the best matching open track, or -1 for a new track.
int findMatchingTrack(const std::vector<services::BatchTrack>& tracks,
                      const std::vector<bool>& matchedThisFrame,
                      const services::FilterResult& detection,
                      uint64_t frameIndex,
                      int frameWidth);

} // namespace backend::processing::science
