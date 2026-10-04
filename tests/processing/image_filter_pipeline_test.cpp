// V2-2 proof: the Qt-free ImageFilterPipeline runs deterministic, ordered
// preprocessing (identity/invert/linear_contrast/gamma/clahe) that no-ops when
// configured to, fails closed on bad stages, and buildDifferenceImage is the
// single background-difference path — Contract 2 (absolute difference) makes
// bright-on-dark and dark-on-bright objects equivalent while Contract 1
// (saturating subtraction) does not. The bundled kernel routes mask + empty
// through that one helper. See docs/architecture/processing-contract-compatibility.md.
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ImageFilterPipeline.h"
#include "support/assert.h"

#include <opencv2/imgproc.hpp>

#include <string>
#include <vector>

namespace proc = backend::processing;

namespace {

bool matEqual(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size() || a.type() != b.type()) return false;
    cv::Mat diff;
    cv::compare(a, b, diff, cv::CMP_NE);
    return cv::countNonZero(diff) == 0;
}

int maxAbsDiff(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat d;
    cv::absdiff(a, b, d);
    double mx = 0.0;
    cv::minMaxLoc(d, nullptr, &mx);
    return static_cast<int>(mx);
}

int differingPixels(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat d;
    cv::compare(a, b, d, cv::CMP_NE);
    return cv::countNonZero(d);
}

proc::ImageFilterPipeline compileOrDie(const std::vector<proc::ImageFilterStageSpec>& stages) {
    std::string error;
    auto pipeline = proc::ImageFilterPipeline::compile(stages, &error);
    MIB_REQUIRE(pipeline.has_value(), std::string("compile failed: ") + error);
    return *pipeline;
}

cv::Mat rampImage() {
    cv::Mat m(16, 16, CV_8UC1);
    for (int y = 0; y < m.rows; ++y) {
        for (int x = 0; x < m.cols; ++x) {
            m.at<uchar>(y, x) = static_cast<uchar>((y * m.cols + x) % 256);
        }
    }
    return m;
}

proc::ImageFilterStageSpec stage(proc::ImageFilterStageKind kind) {
    proc::ImageFilterStageSpec s;
    s.kind = kind;
    return s;
}

void testNoOps() {
    const cv::Mat in = rampImage();
    cv::Mat out;

    // Default (empty) pipeline is identity.
    proc::ImageFilterPipeline defaultPipeline;
    MIB_EXPECT(defaultPipeline.empty(), "default pipeline is empty/identity");
    defaultPipeline.apply(in, out);
    MIB_EXPECT(matEqual(in, out), "empty pipeline is a no-op");

    // Explicit identity.
    compileOrDie({stage(proc::ImageFilterStageKind::Identity)}).apply(in, out);
    MIB_EXPECT(matEqual(in, out), "identity stage is a no-op");

    // gamma = 1.
    proc::ImageFilterStageSpec gammaOne = stage(proc::ImageFilterStageKind::Gamma);
    gammaOne.gamma = 1.0;
    compileOrDie({gammaOne}).apply(in, out);
    MIB_EXPECT(matEqual(in, out), "gamma=1 is a no-op");

    // linear_contrast(alpha=1, beta=0).
    proc::ImageFilterStageSpec linUnit = stage(proc::ImageFilterStageKind::LinearContrast);
    linUnit.alpha = 1.0;
    linUnit.beta = 0.0;
    compileOrDie({linUnit}).apply(in, out);
    MIB_EXPECT(matEqual(in, out), "linear_contrast(1,0) is a no-op");

    // Two inversions.
    compileOrDie({stage(proc::ImageFilterStageKind::Invert),
                  stage(proc::ImageFilterStageKind::Invert)})
        .apply(in, out);
    MIB_EXPECT(matEqual(in, out), "double invert is a no-op");

    // Single inversion is 255 - x.
    compileOrDie({stage(proc::ImageFilterStageKind::Invert)}).apply(in, out);
    cv::Mat inverted;
    cv::bitwise_not(in, inverted);
    MIB_EXPECT(matEqual(inverted, out), "single invert is 255-x");
}

void testOrderingMatters() {
    cv::Mat in(4, 4, CV_8UC1, cv::Scalar(100));
    proc::ImageFilterStageSpec lin = stage(proc::ImageFilterStageKind::LinearContrast);
    lin.alpha = 2.0;
    lin.beta = 0.0;
    const proc::ImageFilterStageSpec inv = stage(proc::ImageFilterStageKind::Invert);

    cv::Mat invThenLin;
    compileOrDie({inv, lin}).apply(in, invThenLin);
    cv::Mat linThenInv;
    compileOrDie({lin, inv}).apply(in, linThenInv);

    MIB_EXPECT(!matEqual(invThenLin, linThenInv), "non-commuting stages depend on order");
    MIB_EXPECT(invThenLin.at<uchar>(0, 0) == 255, "invert(100)=155 -> *2 saturates to 255");
    MIB_EXPECT(linThenInv.at<uchar>(0, 0) == 55, "100*2=200 -> invert = 55");
}

