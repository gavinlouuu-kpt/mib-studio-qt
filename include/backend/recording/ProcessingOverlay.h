#pragma once
#include "backend/processing/ProcessingTypes.h"
#include <opencv2/core.hpp>
namespace backend::recording {
enum class OverlayMode { None, AllContour, OuterInnerColorCoded, AllMask, FilteredMask };
// Shared Qt/Tauri display rendering, RGB888 output; no science/config mutation.
cv::Mat renderProcessingOverlay(const cv::Mat& original,const cv::Mat& mask,
                               const services::FilterResult* validation,OverlayMode mode);
}
