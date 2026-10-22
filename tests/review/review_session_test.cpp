// review_session_test (plan 2026-10-01-standalone-review-app, PR 1)
//
// Round-trip + fault injection for backend::review::ReviewSession, the one
// review implementation behind the Qt tab, the Tauri MIB Studio shell and
// YOFO Review:
//  - experiment fixture (valid/invalid frames, masks, 3-image series, ROI,
//    run snapshot with pixel_to_micron, accounting, stored KDE record):
//    metadata, full-column metrics pages, raw/overlay/ROI images, series,
//    thumbnail strips, scatter (isValid rows only, µm² with the recorded
//    factor — TD-17), accounting text, core-record save/overwrite refusal;
//  - recording fixture (multi-image window): recording-mode semantics;
//  - faults: missing file, bad dataset/index, closed session, legacy file
//    without snapshot/accounting falls back to the host factor;
//  - framesPage / validSet (#512 S5, the facade's rows and its charts): the
//    page as ProcessedFrames equals metricsPage and the reader's own metadata
//    read; the valid-set snapshot keeps identity and rows of one file; absent
//    vs unreadable valid sets;
//  - open() is all or nothing (#512 S5): a metadata read that throws after
//    the reader opened (a malformed background), or a throw inside the
//    reader's own open right after H5Fopen, leaves the session closed with
//    no handle held; isOpen() fails closed on a reader handle the session
//    did not open (a host mirroring it keeps writers out).
// Watchdog guarded.

