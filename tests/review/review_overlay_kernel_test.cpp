// review_overlay_kernel_test (#512 slice 1, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md)
//
// One overlay kernel behind every review shell. review::composeOverlay
// (ReviewSession: YOFO Review's packets) now delegates to
// recording::renderProcessingOverlay, the kernel the Qt OverlayRenderer and
// the facade's render_review_overlay already call. This test proves, over a
// matrix of originals (Mono8 / BGR / BGRA), masks (empty, no contours, disc,
// ring, two holes, ring + disc, nested, smaller mask with value 1), classes
// (null, invalid, valid, single-inner, target) and modes (0-4 and an unknown
// value):
//  - composeOverlay is byte-identical to the implementation it replaced
//    (`legacy::composeOverlay` below, copied verbatim from
//    src/backend/review/OverlayCompose.cpp at 2a4e0a4f as the oracle);
//  - every drawn pixel equals the Qt/facade output (OverlayPassthrough::Rgb),
//    and a Mono8 result is exactly the undrawn original;
//  - the default argument keeps the Qt/facade output (Rgb) unchanged.
// Watchdog guarded.

#include "backend/recording/ProcessingOverlay.h"
#include "backend/review/OverlayCompose.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using backend::review::OverlayMode;
using backend::services::FilterResult;

namespace legacy
{
    // ---- oracle: pre-#512 review::composeOverlay, verbatim -------------------
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

    cv::Vec3b classificationColor(const FilterResult *v)
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
                        pixel[c] = static_cast<uchar>(std::min(255.0, pixel[c] * 0.7 + tint[c] * 0.3));
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

    cv::Mat composeOverlay(const cv::Mat &original, const cv::Mat &mask, const FilterResult *validation,
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
} // namespace legacy

namespace
{
    bool sameMat(const cv::Mat &a, const cv::Mat &b)
    {
        if (a.empty() || b.empty()) return a.empty() && b.empty();
        if (a.type() != b.type() || a.size() != b.size()) return false;
        return cv::norm(a.reshape(1), b.reshape(1), cv::NORM_INF) == 0;
    }

    cv::Mat pattern(int channels)
    {
        cv::Mat m(24, 32, CV_8UC(channels));
        for (int y = 0; y < m.rows; ++y)
            for (int x = 0; x < m.cols; ++x)
                for (int c = 0; c < channels; ++c)
                    m.ptr<uint8_t>(y)[x * channels + c] = static_cast<uint8_t>((x * 3 + y * 5 + c * 41) % 200);
        return m;
    }

