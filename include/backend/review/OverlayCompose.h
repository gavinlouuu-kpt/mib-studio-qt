// Qt-free overlay composition for review images (OpenCV only). The overlay
// itself is the one shared kernel, recording::renderProcessingOverlay
// (backend/recording/ProcessingOverlay.h), which the Qt OverlayRenderer and
// the facade's render_review_overlay also call (#512); the ROI rectangle is
// the Qt tab's. Every shell shows the same pixels.
#pragma once

#include "backend/review/ReviewTypes.h"

#include <opencv2/core.hpp>

namespace backend::services { struct FilterResult; }

namespace backend::review
{

    // Original (Mono8 or BGR) + mask → RGB (CV_8UC3) overlay for `mode`:
    // renderProcessingOverlay with OverlayPassthrough::KeepMono. When nothing
    // is drawn (OverlayMode::None, an empty mask or no usable contour
    // hierarchy) a colour original comes back as RGB and a Mono8 original
    // stays Mono8 (CV_8UC1); every drawn pixel equals the Qt renderer's.
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