#include "backend/review/ReviewSession.h"
#include "backend/review/OverlayCompose.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/KdeCoreRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <hdf5.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using backend::review::OverlayMode;
using backend::review::ReviewDataset;
using backend::review::ReviewImage;
using backend::review::ReviewSession;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace
{
    constexpr int kH = 24, kW = 32, kSeries = 3;
    constexpr double kRecordedFactor = 0.25;

    cv::Mat pattern(uint64_t index, int offset = 0)
    {
        cv::Mat m(kH, kW, CV_8UC1);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7 + offset) % 200);
        return m;
    }

    // A ring mask: outer disc with one inner hole, so every overlay mode draws.
    cv::Mat ringMask()
    {
        cv::Mat m = cv::Mat::zeros(kH, kW, CV_8UC1);
        cv::circle(m, cv::Point(kW / 2, kH / 2), 9, cv::Scalar(255), -1);
        cv::circle(m, cv::Point(kW / 2, kH / 2), 4, cv::Scalar(0), -1);
        return m;
    }

    ProcessedFrame makeFrame(uint64_t idx, bool valid, bool passesValidation, bool series)
    {
        ProcessedFrame f;
        f.index = idx;
        f.timestampNs = (idx + 1) * 1000ULL;
        f.originalImage = pattern(idx, valid ? 0 : 50);
        f.processedImage = ringMask();
        f.validation.isValid = passesValidation;
        f.validation.isTargetGroup = valid && (idx % 4 == 0);
        f.validation.hasSingleInnerContour = true;
        f.validation.objectId = static_cast<int>(idx);
        f.validation.objectCount = 1;
        f.validation.trackId = static_cast<int>(idx / 2);
        f.validation.area = 100.0 + static_cast<double>(idx);
        f.validation.deformability = 0.1 + 0.01 * static_cast<double>(idx);
        f.validation.ringRatio = 0.5;
        f.validation.brightness.q1 = 1.5;
        f.validation.brightness.q4 = 4.5;
        if (series)
            for (int s = 0; s < kSeries; ++s) f.seriesImages.push_back(pattern(idx, 100 + s * 17));
        return f;
    }

    void writeExperiment(const fs::path &path, bool withSnapshot, bool withAccounting, bool withKde)
    {
        std::vector<ProcessedFrame> valid, invalid;
        // valid set: 10 frames; frames 3 and 7 fail validation (no scatter point)
        for (int i = 0; i < 10; ++i) valid.push_back(makeFrame(static_cast<uint64_t>(i * 2), true, i != 3 && i != 7, true));
        for (int i = 0; i < 4; ++i) invalid.push_back(makeFrame(static_cast<uint64_t>(i * 2 + 1), false, false, false));
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()), "open fixture");
        MIB_REQUIRE(hdf5.initializeDatasets(), "init datasets");
        MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append frames");
        backend::services::ProcessingConfig cfg;
        cfg.ring_ratio_min = 12.0; // non-default: the histogram range comes from the file
        cfg.ring_ratio_max = 30.0;
        backend::services::ProcessingService::Roi roi{4, 2, 20, 16};
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi), "experiment info");
        if (withSnapshot)
        {
            MIB_REQUIRE(hdf5.writeRunSnapshotJson("{\"schema\":1,\"pixel_to_micron\":0.25,\"profile_id\":\"p\"}", "{}"),
                        "run snapshot");
        }
        if (withAccounting)
        {
            backend::recording::RecordingAccountingSnapshot a;
            a.admitted = 14;
            a.processed = 10;
            a.scientificallyRejected = 4;
            a.persistenceAdmitted = 14;
            a.persistenceCommitted = 14;
            a.completion = backend::recording::RunCompletionState::Complete;
            a.reconciled = true;
            MIB_REQUIRE(hdf5.writeRunAccounting(a), "accounting");
        }
        if (withKde)
        {
            backend::monitoring::KdeCoreRecord r;
            r.provisional = false;
            r.source = "full-run";
            r.cellCount = 8;
            r.pixelToMicron = kRecordedFactor;
            r.contours.push_back({{1, 0.1}, {2, 0.2}, {1, 0.3}});
            MIB_REQUIRE(hdf5.writeKdeAnalysisJson(backend::monitoring::toJson(r)), "kde record");
        }
        hdf5.closeFile();
    }

    void writeRecording(const fs::path &path, int frames, int multi)
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()), "open recording fixture");
        MIB_REQUIRE(hdf5.initializeRecordingDatasets(), "init recording datasets");
        std::vector<cv::Mat> images;
        std::vector<Hdf5Service::RecordingFrameMeta> meta;
        for (int i = 0; i < frames; ++i)
        {
            images.push_back(pattern(static_cast<uint64_t>(i)));
            meta.push_back({static_cast<uint64_t>(i), static_cast<uint64_t>(i) * 100, kW, kH});
        }
        MIB_REQUIRE(hdf5.appendRecordingFrames(images, meta), "append recording frames");
        MIB_REQUIRE(hdf5.writeRecordingInfo(10, 20, frames, 2, multi > 1, multi), "recording info");
        hdf5.closeFile();
    }

    bool hasRedPixel(const ReviewImage &img)
    {
        if (img.channels != 3) return false;
        for (std::size_t i = 0; i + 2 < img.data.size(); i += 3)
            if (img.data[i] == 255 && img.data[i + 1] == 0 && img.data[i + 2] == 0) return true;
        return false;
    }

    bool differs(const ReviewImage &a, const ReviewImage &b)
    {
        return a.channels != b.channels || a.data != b.data;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("review_session");
    const fs::path experiment = td.path() / "run.h5";
    const fs::path legacy = td.path() / "legacy.h5";
    const fs::path recording = td.path() / "rec.h5";
    writeExperiment(experiment, true, true, true);
    writeExperiment(legacy, false, false, false);
    writeRecording(recording, 9, 3);

    // ---- faults before open -------------------------------------------------
    {
        ReviewSession s;
        std::string err;
        MIB_EXPECT(!s.open((td.path() / "missing.h5").string(), &err), "missing file opens");
        MIB_EXPECT(!err.empty(), "missing file: no error text");
        MIB_EXPECT(!s.isOpen(), "session open after failed open");
        MIB_EXPECT(!s.metadata().fileOpen, "metadata fileOpen when closed");
        std::vector<backend::review::MetricRow> rows;
        uint64_t total = 1;
        MIB_EXPECT(!s.metricsPage(true, 0, 5, rows, total), "page served when closed");
        std::vector<ProcessedFrame> frames(2);
        total = 1;
        MIB_EXPECT(!s.framesPage(true, 0, 5, frames, total) && frames.empty() && total == 0, "frames page when closed");
        const auto closedSet = s.validSet();
        MIB_EXPECT(!closedSet.open && closedSet.filePath.empty() && !closedSet.metadataReadable && closedSet.frames.empty(),
                   "valid set when closed");
        ReviewImage img;
        MIB_EXPECT(!s.fetchImage(ReviewDataset::ValidImage, 0, OverlayMode::None, false, img), "image when closed");
        MIB_EXPECT(s.scatter().frameIndex.empty(), "scatter when closed");
    }
    wd.mark("faults");

    // ---- experiment round trip ----------------------------------------------
    {
        ReviewSession s;
        s.setFallbackPixelToMicron(0.9);
        MIB_REQUIRE(s.open(experiment.string()), "open experiment");
        MIB_EXPECT(s.isOpen() && !s.isRecordingFile(), "experiment flags");
        const auto meta = s.metadata();
        MIB_EXPECT(meta.fileOpen && !meta.recordingFile, "meta flags");
        MIB_EXPECT(meta.totalValid == 10 && meta.totalInvalid == 4, "meta counts");
        MIB_EXPECT(meta.roi.x == 4 && meta.roi.w == 20, "meta roi");
        MIB_EXPECT(meta.validImages.present && meta.validImages.count == 10 && meta.validImages.width == kW, "valid images info");
        MIB_EXPECT(meta.invalidMasks.present && meta.invalidMasks.count == 4, "invalid masks info");
        MIB_EXPECT(!meta.recordedImages.present, "recorded images on experiment file");
        MIB_EXPECT(meta.hasSeries && meta.seriesCount == kSeries, "series info");
        MIB_EXPECT(meta.hasAccounting && meta.accounting.scientificallyRejected == 4, "accounting");
        MIB_EXPECT(meta.pixelToMicronFromFile && std::fabs(meta.pixelToMicron - kRecordedFactor) < 1e-12,
                   "TD-17 recorded factor");
        MIB_EXPECT(std::fabs(s.pixelToMicron() - kRecordedFactor) < 1e-12, "effective factor is the recorded one");
        MIB_EXPECT(!meta.kdeAnalysisJson.empty() && meta.kdeLiveJson.empty(), "stored kde records");
        MIB_EXPECT(meta.hasRecordedConfig && meta.ringRatioMin == 12.0 && meta.ringRatioMax == 30.0, "recorded ring-ratio range");
        MIB_EXPECT(s.frameCount(true) == 10 && s.frameCount(false) == 4, "frame counts");

        // Metrics pages: full columns, bounded, stable totals.
        std::vector<backend::review::MetricRow> rows;
        uint64_t total = 0;
        MIB_REQUIRE(s.metricsPage(true, 2, 3, rows, total), "page");
        MIB_EXPECT(total == 10 && rows.size() == 3, "page bounds");
        MIB_EXPECT(rows[0].frameIndex == 4 && rows[0].objectId == 4 && rows[0].trackId == 2, "page row identity");
        MIB_EXPECT(std::fabs(rows[0].areaUm2 - rows[0].area * kRecordedFactor * kRecordedFactor) < 1e-9, "areaUm2");
        MIB_EXPECT(rows[0].brightnessQ4 == 4.5 && rows[0].hasSingleInnerContour, "full columns");
        MIB_EXPECT(rows[1].valid == false, "frame 3 fails validation in page");
        MIB_REQUIRE(s.metricsPage(false, 0, 50, rows, total), "invalid page");
        MIB_EXPECT(total == 4 && rows.size() == 4 && !rows[0].valid, "invalid page");
        MIB_REQUIRE(s.metricsPage(true, 50, 5, rows, total), "beyond page");
        MIB_EXPECT(rows.empty() && total == 10, "beyond page empty");

        // framesPage (#512 S5): the same page as metadata-only ProcessedFrames,
        // row for row the reader's own metadata read (the facade's source
        // before S5) and metricsPage's rows.
        {
            std::vector<ProcessedFrame> direct, directInvalid;
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(experiment.string()), "direct reader");
            MIB_REQUIRE(reader.readValidMetadata(direct) && reader.readInvalidMetadata(directInvalid), "direct metadata");
            reader.closeFile();
            auto same = [](const ProcessedFrame &a, const ProcessedFrame &b) {
                const auto &x = a.validation;
                const auto &y = b.validation;
                return a.index == b.index && a.timestampNs == b.timestampNs && x.isValid == y.isValid &&
                       x.isTargetGroup == y.isTargetGroup && x.objectId == y.objectId && x.objectCount == y.objectCount &&
                       x.trackId == y.trackId && x.centroidX == y.centroidX && x.centroidY == y.centroidY &&
                       x.area == y.area && x.deformability == y.deformability && x.areaRatio == y.areaRatio &&
                       x.ringRatio == y.ringRatio && x.youngsModulus == y.youngsModulus &&
                       x.analysisPixelToMicronFactor == y.analysisPixelToMicronFactor &&
                       x.brightness.q4 == y.brightness.q4 && a.originalImage.empty() && a.processedImage.empty() &&
                       a.seriesImages.empty();
            };
            std::vector<ProcessedFrame> frames;
            uint64_t ftotal = 0;
            MIB_REQUIRE(s.framesPage(true, 2, 3, frames, ftotal), "frames page");
            MIB_REQUIRE(s.metricsPage(true, 2, 3, rows, total), "metrics page beside frames page");
            MIB_EXPECT(ftotal == 10 && frames.size() == 3, "frames page bounds");
            for (std::size_t i = 0; i < frames.size(); ++i)
            {
                MIB_EXPECT(same(frames[i], direct[2 + i]), "frames page row " + std::to_string(i) + " = reader metadata");
                MIB_EXPECT(frames[i].index == rows[i].frameIndex && frames[i].validation.objectId == rows[i].objectId &&
                               frames[i].validation.area == rows[i].area,
                           "frames page row = metrics page row");
            }
            MIB_REQUIRE(s.framesPage(true, 0, 1000, frames, ftotal), "whole valid set");
            MIB_EXPECT(ftotal == 10 && frames.size() == direct.size(), "whole valid set size");
            bool all = frames.size() == direct.size();
            for (std::size_t i = 0; all && i < frames.size(); ++i) all = same(frames[i], direct[i]);
            MIB_EXPECT(all, "whole valid set = reader metadata");
            MIB_REQUIRE(s.framesPage(false, 0, 50, frames, ftotal), "invalid frames page");
            MIB_EXPECT(ftotal == 4 && frames.size() == 4 && same(frames[3], directInvalid[3]), "invalid frames page");
            MIB_REQUIRE(s.framesPage(true, 50, 5, frames, ftotal), "frames beyond");
            MIB_EXPECT(frames.empty() && ftotal == 10, "frames beyond empty");
            MIB_REQUIRE(s.framesPage(true, 10, 5, frames, ftotal), "frames at end");
            MIB_EXPECT(frames.empty() && ftotal == 10, "frames at end empty");
            MIB_REQUIRE(s.framesPage(true, 1, 0, frames, ftotal), "zero count");
            MIB_EXPECT(frames.empty() && ftotal == 10, "zero count empty");
            MIB_REQUIRE(s.framesPage(true, 3, std::numeric_limits<uint64_t>::max(), frames, ftotal), "huge count");
            MIB_EXPECT(frames.size() == 7 && frames[0].index == direct[3].index, "huge count is the rest, no wrap");
            const auto set = s.validSet();
            MIB_EXPECT(set.open && set.filePath == experiment.string() && !set.recordingFile && set.metadataReadable,
                       "valid set identity");
            bool sameSet = set.frames.size() == direct.size();
            for (std::size_t i = 0; sameSet && i < set.frames.size(); ++i) sameSet = same(set.frames[i], direct[i]);
            MIB_EXPECT(sameSet, "valid set rows = reader metadata");
        }

        // Images: raw Mono8, overlays RGB and different, masks raw, ROI red.
        ReviewImage plain, contour, inner, allMask, filtered, roi, mask;
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::None, false, plain), "plain image");
        MIB_EXPECT(plain.channels == 1 && plain.width == kW && plain.height == kH && plain.data.size() == kW * kH, "plain geometry");
        MIB_EXPECT(plain.data[0] == pattern(2).at<uint8_t>(0, 0), "plain pixels (valid frame 1 = index 2)");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::AllContour, false, contour), "contour");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::OuterInnerColorCoded, false, inner), "inner");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::AllMask, false, allMask), "all mask");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::FilteredMask, false, filtered), "filtered");
        MIB_EXPECT(contour.channels == 3 && inner.channels == 3 && allMask.channels == 3 && filtered.channels == 3, "overlay rgb");
        MIB_EXPECT(differs(contour, plain) && differs(inner, contour) && differs(allMask, contour) && differs(filtered, allMask),
                   "overlay modes draw differently");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::None, true, roi), "roi overlay");
        MIB_EXPECT(roi.channels == 3 && hasRedPixel(roi), "roi rectangle red");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidMask, 1, OverlayMode::AllContour, true, mask), "mask read");
        MIB_EXPECT(mask.channels == 1 && mask.data[kH / 2 * kW + kW / 2 + 6] == 255, "mask raw ring");
        ReviewImage bad;
        MIB_EXPECT(!s.fetchImage(ReviewDataset::ValidImage, 10, OverlayMode::None, false, bad), "index beyond");
        MIB_EXPECT(!s.fetchImage(ReviewDataset::RecordedImage, 0, OverlayMode::None, false, bad), "recorded on experiment");
        MIB_EXPECT(!s.fetchImage(static_cast<ReviewDataset>(99), 0, OverlayMode::None, false, bad), "unknown dataset");
        cv::Mat raw;
        MIB_REQUIRE(s.readRaw(ReviewDataset::InvalidImage, 2, raw), "raw invalid");
        MIB_EXPECT(raw.type() == CV_8UC1 && raw.at<uint8_t>(0, 0) == pattern(5, 50).at<uint8_t>(0, 0), "raw invalid pixels");

        // Series.
        uint64_t count = 0;
        MIB_REQUIRE(s.seriesInfo(0, count), "series info");
        MIB_EXPECT(count == kSeries, "series count");
        ReviewImage s0, s2;
        MIB_REQUIRE(s.fetchSeriesImage(0, 0, OverlayMode::None, false, s0), "series 0");
        MIB_REQUIRE(s.fetchSeriesImage(0, 2, OverlayMode::None, false, s2), "series 2");
        MIB_EXPECT(s0.data[0] == pattern(0, 100).at<uint8_t>(0, 0) && s2.data[0] == pattern(0, 134).at<uint8_t>(0, 0), "series pixels");
        MIB_EXPECT(!s.fetchSeriesImage(0, kSeries, OverlayMode::None, false, bad), "series beyond");
        ProcessedFrame full;
        MIB_REQUIRE(s.loadFrameForDisplay(true, 0, full), "load frame");
        MIB_EXPECT(!full.originalImage.empty() && !full.processedImage.empty() && full.seriesImages.size() == kSeries, "full frame");

        // Thumbnails: packed strip, letterboxed tiles, bounded page.
        backend::review::ThumbnailStrip strip;
        MIB_REQUIRE(s.thumbnails(true, 8, 5, 16, OverlayMode::None, false, strip), "thumbnails");
        MIB_EXPECT(strip.valid && strip.count == 2 && strip.channels == 1 && strip.data.size() == 2 * 16 * 16, "strip geometry");
        MIB_EXPECT(strip.data[16 * 16 - 1] == 0, "letterbox border black (32x24 into 16x16)");
        MIB_REQUIRE(s.thumbnails(true, 0, 3, 16, OverlayMode::AllContour, true, strip), "colour thumbnails");
        MIB_EXPECT(strip.channels == 3 && strip.count == 3 && strip.data.size() == 3 * 16 * 16 * 3, "colour strip");
        MIB_EXPECT(!s.thumbnails(true, 0, 3, 0, OverlayMode::None, false, strip), "zero size refused");

        // Scatter: isValid rows only, valid-set order, µm² with the recorded factor.
        const auto sc = s.scatter();
        MIB_EXPECT(sc.frameIndex.size() == 8, "scatter excludes frames failing validation");
        MIB_EXPECT(sc.validPosition[3] == 4 && sc.frameIndex[3] == 8, "scatter skips position 3");
        MIB_EXPECT(std::fabs(sc.areaUm2[0] - 100.0 * kRecordedFactor * kRecordedFactor) < 1e-9, "scatter µm²");
        MIB_EXPECT(sc.targetGroup[0] == 1 && sc.targetGroup[1] == 0, "target flags");
        MIB_EXPECT(sc.ringRatio.size() == 8 && std::fabs(sc.ringRatio[0] - 0.5) < 1e-12, "ring ratio column");
        MIB_EXPECT(std::fabs(sc.pixelToMicron - kRecordedFactor) < 1e-12, "scatter factor");

        MIB_EXPECT(s.accountingSummary().find("run complete") != std::string::npos, "accounting text: " + s.accountingSummary());
        MIB_EXPECT(s.accountingSummary().find("rejected 4") != std::string::npos, "accounting rejected");

        // Core record: refuse overwrite, then overwrite, reader reopened.
        std::string err;
        MIB_EXPECT(!s.saveCoreRecordJson("{\"x\":1}", false, &err), "overwrite refused");
        backend::monitoring::KdeCoreRecord r;
        r.provisional = false;
        r.source = "full-run";
        r.cellCount = 99;
        MIB_EXPECT(s.saveCoreRecordJson(backend::monitoring::toJson(r), true, &err), "overwrite: " + err);
        MIB_EXPECT(s.isOpen() && s.metadata().kdeAnalysisJson.find("99") != std::string::npos, "record refreshed");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 1, OverlayMode::None, false, plain), "reads after save");

        s.close();
        MIB_EXPECT(!s.isOpen() && s.frameCount(true) == 0, "closed");
    }
    wd.mark("experiment");

    // A partial accounting write must not look like a complete, reconciled run.
    for (const char* attribute : {"accounting_admitted_frames", "accounting_reconciled", "accounting_completion_reason"})
    {
        const auto corrupt = td.path() / "unreadable_accounting.h5";
        writeExperiment(corrupt, false, true, false);
        const hid_t file = H5Fopen(corrupt.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open accounting fault fixture");
        const hid_t group = H5Gopen2(file, "/experiment_info", H5P_DEFAULT);
        MIB_REQUIRE(group >= 0, "open accounting group");
        MIB_REQUIRE(H5Adelete(group, attribute) >= 0, "delete required accounting attribute");
        H5Gclose(group);
        H5Fclose(file);
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(corrupt.string()), "load accounting fault fixture");
        backend::recording::RecordingAccountingSnapshot a;
        MIB_EXPECT(reader.readRunAccounting(a), "accounting remains present");
        MIB_EXPECT(a.completion == backend::recording::RunCompletionState::Unknown && !a.reconciled,
                   "unreadable accounting is Unknown, not reconciled");
        MIB_EXPECT(a.completionReason == std::string("accounting unreadable: missing ") + attribute,
                   "unreadable accounting names missing attribute");
        MIB_EXPECT(!a.readError.empty() && a.processed == 0, "explicit unreadable state clears partial counters");
        reader.closeFile();
        ReviewSession s;
        MIB_REQUIRE(s.open(corrupt.string()), "review corrupt accounting");
        MIB_EXPECT(s.metadata().hasAccounting, "review retains accounting presence");
        MIB_EXPECT(s.accountingSummary().find("run unknown") != std::string::npos &&
                       s.accountingSummary().find(a.completionReason) != std::string::npos,
                   "review shows Unknown with the unreadable reason");
    }

    // ---- legacy file: no snapshot, no accounting ------------------------------
    {
        ReviewSession s;
        s.setFallbackPixelToMicron(0.5);
        MIB_REQUIRE(s.open(legacy.string()), "open legacy");
        const auto meta = s.metadata();
        MIB_EXPECT(!meta.pixelToMicronFromFile && std::fabs(meta.pixelToMicron - 0.5) < 1e-12, "legacy factor fallback");
        MIB_EXPECT(!meta.hasAccounting, "legacy accounting");
        {
            // Closed before the save below: an open reader holds the file and
            // HDF5 file locking (on by default on CI) refuses the writer.
            Hdf5Service reader;
            MIB_REQUIRE(reader.loadFile(legacy.string()), "load legacy accounting fixture");
            backend::recording::RecordingAccountingSnapshot a;
            MIB_EXPECT(!reader.readRunAccounting(a) && a.readError.empty() &&
                           a.completion == backend::recording::RunCompletionState::Unknown,
                       "legacy read remains absent, Unknown, without corruption reason");
            reader.closeFile();
        }
        MIB_EXPECT(s.accountingSummary().find("legacy") != std::string::npos, "legacy accounting text");
        s.setFallbackPixelToMicron(2.0);
        MIB_EXPECT(std::fabs(s.scatter().areaUm2[0] - 100.0 * 4.0) < 1e-9, "fallback change applies");
        std::string err;
        MIB_EXPECT(s.saveCoreRecordJson("{\"schema\":1}", false, &err), "first record on legacy: " + err);
    }
    wd.mark("legacy");

    // ---- recording round trip --------------------------------------------------
    {
        ReviewSession s;
        MIB_REQUIRE(s.open(recording.string()), "open recording");
        MIB_EXPECT(s.isRecordingFile(), "recording flag");
        const auto meta = s.metadata();
        MIB_EXPECT(meta.recordingFile && meta.totalValid == 9 && meta.filteredFrames == 2, "recording counts");
        MIB_EXPECT(meta.multiImageEnabled && meta.multiImageCount == 3, "multi image flags");
        MIB_EXPECT(meta.recordedImages.present && meta.recordedImages.count == 9, "recorded dataset");
        MIB_EXPECT(!meta.validMasks.present, "no masks in recording");
        MIB_EXPECT(s.frameCount(true) == 9 && s.frameCount(false) == 0, "recording sets");
        {
            std::vector<ProcessedFrame> frames;
            uint64_t ftotal = 0;
            MIB_REQUIRE(s.framesPage(true, 7, 5, frames, ftotal), "recording frames page");
            MIB_EXPECT(ftotal == 9 && frames.size() == 2 && frames[0].index == 7 && frames[1].timestampNs == 800,
                       "recording frames page rows");
            MIB_REQUIRE(s.framesPage(false, 0, 5, frames, ftotal), "recording invalid frames page");
            MIB_EXPECT(ftotal == 0 && frames.empty(), "recording has no invalid set");
            const auto set = s.validSet();
            MIB_EXPECT(set.open && set.recordingFile && set.metadataReadable && set.frames.size() == 9 &&
                           set.filePath == recording.string(),
                       "recording valid set");
        }
        ReviewImage img;
        MIB_REQUIRE(s.fetchImage(ReviewDataset::RecordedImage, 4, OverlayMode::AllContour, true, img), "recorded image");
        MIB_EXPECT(img.channels == 1 && img.data[0] == pattern(4).at<uint8_t>(0, 0), "recorded: overlay/roi ignored");
        MIB_REQUIRE(s.fetchImage(ReviewDataset::ValidImage, 4, OverlayMode::AllContour, false, img), "valid set routes to recorded");
        MIB_EXPECT(img.channels == 1, "recording valid set mono");
        uint64_t count = 0;
        MIB_REQUIRE(s.seriesInfo(0, count), "recording series info");
        MIB_EXPECT(count == 3, "recording window");
        MIB_REQUIRE(s.seriesInfo(8, count), "last frame series info");
        MIB_EXPECT(count == 0, "last frame has no window");
        ReviewImage w2;
        MIB_REQUIRE(s.fetchSeriesImage(1, 2, OverlayMode::None, false, w2), "window image");
        MIB_EXPECT(w2.data[0] == pattern(3).at<uint8_t>(0, 0), "window image is frame 3");
        MIB_EXPECT(s.scatter().frameIndex.empty(), "no scatter for recording");
        std::string err;
        MIB_EXPECT(!s.saveCoreRecordJson("{}", true, &err), "no record on recording");
        backend::review::ThumbnailStrip strip;
        MIB_REQUIRE(s.thumbnails(true, 0, 20, 8, OverlayMode::AllMask, true, strip), "recording thumbnails");
        MIB_EXPECT(strip.count == 9 && strip.channels == 1, "recording thumbnails mono");
    }
    wd.mark("recording");

    // ---- absent vs unreadable valid set (#512 S5) -----------------------------
    // An absent /valid_frames group is an empty, readable set; a metadata
    // dataset that exists but cannot be read is not (the facade's charts
    // refuse it with "Cannot read valid-frame metrics", as they did with a
    // reader of their own before S5). The invalid set is served either way.
    for (const bool unreadable : {false, true})
    {
        const auto path = td.path() / (unreadable ? "unreadable_valid.h5" : "no_valid_group.h5");
        writeExperiment(path, false, false, false);
        const hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open valid-set fault fixture");
        if (unreadable)
        {
            MIB_REQUIRE(H5Ldelete(file, "/valid_frames/metadata", H5P_DEFAULT) >= 0, "delete valid metadata");
            hsize_t dims[1] = {3};
            const hid_t space = H5Screate_simple(1, dims, nullptr);
            const hid_t ds = H5Dcreate2(file, "/valid_frames/metadata", H5T_NATIVE_INT, space, H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT);
            MIB_REQUIRE(ds >= 0, "replace valid metadata with an int dataset");
            const int data[3] = {1, 2, 3};
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
            H5Dclose(ds);
            H5Sclose(space);
        }
        else
        {
            MIB_REQUIRE(H5Ldelete(file, "/valid_frames", H5P_DEFAULT) >= 0, "delete valid group");
        }
        H5Fclose(file);
        ReviewSession s;
        MIB_REQUIRE(s.open(path.string()), "open valid-set fault fixture");
        MIB_EXPECT(s.validSet().metadataReadable == !unreadable,
                   unreadable ? "unreadable valid metadata reported readable" : "absent valid set reported unreadable");
        MIB_EXPECT(s.validSet().frames.empty(), "faulty valid set has no rows in the snapshot");
        const auto analysisRequest = nlohmann::json{{"expectedPath", path.string()},
                                                    {"feature", "area"},
                                                    {"minimum", nullptr},
                                                    {"maximum", nullptr}};
        MIB_EXPECT(!nlohmann::json::parse(s.analyzePopulation(analysisRequest.dump()))
                        .at("ok")
                        .get<bool>(),
                   "analysis refuses unreadable or count-incomplete metadata instead of reducing a "
                   "partial population");
        std::vector<ProcessedFrame> frames;
        uint64_t ftotal = 9;
        MIB_REQUIRE(s.framesPage(true, 0, 5, frames, ftotal), "valid page of a faulty set");
        MIB_EXPECT(frames.empty() && ftotal == 0, "faulty valid set serves no rows");
        MIB_REQUIRE(s.framesPage(false, 0, 5, frames, ftotal), "invalid page beside a faulty valid set");
        MIB_EXPECT(ftotal == 4 && frames.size() == 4, "invalid set still served");
        s.close();
        MIB_EXPECT(!s.validSet().metadataReadable && !s.validSet().open, "closed session not readable");
    }
    wd.mark("valid set faults");
    // Missing columns are unknown, never the legacy reader's default zero.
    {
        const auto path = td.path() / "missing_area.h5";
        writeExperiment(path, false, false, false);
        const hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open missing-column fixture");
        MIB_REQUIRE(H5Ldelete(file, "/valid_frames/metadata", H5P_DEFAULT) >= 0, "remove metadata");
        const hsize_t count = 10;
        const hid_t space = H5Screate_simple(1, &count, nullptr);
        const hid_t type = H5Tcreate(H5T_COMPOUND, sizeof(double));
        MIB_REQUIRE(H5Tinsert(type, "deformability", 0, H5T_NATIVE_DOUBLE) >= 0, "create partial schema");
        const hid_t ds = H5Dcreate2(file, "/valid_frames/metadata", type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        double values[10] = {};
        MIB_REQUIRE(ds >= 0 && H5Dwrite(ds, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) >= 0, "write partial schema");
        H5Dclose(ds); H5Tclose(type); H5Sclose(space); H5Fclose(file);
        ReviewSession s;
        MIB_REQUIRE(s.open(path.string()), "open partial-schema source");
        const auto request = nlohmann::json{{"expectedPath", path.string()}, {"feature", "area"},
            {"minimum", nullptr}, {"maximum", nullptr}};
        const auto result = nlohmann::json::parse(s.analyzePopulation(request.dump()));
        MIB_REQUIRE(result.at("ok").get<bool>(), "partial schema has readable counts");
        MIB_EXPECT(result.at("unknown") == "10" && result.at("selected") == "4",
                   "missing valid area is unknown while invalid recorded areas remain selected");
    }
    wd.mark("population missing column");


    // ---- open() is all or nothing (#512 S5) -----------------------------------
    // A malformed background (a dataset whose height does not fit an int)
    // makes cv::Mat throw while the metadata is read, after the reader has
    // opened. open() must close the reader again and report the failure, so a
    // host mirroring isOpen() (the facade's D1 flag) never misses an open
    // reader; a previously open file stays closed, and the next open works.
    {
        const auto bad = td.path() / "bad_background.h5";
        writeExperiment(bad, false, false, false);
        {
            const hid_t file = H5Fopen(bad.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            MIB_REQUIRE(file >= 0, "open background fault fixture");
            hsize_t dims[3] = {1, hsize_t{1} << 31, 1};
            hsize_t chunk[3] = {1, 1024, 1};
            const hid_t space = H5Screate_simple(3, dims, nullptr);
            const hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
            H5Pset_chunk(dcpl, 3, chunk);
            const hid_t ds = H5Dcreate2(file, "/experiment_info/background", H5T_NATIVE_UINT8, space, H5P_DEFAULT,
                                        dcpl, H5P_DEFAULT);
            MIB_REQUIRE(ds >= 0, "malformed background dataset");
            H5Dclose(ds);
            H5Pclose(dcpl);
            H5Sclose(space);
            H5Fclose(file);
        }
        {
            Hdf5Service probe;
            MIB_REQUIRE(probe.loadFile(bad.string()), "probe the fixture");
            bool threw = false;
            try
            {
                cv::Mat bg;
                probe.readBackgroundImage(bg);
            }
            catch (const std::exception &)
            {
                threw = true;
            }
            probe.closeFile();
            MIB_REQUIRE(threw, "the malformed background throws inside Hdf5Service (fixture premise)");
        }
        ReviewSession s;
        MIB_REQUIRE(s.open(experiment.string()), "a good file first");
        std::string err;
        bool threw = false;
        bool opened = true;
        try
        {
            opened = s.open(bad.string(), &err);
        }
        catch (...)
        {
            threw = true;
        }
        MIB_EXPECT(!threw, "open() does not throw for a malformed file");
        MIB_EXPECT(!opened && !s.isOpen(), "a throwing metadata read leaves the session closed");
        MIB_EXPECT(err.find("Failed to read " + bad.string()) == 0, "the error names the file: " + err);
        MIB_EXPECT(!s.metadata().fileOpen && s.filePath().empty() && s.frameCount(true) == 0 && !s.validSet().open,
                   "nothing of either file is left");
        ReviewImage img;
        MIB_EXPECT(!s.fetchImage(ReviewDataset::ValidImage, 0, OverlayMode::None, false, img), "no reads after it");
        threw = false;
        try
        {
            opened = s.open(bad.string(), &err);
        }
        catch (...)
        {
            threw = true;
        }
        MIB_EXPECT(!threw && !opened && !s.isOpen(), "from closed: still all or nothing");
        MIB_REQUIRE(s.open(legacy.string()), "the next open works");
        const auto set = s.validSet();
        MIB_EXPECT(set.open && set.filePath == legacy.string() && set.frames.size() == 10, "snapshot follows the reopen");
    }
    wd.mark("all or nothing");

    // ---- no reader handle outlives a throw; isOpen() fails closed (#512 S5) ----
    {
        const auto openFiles = [] { return H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_FILE); };
        const auto baseline = openFiles();
        ReviewSession s;
        auto &reader = backend::review::ReviewSessionTestAccess::reader(s);
        // A throw inside the reader's open, right after H5Fopen: the reader
        // releases the handle, the session reports the failure, closed.
        backend::services::Hdf5ServiceTestAccess::setAfterOpenHook(reader, [] { throw std::bad_alloc(); });
        std::string err;
        bool threw = false;
        bool opened = true;
        try
        {
            opened = s.open(experiment.string(), &err);
        }
        catch (...)
        {
            threw = true;
        }
        MIB_EXPECT(!threw && !opened && !s.isOpen() && !reader.holdsFileHandle(),
                   "a throw after H5Fopen leaves the session closed with no handle");
        MIB_EXPECT(err.find("Failed to read " + experiment.string()) == 0, "the error names the file: " + err);
        MIB_EXPECT(openFiles() == baseline, "no file handle leaked");
        backend::services::Hdf5ServiceTestAccess::setAfterOpenHook(reader, {});

        // A reader handle the session did not open (its own flag false):
        // isOpen() fails closed, nothing is served from it, close() and the
        // next open() release it.
        MIB_REQUIRE(reader.loadFile(experiment.string()), "a handle behind the session's back");
        MIB_EXPECT(s.isOpen(), "isOpen() counts a held reader handle");
        MIB_EXPECT(!s.metadata().fileOpen && !s.validSet().open && s.frameCount(true) == 0,
                   "nothing is served from a handle the session did not open");
        s.close();
        MIB_EXPECT(!s.isOpen() && !reader.holdsFileHandle() && openFiles() == baseline, "close() releases it");
        MIB_REQUIRE(reader.loadFile(experiment.string()), "a stray handle again");
        MIB_REQUIRE(s.open(legacy.string()), "open() over a stray handle");
        MIB_EXPECT(s.isOpen() && s.filePath() == legacy.string() && openFiles() == baseline + 1,
                   "open() released the stray handle first");
        s.close();
        MIB_EXPECT(!s.isOpen() && openFiles() == baseline, "closed");
    }
    wd.mark("fail closed");

    // ---- overlay compose is a pure function of its inputs ---------------------
    {
        cv::Mat original = pattern(1);
        cv::Mat mask = ringMask();
        backend::services::FilterResult v;
        v.isValid = true;
        const cv::Mat a = backend::review::composeOverlay(original, mask, &v, OverlayMode::AllContour);
        const cv::Mat b = backend::review::composeOverlay(original, mask, &v, OverlayMode::AllContour);
        MIB_EXPECT(a.type() == CV_8UC3 && cv::norm(a, b, cv::NORM_INF) == 0, "deterministic overlay");
        const cv::Mat none = backend::review::composeOverlay(original, cv::Mat(), &v, OverlayMode::AllContour);
        MIB_EXPECT(none.type() == CV_8UC1, "no mask falls back to original");
        // Valid is green on the contour; target is blue.
        bool green = false, blue = false;
        for (int y = 0; y < a.rows; ++y)
            for (int x = 0; x < a.cols; ++x)
            {
                const auto p = a.at<cv::Vec3b>(y, x);
                if (p == cv::Vec3b(0, 255, 0)) green = true;
            }
        v.isTargetGroup = true;
        const cv::Mat t = backend::review::composeOverlay(original, mask, &v, OverlayMode::AllContour);
        for (int y = 0; y < t.rows; ++y)
            for (int x = 0; x < t.cols; ++x)
                if (t.at<cv::Vec3b>(y, x) == cv::Vec3b(0, 120, 255)) blue = true;
        MIB_EXPECT(green && blue, "classification colours");
    }

    // One session mutex fences analysis against reopen/close across sources.
    // The existing watchdog bounds both joins (a lock-order regression exits 99).
    {
        ReviewSession s;
        std::atomic<bool> start{false};
        std::atomic<int> failures{0};
        const auto request = nlohmann::json{{"expectedPath", experiment.string()}, {"feature", "area"},
            {"minimum", nullptr}, {"maximum", nullptr}}.dump();
        std::thread reader([&] {
            while (!start.load()) std::this_thread::yield();
            for (int i = 0; i < 200; ++i) {
                const auto result = nlohmann::json::parse(s.analyzePopulation(request));
                if (result.at("ok").get<bool>() &&
                    (result.at("path") != experiment.string() || result.at("selected") != "14" ||
                     result.at("valid") != "10" || result.at("invalid") != "4")) ++failures;
            }
        });
        std::thread writer([&] {
            start.store(true);
            for (int i = 0; i < 20; ++i) {
                if (!s.open(i % 2 ? experiment.string() : legacy.string())) ++failures;
                if (i % 3 == 0) s.close();
            }
        });
        reader.join(); writer.join();
        MIB_EXPECT(failures.load() == 0, "analysis must not mix sources during reopen/close");
    }
    wd.mark("population source concurrency");
    return mib::test::exitCode();
}