    std::vector<std::pair<std::string, cv::Mat>> masks()
    {
        std::vector<std::pair<std::string, cv::Mat>> out;
        out.emplace_back("empty", cv::Mat());
        out.emplace_back("zeros", cv::Mat::zeros(24, 32, CV_8UC1));
        cv::Mat disc = cv::Mat::zeros(24, 32, CV_8UC1);
        cv::circle(disc, {16, 12}, 8, cv::Scalar(255), -1);
        out.emplace_back("disc", disc);
        cv::Mat ring = disc.clone();
        cv::circle(ring, {16, 12}, 3, cv::Scalar(0), -1);
        out.emplace_back("ring", ring);
        cv::Mat two = cv::Mat::zeros(24, 32, CV_8UC1);
        cv::rectangle(two, {2, 2}, {29, 21}, cv::Scalar(255), -1);
        cv::circle(two, {9, 11}, 3, cv::Scalar(0), -1);
        cv::circle(two, {22, 11}, 3, cv::Scalar(0), -1);
        out.emplace_back("two_holes", two);
        cv::Mat pair = cv::Mat::zeros(24, 32, CV_8UC1);
        cv::circle(pair, {8, 12}, 6, cv::Scalar(255), -1);
        cv::circle(pair, {8, 12}, 2, cv::Scalar(0), -1);
        cv::circle(pair, {24, 12}, 6, cv::Scalar(255), -1);
        out.emplace_back("ring_and_disc", pair);
        cv::Mat nested = cv::Mat::zeros(24, 32, CV_8UC1);
        cv::rectangle(nested, {1, 1}, {30, 22}, cv::Scalar(255), -1);
        cv::rectangle(nested, {5, 5}, {26, 18}, cv::Scalar(0), -1);
        cv::rectangle(nested, {9, 8}, {22, 15}, cv::Scalar(255), -1);
        cv::rectangle(nested, {13, 10}, {18, 13}, cv::Scalar(0), -1);
        out.emplace_back("nested", nested);
        cv::Mat small = cv::Mat::zeros(20, 20, CV_8UC1);
        cv::circle(small, {10, 10}, 7, cv::Scalar(1), -1);
        cv::circle(small, {10, 10}, 2, cv::Scalar(0), -1);
        out.emplace_back("small_value_1", small);
        return out;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(60);

    const std::vector<std::pair<std::string, cv::Mat>> originals{
        {"mono", pattern(1)}, {"bgr", pattern(3)}, {"bgra", pattern(4)}};
    std::vector<std::pair<std::string, std::optional<FilterResult>>> classes;
    classes.emplace_back("null", std::nullopt);
    FilterResult v;
    classes.emplace_back("invalid", v);
    v.isValid = true;
    classes.emplace_back("valid", v);
    v.hasSingleInnerContour = true;
    classes.emplace_back("valid_single_inner", v);
    v.isTargetGroup = true;
    classes.emplace_back("target_single_inner", v);
    v.hasSingleInnerContour = false;
    classes.emplace_back("target", v);

    int cases = 0, mono = 0, drawn = 0;
    for (const auto &[originalName, original] : originals)
        for (const auto &[maskName, mask] : masks())
            for (const auto &[className, cls] : classes)
                for (const std::uint32_t modeValue : {0u, 1u, 2u, 3u, 4u, 7u})
                {
                    const FilterResult *validation = cls ? &*cls : nullptr;
                    const auto mode = static_cast<OverlayMode>(modeValue);
                    const auto qtMode = static_cast<backend::recording::OverlayMode>(modeValue);
                    const std::string label = originalName + "/" + maskName + "/" + className + "/mode " +
                                              std::to_string(modeValue);
                    const cv::Mat composed = backend::review::composeOverlay(original, mask, validation, mode);
                    const cv::Mat oracle = legacy::composeOverlay(original, mask, validation, mode);
                    MIB_EXPECT(sameMat(composed, oracle), "identical to the pre-#512 composeOverlay: " + label);

                    const cv::Mat qt = backend::recording::renderProcessingOverlay(original, mask, validation, qtMode);
                    const cv::Mat qtExplicit = backend::recording::renderProcessingOverlay(
                        original, mask, validation, qtMode, backend::recording::OverlayPassthrough::Rgb);
                    MIB_EXPECT(sameMat(qt, qtExplicit), "default passthrough is Rgb: " + label);
                    MIB_EXPECT(qt.type() == CV_8UC3, "Qt/facade output is RGB888: " + label);
                    if (composed.type() == CV_8UC1)
                    {
                        ++mono;
                        MIB_EXPECT(original.channels() == 1 && sameMat(composed, original),
                                   "Mono8 result is the undrawn original: " + label);
                        cv::Mat rgb;
                        cv::cvtColor(original, rgb, cv::COLOR_GRAY2RGB);
                        MIB_EXPECT(sameMat(qt, rgb), "Qt/facade returns the same original as RGB: " + label);
                    }
                    else
                    {
                        ++drawn;
                        MIB_EXPECT(sameMat(composed, qt), "drawn pixels equal the Qt/facade kernel: " + label);
                    }
                    ++cases;
                }
    MIB_EXPECT(cases == 3 * 8 * 6 * 6, "full matrix ran");
    MIB_EXPECT(mono > 0 && drawn > 0, "matrix covers both passthrough and drawn results");

    // Empty original: nothing to compose in either path.
    MIB_EXPECT(backend::review::composeOverlay(cv::Mat(), masks()[3].second, nullptr, OverlayMode::AllContour).empty(),
               "empty original composes to empty");
    MIB_EXPECT(backend::recording::renderProcessingOverlay(cv::Mat(), masks()[3].second, nullptr,
                                                           backend::recording::OverlayMode::AllContour)
                   .empty(),
               "empty original renders to empty");

    return mib::test::exitCode();
}
