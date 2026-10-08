// multi_image_series_roundtrip_test
//
// Round-trip coverage for the multi-image series save path: appendFrames is
// called in several batches (first batch creates /valid_frames/series_images
// via writeSeriesImageDataset, later batches extend it via
// appendSeriesImageDataset), then the file is reloaded and every series image
// is verified pixel-for-pixel. Also verifies the regular per-frame images
// across batches. Guards the chunk-per-image dataset layout that keeps
// appends full-chunk writes (no whole-chunk read-modify-write) — the fix for
// the multi-image save stall / trigger jitter after the first flush.

#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>
#include <hdf5.h>

#include <cstdint>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace {

constexpr int kHeight = 16;
constexpr int kWidth = 24;
constexpr size_t kSeriesCount = 3;
constexpr size_t kBatchCount = 3;
constexpr size_t kFramesPerBatch = 4;

unsigned char pixelValue(size_t record, size_t series) {
    return static_cast<unsigned char>((record * 7 + series * 3 + 1) % 251);
}

// Every other series image is a non-continuous ROI view of a larger Mat to
// exercise the row-copy scratch path in the series writers.
cv::Mat makeSeriesImage(size_t record, size_t series) {
    const cv::Scalar value(pixelValue(record, series));
    if ((record + series) % 2 == 0) {
        return cv::Mat(kHeight, kWidth, CV_8UC1, value);
    }
    cv::Mat parent(kHeight * 2, kWidth * 2, CV_8UC1, cv::Scalar(0));
    cv::Mat view = parent(cv::Rect(0, 0, kWidth, kHeight));
    view.setTo(value);
    return view;
}

ProcessedFrame makeSeriesFrame(size_t record) {
    ProcessedFrame f;
    f.index = record;
    f.timestampNs = (record + 1) * 1000ULL;
    f.originalImage = cv::Mat(kHeight, kWidth, CV_8UC1, cv::Scalar(pixelValue(record, 0)));
    f.processedImage = cv::Mat(kHeight, kWidth, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = true;
    f.validation.objectId = static_cast<int>(record);
    for (size_t s = 0; s < kSeriesCount; ++s) {
        f.seriesImages.push_back(makeSeriesImage(record, s));
    }
    return f;
}

} // namespace

int main() {
    mib::test::TempDir td("mib_multi_image_series_roundtrip");
    const std::string path = (td / "multi_image.h5").string();
    constexpr size_t kTotalRecords = kBatchCount * kFramesPerBatch;

    // --- Write in batches, as the experiment flush does ---
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "openFile");
        MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets");

        size_t record = 0;
        for (size_t batch = 0; batch < kBatchCount; ++batch) {
            std::vector<ProcessedFrame> valid;
            for (size_t i = 0; i < kFramesPerBatch; ++i) {
                valid.push_back(makeSeriesFrame(record++));
            }
            // One sampled invalid frame per batch (no series images).
            ProcessedFrame inv;
            inv.index = 1000 + batch;
            inv.timestampNs = (1000 + batch) * 1000ULL;
            inv.originalImage = cv::Mat(kHeight, kWidth, CV_8UC1, cv::Scalar(9));
            inv.processedImage = cv::Mat(kHeight, kWidth, CV_8UC1, cv::Scalar(0));
            inv.validation.isValid = false;
            std::vector<ProcessedFrame> invalid{inv};

            MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "appendFrames batch");
        }
        hdf5.closeFile();
    }

    // Layout, unlike pixel round-trip alone, distinguishes the regression.
    hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    MIB_REQUIRE(file >= 0, "inspect saved layout");
    for (const char* name : {"/valid_frames/images", "/valid_frames/series_images"}) {
        hid_t dataset = H5Dopen2(file, name, H5P_DEFAULT);
        MIB_REQUIRE(dataset >= 0, name);
        hid_t props = H5Dget_create_plist(dataset);
        hsize_t chunks[4]{};
        const int rank = H5Pget_chunk(props, 4, chunks);
        MIB_REQUIRE(rank == 3 || rank == 4, "expected chunk rank");
        MIB_EXPECT(chunks[0] == 1, "one record per chunk");
        if (rank == 4) MIB_EXPECT(chunks[1] == 1, "one series member per chunk");
        H5Pclose(props);
        H5Dclose(dataset);
    }
    H5Fclose(file);

    // --- Reload and verify every series image and per-frame image ---
    {
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "reload");

        size_t count = 0, seriesCount = 0;
        int h = 0, w = 0;
        MIB_REQUIRE(r.getSeriesImageInfo(count, seriesCount, h, w), "getSeriesImageInfo");
        MIB_EXPECT(count == kTotalRecords, "series record count round-trips");
        MIB_EXPECT(seriesCount == kSeriesCount, "series count round-trips");
        MIB_EXPECT(h == kHeight && w == kWidth, "series dims round-trip");

        for (size_t record = 0; record < kTotalRecords; ++record) {
            std::vector<cv::Mat> images;
            MIB_REQUIRE(r.readSeriesImagesByIndex(record, images), "readSeriesImagesByIndex");
            MIB_EXPECT(images.size() == kSeriesCount, "series size per record");
            for (size_t s = 0; s < images.size(); ++s) {
                const cv::Mat& img = images[s];
                MIB_REQUIRE(!img.empty() && img.rows == kHeight && img.cols == kWidth,
                            "series image shape");
                const unsigned char expected = pixelValue(record, s);
                MIB_EXPECT(cv::countNonZero(img != expected) == 0,
                           "series image pixels round-trip");
            }
        }

        // Regular per-frame images written across multiple append batches.
        for (size_t record : {size_t{0}, kTotalRecords / 2, kTotalRecords - 1}) {
            cv::Mat img;
            MIB_REQUIRE(r.readImageByIndex("/valid_frames/images", record, img),
                        "readImageByIndex valid image");
            const unsigned char expected = pixelValue(record, 0);
            MIB_EXPECT(!img.empty() && img.at<unsigned char>(0, 0) == expected,
                       "valid image pixels round-trip across batches");
        }
        r.closeFile();
    }

    if (mib::test::exitCode() == 0) {
        std::printf("multi-image series save path round-trips across append batches\n");
    }
    return mib::test::exitCode();
}
