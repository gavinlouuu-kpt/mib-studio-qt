// A7 golden baseline: pins the exact scientific outputs of the desktop
// pipeline (contour extraction, metrics, range gating, brightness quantiles,
// deterministic object ordering, and batch tracking) over deterministic
// synthetic frames. Captured BEFORE the science moved behind
// IProcessingKernel so the migration can prove golden behavior is preserved
// bit-for-bit. Contract 2 must use separate goldens; do not re-baseline these
// Contract-1 values to accommodate a new algorithm.
#include "backend/processing/IProcessingKernel.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kTolerance = 1e-9;

std::string describe(const backend::services::FilterResult& validation) {
    std::ostringstream out;
    out.precision(12);
    out << "objectId=" << validation.objectId << " objectCount=" << validation.objectCount
        << " valid=" << validation.isValid << " inRange=" << validation.inRange
        << " target=" << validation.isTargetGroup << " border=" << validation.touchesBorder
        << " innerCount=" << validation.innerContourCount << " trackId=" << validation.trackId
        << " trackFirst=" << validation.trackFirstFrame
        << " trackLast=" << validation.trackLastFrame
        << " trackObs=" << validation.trackObservationCount << " bbox=(" << validation.bboxX
        << "," << validation.bboxY << "," << validation.bboxWidth << ","
        << validation.bboxHeight << ")"
        << " centroid=(" << validation.centroidX << "," << validation.centroidY << ")"
        << " area=" << validation.area << " deform=" << validation.deformability
        << " areaRatio=" << validation.areaRatio << " ringRatio=" << validation.ringRatio
        << " brightness=(" << validation.brightness.q1 << "," << validation.brightness.q2
        << "," << validation.brightness.q3 << "," << validation.brightness.q4 << ")";
    return out.str();
}

void expectNear(double actual, double expected, const std::string& what) {
    MIB_EXPECT(std::abs(actual - expected) <= kTolerance,
               what + ": got " + std::to_string(actual) + " expected " +
                   std::to_string(expected));
}

// Two ring objects (outer disc with a hole) plus brightness texture, exactly
// reproducible: no blur ambiguity (blur size 1), direct threshold at 127.
cv::Mat makeRingFrame(int leftShift) {
    cv::Mat image(96, 160, CV_8UC1, cv::Scalar(0));
    cv::circle(image, cv::Point(48 + leftShift, 48), 22, cv::Scalar(200), cv::FILLED);
    cv::circle(image, cv::Point(48 + leftShift, 48), 11, cv::Scalar(40), cv::FILLED);
    cv::circle(image, cv::Point(112, 48), 18, cv::Scalar(255), cv::FILLED);
    cv::circle(image, cv::Point(112, 48), 9, cv::Scalar(0), cv::FILLED);
    return image;
}

backend::services::ProcessingConfig goldenConfig() {
    backend::services::ProcessingConfig config;
    config.gaussian_blur_size = 1;
    config.bg_subtract_threshold = 127;
    config.morph_kernel_size = 1;
    config.morph_iterations = 1;
    config.enable_border_check = true;
    config.enable_area_range_check = false;
    config.enable_ring_ratio_check = false;
    config.enable_deformability_range_check = false;
    config.enable_area_ratio_check = false;
    config.require_single_inner_contour = false;
    return config;
}

