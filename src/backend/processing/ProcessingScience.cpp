#include "backend/processing/ProcessingScience.h"

#include "backend/processing/EModulusLut.h"

#include <opencv2/core.hpp>
#if __has_include(<opencv2/geometry.hpp>)
#include <opencv2/geometry.hpp> // OpenCV 5 moved contour geometry out of imgproc.hpp
#endif
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace backend::processing::science {

using services::BatchTrack;
using services::BrightnessQuantiles;
using services::FilterResult;
using services::ProcessingConfig;

namespace {

double rectArea(const cv::Rect2d& rect) {
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

double rectIou(const cv::Rect2d& a, const cv::Rect2d& b) {
    const double x1 = std::max(a.x, b.x);
    const double y1 = std::max(a.y, b.y);
    const double x2 = std::min(a.x + a.width, b.x + b.width);
    const double y2 = std::min(a.y + a.height, b.y + b.height);
    const double intersection = std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
    const double unionArea = rectArea(a) + rectArea(b) - intersection;
    return unionArea > 0.0 ? intersection / unionArea : 0.0;
}

void populateGeometry(FilterResult& result, const std::vector<cv::Point>& contour) {
    if (contour.empty()) {
        return;
    }

    const cv::Rect bbox = cv::boundingRect(contour);
    result.bboxX = static_cast<double>(bbox.x);
    result.bboxY = static_cast<double>(bbox.y);
    result.bboxWidth = static_cast<double>(bbox.width);
    result.bboxHeight = static_cast<double>(bbox.height);

    const cv::Moments moments = cv::moments(contour);
    if (std::abs(moments.m00) > std::numeric_limits<double>::epsilon()) {
        result.centroidX = moments.m10 / moments.m00;
        result.centroidY = moments.m01 / moments.m00;
    } else {
        result.centroidX = result.bboxX + result.bboxWidth * 0.5;
        result.centroidY = result.bboxY + result.bboxHeight * 0.5;
    }
}

} // namespace

cv::Rect2d resultBbox(const FilterResult& result) {
    return cv::Rect2d(result.bboxX, result.bboxY, result.bboxWidth, result.bboxHeight);
}

int findMatchingTrack(const std::vector<BatchTrack>& tracks,
                      const std::vector<bool>& matchedThisFrame, const FilterResult& detection,
                      uint64_t frameIndex, int frameWidth) {
    constexpr uint64_t kMaxFrameGap = 5;
    constexpr double kMinIou = 0.08;
    constexpr double kBaseCentroidThresholdPx = 24.0;
    constexpr double kMaxLeftwardJitterPx = 2.0;
    constexpr double kDirectionalFrameFraction = 0.35;
    constexpr double kMinDirectionalStepPx = 64.0;
    constexpr double kMinVerticalTolerancePx = 20.0;

    const cv::Rect2d bbox = resultBbox(detection);
    if (rectArea(bbox) <= 0.0) {
        return -1;
    }

    int bestTrack = -1;
    double bestScore = std::numeric_limits<double>::max();
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& track = tracks[i];
        if (i < matchedThisFrame.size() && matchedThisFrame[i]) {
            continue;
        }
        if (frameIndex <= track.lastFrame) {
            continue;
        }

        const uint64_t frameGap = frameIndex - track.lastFrame;
        if (frameGap > kMaxFrameGap) {
            continue;
        }

        if (detection.centroidX + kMaxLeftwardJitterPx < track.lastCentroid.x) {
            continue;
        }

        const double iou = rectIou(bbox, track.lastBbox);
        const double dx = detection.centroidX - track.lastCentroid.x;
        const double dy = std::abs(detection.centroidY - track.lastCentroid.y);
        const double motionThreshold = std::max(
            kBaseCentroidThresholdPx, std::max(track.lastBbox.width, track.lastBbox.height) * 1.25 +
                                          static_cast<double>(frameGap) * 8.0);
        const double directionalStep =
            std::max(motionThreshold,
                     std::max(kMinDirectionalStepPx, static_cast<double>(std::max(1, frameWidth)) *
                                                         kDirectionalFrameFraction) *
                         static_cast<double>(frameGap));
        const double verticalTolerance =
            std::max(kMinVerticalTolerancePx, std::max(track.lastBbox.height, bbox.height) * 1.5);
        if (iou < kMinIou && (dx > directionalStep || dy > verticalTolerance)) {
            continue;
        }

        const double score =
            (1.0 - std::min(1.0, iou)) + (std::max(0.0, dx) / std::max(1.0, directionalStep)) +
            (dy / std::max(1.0, verticalTolerance)) + static_cast<double>(frameGap) * 0.05;
        if (score < bestScore) {
            bestScore = score;
            bestTrack = static_cast<int>(i);
        }
    }
    return bestTrack;
}

double calculateRingRatio(const std::vector<cv::Point>& innerContour,
                                             const std::vector<cv::Point>& outerContour) {
    double innerArea = cv::contourArea(innerContour);
    double outerArea = cv::contourArea(outerContour);
    if (outerArea <= innerArea) return 0.0;
    return std::sqrt(outerArea - innerArea);
}

ContourAnalysis findContours(const cv::Mat& processedImage) {
    ContourAnalysis analysis;
    cv::findContours(processedImage, analysis.allContours, analysis.hierarchy, cv::RETR_TREE,
                     cv::CHAIN_APPROX_SIMPLE);

    const double minNoiseArea = 10.0;

    for (size_t i = 0; i < analysis.allContours.size(); i++) {
        double area = cv::contourArea(analysis.allContours[i]);
        if (area >= minNoiseArea) {
            analysis.filteredContours.push_back(analysis.allContours[i]);
            analysis.originalIndices.push_back(i);
        }
    }

    for (size_t i = 0; i < analysis.originalIndices.size(); i++) {
        size_t origIdx = analysis.originalIndices[i];
        if (origIdx < analysis.hierarchy.size() && analysis.hierarchy[origIdx][3] > -1) {
            analysis.innerContours.push_back(analysis.filteredContours[i]);
            analysis.innerFilteredIndices.push_back(static_cast<int>(i));
            int parentOrigIdx = analysis.hierarchy[origIdx][3];
            int filteredParentIdx = -1;
            for (size_t j = 0; j < analysis.originalIndices.size(); j++) {
                if (analysis.originalIndices[j] == static_cast<size_t>(parentOrigIdx)) {
                    filteredParentIdx = static_cast<int>(j);
                    break;
                }
            }
            analysis.parentIndices.push_back(filteredParentIdx);
        }
    }

    return analysis;
}

services::BrightnessQuantiles calculateBrightnessQuantiles(const cv::Mat& originalImage,
                                                                    const cv::Mat& mask,
                                                                    const cv::Rect& region) {
    BrightnessQuantiles result;
    if (originalImage.empty() || mask.empty()) {
        return result;
    }

    // Avoid cloning when the input is already single-channel (the common case).
    cv::Mat converted;
    const cv::Mat* grayPtr = &originalImage;
    if (originalImage.channels() == 3) {
        cv::cvtColor(originalImage, converted, cv::COLOR_BGR2GRAY);
        grayPtr = &converted;
    }
    const cv::Mat& grayImage = *grayPtr;

    // Scan the whole image by default, or just the requested sub-rectangle,
    // clamped to the bounds shared by both images.
    cv::Rect scan(0, 0, std::min(grayImage.cols, mask.cols), std::min(grayImage.rows, mask.rows));
    if (region.width > 0 && region.height > 0) {
        scan &= region;
    }
    if (scan.width <= 0 || scan.height <= 0) {
        return result;
    }

    std::vector<uchar> brightness;
    brightness.reserve(static_cast<size_t>(scan.width) * static_cast<size_t>(scan.height) / 4 + 1);

    // Row-pointer access avoids the per-pixel bounds checks of cv::Mat::at<>.
    for (int y = scan.y; y < scan.y + scan.height; ++y) {
        const uchar* maskRow = mask.ptr<uchar>(y);
        const uchar* grayRow = grayImage.ptr<uchar>(y);
        for (int x = scan.x; x < scan.x + scan.width; ++x) {
            if (maskRow[x] > 0) {
                brightness.push_back(grayRow[x]);
            }
        }
    }

    if (brightness.empty()) return result;

    std::sort(brightness.begin(), brightness.end());
    size_t n = brightness.size();
    result.q1 = brightness[n / 4];
    result.q2 = brightness[n / 2];
    result.q3 = brightness[(3 * n) / 4];
    result.q4 = brightness[n - 1];

    return result;
}

cv::Mat makeObjectMask(const cv::Size& size,
                                          const std::vector<std::vector<cv::Point>>& contours,
                                          int contourIdx, int parentIdx, bool nested) {
    cv::Mat mask(size, CV_8UC1, cv::Scalar(0));
    if (nested && parentIdx >= 0 && parentIdx < static_cast<int>(contours.size())) {
        cv::drawContours(mask, contours, parentIdx, cv::Scalar(255), cv::FILLED);
        if (contourIdx >= 0 && contourIdx < static_cast<int>(contours.size())) {
            cv::drawContours(mask, contours, contourIdx, cv::Scalar(0), cv::FILLED);
        }
    } else if (contourIdx >= 0 && contourIdx < static_cast<int>(contours.size())) {
        cv::drawContours(mask, contours, contourIdx, cv::Scalar(255), cv::FILLED);
    }
    return mask;
}

bool contourTouchesRoiBorder(const std::vector<cv::Point>& contour,
                                                const cv::Rect& roi) {
    constexpr int borderThreshold = 2;
    for (const auto& point : contour) {
        const int x = point.x - roi.x;
        const int y = point.y - roi.y;
        if (x >= 0 && x < roi.width && y >= 0 && y < roi.height) {
            if (x < borderThreshold || x >= roi.width - borderThreshold || y < borderThreshold ||
                y >= roi.height - borderThreshold) {
                return true;
            }
        } else {
            return true;
        }
    }
    return false;
}

std::vector<InvalidReasonCode> classifyInvalidReasons(const FilterResult& result,
                                                      const ProcessingConfig& config,
                                                      double pixelToMicronFactor) {
    std::vector<InvalidReasonCode> reasons;
    if (result.isValid) {
        return reasons;
    }

    // Early exits mirror the gating: when there is no inner contour, or the
    // contour touches the border, per-object metrics are not computed, so the
    // metric-range reasons below would read uninitialised gates. Report only
    // the early-exit reason in those cases (matches getInvalidReasons()).
    if (config.require_single_inner_contour && result.innerContourCount == 0) {
        reasons.push_back(InvalidReasonCode::NoContour);
        return reasons;
    }
    if (config.enable_border_check && result.touchesBorder) {
        reasons.push_back(InvalidReasonCode::Border);
        return reasons;
    }

    const double areaUm = result.area * pixelToMicronFactor * pixelToMicronFactor;
    if (config.enable_area_range_check &&
        (areaUm < config.area_threshold_min || areaUm > config.area_threshold_max)) {
        reasons.push_back(InvalidReasonCode::Area);
    }
    if (config.enable_ring_ratio_check &&
        (result.ringRatio <= config.ring_ratio_min || result.ringRatio >= config.ring_ratio_max)) {
        reasons.push_back(InvalidReasonCode::Ring);
    }
    if (config.enable_deformability_range_check &&
        (result.deformability < config.deformability_threshold_min ||
         result.deformability > config.deformability_threshold_max)) {
        reasons.push_back(InvalidReasonCode::Deform);
    }
    if (config.enable_area_ratio_check && result.areaRatio > config.area_ratio_threshold_max) {
        reasons.push_back(InvalidReasonCode::AreaRatio);
    }
    return reasons;
}

namespace {

FilterResult evaluateInnerContourObject(
    const ContourAnalysis& analysis, size_t innerIdx, int objectId, int objectCount,
    const cv::Mat& processedImage, const cv::Rect& roi, const ProcessingConfig& config,
    const cv::Mat& originalImage, double pixelToMicronFactor,
    const backend::EModulusLut* eModulusLut) {
    FilterResult result{};
    // allContours is assigned once (shared) by filterProcessedObjects after all
    // objects are evaluated; hierarchy is no longer retained on the result.
    result.innerContourCount = static_cast<int>(analysis.innerContours.size());
    result.hasSingleInnerContour = (analysis.innerContours.size() == 1);
    result.objectId = objectId;
    result.objectCount = objectCount;

    if (innerIdx >= analysis.innerContours.size()) {
        return result;
    }

    const auto& innerContour = analysis.innerContours[innerIdx];
    const int parentIdx =
        innerIdx < analysis.parentIndices.size() ? analysis.parentIndices[innerIdx] : -1;
    const int innerFilteredIdx = innerIdx < analysis.innerFilteredIndices.size()
                                     ? analysis.innerFilteredIndices[innerIdx]
                                     : -1;

    const cv::Mat objectMask = makeObjectMask(processedImage.size(), analysis.filteredContours,
                                              innerFilteredIdx, parentIdx, true);
    const auto& geometryContour =
        (parentIdx >= 0 && parentIdx < static_cast<int>(analysis.filteredContours.size()))
            ? analysis.filteredContours[static_cast<size_t>(parentIdx)]
            : innerContour;
    populateGeometry(result, geometryContour);
    if (!originalImage.empty()) {
        const cv::Rect bbox(static_cast<int>(result.bboxX), static_cast<int>(result.bboxY),
                            static_cast<int>(result.bboxWidth),
                            static_cast<int>(result.bboxHeight));
        result.brightness = calculateBrightnessQuantiles(originalImage, objectMask, bbox);
    }

    if (config.enable_border_check && contourTouchesRoiBorder(innerContour, roi)) {
        result.touchesBorder = true;
        return result;
    }

    const double contourArea = cv::contourArea(innerContour);
    if (contourArea <= 0.0) {
        return result;
    }

    std::vector<cv::Point> hull;
    cv::convexHull(innerContour, hull);
    const double hullArea = cv::contourArea(hull);
    result.areaRatio = hullArea / contourArea;
    const double perimeter = cv::arcLength(hull, true);
    const double circularity = (perimeter > 0.0) ? std::sqrt(4 * M_PI * hullArea) / perimeter : 0.0;
    result.deformability = 1.0 - circularity;
    result.area = hullArea;

    if (parentIdx >= 0 && parentIdx < static_cast<int>(analysis.filteredContours.size())) {
        result.ringRatio = calculateRingRatio(innerContour, analysis.filteredContours[parentIdx]);
    }

    const double pxToUm = pixelToMicronFactor;
    const double areaUm = hullArea * pxToUm * pxToUm;

    const bool areaInRange =
        !config.enable_area_range_check ||
        (areaUm >= config.area_threshold_min && areaUm <= config.area_threshold_max);
    const bool ringRatioInRange =
        !config.enable_ring_ratio_check ||
        (result.ringRatio > config.ring_ratio_min && result.ringRatio < config.ring_ratio_max);
    const bool deformabilityInRange = !config.enable_deformability_range_check ||
                                      (result.deformability >= config.deformability_threshold_min &&
                                       result.deformability <= config.deformability_threshold_max);
    const bool areaRatioInRange =
        !config.enable_area_ratio_check || (result.areaRatio <= config.area_ratio_threshold_max);

    if (areaInRange && ringRatioInRange && deformabilityInRange && areaRatioInRange) {
        result.inRange = true;
        result.isValid = true;
    }

    if (eModulusLut && eModulusLut->isLoaded()) {
        result.youngsModulus = eModulusLut->lookup(areaUm, result.deformability);
    }
    if (result.isValid && config.enable_target_group) {
        const bool tgArea =
            (areaUm >= config.target_group_area_min && areaUm <= config.target_group_area_max);
        const bool tgDeform = (result.deformability >= config.target_group_deformability_min &&
                               result.deformability <= config.target_group_deformability_max);
        const bool tgEmod = !config.enable_target_group_emodulus ||
                            (!std::isnan(result.youngsModulus) &&
                             result.youngsModulus >= config.target_group_emodulus_min &&
                             result.youngsModulus <= config.target_group_emodulus_max);
        result.isTargetGroup = tgArea && tgDeform && tgEmod;
    }

    return result;
}

FilterResult evaluateOuterContourObject(
    const ContourAnalysis& analysis, size_t contourIdx, int objectId, int objectCount,
    const cv::Mat& processedImage, const cv::Rect& roi, const ProcessingConfig& config,
    const cv::Mat& originalImage, double pixelToMicronFactor,
    const backend::EModulusLut* eModulusLut) {
    FilterResult result{};
    // allContours is assigned once (shared) by filterProcessedObjects after all
    // objects are evaluated; hierarchy is no longer retained on the result.
    result.innerContourCount = static_cast<int>(analysis.innerContours.size());
    result.hasSingleInnerContour = (analysis.innerContours.size() == 1);
    result.objectId = objectId;
    result.objectCount = objectCount;

    if (contourIdx >= analysis.filteredContours.size()) {
        return result;
    }

    const auto& contour = analysis.filteredContours[contourIdx];
    const cv::Mat objectMask = makeObjectMask(processedImage.size(), analysis.filteredContours,
                                              static_cast<int>(contourIdx), -1, false);
    populateGeometry(result, contour);
    if (!originalImage.empty()) {
        const cv::Rect bbox(static_cast<int>(result.bboxX), static_cast<int>(result.bboxY),
                            static_cast<int>(result.bboxWidth),
                            static_cast<int>(result.bboxHeight));
        result.brightness = calculateBrightnessQuantiles(originalImage, objectMask, bbox);
    }

    if (config.enable_border_check && contourTouchesRoiBorder(contour, roi)) {
        result.touchesBorder = true;
        return result;
    }

    const double contourArea = cv::contourArea(contour);
    if (contourArea <= 0.0) {
        return result;
    }

    std::vector<cv::Point> hull;
    cv::convexHull(contour, hull);
    const double hullArea = cv::contourArea(hull);
    result.areaRatio = hullArea / contourArea;
    const double perimeter = cv::arcLength(hull, true);
    const double circularity = (perimeter > 0.0) ? std::sqrt(4 * M_PI * hullArea) / perimeter : 0.0;
    result.deformability = 1.0 - circularity;
    result.area = hullArea;

    const double pxToUm = pixelToMicronFactor;
    const double areaUm = hullArea * pxToUm * pxToUm;

    const bool areaInRange =
        !config.enable_area_range_check ||
        (areaUm >= config.area_threshold_min && areaUm <= config.area_threshold_max);
    const bool deformabilityInRange = !config.enable_deformability_range_check ||
                                      (result.deformability >= config.deformability_threshold_min &&
                                       result.deformability <= config.deformability_threshold_max);
    const bool areaRatioInRange =
        !config.enable_area_ratio_check || (result.areaRatio <= config.area_ratio_threshold_max);

    if (areaInRange && deformabilityInRange && areaRatioInRange) {
        result.inRange = true;
        result.isValid = true;
    }

    if (eModulusLut && eModulusLut->isLoaded()) {
        result.youngsModulus = eModulusLut->lookup(areaUm, result.deformability);
    }
    if (result.isValid && config.enable_target_group) {
        const bool tgArea =
            (areaUm >= config.target_group_area_min && areaUm <= config.target_group_area_max);
        const bool tgDeform = (result.deformability >= config.target_group_deformability_min &&
                               result.deformability <= config.target_group_deformability_max);
        const bool tgEmod = !config.enable_target_group_emodulus ||
                            (!std::isnan(result.youngsModulus) &&
                             result.youngsModulus >= config.target_group_emodulus_min &&
                             result.youngsModulus <= config.target_group_emodulus_max);
        result.isTargetGroup = tgArea && tgDeform && tgEmod;
    }

    return result;
}

} // namespace

std::vector<services::FilterResult> filterProcessedObjects(
    const cv::Mat& processedImage,
    const cv::Rect& roi,
    const services::ProcessingConfig& config,
    const cv::Mat& originalImage,
    double pixelToMicronFactor,
    const backend::EModulusLut* eModulusLut) {
    const ContourAnalysis analysis = findContours(processedImage);

    // One shared copy of the frame's contours, referenced by every result (and
    // by the downstream monitoring / experiment copies) instead of deep-copying
    // all contour points per object.
    auto sharedContours =
        std::make_shared<const std::vector<std::vector<cv::Point>>>(analysis.allContours);

    FilterResult emptyResult{};
    emptyResult.allContours = sharedContours;
    emptyResult.innerContourCount = static_cast<int>(analysis.innerContours.size());
    emptyResult.hasSingleInnerContour = (analysis.innerContours.size() == 1);
    if (!originalImage.empty()) {
        emptyResult.brightness = calculateBrightnessQuantiles(originalImage, processedImage);
    }

    if (config.require_single_inner_contour && analysis.innerContours.empty()) {
        return {std::move(emptyResult)};
    }

    if (!analysis.innerContours.empty()) {
        std::vector<size_t> objectOrder;
        objectOrder.reserve(analysis.innerContours.size());
        for (size_t i = 0; i < analysis.innerContours.size(); ++i) {
            objectOrder.push_back(i);
        }
        std::sort(objectOrder.begin(), objectOrder.end(), [&](size_t lhs, size_t rhs) {
            const cv::Rect lhsBox = cv::boundingRect(analysis.innerContours[lhs]);
            const cv::Rect rhsBox = cv::boundingRect(analysis.innerContours[rhs]);
            return std::tie(lhsBox.x, lhsBox.y, lhs) < std::tie(rhsBox.x, rhsBox.y, rhs);
        });

        std::vector<FilterResult> results;
        results.reserve(objectOrder.size());
        const int objectCount = static_cast<int>(analysis.innerContours.size());
        for (size_t i = 0; i < objectOrder.size(); ++i) {
            results.push_back(evaluateInnerContourObject(
                analysis, objectOrder[i], static_cast<int>(i + 1), objectCount, processedImage, roi,
                config, originalImage, pixelToMicronFactor, eModulusLut));
        }
        for (auto& result : results) {
            result.allContours = sharedContours;
        }
        return results;
    }

    if (!analysis.filteredContours.empty() && !config.require_single_inner_contour) {
        std::vector<size_t> topLevelContours;
        for (size_t i = 0; i < analysis.filteredContours.size(); ++i) {
            const size_t origIdx =
                i < analysis.originalIndices.size() ? analysis.originalIndices[i] : 0;
            const bool hasParent =
                origIdx < analysis.hierarchy.size() && analysis.hierarchy[origIdx][3] > -1;
            if (!hasParent) {
                topLevelContours.push_back(i);
            }
        }
        if (topLevelContours.empty()) {
            for (size_t i = 0; i < analysis.filteredContours.size(); ++i) {
                topLevelContours.push_back(i);
            }
        }
        std::sort(topLevelContours.begin(), topLevelContours.end(), [&](size_t lhs, size_t rhs) {
            const cv::Rect lhsBox = cv::boundingRect(analysis.filteredContours[lhs]);
            const cv::Rect rhsBox = cv::boundingRect(analysis.filteredContours[rhs]);
            return std::tie(lhsBox.x, lhsBox.y, lhs) < std::tie(rhsBox.x, rhsBox.y, rhs);
        });

        std::vector<FilterResult> results;
        results.reserve(topLevelContours.size());
        const int objectCount = static_cast<int>(topLevelContours.size());
        for (size_t i = 0; i < topLevelContours.size(); ++i) {
            results.push_back(evaluateOuterContourObject(
                analysis, topLevelContours[i], static_cast<int>(i + 1), objectCount, processedImage,
                roi, config, originalImage, pixelToMicronFactor, eModulusLut));
        }
        for (auto& result : results) {
            result.allContours = sharedContours;
        }
        return results;
    }

    return {std::move(emptyResult)};
}

services::FilterResult filterProcessedImage(const cv::Mat& processedImage,
                                            const cv::Rect& roi,
                                            const services::ProcessingConfig& config,
                                            const cv::Mat& originalImage,
                                            double pixelToMicronFactor,
                                            const backend::EModulusLut* eModulusLut) {
    auto results = filterProcessedObjects(processedImage, roi, config, originalImage,
                                          pixelToMicronFactor, eModulusLut);
    if (results.empty()) {
        return {};
    }
    return std::move(results.front());
}

} // namespace backend::processing::science
