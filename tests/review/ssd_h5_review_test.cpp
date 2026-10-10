// ssd_h5_review_test (#667): an HDF5 file made by tools/pzrec_to_h5 from a downloaded SSD run opens in the one Review implementation (ReviewSession,
// behind the Qt tab, MIB Studio and YOFO Review) with the counts the converter promised, readable images and masks, metadata rows with the frame id as
// index, and a working scatter. Usage: ssd_h5_review_test <file.h5> <valid rows> <invalid rows> [<first valid frame id>]
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using backend::review::OverlayMode;
using backend::review::ReviewDataset;
using backend::review::ReviewSession;

int main(int argc, char** argv) {
    mib::test::Watchdog wd(120);
    MIB_REQUIRE(argc > 3, "usage: ssd_h5_review_test <file.h5> <valid rows> <invalid rows> [<first valid frame id>]");
    const std::uint64_t wantValid = std::strtoull(argv[2], nullptr, 10), wantInvalid = std::strtoull(argv[3], nullptr, 10);
    ReviewSession s;
    std::string error;
    MIB_REQUIRE(s.open(argv[1], &error), "open: " + error);
    const auto meta = s.metadata();
    MIB_EXPECT(!meta.recordingFile, "an experiment file");
    MIB_EXPECT(s.frameCount(true) == wantValid && s.frameCount(false) == wantInvalid,
               "rows " + std::to_string(s.frameCount(true)) + "/" + std::to_string(s.frameCount(false)));
    MIB_EXPECT(meta.validImages.present && meta.validImages.count == wantValid && meta.validImages.height == 96 && meta.validImages.width == 512, "valid images dataset");
    MIB_EXPECT(meta.validMasks.present && meta.validMasks.count == wantValid, "valid masks dataset");
    MIB_EXPECT(meta.roi.w == 512 && meta.roi.h == 96, "roi");
    MIB_EXPECT(meta.startTimeNs > 0 && meta.endTimeNs >= meta.startTimeNs, "start/end time");
    for (const bool valid : {true, false}) {
        const std::uint64_t n = s.frameCount(valid);
        for (std::uint64_t i : {std::uint64_t{0}, n ? n - 1 : 0}) {
            if (n == 0) break;
            backend::services::ProcessedFrame f;
            MIB_REQUIRE(s.frameMeta(valid, i, f), "frameMeta");
            MIB_EXPECT(f.index > 0 && f.timestampNs > 0, "index (the frame id) and timestamp");
            cv::Mat img, mask;
            MIB_REQUIRE(s.readRaw(valid ? ReviewDataset::ValidImage : ReviewDataset::InvalidImage, i, img), "image");
            MIB_REQUIRE(s.readRaw(valid ? ReviewDataset::ValidMask : ReviewDataset::InvalidMask, i, mask), "mask");
            MIB_EXPECT(img.rows == 96 && img.cols == 512 && img.type() == CV_8UC1, "image shape");
            MIB_EXPECT(mask.rows == 96 && mask.cols == 512 && mask.type() == CV_8UC1, "mask shape");
            cv::Mat notZero, not255;
            cv::compare(mask, 0, notZero, cv::CMP_NE);
            cv::compare(mask, 255, not255, cv::CMP_NE);
            cv::Mat bad;
            cv::bitwise_and(notZero, not255, bad);
            MIB_EXPECT(cv::countNonZero(bad) == 0, "mask values are 0 or 255");
            backend::review::ReviewImage shown;
            MIB_EXPECT(s.fetchImage(valid ? ReviewDataset::ValidImage : ReviewDataset::InvalidImage, i, OverlayMode::AllContour, false, shown), "overlay");
            backend::services::ProcessedFrame whole;
            MIB_EXPECT(s.loadFrameForDisplay(valid, i, whole) && !whole.originalImage.empty(), "loadFrameForDisplay");
        }
    }
    if (argc > 4 && wantValid) {
        backend::services::ProcessedFrame f;
        MIB_REQUIRE(s.frameMeta(true, 0, f), "frameMeta 0");
        MIB_EXPECT(f.index == std::strtoull(argv[4], nullptr, 10), "first valid frame id " + std::to_string(f.index));
    }
    const auto scatter = s.scatter();
    MIB_EXPECT(!scatter.areaUm2.empty() || wantValid == 0, "scatter has the valid rows");
    std::printf("ssd_h5_review_test: %s opens: %llu valid / %llu invalid rows\n", argv[1], static_cast<unsigned long long>(wantValid),
                static_cast<unsigned long long>(wantInvalid));
    return mib::test::exitCode();
}
