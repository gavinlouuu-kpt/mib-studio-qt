// PL science, channel band from an off-path background (plan T3.7): with the
// science on the PL no host frame is classified, so background calibration
// takes the per-pixel median of preview frames from the FrameStore. Cells
// passing through are removed by the median; with auto_roi_from_background on,
// the channel walls are detected in that background and the band reaches the
// effective config and the compiled PL profile page (word 1). Cancel works and
// a stalled source times out.
#include "backend/playback/FrameStore.h"
#include "backend/processing/ChannelRoiDetect.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/pz/PzProfileCompiler.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/imgproc.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using backend::services::ProcessingService;

namespace {

// 96x512 channel: textured walls top (0-13) and bottom (82-95), channel 140.
cv::Mat channelBackground() {
    cv::Mat bg(96, 512, CV_8UC1, cv::Scalar(140));
    for (int r = 0; r < 14; ++r) bg.row(r).setTo(200 + (r % 5) * 6);
    for (int r = 82; r < 96; ++r) bg.row(r).setTo(195 + (r % 5) * 6);
    return bg;
}

bool waitState(ProcessingService& svc, ProcessingService::BackgroundCalibrationState want, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (svc.backgroundCalibrationStatus().state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return svc.backgroundCalibrationStatus().state == want;
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    const cv::Mat bg = channelBackground();
    auto store = std::make_shared<backend::playback::FrameStore>(64);
    ProcessingService svc;
    auto cfg = svc.getProcessingConfig();
    cfg.auto_roi_from_background = true;
    svc.setProcessingConfig(cfg);

    // Preview frames: the channel with a cell drifting along it.
    std::atomic<bool> feeding{true};
    std::thread feeder([&] {
        uint64_t n = 0;
        while (feeding.load()) {
            cv::Mat f = bg.clone();
            cv::circle(f, cv::Point(static_cast<int>(30 + (n * 37) % 450), 48), 12, cv::Scalar(60), cv::FILLED);
            store->pushFrame(f.data, f.total(), 512, 96, 512, 0x01080001, ++n, n);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    ProcessingService::BackgroundCalibrationRequest req;
    req.requiredAccepted = 15;
    req.maxAttempts = 15;
    req.timeoutMs = 5000;
    std::string error;
    MIB_REQUIRE(svc.startPreviewBackgroundCalibration(store, req, &error), "calibration starts: " + error);
    MIB_REQUIRE(waitState(svc, ProcessingService::BackgroundCalibrationState::Succeeded, 5000),
                "calibration succeeds: " + svc.backgroundCalibrationStatus().message);
    const auto status = svc.backgroundCalibrationStatus();
    MIB_EXPECT(status.accepted == 15 && status.publishedBackgroundGeneration > 0 && !status.publishedSha256.empty(),
               "15 preview frames, background published");

    const cv::Mat published = svc.getRealtimeBackgroundGray();
    cv::Mat diff;
    cv::absdiff(published, bg, diff);
    MIB_EXPECT(cv::countNonZero(diff) == 0, "the median removes the passing cell: background equals the empty channel");

    const auto band = svc.getChannelBand();
    const auto direct = backend::processing::detectChannelRoi(bg);
    MIB_EXPECT(band.h > 0 && band.y == direct.y && band.h == direct.h,
               "channel band detected: rows " + std::to_string(band.y) + "+" + std::to_string(band.h));
    const auto eff = svc.getEffectiveProcessingConfig();
    MIB_EXPECT(eff.channel_band_y == band.y && eff.channel_band_h == band.h, "effective config carries the band");

    backend::processing::pz::UnetCellsProfileInputs in;
    in.config = eff;
    in.config.enable_target_group_emodulus = false;
    in.pixelToMicron = 0.5;
    const auto profile = backend::processing::pz::compileUnetCellsV2(in);
    MIB_EXPECT(profile.ok() && profile.page[1] == (static_cast<uint32_t>(band.h) << 16 | static_cast<uint32_t>(band.y)),
               "the band is in the PL profile page (word 1)");

    // Off: the band is not applied.
    cfg.auto_roi_from_background = false;
    svc.setProcessingConfig(cfg);
    MIB_EXPECT(svc.getEffectiveProcessingConfig().channel_band_h == 0, "auto band off: no band in the effective config");

    // Cancel while running.
    req.requiredAccepted = 1000;
    req.maxAttempts = 1000;
    req.timeoutMs = 20000;
    MIB_REQUIRE(svc.startPreviewBackgroundCalibration(store, req, &error), "second calibration starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    svc.cancelBackgroundCalibration();
    MIB_EXPECT(waitState(svc, ProcessingService::BackgroundCalibrationState::Cancelled, 1000), "cancel");

    feeding.store(false);
    feeder.join();

    // A stalled source times out.
    req.requiredAccepted = 5;
    req.maxAttempts = 5;
    req.timeoutMs = 200;
    MIB_REQUIRE(svc.startPreviewBackgroundCalibration(store, req, &error), "third calibration starts");
    MIB_EXPECT(waitState(svc, ProcessingService::BackgroundCalibrationState::FailedTimeout, 2000),
               "no new preview frames: timeout");
    return mib::test::exitCode();
}
