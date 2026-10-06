// Qt-free overlay composition for review images (OpenCV only). The one
// counterpart of frontend/utils/OverlayRenderer.cpp: same colours, same
// contour rules, same ROI rectangle, so every shell shows the same pixels.
#pragma once

#include "backend/review/ReviewTypes.h"

#include <opencv2/core.hpp>

namespace backend::services { struct FilterResult; }

namespace backend::review
{

    // Original (Mono8 or BGR) + mask → RGB (CV_8UC3) overlay for `mode`.
    // OverlayMode::None returns the original converted to RGB only when it
    // already has three channels; a Mono8 original stays Mono8 (CV_8UC1).
    // An empty mask or no usable contour hierarchy falls back to the
    // original, as the Qt renderer does.
    cv::Mat composeOverlay(const cv::Mat &original,
                           const cv::Mat &mask,
                           const services::FilterResult *validation,
                           OverlayMode mode);

    // Draw the recorded ROI as a red 3 px rectangle on `image` (CV_8UC1 or
    // CV_8UC3, modified in place; a Mono8 image is promoted to RGB). `roi`
    // is in original-frame coordinates of `origWidth` × `origHeight`.
    void drawRoiOverlay(cv::Mat &image,
                        const services::ProcessingService::Roi &roi,
                        int origWidth,
                        int origHeight);

    // Pack a CV_8UC1 / CV_8UC3 matrix into a ReviewImage (RGB order for
    // three channels; `image` is assumed RGB already).
    void packImage(const cv::Mat &image, std::uint64_t frameIndex, ReviewImage &out);

} // namespace backend::review
