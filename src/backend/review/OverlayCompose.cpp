#include "backend/review/OverlayCompose.h"

#include "backend/processing/ProcessingTypes.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>

namespace backend::review
{

    namespace
    {
        cv::Mat toRgb(const cv::Mat &original)
        {
            if (original.empty()) return {};
            cv::Mat rgb;
            if (original.channels() == 1)
            {
                cv::cvtColor(original, rgb, cv::COLOR_GRAY2RGB);
            }
            else
            {
                rgb = original.clone();
                if (rgb.channels() == 3) cv::cvtColor(rgb, rgb, cv::COLOR_BGR2RGB);
                else if (rgb.channels() == 4) cv::cvtColor(rgb, rgb, cv::COLOR_BGRA2RGB);
            }
            return rgb;
        }

        // Blue = target group, green = valid, red = invalid (RGB order).
        cv::Vec3b classificationColor(const services::FilterResult *v)
        {
            if (v && v->isTargetGroup) return {0, 120, 255};
            if (v && v->isValid) return {0, 255, 0};
            return {255, 0, 0};
        }

        void applyMaskTint(cv::Mat &rgb, const cv::Mat &mask, const cv::Vec3b &tint)
        {
            if (rgb.empty() || mask.empty()) return;
            for (int y = 0; y < rgb.rows && y < mask.rows; ++y)
            {
                for (int x = 0; x < rgb.cols && x < mask.cols; ++x)
                {
                    if (mask.at<uchar>(y, x) > 0)
                    {
                        cv::Vec3b &pixel = rgb.at<cv::Vec3b>(y, x);
                        for (int c = 0; c < 3; ++c)
                        {
                            pixel[c] = static_cast<uchar>(
                                std::min(255.0, pixel[c] * 0.7 + tint[c] * 0.3));
                        }
                    }
                }
            }
        }

        cv::Mat passthrough(const cv::Mat &original)
        {
            if (original.channels() == 1) return original.clone();
            return toRgb(original);
        }
    } // namespace

    cv::Mat composeOverlay(const cv::Mat &original,
                           const cv::Mat &mask,
                           const services::FilterResult *validation,
                           OverlayMode mode)
    {
        if (original.empty()) return {};
        if (mode == OverlayMode::None) return passthrough(original);
        cv::Mat rgb = toRgb(original);
        if (rgb.empty()) return {};

        if (mode == OverlayMode::AllMask)
        {
            if (mask.empty()) return passthrough(original);
            applyMaskTint(rgb, mask, classificationColor(validation));
            return rgb;
        }

        if (mask.empty()) return passthrough(original);

        std::vector<std::vector<cv::Point>> contours;
        cv::Mat hierarchyMat;
        cv::findContours(mask.clone(), contours, hierarchyMat, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);
        const int n = static_cast<int>(contours.size());
        if (n == 0) return passthrough(original);
        const bool hasHierarchy = !hierarchyMat.empty() && hierarchyMat.rows == 1 && hierarchyMat.cols == n;

        if (mode == OverlayMode::AllContour)
        {
            const cv::Vec3b c = classificationColor(validation);
            cv::drawContours(rgb, contours, -1, cv::Scalar(c[0], c[1], c[2]), 2);
            return rgb;
        }

        if (mode == OverlayMode::OuterInnerColorCoded && hasHierarchy)
        {
            const cv::Vec3b c = classificationColor(validation);
            const cv::Scalar outerColor(c[0], c[1], c[2]);
            const cv::Scalar innerColor(std::min(255, c[0] + 80), std::min(255, c[1] + 80),
                                        std::min(255, c[2] + 80));
            for (int i = 0; i < n; ++i)
            {
                const int parent = hierarchyMat.at<cv::Vec4i>(0, i)[3];
                cv::drawContours(rgb, contours, i, parent < 0 ? outerColor : innerColor, 2);
            }
            return rgb;
        }

        if (mode == OverlayMode::FilteredMask && hasHierarchy)
        {
            int acceptedOuter = -1;
            for (int i = 0; i < n; ++i)
            {
                const int parent = hierarchyMat.at<cv::Vec4i>(0, i)[3];
                if (parent >= 0) continue;
                const int firstChild = hierarchyMat.at<cv::Vec4i>(0, i)[2];
                if (firstChild < 0) continue;
                int childCount = 0;
                for (int c = firstChild; c >= 0; c = hierarchyMat.at<cv::Vec4i>(0, c)[0]) ++childCount;
                if (validation && validation->hasSingleInnerContour)
                {
                    if (childCount == 1) { acceptedOuter = i; break; }
                }
                else if (childCount >= 1)
                {
                    acceptedOuter = i;
                    break;
                }
            }
            if (acceptedOuter >= 0)
            {
                cv::Mat filteredMask = cv::Mat::zeros(mask.rows, mask.cols, CV_8UC1);
                cv::drawContours(filteredMask, contours, acceptedOuter, cv::Scalar(255), -1);
                const int firstChild = hierarchyMat.at<cv::Vec4i>(0, acceptedOuter)[2];
                for (int c = firstChild; c >= 0; c = hierarchyMat.at<cv::Vec4i>(0, c)[0])
                {
                    cv::drawContours(filteredMask, contours, c, cv::Scalar(0), -1);
                }
                applyMaskTint(rgb, filteredMask, classificationColor(validation));
            }
            return rgb;
        }

        return passthrough(original);
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
