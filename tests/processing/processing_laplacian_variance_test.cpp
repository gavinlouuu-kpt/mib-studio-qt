// V2-3 proof: the Contract-2 object focus metric (per-object Laplacian
// variance) is computed only after detection, once per emitted object, from
// the object's own contour; masking is applied to the variance statistic, not
// before convolution. Progressive blur lowers the score, grayscale inversion
// preserves it, and neighbouring/background pixels outside the object mask do
// not contribute. See ADR 0001 and docs/architecture/processing-contract-compatibility.md.
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingTypes.h"
#include "support/assert.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace science = backend::processing::science;
using backend::services::ProcessingConfig;

namespace {

std::vector<cv::Point> rectContour(const cv::Rect& r) {
    return {{r.x, r.y}, {r.x + r.width - 1, r.y}, {r.x + r.width - 1, r.y + r.height - 1},
            {r.x, r.y + r.height - 1}};
}

// Flat background with a deterministic 1px checkerboard inside `object`.
cv::Mat checkerObject(const cv::Size& size, const cv::Rect& object, int bg) {
    cv::Mat m(size, CV_8UC1, cv::Scalar(bg));
    for (int y = object.y; y < object.y + object.height; ++y) {
        for (int x = object.x; x < object.x + object.width; ++x) {
            m.at<uchar>(y, x) = ((x + y) % 2 == 0) ? 0 : 255;
        }
    }
    return m;
}

void testBlurReducesScore() {
    const cv::Rect object(10, 10, 20, 20);
    const auto contour = rectContour(object);
    const cv::Mat sharp = checkerObject({40, 40}, object, 128);
    cv::Mat blurred;
    cv::GaussianBlur(sharp, blurred, cv::Size(5, 5), 0);

    const double sharpVar = science::calculateLaplacianVariance(sharp, contour);
    const double blurVar = science::calculateLaplacianVariance(blurred, contour);
    MIB_REQUIRE(std::isfinite(sharpVar) && std::isfinite(blurVar), "both variances finite");
    MIB_EXPECT(sharpVar > 0.0, "sharp textured object has non-zero focus score");
    MIB_EXPECT(blurVar < sharpVar * 0.75, "progressive blur lowers the focus score");
}

void testInversionPreservesScore() {
    const cv::Rect object(10, 10, 20, 20);
    const auto contour = rectContour(object);
    const cv::Mat sharp = checkerObject({40, 40}, object, 128);
    cv::Mat inverted;
    cv::bitwise_not(sharp, inverted); // 255 - I

    const double var = science::calculateLaplacianVariance(sharp, contour);
    const double invVar = science::calculateLaplacianVariance(inverted, contour);
    MIB_REQUIRE(std::isfinite(var) && std::isfinite(invVar), "both variances finite");
    // Laplacian is linear: L(255 - I) = -L(I), so the variance is unchanged.
    MIB_EXPECT(std::abs(var - invVar) <= var * 1e-6 + 1e-6,
               "grayscale inversion preserves the focus score");
}

void testIsolationFromNeighbours() {
    const cv::Rect object(12, 12, 16, 16);
    const auto contour = rectContour(object);
    const cv::Mat base = checkerObject({48, 48}, object, 128);
    const double baseVar = science::calculateLaplacianVariance(base, contour);
    MIB_REQUIRE(std::isfinite(baseVar) && baseVar > 0.0, "base focus score is finite/positive");

    // Fill everything more than 2px outside the object with heavy texture — an
    // adjacent object / noisy background. Object pixels and their immediate
    // neighbours are untouched, so the masked variance must not change.
    cv::Mat objectMask(base.size(), CV_8UC1, cv::Scalar(0));
    const std::vector<std::vector<cv::Point>> polys{contour};
    cv::drawContours(objectMask, polys, 0, cv::Scalar(255), cv::FILLED);
    cv::Mat dilated;
    cv::dilate(objectMask, dilated, cv::getStructuringElement(cv::MORPH_RECT, {5, 5}));

    cv::Mat noisy = base.clone();
    for (int y = 0; y < noisy.rows; ++y) {
        for (int x = 0; x < noisy.cols; ++x) {
            if (dilated.at<uchar>(y, x) == 0) {
                noisy.at<uchar>(y, x) = static_cast<uchar>((x * 37 + y * 101) % 256);
            }
        }
    }
    const double noisyVar = science::calculateLaplacianVariance(noisy, contour);
    MIB_EXPECT(std::abs(noisyVar - baseVar) <= 1e-9,
               "background/adjacent pixels outside the mask do not contribute");
}

void testNanCases() {
    const cv::Mat img(40, 40, CV_8UC1, cv::Scalar(100));
    MIB_EXPECT(std::isnan(science::calculateLaplacianVariance(img, {})),
               "empty contour yields NaN");
    MIB_EXPECT(std::isnan(science::calculateLaplacianVariance(cv::Mat{}, rectContour({5, 5, 5, 5}))),
               "empty image yields NaN");
    // A contour entirely outside the image clips to an empty crop.
    MIB_EXPECT(std::isnan(science::calculateLaplacianVariance(img, rectContour({100, 100, 8, 8}))),
               "off-image contour yields NaN");
}

// Two separated filled blobs, each with a different interior texture.
void testPerObjectIndependenceAndNoDetection() {
    // No detection -> no Laplacian operation (result carries NaN).
    ProcessingConfig config;
    config.require_single_inner_contour = false;
    const cv::Mat empty(60, 60, CV_8UC1, cv::Scalar(0));
    const cv::Mat original(60, 60, CV_8UC1, cv::Scalar(100));
    auto none = science::filterProcessedObjects(empty, cv::Rect(0, 0, 60, 60), config, original,
                                                0.5, nullptr);
    MIB_REQUIRE(none.size() == 1, "empty frame emits a single empty result");
    MIB_EXPECT(std::isnan(none.front().laplacianVariance), "no detection -> NaN focus score");

    // Two blobs in the processed (binary) image.
    cv::Mat processed(60, 60, CV_8UC1, cv::Scalar(0));
    const cv::Rect blobA(8, 20, 14, 14);
    const cv::Rect blobB(38, 20, 14, 14);
    cv::rectangle(processed, blobA, cv::Scalar(255), cv::FILLED);
    cv::rectangle(processed, blobB, cv::Scalar(255), cv::FILLED);

    // Original: blob A is a fine checkerboard (high focus), blob B is flat (low).
    cv::Mat orig(60, 60, CV_8UC1, cv::Scalar(120));
    for (int y = blobA.y; y < blobA.y + blobA.height; ++y) {
        for (int x = blobA.x; x < blobA.x + blobA.width; ++x) {
            orig.at<uchar>(y, x) = ((x + y) % 2 == 0) ? 0 : 255;
        }
    }

    auto results = science::filterProcessedObjects(processed, cv::Rect(0, 0, 60, 60), config, orig,
                                                   0.5, nullptr);
    MIB_REQUIRE(results.size() == 2, "two objects emit two results");
    MIB_EXPECT(std::isfinite(results[0].laplacianVariance) &&
                   std::isfinite(results[1].laplacianVariance),
               "each emitted object has a finite focus score");
    // Ordering is left-to-right, so results[0] is blob A (textured).
    MIB_EXPECT(results[0].laplacianVariance > results[1].laplacianVariance,
               "the textured object scores higher than the flat one (independent values)");
}

void testGateDisabledByDefault() {
    // Reach the metric gates: no inner-contour requirement, no border/area gate.
    ProcessingConfig config;
    config.require_single_inner_contour = false;
    config.enable_border_check = false;
    config.enable_area_range_check = false;

    backend::services::FilterResult result;
    result.laplacianVariance = std::numeric_limits<double>::quiet_NaN();

    // With the gate disabled (default), a NaN focus score never invalidates.
    for (const auto r : science::classifyInvalidReasons(result, config, 1.0)) {
        MIB_EXPECT(r != science::InvalidReasonCode::Laplacian,
                   "disabled gate never reports a Laplacian reason");
    }

    // Enabled with the object outside [min,max] -> reported.
    config.enable_laplacian_variance_check = true;
    config.laplacian_variance_min = 10.0;
    config.laplacian_variance_max = 20.0;
    result.laplacianVariance = 5.0; // below range
    bool found = false;
    for (const auto r : science::classifyInvalidReasons(result, config, 1.0)) {
        if (r == science::InvalidReasonCode::Laplacian) found = true;
    }
    MIB_EXPECT(found, "enabled gate reports out-of-range focus score");
}

} // namespace

int main() {
    testBlurReducesScore();
    testInversionPreservesScore();
    testIsolationFromNeighbours();
    testNanCases();
    testPerObjectIndependenceAndNoDetection();
    testGateDisabledByDefault();
    return mib::test::exitCode();
}