// Independent pixel expectations, not a second implementation of subtraction,
// thresholding or morphology. Keep these Contract-1 defaults frozen when the
// Contract-2 core is added. All pixels, including outside the ROI, are checked.
void maskGoldens() {
    auto kernel = backend::processing::makeBundledProcessingKernel();
    MIB_REQUIRE(kernel->identity().contractVersion == 1, "Contract-1 fixture requires Contract 1");
    backend::processing::KernelConfig config;
    config.gaussianBlurSize = 1;
    config.backgroundSubtractThreshold = 20;
    config.morphologyKernelSize = 1;
    config.morphologyIterations = 1;
    config.emptyFramePixelThreshold = 1;
    config.absoluteBackgroundDifference = false;
    const backend::processing::KernelRoi full{0, 0, 19, 17};
    const cv::Mat background(17, 19, CV_8UC1, cv::Scalar(100));
    const cv::Mat zero(17, 19, CV_8UC1, cv::Scalar(0));
    cv::Mat rectangle = zero.clone();
    rectangle(cv::Rect(6, 5, 5, 5)).setTo(255);

    const auto check = [&](const char* name, const cv::Mat& input, const cv::Mat& bg,
                           const backend::processing::KernelRoi& roi, const cv::Mat& expected,
                           bool expectedEmpty) {
        const cv::Mat inputBefore = input.clone();
        const cv::Mat bgBefore = bg.clone();
        cv::Mat mask;
        bool empty = !expectedEmpty;
        std::string error;
        MIB_REQUIRE(kernel->processMask(input, bg, config, roi, mask, &error),
                    std::string(name) + ": " + error);
        MIB_REQUIRE(mask.type() == CV_8UC1 && mask.size() == expected.size(), name);
        MIB_EXPECT(cv::countNonZero(mask != expected) == 0,
                   std::string(name) + ": complete mask bytes");
        MIB_REQUIRE(kernel->isEmpty(input, bg, config, roi, empty, &error), error);
        MIB_EXPECT(empty == expectedEmpty, std::string(name) + ": empty decision");
        MIB_EXPECT(cv::countNonZero(input != inputBefore) == 0,
                   std::string(name) + ": borrowed input unchanged");
        if (!bg.empty()) {
            MIB_EXPECT(cv::countNonZero(bg != bgBefore) == 0,
                       std::string(name) + ": borrowed background unchanged");
        }
    };

    check("empty", background, background, full, zero, true);
    cv::Mat bright = background.clone();
    bright(cv::Rect(6, 5, 5, 5)).setTo(200);
    check("bright foreground", bright, background, full, rectangle, false);
    cv::Mat dark = background.clone();
    dark(cv::Rect(6, 5, 5, 5)).setTo(0);
    check("dark foreground is suppressed by legacy subtraction", dark, background, full, zero,
          true);
    cv::Mat atThreshold = background.clone();
    atThreshold(cv::Rect(6, 5, 5, 5)).setTo(120);
    check("difference equal to threshold is excluded", atThreshold, background, full, zero, true);
    check("no background", rectangle, {}, full, rectangle, false);

    cv::Mat cropped = zero.clone();
    cropped(cv::Rect(8, 5, 3, 5)).setTo(255);
    check("ROI clips foreground and zeros exterior", bright, background, {8, 4, 6, 8}, cropped,
          false);

    // Empty classification deliberately precedes morphology. A rejected speck
    // is a non-empty candidate with an empty final mask; this is distinct from
    // the difference-policy inconsistency assigned to phase 1 of issue #394.
    cv::Mat speck = background.clone();
    speck.at<unsigned char>(8, 9) = 200;
    config.morphologyKernelSize = 3;
    check("morphology removes isolated candidate", speck, background, full, zero, false);
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(20);
    watchdog.mark("Contract-1 pixel goldens");
    maskGoldens();
    watchdog.mark("Contract-1 science and tracking goldens");
    backend::services::ProcessingService service;
    // Pin the spatial calibration so the micron-space gates are exact.
    service.setPixelToMicronFactor(0.5);
    const auto config = goldenConfig();

    // Three frames: the left ring drifts right by 6 px/frame (tracked as one
    // object), the right ring stays still (tracked as another).
    std::vector<cv::Mat> frames{makeRingFrame(0), makeRingFrame(6), makeRingFrame(12)};
    const auto records = service.processBatch(frames, config);

    for (const auto& record : records) {
        std::cout << "frame=" << record.index << " " << describe(record.validation) << "\n";
    }

    // processBatch deduplicates by track: one record per tracked object,
    // updated in place as later frames re-observe it.
    MIB_REQUIRE(records.size() == 2, "one record per tracked object");

    // Record 0: the drifting ring (leftmost first — deterministic
    // left-to-right object ordering). Metrics come from the first
    // observation; track fields reflect the whole batch.
    const auto& drifting = records[0].validation;
    MIB_EXPECT(drifting.objectId == 1 && drifting.objectCount == 2,
               "deterministic object ordering");
    MIB_EXPECT(drifting.isValid && drifting.inRange && !drifting.touchesBorder,
               "drifting ring is a valid in-range object");
    MIB_EXPECT(drifting.innerContourCount == 2, "both ring holes detected as inner contours");
    MIB_EXPECT(drifting.trackId == 1 && drifting.trackObservationCount == 3 &&
                   drifting.trackFirstFrame == 0 && drifting.trackLastFrame == 2,
               "drifting ring tracked across all three frames as track 1");
    expectNear(drifting.bboxX, 26.0, "drifting bboxX");
    expectNear(drifting.bboxY, 26.0, "drifting bboxY");
    expectNear(drifting.bboxWidth, 45.0, "drifting bboxWidth");
    expectNear(drifting.bboxHeight, 45.0, "drifting bboxHeight");
    expectNear(drifting.area, 424.0, "drifting inner-contour hull area (px^2)");
    expectNear(drifting.deformability, 0.00689363522263, "drifting deformability");
    expectNear(drifting.areaRatio, 1.03921568627, "drifting hull/contour area ratio");
    expectNear(drifting.ringRatio, 32.3419232576, "drifting ring ratio");
    expectNear(drifting.brightness.q1, 200.0, "drifting brightness q1");
    expectNear(drifting.brightness.q2, 200.0, "drifting brightness q2");
    expectNear(drifting.brightness.q3, 200.0, "drifting brightness q3");
    expectNear(drifting.brightness.q4, 200.0, "drifting brightness q4");

    // Record 1: the static ring.
    const auto& fixed = records[1].validation;
    MIB_EXPECT(fixed.objectId == 2 && fixed.trackId == 2, "static ring starts track 2");
    MIB_EXPECT(fixed.trackObservationCount == 3 && fixed.trackFirstFrame == 0 &&
                   fixed.trackLastFrame == 2,
               "static ring tracked across all three frames as track 2");
    expectNear(fixed.bboxX, 94.0, "static bboxX");
    expectNear(fixed.area, 290.0, "static inner-contour hull area (px^2)");
    expectNear(fixed.deformability, 0.0147139803752, "static deformability");
    expectNear(fixed.ringRatio, 26.0768096208, "static ring ratio");
    expectNear(fixed.brightness.q4, 255.0, "static brightness max");

    // Single-inner-contour gate: the same frame with the stricter default
    // config yields exactly one non-valid empty record (two inner contours).
    auto strict = config;
    strict.require_single_inner_contour = true;
    const auto strictRecords = service.processBatch({makeRingFrame(0)}, strict);
    MIB_REQUIRE(strictRecords.size() == 2, "strict config still emits one record per object");
    MIB_EXPECT(!strictRecords[0].validation.hasSingleInnerContour,
               "two ring holes are not a single inner contour");

    // Target-group gating (LUT-free path): the drifting ring's metrics land
    // inside the group; the static ring's do not (area window excludes it).
    auto targeted = config;
    targeted.enable_target_group = true;
    targeted.target_group_area_min = 100;  // um^2: drifting = 424 px^2 * 0.25
    targeted.target_group_area_max = 110;
    targeted.target_group_deformability_min = 0.0;
    targeted.target_group_deformability_max = 0.01;
    const auto targetRecords = service.processBatch({makeRingFrame(0)}, targeted);
    MIB_REQUIRE(targetRecords.size() == 2, "target config emits both objects");
    MIB_EXPECT(targetRecords[0].validation.isTargetGroup,
               "drifting ring matches the target group");
    MIB_EXPECT(!targetRecords[1].validation.isTargetGroup,
               "static ring stays outside the target group");

    // Freeze actual ring validation, not only the metric with its gate off.
    // The left ring is 32.3419..., the right ring is 26.0768....
    auto ringGated = targeted;
    ringGated.enable_ring_ratio_check = true;
    ringGated.ring_ratio_min = 30.0;
    ringGated.ring_ratio_max = 35.0;
    const auto ringRecords = service.processBatch({makeRingFrame(0)}, ringGated);
    MIB_REQUIRE(ringRecords.size() == 2, "ring gate retains both object records");
    MIB_EXPECT(ringRecords[0].validation.isValid && ringRecords[0].validation.inRange &&
                   ringRecords[0].validation.isTargetGroup,
               "ring inside focus window remains valid and targetable");
    MIB_EXPECT(!ringRecords[1].validation.isValid && !ringRecords[1].validation.inRange &&
                   !ringRecords[1].validation.isTargetGroup,
               "ring outside focus window is invalid and cannot trigger");
    expectNear(ringRecords[1].validation.ringRatio, 26.0768096208,
               "rejected ring preserves measured metric");

    // Border gating: an object overlapping the ROI border is flagged and
    // excluded from validity without disturbing sibling objects.
    cv::Mat borderFrame(96, 160, CV_8UC1, cv::Scalar(0));
    cv::circle(borderFrame, cv::Point(4, 48), 12, cv::Scalar(255), cv::FILLED);
    cv::circle(borderFrame, cv::Point(112, 48), 18, cv::Scalar(255), cv::FILLED);
    const auto borderRecords = service.processBatch({borderFrame}, config);
    MIB_REQUIRE(borderRecords.size() == 2, "border frame emits both objects");
    MIB_EXPECT(borderRecords[0].validation.touchesBorder &&
                   !borderRecords[0].validation.isValid,
               "border-touching object is flagged and invalid");
    MIB_EXPECT(borderRecords[1].validation.isValid, "interior object stays valid");

    return mib::test::exitCode();
}
