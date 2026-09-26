// V2-7 release gate (deterministic portion): a single synthetic conformance
// check that ties together the Contract-2 properties delivered by V2-2 and
// V2-3 — polarity symmetry, filter identity/order, progressive blur, object
// isolation, tiny/no-object handling, and invalid-background rejection — with
// explicit, reproducible expectations. The real-corpus, hardware, MLflow, and
// native-plugin conformance are tracked in docs/processing-contract-v2-validation.md.
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ImageFilterPipeline.h"
#include "backend/processing/ProcessingContract.h"
#include "backend/processing/ProcessingScience.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "support/assert.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace proc = backend::processing;
namespace science = backend::processing::science;
using backend::services::ProcessingConfig;

namespace {

int maxAbsDiff(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat d;
    cv::absdiff(a, b, d);
    double mx = 0.0;
    cv::minMaxLoc(d, nullptr, &mx);
    return static_cast<int>(mx);
}

std::vector<cv::Point> rectContour(const cv::Rect& r) {
    return {{r.x, r.y}, {r.x + r.width - 1, r.y}, {r.x + r.width - 1, r.y + r.height - 1},
            {r.x, r.y + r.height - 1}};
}

cv::Mat checkerObject(const cv::Size& size, const cv::Rect& obj, int bg) {
    cv::Mat m(size, CV_8UC1, cv::Scalar(bg));
    for (int y = obj.y; y < obj.y + obj.height; ++y)
        for (int x = obj.x; x < obj.x + obj.width; ++x)
            m.at<uchar>(y, x) = ((x + y) % 2 == 0) ? 0 : 255;
    return m;
}

// C-1: absolute difference is polarity-symmetric; saturating subtraction is not.
void conformPolaritySymmetry() {
    const cv::Mat bg(40, 40, CV_8UC1, cv::Scalar(128));
    cv::Mat bright(40, 40, CV_8UC1, cv::Scalar(128));
    cv::Mat dark(40, 40, CV_8UC1, cv::Scalar(128));
    cv::rectangle(bright, cv::Rect(14, 14, 12, 12), cv::Scalar(168), cv::FILLED);
    cv::rectangle(dark, cv::Rect(14, 14, 12, 12), cv::Scalar(88), cv::FILLED);
    const cv::Rect region(0, 0, 40, 40);
    const proc::ImageFilterPipeline id;

    cv::Mat ab, ad;
    proc::buildDifferenceImage(bright, bg, region, id, id, 3, true, ab, nullptr);
    proc::buildDifferenceImage(dark, bg, region, id, id, 3, true, ad, nullptr);
    MIB_EXPECT(maxAbsDiff(ab, ad) <= 1, "conformance: absdiff polarity-symmetric");

    cv::Mat sd;
    proc::buildDifferenceImage(dark, bg, region, id, id, 3, false, sd, nullptr);
    MIB_EXPECT(cv::countNonZero(sd) == 0, "conformance: subtract drops darker-than-bg");
}

// C-2: identity filter chain equals the omitted baseline; ordering matters.
void conformFilters() {
    const cv::Mat in = checkerObject({24, 24}, cv::Rect(4, 4, 16, 16), 128);
    cv::Mat viaIdentity, baseline;
    proc::ImageFilterStageSpec identity;
    std::string e;
    proc::ImageFilterPipeline::compile({identity}, &e)->apply(in, viaIdentity);
    proc::ImageFilterPipeline().apply(in, baseline); // empty == identity
    MIB_EXPECT(maxAbsDiff(viaIdentity, baseline) == 0, "conformance: identity == baseline");

    proc::ImageFilterStageSpec inv;
    inv.kind = proc::ImageFilterStageKind::Invert;
    proc::ImageFilterStageSpec lin;
    lin.kind = proc::ImageFilterStageKind::LinearContrast;
    lin.alpha = 2.0;
    cv::Mat a, b;
    proc::ImageFilterPipeline::compile({inv, lin}, &e)->apply(in, a);
    proc::ImageFilterPipeline::compile({lin, inv}, &e)->apply(in, b);
    MIB_EXPECT(maxAbsDiff(a, b) > 0, "conformance: filter order is significant");
}

// C-3: focus score drops under blur, is preserved under inversion, and excludes
// background/adjacent pixels.
void conformFocusMetric() {
    const cv::Rect obj(10, 10, 20, 20);
    const auto contour = rectContour(obj);
    const cv::Mat sharp = checkerObject({40, 40}, obj, 128);
    cv::Mat blurred, inverted;
    cv::GaussianBlur(sharp, blurred, cv::Size(5, 5), 0);
    cv::bitwise_not(sharp, inverted);

    const double vs = science::calculateLaplacianVariance(sharp, contour);
    const double vb = science::calculateLaplacianVariance(blurred, contour);
    const double vi = science::calculateLaplacianVariance(inverted, contour);
    MIB_EXPECT(vb < vs * 0.75, "conformance: blur lowers focus score");
    MIB_EXPECT(std::abs(vs - vi) <= vs * 1e-6 + 1e-6, "conformance: inversion preserves score");

    // Adjacent bright neighbour beyond the object is excluded by the mask.
    cv::Mat withNeighbour = sharp.clone();
    cv::rectangle(withNeighbour, cv::Rect(34, 34, 4, 4), cv::Scalar(255), cv::FILLED);
    const double vn = science::calculateLaplacianVariance(withNeighbour, contour);
    MIB_EXPECT(std::abs(vn - vs) <= 1e-9, "conformance: neighbour outside crop excluded");
}

// C-4: tiny/no-object determinism and invalid-background rejection.
void conformDegenerate() {
    const cv::Mat img(40, 40, CV_8UC1, cv::Scalar(100));
    MIB_EXPECT(std::isnan(science::calculateLaplacianVariance(img, {})),
               "conformance: empty contour -> NaN");

    ProcessingConfig cfg;
    const cv::Mat empty(40, 40, CV_8UC1, cv::Scalar(0));
    auto none = science::filterProcessedObjects(empty, cv::Rect(0, 0, 40, 40), cfg, img, 0.5,
                                                nullptr);
    MIB_REQUIRE(none.size() == 1, "conformance: empty frame -> one empty record");
    MIB_EXPECT(std::isnan(none.front().laplacianVariance), "conformance: no detection -> NaN score");

    const cv::Mat wrongBg(20, 20, CV_8UC1, cv::Scalar(50));
    const proc::ImageFilterPipeline id;
    cv::Mat diff;
    std::string err;
    MIB_EXPECT(!proc::buildDifferenceImage(img, wrongBg, cv::Rect(0, 0, 40, 40), id, id, 3, true,
                                           diff, &err),
               "conformance: Contract-2 rejects an incompatible background");
}

// C-5: the Laplacian gate stays disabled by default (calibration pending), so
// Contract-2 does not silently invalidate objects on an uncalibrated score.
void conformGateDefaultOff() {
    ProcessingConfig cfg;
    MIB_EXPECT(!cfg.enable_laplacian_variance_check, "conformance: focus gate disabled by default");
    backend::services::FilterResult r;
    r.laplacianVariance = std::numeric_limits<double>::quiet_NaN();
    cfg.require_single_inner_contour = false;
    cfg.enable_border_check = false;
    cfg.enable_area_range_check = false;
    for (auto reason : science::classifyInvalidReasons(r, cfg, 1.0)) {
        MIB_EXPECT(reason != science::InvalidReasonCode::Laplacian,
                   "conformance: disabled gate never fails an object");
    }
}

// C-7: contract selection is a ProcessingConfig property executed by the host
// service (and therefore by the Python wheel): Contract 2 detects a dark-on-
// bright object through cv::absdiff, abolishes ring width (NaN, gate ignored)
// and carries a finite per-object Laplacian variance; Contract 1 on the same
// frame is unchanged (saturating subtraction sees nothing, ring stays a
// number); an unsupported contract fails closed.
void conformServiceContractSelection() {
    using backend::services::ProcessedFrame;
    using backend::services::ProcessingService;
    const cv::Mat bg(60, 80, CV_8UC1, cv::Scalar(128));
    cv::Mat frame = bg.clone();
    cv::rectangle(frame, cv::Rect(30, 20, 20, 20), cv::Scalar(88), cv::FILLED); // dark object

    backend::services::ProcessingConfig cfg;
    cfg.enable_area_range_check = false;
    cfg.enable_border_check = false;
    cfg.require_single_inner_contour = false;
    cfg.enable_ring_ratio_check = true; // must be ignored under Contract 2
    cfg.ring_ratio_min = 15.0;
    cfg.ring_ratio_max = 25.0;
    cfg.bg_subtract_threshold = 8;
    const ProcessingService::Roi roi{0, 0, 80, 60};

    ProcessingService service;
    cfg.processing_contract_version = 1;
    const ProcessedFrame v1 = service.computeProcessedFrame(frame, bg, cfg, roi, 0, 0);
    MIB_EXPECT(v1.validation.objectCount == 0, "contract 1: saturating subtraction ignores a dark object");
    MIB_EXPECT(!std::isnan(v1.validation.ringRatio), "contract 1: ring width stays a number");

    cfg.processing_contract_version = 2;
    const ProcessedFrame v2 = service.computeProcessedFrame(frame, bg, cfg, roi, 0, 0);
    MIB_REQUIRE(v2.validation.objectCount == 1, "contract 2: absdiff detects the dark object");
    MIB_EXPECT(v2.validation.isValid, "contract 2: ring gate is ignored (object valid)");
    MIB_EXPECT(std::isnan(v2.validation.ringRatio), "contract 2: ring width is NaN");
    MIB_EXPECT(std::isfinite(v2.validation.laplacianVariance) && v2.validation.laplacianVariance > 0.0,
               "contract 2: finite per-object Laplacian variance");
    const auto reasons = science::classifyInvalidReasons(v2.validation, cfg, 0.5);
    MIB_EXPECT(reasons.empty(), "contract 2: no Ring invalid reason");

    // Contract 2 objects are top-level contours: a solid blob with a hole is one
    // object even when the (Contract-1) single-inner-contour rule is on, and a
    // hole-free blob is not rejected as "no contour".
    cv::Mat holed = frame.clone();
    cv::rectangle(holed, cv::Rect(38, 28, 4, 4), cv::Scalar(128), cv::FILLED); // hole in the dark object
    cfg.require_single_inner_contour = true;
    const ProcessedFrame v2h = service.computeProcessedFrame(holed, bg, cfg, roi, 0, 0);
    MIB_EXPECT(v2h.validation.objectCount == 1 && v2h.validation.area > 300.0,
               "contract 2: the top-level blob is the object, not the hole");
    const ProcessedFrame v2s = service.computeProcessedFrame(frame, bg, cfg, roi, 0, 0);
    MIB_EXPECT(v2s.validation.objectCount == 1 && v2s.validation.isValid,
               "contract 2: hole-free blob is valid despite require_single_inner_contour");
    MIB_EXPECT(science::classifyInvalidReasons(v2s.validation, cfg, 0.5).empty(),
               "contract 2: no NoContour reason for a hole-free blob");
    cfg.require_single_inner_contour = false;

    cfg.processing_contract_version = 3;
    const ProcessedFrame v3 = service.computeProcessedFrame(frame, bg, cfg, roi, 0, 0);
    MIB_EXPECT(v3.processedImage.empty(), "unsupported contract fails closed (no mask)");
    MIB_EXPECT(v3.validation.objectCount == 0, "unsupported contract fails closed (no objects)");

    MIB_EXPECT(proc::contract::contractUsesAbsoluteDifference(2) && !proc::contract::contractUsesAbsoluteDifference(1),
               "contract helpers: absdiff is Contract 2 only");
    MIB_EXPECT(proc::contract::contractHasRingWidth(1) && !proc::contract::contractHasRingWidth(2),
               "contract helpers: ring width is Contract 1 only");
}

} // namespace

int main() {
    conformPolaritySymmetry();
    conformFilters();
    conformFocusMetric();
    conformDegenerate();
    conformGateDefaultOff();
    conformServiceContractSelection();
    return mib::test::exitCode();
}
