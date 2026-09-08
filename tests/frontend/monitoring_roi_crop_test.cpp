// monitoring_roi_crop_test
//
// Guard for the Monitoring tab's ROI crop: frames accumulated under an older
// ROI / camera geometry must never be cropped with an unclamped rectangle
// (seven `cv::Mat::Mat` ROI assertion aborts on the installed 1.0.7 app,
// crash review 2026-09-08).

#include "frontend/tabs/MonitoringRoiCrop.h"

#include "support/assert.h"

#include <opencv2/core.hpp>

int main()
{
    using frontend::monitoring::cropToRoi;
    using frontend::monitoring::clampedRoiRect;

    const cv::Mat small(50, 100, CV_8UC1, cv::Scalar(7)); // rows=50, cols=100

    // The crash: a 512x96 ROI applied to a 100x50 frame.
    const cv::Mat a = cropToRoi(small, 0, 0, 512, 96);
    MIB_EXPECT(a.cols == 100 && a.rows == 50, "oversized ROI clamps to the whole frame");

    // ROI origin outside the frame.
    const cv::Mat b = cropToRoi(small, 300, 200, 20, 20);
    MIB_EXPECT(b.cols >= 1 && b.rows >= 1 && b.cols <= 100 && b.rows <= 50, "out-of-frame origin clamps inside");

    // ROI partially outside: width/height shrink to what is left.
    const cv::Mat c = cropToRoi(small, 90, 40, 20, 20);
    MIB_EXPECT(c.cols == 10 && c.rows == 10, "partial overlap crops the remainder");

    // Empty / invalid ROI means the full frame; ROI-sized frame is returned as is.
    MIB_EXPECT(cropToRoi(small, 0, 0, 0, 0).cols == 100, "empty ROI -> full frame");
    const cv::Mat roiSized(96, 512, CV_8UC1, cv::Scalar(1));
    MIB_EXPECT(cropToRoi(roiSized, 10, 10, 512, 96).data == roiSized.data, "ROI-sized frame shown as is");

    // Empty image never asserts.
    const cv::Mat none;
    MIB_EXPECT(cropToRoi(none, 0, 0, 512, 96).empty(), "empty image -> empty crop");
    MIB_EXPECT(clampedRoiRect(none, 0, 0, 512, 96).area() == 0, "empty image -> empty rect");

    return mib::test::exitCode();
}
