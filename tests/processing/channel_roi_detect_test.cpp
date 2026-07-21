// Regression test for auto-fit processing ROI (issue #295).
//
// Covers the pure wall detector (backend::processing::detectChannelRoi) and its
// integration through ProcessingService::computeAutoRoiFromBackground, which is
// gated on ProcessingConfig::auto_roi_from_background.

#include "backend/processing/ChannelRoiDetect.h"
#include "backend/processing/ProcessingService.h"

#include <opencv2/imgproc.hpp>

#include <iostream>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

// A 96x512 channel background: bright wall bands top and bottom, textured
// (non-flat) so it behaves like a real capture, with a flat-ish central channel.
cv::Mat makeChannelBackground() {
    cv::Mat bg(96, 512, CV_8UC1, cv::Scalar(140));
    // Wall bands with internal texture (gradient), like real channel walls.
    for (int r = 0; r < 14; ++r) {
        bg.row(r).setTo(200 + (r % 5) * 6);   // top wall
    }
    for (int r = 82; r < 96; ++r) {
        bg.row(r).setTo(195 + (r % 5) * 6);   // bottom wall
    }
    return bg;
}

} // namespace

int main() {
    using backend::processing::ChannelRoi;
    using backend::processing::ChannelRoiParams;
    using backend::processing::detectChannelRoi;

    const cv::Mat bg = makeChannelBackground();

    // 1) Detector excludes the wall bands, keeps full width, contains the centre.
    {
        const ChannelRoi r = detectChannelRoi(bg);
        check(r.w == bg.cols, "detector preserves full width");
        check(r.h < bg.rows, "detector crops below full height");
        check(r.y >= 12 && r.y <= 30, "detector top clears the top wall band");
        check(r.y + r.h <= 84 && r.y + r.h >= 66, "detector bottom clears the bottom wall band");
        // The central channel row (48) must remain inside the ROI.
        check(r.y <= 48 && r.y + r.h > 48, "detector keeps the central channel");
    }

    // 2) Fail-safe: a flat frame yields the full frame (no spurious crop).
    {
        cv::Mat flat(96, 512, CV_8UC1, cv::Scalar(128));
        const ChannelRoi r = detectChannelRoi(flat);
        check(r.x == 0 && r.y == 0 && r.w == 512 && r.h == 96, "flat frame -> full frame");
    }

    // 3) Fail-safe: empty input yields a 0x0 (full-frame sentinel) ROI.
    {
        const ChannelRoi r = detectChannelRoi(cv::Mat{});
        check(r.w == 0 && r.h == 0, "empty input -> 0x0 ROI");
    }

    // 4) Margin widens the exclusion: a larger margin never extends the band.
    {
        ChannelRoiParams tight;
        tight.marginRows = 0;
        ChannelRoiParams loose;
        loose.marginRows = 4;
        const ChannelRoi a = detectChannelRoi(bg, tight);
        const ChannelRoi b = detectChannelRoi(bg, loose);
        check(b.y >= a.y && (b.y + b.h) <= (a.y + a.h), "larger margin shrinks (never grows) the band");
    }

    // 5) Service integration: disabled by default -> empty ROI (full frame).
    {
        backend::services::ProcessingService service;
        const auto roi = service.computeAutoRoiFromBackground(bg);
        check(roi.w == 0 && roi.h == 0, "auto-ROI disabled by default returns full-frame sentinel");
    }

    // 6) Service integration: enabled -> wall-avoiding ROI matching the detector.
    {
        backend::services::ProcessingService service;
        backend::services::ProcessingConfig config;
        config.auto_roi_from_background = true;
        service.setProcessingConfig(config);
        const auto roi = service.computeAutoRoiFromBackground(bg);
        const ChannelRoi direct = detectChannelRoi(bg);
        check(roi.w == direct.w && roi.h == direct.h && roi.x == direct.x && roi.y == direct.y,
              "enabled auto-ROI matches the pure detector");
        check(roi.h > 0 && roi.h < bg.rows, "enabled auto-ROI excludes the walls");
    }

    if (failures == 0) {
        std::cerr << "channel_roi_detect_test: ALL PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
