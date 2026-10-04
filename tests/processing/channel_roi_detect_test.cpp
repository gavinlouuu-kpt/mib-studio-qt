// Regression test for auto-fit processing ROI (issue #295).
//
// Covers the pure wall detector (backend::processing::detectChannelRoi) and its
// integration through ProcessingService::computeAutoRoiFromBackground, which is
// gated on ProcessingConfig::auto_roi_from_background.

#include "backend/processing/ChannelRoiDetect.h"
#include "backend/processing/ProcessingScience.h"
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

// A 240x1184 MIB-style background: the channel sits mid-frame (~22% of the
// height) between dark textured walls, with flat glass above and below. The
// flat glass runs are longer than the channel, so a longest-run pick would
// land on the glass.
cv::Mat makeMidFrameChannelBackground() {
    cv::Mat bg(240, 1184, CV_8UC1, cv::Scalar(150)); // flat glass
    for (int r = 90; r < 96; ++r) {
        bg.row(r).setTo(60 + (r % 3) * 10); // top wall
    }
    for (int r = 96; r < 148; ++r) {
        bg.row(r).setTo(120 + (r % 3) * 3); // lightly textured channel
    }
    for (int r = 148; r < 154; ++r) {
        bg.row(r).setTo(60 + (r % 3) * 10); // bottom wall
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

    // 5) Mid-frame channel: pick the wall-bounded band, not the longer flat
    //    glass runs touching the frame edge, and accept a ~22%-high band.
    {
        const cv::Mat mid = makeMidFrameChannelBackground();
        const ChannelRoi r = detectChannelRoi(mid);
        check(r.w == mid.cols, "mid-frame channel keeps full width");
        check(r.y >= 94 && r.y <= 104, "mid-frame channel top sits at the top wall");
        check(r.y + r.h >= 140 && r.y + r.h <= 150, "mid-frame channel bottom sits at the bottom wall");
    }

    // 6) Service integration: disabled by default -> empty ROI (full frame).
    {
        backend::services::ProcessingService service;
        const auto roi = service.computeAutoRoiFromBackground(bg);
        check(roi.w == 0 && roi.h == 0, "auto-ROI disabled by default returns full-frame sentinel");
    }

    // 7) Service integration: enabled -> wall-avoiding ROI matching the detector.
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

    // 8) Background capture publishes the channel band and leaves the ROI
    //    alone (no crop): objects are gated by centroid instead.
    {
        backend::services::ProcessingService service;
        backend::services::ProcessingConfig config;
        config.auto_roi_from_background = true;
        service.setProcessingConfig(config);
        const auto roiBefore = service.getRealtimeRoi();
        service.setRealtimeBackgroundGray(bg);
        const auto band = service.getChannelBand();
        const ChannelRoi direct = detectChannelRoi(bg);
        check(band.y == direct.y && band.h == direct.h, "background publishes the detected band");
        const auto roiAfter = service.getRealtimeRoi();
        check(roiAfter.x == roiBefore.x && roiAfter.y == roiBefore.y && roiAfter.w == roiBefore.w &&
                  roiAfter.h == roiBefore.h,
              "channel band does not crop the ROI");
        service.setRealtimeBackgroundGray(cv::Mat{});
        check(service.getChannelBand().h == 0, "clearing the background clears the band");
    }

    // 9) Object filter: centroid outside the band -> invalid with reason
    //    Channel; inside -> valid. No band -> both valid.
    {
        namespace science = backend::processing::science;
        cv::Mat mask(96, 200, CV_8UC1, cv::Scalar(0));
        cv::rectangle(mask, cv::Rect(20, 2, 16, 10), cv::Scalar(255), cv::FILLED);  // on the wall
        cv::rectangle(mask, cv::Rect(120, 40, 16, 16), cv::Scalar(255), cv::FILLED); // in channel
        backend::services::ProcessingConfig config;
        config.processing_contract_version = 2;
        config.enable_area_range_check = false;
        config.enable_border_check = false;
        const cv::Rect frame(0, 0, mask.cols, mask.rows);

        auto open = science::filterProcessedObjects(mask, frame, config, cv::Mat{}, 1.0, nullptr);
        check(open.size() == 2 && open[0].isValid && open[1].isValid && open[0].inChannel &&
                  open[1].inChannel,
              "no band: both objects valid and in channel");

        config.channel_band_y = 20;
        config.channel_band_h = 60;
        auto gated = science::filterProcessedObjects(mask, frame, config, cv::Mat{}, 1.0, nullptr);
        check(gated.size() == 2, "band: both objects still reported");
        if (gated.size() == 2) {
            check(!gated[0].inChannel && !gated[0].isValid, "wall object rejected by centroid");
            check(gated[1].inChannel && gated[1].isValid, "channel object kept");
            const auto reasons = science::classifyInvalidReasons(gated[0], config);
            check(reasons.size() == 1 && reasons[0] == science::InvalidReasonCode::Channel,
                  "wall object reason is Channel");
        }
    }

    if (failures == 0) {
        std::cerr << "channel_roi_detect_test: ALL PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
