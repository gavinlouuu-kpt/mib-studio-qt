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
};
inline constexpr int kInvalidReasonCount = 6;

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
