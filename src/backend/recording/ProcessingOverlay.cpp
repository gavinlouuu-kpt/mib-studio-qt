#include "backend/recording/ProcessingOverlay.h"
#include "backend/processing/ProcessingService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace backend::recording {

namespace {

static cv::Mat toRgb(const cv::Mat& original) {
    if (original.empty()) return cv::Mat();
    cv::Mat rgb;
    if (original.channels() == 1) {
        cv::cvtColor(original, rgb, cv::COLOR_GRAY2RGB);
    } else {
        rgb = original.clone();
        if (rgb.channels() == 4) {cv::cvtColor(rgb,rgb,cv::COLOR_BGRA2RGB);}
        else if (rgb.channels() == 3) {
            cv::cvtColor(rgb, rgb, cv::COLOR_BGR2RGB);
        }
    }
    return rgb;
}

/** Map FilterResult classification to an RGB color: blue=target, green=valid, red=invalid. */
static cv::Vec3b classificationColor(const backend::services::FilterResult* v) {
    if (v && v->isTargetGroup) return {0, 120, 255};   // Blue
    if (v && v->isValid)       return {0, 255, 0};     // Green
    return {255, 0, 0};                                 // Red
}

/** Apply colored tint to overlay where mask is non-zero (in-place on rgb). */
static void applyMaskTint(cv::Mat& rgb, const cv::Mat& mask, const cv::Vec3b& tint) {
    if (rgb.empty() || mask.empty()) return;
    for (int y = 0; y < rgb.rows && y < mask.rows; ++y) {
        for (int x = 0; x < rgb.cols && x < mask.cols; ++x) {
            if (mask.at<uchar>(y, x) > 0) {
                cv::Vec3b& pixel = rgb.at<cv::Vec3b>(y, x);
                pixel[0] = static_cast<uchar>(std::min(255.0, pixel[0] * 0.7 + tint[0] * 0.3));
                pixel[1] = static_cast<uchar>(std::min(255.0, pixel[1] * 0.7 + tint[1] * 0.3));
                pixel[2] = static_cast<uchar>(std::min(255.0, pixel[2] * 0.7 + tint[2] * 0.3));
            }
        }
    }
}

} // namespace

cv::Mat renderProcessingOverlay(const cv::Mat& original,
                               const cv::Mat& mask,
                               const backend::services::FilterResult* validation,
                               OverlayMode mode) {
    if (original.empty()) {
        return cv::Mat();
    }
    if (mode == OverlayMode::None) {
        return toRgb(original);
    }
    cv::Mat rgb = toRgb(original);
    if (rgb.empty()) return cv::Mat();

    if (mode == OverlayMode::AllMask) {
        if (mask.empty()) return toRgb(original);
        applyMaskTint(rgb, mask, classificationColor(validation));
        return rgb;
    }

    if (mask.empty()) {
        return toRgb(original);
    }

    std::vector<std::vector<cv::Point>> contours;
    cv::Mat hierarchyMat;
    cv::findContours(mask.clone(), contours, hierarchyMat, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);

    const int n = static_cast<int>(contours.size());
    if (n == 0) {
        return toRgb(original);
    }

    // hierarchy: [next, prev, first_child, parent] per contour
    const bool hasHierarchy = !hierarchyMat.empty() && hierarchyMat.rows == 1 && hierarchyMat.cols == n;

    if (mode == OverlayMode::AllContour) {
        const cv::Vec3b c = classificationColor(validation);
        cv::drawContours(rgb, contours, -1, cv::Scalar(c[0], c[1], c[2]), 2);
        return rgb;
    }

    if (mode == OverlayMode::OuterInnerColorCoded && hasHierarchy) {
        const cv::Vec3b c = classificationColor(validation);
        const cv::Scalar outerColor(c[0], c[1], c[2]);
        // Lighter shade for inner contours
        const cv::Scalar innerColor(std::min(255, c[0] + 80),
                                    std::min(255, c[1] + 80),
                                    std::min(255, c[2] + 80));
        for (int i = 0; i < n; ++i) {
            const int parent = hierarchyMat.at<cv::Vec4i>(0, i)[3];
            if (parent < 0) {
                cv::drawContours(rgb, contours, i, outerColor, 2);
            } else {
                cv::drawContours(rgb, contours, i, innerColor, 2);
            }
        }
        return rgb;
    }

    if (mode == OverlayMode::FilteredMask && hasHierarchy) {
        // Find outer contour that has exactly one inner child (accepted pair)
        int acceptedOuter = -1;
        for (int i = 0; i < n; ++i) {
            const int parent = hierarchyMat.at<cv::Vec4i>(0, i)[3];
            if (parent >= 0) continue;
            const int firstChild = hierarchyMat.at<cv::Vec4i>(0, i)[2];
            if (firstChild < 0) continue;
            int childCount = 0;
            for (int c = firstChild; c >= 0; c = hierarchyMat.at<cv::Vec4i>(0, c)[0]) {
                ++childCount;
            }
            if (validation && validation->hasSingleInnerContour) {
                if (childCount == 1) {
                    acceptedOuter = i;
                    break;
                }
            } else {
                if (childCount >= 1) {
                    acceptedOuter = i;
                    break;
                }
            }
        }
        if (acceptedOuter >= 0) {
            cv::Mat filteredMask = cv::Mat::zeros(mask.rows, mask.cols, CV_8UC1);
            cv::drawContours(filteredMask, contours, acceptedOuter, cv::Scalar(255), -1);
            const int firstChild = hierarchyMat.at<cv::Vec4i>(0, acceptedOuter)[2];
            if (firstChild >= 0) {
                for (int c = firstChild; c >= 0; c = hierarchyMat.at<cv::Vec4i>(0, c)[0]) {
                    cv::drawContours(filteredMask, contours, c, cv::Scalar(0), -1);
                }
            }
            applyMaskTint(rgb, filteredMask, classificationColor(validation));
        }
        return rgb;
    }

    return toRgb(original);
}

} // namespace backend::recording
