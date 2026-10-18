#include "backend/review/OverlayCompose.h"

#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/ProcessingOverlay.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>

namespace backend::review
{

    // The values cross as the shared kernel's enum: same order, same meaning.
    static_assert(static_cast<int>(recording::OverlayMode::None) == static_cast<int>(OverlayMode::None));
    static_assert(static_cast<int>(recording::OverlayMode::AllContour) == static_cast<int>(OverlayMode::AllContour));
    static_assert(static_cast<int>(recording::OverlayMode::OuterInnerColorCoded) ==
                  static_cast<int>(OverlayMode::OuterInnerColorCoded));
    static_assert(static_cast<int>(recording::OverlayMode::AllMask) == static_cast<int>(OverlayMode::AllMask));
    static_assert(static_cast<int>(recording::OverlayMode::FilteredMask) == static_cast<int>(OverlayMode::FilteredMask));

    cv::Mat composeOverlay(const cv::Mat &original,
                           const cv::Mat &mask,
                           const services::FilterResult *validation,
                           OverlayMode mode)
    {
        // One kernel for every shell (#512): the Qt OverlayRenderer and the
        // facade's render_review_overlay call the same function; review
        // packets keep an undrawn Mono8 original Mono8.
        return recording::renderProcessingOverlay(original, mask, validation,
                                                  static_cast<recording::OverlayMode>(static_cast<int>(mode)),
                                                  recording::OverlayPassthrough::KeepMono);
    }

    void drawRoiOverlay(cv::Mat &image,
                        const services::ProcessingService::Roi &roi,
                        int origWidth,
                        int origHeight)
    {
        if (image.empty() || roi.w <= 0 || roi.h <= 0 || origWidth <= 0 || origHeight <= 0) return;
        if (image.channels() == 1) cv::cvtColor(image, image, cv::COLOR_GRAY2RGB);
        const double scaleX = static_cast<double>(image.cols) / origWidth;
        const double scaleY = static_cast<double>(image.rows) / origHeight;
        int x = static_cast<int>(roi.x * scaleX);
        int y = static_cast<int>(roi.y * scaleY);
        int w = static_cast<int>(roi.w * scaleX);
        int h = static_cast<int>(roi.h * scaleY);
        x = std::max(0, std::min(x, image.cols - 1));
        y = std::max(0, std::min(y, image.rows - 1));
        w = std::max(1, std::min(w, image.cols - x));
        h = std::max(1, std::min(h, image.rows - y));
        cv::rectangle(image, cv::Rect(x, y, w, h), cv::Scalar(255, 0, 0), 3);
    }

    void packImage(const cv::Mat &image, std::uint64_t frameIndex, ReviewImage &out)
    {
        out = ReviewImage{};
        if (image.empty()) return;
        cv::Mat src = image;
        if (src.channels() == 4) cv::cvtColor(src, src, cv::COLOR_RGBA2RGB);
        if (src.depth() != CV_8U) src.convertTo(src, CV_8U);
        out.frameIndex = frameIndex;
        out.width = static_cast<std::uint64_t>(src.cols);
        out.height = static_cast<std::uint64_t>(src.rows);
        out.channels = static_cast<std::uint32_t>(src.channels());
        const std::size_t rowBytes = static_cast<std::size_t>(src.cols) * src.channels();
        out.data.resize(rowBytes * static_cast<std::size_t>(src.rows));
        for (int r = 0; r < src.rows; ++r)
        {
            std::memcpy(out.data.data() + static_cast<std::size_t>(r) * rowBytes, src.ptr(r), rowBytes);
        }
    }

} // namespace backend::review