void testDeterministic() {
    const cv::Mat in = rampImage();
    proc::ImageFilterStageSpec g = stage(proc::ImageFilterStageKind::Clahe);
    g.clipLimit = 3.0;
    g.tileGridSize = 4;
    auto pipeline = compileOrDie({g});
    cv::Mat a;
    cv::Mat b;
    pipeline.apply(in, a);
    pipeline.apply(in, b);
    MIB_EXPECT(matEqual(a, b), "repeated apply is deterministic");
}

void testFailClosed() {
    std::string error;

    proc::ImageFilterStageSpec badGamma = stage(proc::ImageFilterStageKind::Gamma);
    badGamma.gamma = 0.0;
    MIB_EXPECT(!proc::ImageFilterPipeline::compile({badGamma}, &error).has_value(),
               "gamma<=0 fails to compile");

    proc::ImageFilterStageSpec badClahe = stage(proc::ImageFilterStageKind::Clahe);
    badClahe.clipLimit = 0.0;
    MIB_EXPECT(!proc::ImageFilterPipeline::compile({badClahe}, &error).has_value(),
               "clahe clipLimit<=0 fails to compile");

    proc::ImageFilterStageSpec badTiles = stage(proc::ImageFilterStageKind::Clahe);
    badTiles.tileGridSize = 0;
    MIB_EXPECT(!proc::ImageFilterPipeline::compile({badTiles}, &error).has_value(),
               "clahe tileGridSize<1 fails to compile");

    proc::ImageFilterStageKind kind;
    MIB_EXPECT(proc::parseImageFilterStageKind("gamma", kind), "known stage name parses");
    MIB_EXPECT(!proc::parseImageFilterStageKind("sharpen", kind), "unknown stage name rejected");
}

// A uniform gray background with a centered square offset by +delta / -delta.
cv::Mat objectOnField(int base, int delta) {
    cv::Mat m(40, 40, CV_8UC1, cv::Scalar(base));
    cv::rectangle(m, cv::Rect(14, 14, 12, 12), cv::Scalar(base + delta), cv::FILLED);
    return m;
}

void testDifferencePolarity() {
    const cv::Mat background(40, 40, CV_8UC1, cv::Scalar(128));
    const cv::Mat bright = objectOnField(128, +40); // brighter object
    const cv::Mat dark = objectOnField(128, -40);   // darker object
    const cv::Rect region(0, 0, 40, 40);
    const proc::ImageFilterPipeline identity;

    // Contract 2: absolute difference makes the two polarities equivalent.
    cv::Mat diffBrightAbs;
    cv::Mat diffDarkAbs;
    MIB_REQUIRE(proc::buildDifferenceImage(bright, background, region, identity, identity, 3, true,
                                           diffBrightAbs, nullptr),
                "absdiff bright ok");
    MIB_REQUIRE(proc::buildDifferenceImage(dark, background, region, identity, identity, 3, true,
                                           diffDarkAbs, nullptr),
                "absdiff dark ok");
    // Equivalent within the ±1 rounding of OpenCV's integer Gaussian blur.
    MIB_EXPECT(maxAbsDiff(diffBrightAbs, diffDarkAbs) <= 1,
               "Contract 2: bright-on-dark and dark-on-bright differences are equivalent");
    MIB_EXPECT(cv::countNonZero(diffBrightAbs) > 0, "absdiff difference is non-trivial");

    // Contract 1: saturating subtraction is polarity-sensitive.
    cv::Mat diffBrightSub;
    cv::Mat diffDarkSub;
    proc::buildDifferenceImage(bright, background, region, identity, identity, 3, false,
                               diffBrightSub, nullptr);
    proc::buildDifferenceImage(dark, background, region, identity, identity, 3, false, diffDarkSub,
                               nullptr);
    MIB_EXPECT(!matEqual(diffBrightSub, diffDarkSub),
               "Contract 1: subtraction is polarity-sensitive");
    MIB_EXPECT(cv::countNonZero(diffDarkSub) == 0,
               "Contract 1: darker-than-background saturates to zero");
}

