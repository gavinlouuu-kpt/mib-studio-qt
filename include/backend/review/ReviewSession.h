// ReviewSession — the one review implementation behind every shell (plan
// 2026-10-01-standalone-review-app, ADR 0014).
//
// Owns the open HDF5 file through its own Hdf5Service reader (never the
// experiment writer's handle), caches the metadata rows once per open, and
// serves bounded reads: metrics pages, single frames with the overlay
// composed here, series frames, packed thumbnail strips, the columnar
// scatter, run accounting, stored KDE records and the recorded
// pixel-to-micron factor (TD-17). Qt-free; lives in `mib_review_core`,
// built on `mib_processing` only.
//
// Thread safety: every public call locks one mutex (Hdf5Service is not
// thread-safe). Long jobs (export, batch, regenerate masks, core contour)
// open their own readers — see ReviewJobs.h — so they never block a read.
#pragma once

#include "backend/review/ReviewTypes.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cv { class Mat; }
namespace backend::services { class Hdf5Service; struct ProcessedFrame; }

namespace backend::review
{

    class ReviewSession
    {
    public:
        ReviewSession();
        ~ReviewSession();
        ReviewSession(const ReviewSession &) = delete;
        ReviewSession &operator=(const ReviewSession &) = delete;

        // Opens `path` read-only, replacing any open file. On failure the
        // session is closed and `error` (optional) names the reason.
        bool open(const std::string &path, std::string *error = nullptr);
        void close();
        bool isOpen() const;
        std::string filePath() const;

        // Factor used when the file carries none (legacy files): the host's
        // live/preferred pixel-to-micron. Takes effect on the next open and
        // immediately for scatter/metrics when the file has no recorded value.
        void setFallbackPixelToMicron(double factor);
        // The effective factor (recorded value when present, else fallback).
        double pixelToMicron() const;
        double fallbackPixelToMicron() const;
        // TD-17: the factor `reader`'s open file was recorded with (run
        // snapshot `pixel_to_micron`); 0 when the file records none.
        static double recordedPixelToMicron(const services::Hdf5Service &reader);

        ReviewMetadata metadata() const;
        bool isRecordingFile() const;

        // Metadata rows of a set (valid / invalid; recording files have only
        // the valid set with index + timestamp filled).
        std::uint64_t frameCount(bool valid) const;
        bool metricsPage(bool valid, std::uint64_t offset, std::uint64_t count,
                         std::vector<MetricRow> &rows, std::uint64_t &totalOut) const;
        // Copy of one metadata row (no pixels).
        bool frameMeta(bool valid, std::uint64_t index, services::ProcessedFrame &out) const;

        // One image from a dataset with the overlay composed here. Mask
        // datasets and RecordedImage ignore `mode`; `roiOverlay` draws the
        // recorded ROI for experiment files. Images are Mono8 unless an
        // overlay or ROI was drawn (then RGB).
        bool fetchImage(ReviewDataset dataset, std::uint64_t index, OverlayMode mode,
                        bool roiOverlay, ReviewImage &out) const;
        // Raw read (Mono8 / as stored), no overlay — for jobs and tests.
        bool readRaw(ReviewDataset dataset, std::uint64_t index, cv::Mat &out) const;

        // Multi-image series of one valid frame: experiment 4D dataset or the
        // recording's per-frame window. `count` is 0 when the frame has none.
        bool seriesInfo(std::uint64_t index, std::uint64_t &count) const;
        bool fetchSeriesImage(std::uint64_t index, std::uint64_t k, OverlayMode mode,
                              bool roiOverlay, ReviewImage &out) const;

        // `count` thumbnails of set `valid` from `offset`, each letterboxed
        // into a `size` × `size` tile; one packed buffer per page.
        bool thumbnails(bool valid, std::uint64_t offset, std::uint64_t count, std::uint32_t size,
                        OverlayMode mode, bool roiOverlay, ThumbnailStrip &out) const;

        // Columnar valid-set scatter (validation.isValid rows only).
        ScatterData scatter() const;

        // The Qt tab's status-line accounting text (issue #367).
        std::string accountingSummary() const;

        // Write the full-run KDE core record (`/analysis @kde_core_json`).
        // Refuses when a record exists and `overwrite` is false, or the file
        // cannot be opened for update. The reader is closed for the write
        // and reopened; metadata is refreshed.
        bool saveCoreRecordJson(const std::string &json, bool overwrite, std::string *error = nullptr);

        // The complete frame as the Qt viewer loads it (image, mask, series):
        // shared by the frame pane, the viewer and the jobs.
        bool loadFrameForDisplay(bool valid, std::uint64_t index, services::ProcessedFrame &out) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace backend::review
