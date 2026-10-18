#pragma once
#include "backend/processing/ProcessingTypes.h"
#include <opencv2/core.hpp>
namespace backend::recording {
enum class OverlayMode { None, AllContour, OuterInnerColorCoded, AllMask, FilteredMask };
// What an unchanged original comes back as when nothing is drawn (mode None,
// no usable mask or contour hierarchy): RGB888 always (Qt, the facade's PNG),
// or a Mono8 original kept Mono8 (ReviewSession's packets, #512).
enum class OverlayPassthrough { Rgb, KeepMono };
// The one review overlay kernel (Qt OverlayRenderer, the facade's
// render_review_overlay, review::composeOverlay): RGB888 output; no
// science/config mutation.
cv::Mat renderProcessingOverlay(const cv::Mat& original,const cv::Mat& mask,
                               const services::FilterResult* validation,OverlayMode mode,
                               OverlayPassthrough passthrough=OverlayPassthrough::Rgb);
}