void testIncompatibleBackground() {
    const cv::Mat gray(40, 40, CV_8UC1, cv::Scalar(100));
    const cv::Mat wrongSize(20, 20, CV_8UC1, cv::Scalar(50));
    const cv::Rect region(0, 0, 40, 40);
    const proc::ImageFilterPipeline identity;
    std::string error;

    // Contract 2: a supplied-but-incompatible background is a hard error.
    cv::Mat diff;
    MIB_EXPECT(!proc::buildDifferenceImage(gray, wrongSize, region, identity, identity, 3, true,
                                           diff, &error),
               "Contract 2 rejects an incompatible background");
    MIB_EXPECT(!error.empty(), "error message is set");

    // Contract 1: falls back to current-only (legacy behavior).
    cv::Mat diffLenient;
    MIB_EXPECT(proc::buildDifferenceImage(gray, wrongSize, region, identity, identity, 3, false,
                                          diffLenient, nullptr),
               "Contract 1 tolerates an incompatible background");
    cv::Mat currentOnly;
    proc::buildDifferenceImage(gray, cv::Mat{}, region, identity, identity, 3, false, currentOnly,
                               nullptr);
    MIB_EXPECT(matEqual(diffLenient, currentOnly), "Contract 1 fallback equals current-only");
}

void testRegionMatchesCropped() {
    const cv::Mat bright = objectOnField(128, +40);
    const cv::Mat background(40, 40, CV_8UC1, cv::Scalar(128));
    const cv::Rect region(10, 8, 20, 24);
    const proc::ImageFilterPipeline identity;

    cv::Mat viaRegion;
    proc::buildDifferenceImage(bright, background, region, identity, identity, 3, true, viaRegion,
                               nullptr);
    cv::Mat viaCropped;
    proc::buildDifferenceImageCropped(bright(region), background(region), identity, identity, 3,
                                      true, viaCropped, nullptr);
    MIB_EXPECT(matEqual(viaRegion, viaCropped), "region and cropped entry points agree");
}

proc::KernelConfig kernelConfig(bool absolute) {
    proc::KernelConfig c;
    c.gaussianBlurSize = 3;
    c.backgroundSubtractThreshold = 8;
    c.morphologyKernelSize = 3;
    c.morphologyIterations = 1;
    c.emptyFramePixelThreshold = 5;
    c.absoluteBackgroundDifference = absolute;
    return c;
}

void testBundledKernelMask() {
    auto kernel = proc::makeBundledProcessingKernel();
    const cv::Mat background(40, 40, CV_8UC1, cv::Scalar(128));
    const cv::Mat bright = objectOnField(128, +60);
    const cv::Mat dark = objectOnField(128, -60);
    const proc::KernelRoi roi{0, 0, 40, 40};

    // Contract 2 (absolute): equivalent polarities produce equal masks.
    cv::Mat maskBright;
    cv::Mat maskDark;
    std::string error;
    MIB_REQUIRE(kernel->processMask(bright, background, kernelConfig(true), roi, maskBright, &error),
                std::string("mask bright: ") + error);
    MIB_REQUIRE(kernel->processMask(dark, background, kernelConfig(true), roi, maskDark, &error),
                std::string("mask dark: ") + error);
    const int brightCount = cv::countNonZero(maskBright);
    MIB_EXPECT(brightCount > 0 && cv::countNonZero(maskDark) > 0,
               "Contract 2 detects both polarities");
    // Polarity-invariant up to a small boundary discrepancy from blur rounding.
    MIB_EXPECT(differingPixels(maskBright, maskDark) <= brightCount / 20,
               "Contract 2 masks are polarity-invariant (within 5%)");

    // Contract 1 (subtract): the darker object is not detected (legacy behavior).
    cv::Mat maskDarkV1;
    MIB_REQUIRE(kernel->processMask(dark, background, kernelConfig(false), roi, maskDarkV1, &error),
                std::string("mask dark v1: ") + error);
    MIB_EXPECT(cv::countNonZero(maskDarkV1) == 0,
               "Contract 1 saturating subtraction misses the darker object");

    // Full-size, zero-outside-ROI behavior with a sub-ROI.
    const proc::KernelRoi subRoi{14, 14, 12, 12};
    cv::Mat subMask;
    MIB_REQUIRE(kernel->processMask(bright, background, kernelConfig(true), subRoi, subMask, &error),
                std::string("sub-roi mask: ") + error);
    MIB_EXPECT(subMask.rows == 40 && subMask.cols == 40, "mask is full frame size");
    cv::Mat outside = subMask.clone();
    cv::rectangle(outside, cv::Rect(14, 14, 12, 12), cv::Scalar(0), cv::FILLED);
    MIB_EXPECT(cv::countNonZero(outside) == 0, "pixels outside the ROI are zero");
}

} // namespace

int main() {
    testNoOps();
    testOrderingMatters();
    testDeterministic();
    testFailClosed();
    testDifferencePolarity();
    testIncompatibleBackground();
    testRegionMatchesCropped();
    testBundledKernelMask();
    return mib::test::exitCode();
}
