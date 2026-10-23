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
// The identity probe (#717 B1a) takes the same mutex per file.
#pragma once

#include "backend/review/ReviewTypes.h"
#include "backend/review/RunStatistics.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cv { class Mat; }
namespace backend::services { class Hdf5Service; struct ProcessedFrame; }

namespace backend::review
{

    struct ReviewSessionTestAccess; // the session's reader, for fault tests

    class ReviewSession
    {
    public:
        ReviewSession();
        ~ReviewSession();
        ReviewSession(const ReviewSession &) = delete;
        ReviewSession &operator=(const ReviewSession &) = delete;

        // Opens `path` read-only, replacing any open file. All or nothing: on
        // any failure, including a metadata read that throws after the
        // reader opened, the session is closed (never left open) and `error`
        // (optional) names the reason; it does not throw for a bad file.
        bool open(const std::string &path, std::string *error = nullptr);
        void close();
        // True while a file is open, and also while the reader holds a handle
        // whose open did not complete (fail closed: hosts mirror it to keep
        // writers out, #512 S5); close() releases either.
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
        // snapshot `pixel_to_micron`), else the factor a regenerated or
        // reanalysed file was analysed at (`/experiment_info
        // @pixel_to_micron`, #758) unless its `@pixel_to_micron_source` says
        // it was a fallback (#764: anything but "recorded" or "operator";
        // files without the attribute read as before); 0 when the file
        // records neither.
        static double recordedPixelToMicron(const services::Hdf5Service &reader);
        // #717 B1a: the run identity of `reader`'s open file (snapshot v3 +
        // /experiment_annotation, and a regenerated file's E-LUT record,
        // review ABI 5). Attributes only; absent members for a legacy file,
        // never a default. Does not throw for a bad attribute.
        static RunIdentity readRunIdentity(const services::Hdf5Service &reader);
        // One metrics row of a processed frame, area in µm² by `pixelToMicron`
        // (the row metricsPage serves; #717 B3b reprocess uses it too).
        static MetricRow metricRow(const services::ProcessedFrame &frame, double pixelToMicron);

        // #717 B1a: the experiment library's identity probe. Each path is
        // opened with its own short-lived reader, its header attributes read
        // (RunProbe: identity, times, counts, accounting, core version; no
        // images, no metadata rows) and closed again. A file that cannot be
        // read is a per-file error (`ok` false), never a failure of the call.
        // Refused (false, `error`) for more than kReviewProbeMaxPaths paths.
        //
        // Serialized with this session's own HDF5 access (#722: HDF5 is built
        // without thread safety): each file is probed under the session's
        // mutex, the one every read, open and close takes. The open file is
        // left as it was. Review jobs open their own readers outside that
        // mutex, so hosts gate the probe on the job lane as well
        // (ReviewJobs::probeRuns); the bridge only calls that one.
        bool probeRuns(const std::vector<std::string> &paths, std::vector<RunProbe> &out,
                       std::string *error = nullptr) const;

        // #716 A7a: one file's run statistics (RunStatistics.h,
        // readRunStatistics: a short-lived reader of its own, the valid set
        // read as bounded columns), under this session's mutex like the probe
        // (#722); the open file is left as it was. Hosts gate it on the job
        // lanes as well (ReviewJobs::runStatistics); the bridges only call that.
        RunStatisticsResult runStatistics(const std::string &path, const std::string &computedAt) const;

        // Bounded atomic both-set native interval analysis of the open file.
        // Read-only metadata; result identity is ephemeral, not a durable fingerprint.
        std::string analyzePopulation(const std::string& requestJson) const;
        ReviewMetadata metadata() const;
        bool isRecordingFile() const;

        // #717 B3b: the records of a set (valid / invalid) that share
        // `trackId`, in set order, at most `max` rows (TrackRows). From the
        // metadata read at open: no HDF5 read, an O(n) scan under the
        // session's mutex. Refused (false, `error`) when no file is open, for
        // a recording file (no records), for `max` above kReviewTrackRowsMax
        // and for a negative track id (an untracked record has no links).
        bool trackRows(bool valid, int trackId, std::size_t max, TrackRows &out,
                       std::string *error = nullptr) const;

        // #717 B3b: what reprocessing one record needs, read under the
        // session's mutex: the record's stored row, its image (gray, as the
        // regenerate engine converts colour frames) and, for Recorded, the
        // file's recorded config, ROI and background (the defaults when the
        // file records no config); for Default, the default config, the whole
        // image and no background. Refused (false, `error`) when no file is
        // open, for a recording file, for an index outside the set and for a
        // record whose image cannot be read. Hosts gate it on the job lanes
        // (ReviewJobs::reprocessRecord): jobs read outside this mutex.
        struct ReprocessInputs
        {
            MetricRow stored;
            cv::Mat image; // gray
            cv::Mat background;
            services::ProcessingConfig config{};
            services::ProcessingService::Roi roi{}; // the ROI to process with
            bool usedRecordedSettings{false};
            std::string settingsLabel;
            double pixelToMicron{0.0}; // the stored rows' factor (recorded or fallback)
            // The factor to process with (Recorded: the file's, or the
            // fallback when it records none; 0 for Default: the processing
            // default) and whether the file records it.
            double processingPixelToMicron{0.0};
            bool pixelToMicronFromFile{false};
            // The stored row's coordinate space (ReviewMetadata): `stored`
            // is as stored; toImageSpace places it.
            ObjectCoordinates objectCoordinates;
            // The E-LUT the file records (snapshot v3 `emodulus_lut`, else a
            // regenerated file's `@emodulus_lut_json`; read at open);
            // nullopt when it records none (#764).
            std::optional<recording::ProvenancedEModulusLut> eModulusLut;
        };
        bool reprocessInputs(bool valid, std::uint64_t index, ReprocessSettings settings, ReprocessInputs &out,
                             std::string *error = nullptr) const;

        // Metadata rows of a set (valid / invalid; recording files have only
        // the valid set with index + timestamp filled).
        std::uint64_t frameCount(bool valid) const;
        bool metricsPage(bool valid, std::uint64_t offset, std::uint64_t count,
                         std::vector<MetricRow> &rows, std::uint64_t &totalOut) const;
        // The same page as metadata-only ProcessedFrames (the rows as read,
        // no pixels), for hosts that map them to their own row type (the
        // facade's MonitoringObjectRow, #512 S5). Same bounds and totals as
        // metricsPage; false only when no file is open.
        bool framesPage(bool valid, std::uint64_t offset, std::uint64_t count,
                        std::vector<services::ProcessedFrame> &frames, std::uint64_t &totalOut) const;
        // The open file's valid set in one snapshot, taken under one lock so
        // the identity and the rows belong to the same file even while another
        // thread opens a different one (the facade's charts, #512 S5).
        // `metadataReadable` is false for an experiment file whose
        // `/valid_frames/metadata` exists but could not be read at open, or
        // whose presence could not be determined (an absent dataset is an
        // empty, readable set: "Cannot read valid-frame metrics" vs no rows);
        // true for recording files. All empty / false when no file is open.
        struct ValidSet
        {
            bool open{false};
            std::string filePath;
            bool recordingFile{false};
            bool metadataReadable{false};
            std::vector<services::ProcessedFrame> frames; // metadata only, no pixels
        };
        ValidSet validSet() const;
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
        friend struct ReviewSessionTestAccess;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Test-only: the session's own reader (fault injection through
    // services::Hdf5ServiceTestAccess, or a handle held behind its back).
    struct ReviewSessionTestAccess
    {
        static services::Hdf5Service &reader(ReviewSession &session);
    };

} // namespace backend::review
