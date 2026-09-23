#include "frontend/utils/OverlayRenderer.h"
#include "backend/recording/ProcessingOverlay.h"
namespace frontend {
QImage createProcessingOverlay(const cv::Mat& original,const cv::Mat& mask,
                               const backend::services::FilterResult* validation,OverlayMode mode) {
    const auto rgb=backend::recording::renderProcessingOverlay(original,mask,validation,
        static_cast<backend::recording::OverlayMode>(mode));
    if(rgb.empty())return {};
    return QImage(rgb.data,rgb.cols,rgb.rows,static_cast<int>(rgb.step),QImage::Format_RGB888).copy();
}
} // namespace frontend
