// Clamped ROI crop for monitoring frames (Qt-free, header-only so it is unit
// testable). The Monitoring tab shows frames accumulated under whatever ROI
// and camera geometry was active when they were processed; the *current*
// ROI may not fit them any more (ROI edited, camera or config switched). The
// installed 1.0.7 app aborted seven times on
// `cv::Mat::Mat: Assertion failed 0 <= roi.x && ... roi.x + roi.width <= m.cols`
// from exactly that crop (crash review, 2026-09-08). Never construct a
// cv::Mat ROI from an unclamped rectangle.
#pragma once

#include "backend/recording/RoiCrop.h"

#include <opencv2/core.hpp>

namespace frontend::monitoring {

// Rectangle to crop `image` with for a requested ROI: the ROI clamped to the
// image, the full image when the ROI is empty/invalid, an empty rect for an
// empty image (cv::Mat(empty rect) on an empty Mat is a no-op, not an assert).
inline cv::Rect clampedRoiRect(const cv::Mat& image, int roiX, int roiY, int roiW, int roiH)
{
    if (image.empty()) return cv::Rect(0, 0, 0, 0);
    const auto r = backend::recording::clampRoiToFrame(image.cols, image.rows, roiX, roiY, roiW, roiH);
    return cv::Rect(r.x, r.y, r.w, r.h);
}

// True when the frame already is an ROI-sized crop (monitoring may store
// ROI-only images); such frames are shown as they are.
inline bool isRoiSized(const cv::Mat& image, int roiW, int roiH)
{
    return !image.empty() && image.cols == roiW && image.rows == roiH;
}

// Crop `image` to the ROI safely (see clampedRoiRect). Shares pixels.
inline cv::Mat cropToRoi(const cv::Mat& image, int roiX, int roiY, int roiW, int roiH)
{
    if (isRoiSized(image, roiW, roiH)) return image;
    const cv::Rect rect = clampedRoiRect(image, roiX, roiY, roiW, roiH);
    if (rect.width <= 0 || rect.height <= 0) return cv::Mat();
    return image(rect);
}

} // namespace frontend::monitoring
