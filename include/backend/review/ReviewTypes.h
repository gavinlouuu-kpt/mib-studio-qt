// Review data types shared by every shell (Qt, React + Tauri MIB Studio,
// YOFO Review) — plan 2026-10-01-standalone-review-app, ADR 0014.
//
// Qt-free and OpenCV-light: images cross as packed byte buffers so the
// bridge can hand them to a webview without re-encoding. Enumerations carry
// contract-pinned values (crates/mib-bridge/contract/bridge-contract.json);
// append, never renumber.
#pragma once

#include "backend/processing/ProcessingService.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/recording/RunProvenanceRecord.h"
#include "backend/recording/RunStartIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::review
{

    // Review contract `review_probe.max_paths`: the most paths one identity
    // probe accepts (the experiment library probes a folder in pages).
    inline constexpr std::size_t kReviewProbeMaxPaths = 256;

    // Review contract `review_track_rows.max_rows` (#717 B3b): the most rows
    // one linked-records query returns.
    inline constexpr std::size_t kReviewTrackRowsMax = 256;

    // Review contract `review_reprocess_record.max_objects` (#717 B3b): the
    // most reprocessed objects one record's reprocess returns.
    inline constexpr std::size_t kReviewReprocessMaxObjects = 64;

    // Review contract `review_reprocess_record.settings`: which processing
    // inputs a one-record reprocess uses. Append, never renumber.
    enum class ReprocessSettings : std::uint32_t
    {
        // The file's recorded config, ROI and background (the regenerate
        // default); the defaults when the file records no config.
        Recorded = 0,
        // The default config, the whole image as ROI, no background (the
        // regenerate "default settings").
        Default = 1,
    };

    // What a run file says about its own identity (#717 B1a), read from run
    // snapshot v3 (#712, `/run_provenance @run_snapshot_json`) and the planned
    // annotation (`/experiment_annotation`). Nothing is fabricated: a file
    // that predates the snapshot or the annotation reads with `hasSnapshot` /
    // `annotation` false / nullopt and every member nullopt, never 0 or "".
    struct RunIdentity
    {
        // The file carries a run snapshot that parses as a JSON object.
        bool hasSnapshot{false};
        // The file carries a snapshot attribute that is not a JSON object:
        // present but unreadable, which is not the same as a legacy file.
        bool snapshotUnreadable{false};
        // run_snapshot_json "schema_version" (1 for a snapshot that predates
        // the field); meaningful only when hasSnapshot.
        std::uint32_t snapshotVersion{0};
        std::optional<std::string> runId;               // the run's UUID (v3)
        recording::RunStartIdentity declared;           // experiment_id, run_index, sample_ref, ... (v3)
        // /experiment_annotation as planned at Start; nullopt when the file has
        // none. `annotationUnreadable`: the block exists but is not a JSON object.
        std::optional<recording::ExperimentAnnotation> annotation;
        bool annotationUnreadable{false};
        // @annotation_schema_version; nullopt when the attribute is missing or
        // unreadable (a recorded 0 stays 0).
        std::optional<std::uint32_t> annotationSchemaVersion;
        // The E-modulus LUT the run used (snapshot v3 `emodulus_lut`, #717
        // B3b); nullopt when the snapshot records none (before v3, or null).
        // `loaded` false: the run had no LUT (E values were unavailable).
        std::optional<recording::EModulusLutRecord> eModulusLut;
        // The snapshot's `profile_id` (the only "method" a file records);
        // nullopt when missing, not a string, or empty.
        std::optional<std::string> profileId;
    };

    // One file of the experiment library's identity probe (#717 B1a): header
    // attributes only, no images and no metadata rows. `ok` false = the file
    // could not be read (`message` says why); every other member is then
    // absent. Each count and time is read on its own, presence-aware: nullopt
    // when its attribute is missing or unreadable, and a recorded 0 count is 0.
    struct RunProbe
    {
        std::string path;
        bool ok{false};
        std::string message;
        bool recordingFile{false};
        RunIdentity identity;
        std::optional<std::uint64_t> startTimeNs;       // 0 / missing attribute = not recorded
        std::optional<std::uint64_t> endTimeNs;
        // /experiment_info total_valid_frames / total_invalid_frames, or
        // /recording_info total_recorded_frames (valid; no invalid count) for
        // a recording file.
        std::optional<std::uint64_t> totalValid;
        std::optional<std::uint64_t> totalInvalid;
        // Run accounting (#367); nullopt for a file that predates it.
        std::optional<recording::RunCompletionState> completion;
        // The processing core that wrote the file; nullopt when not recorded.
        std::optional<std::string> coreVersion;
    };

    // Contract `review_image_datasets` (ABI 9+).
    enum class ReviewDataset : std::uint32_t
    {
        ValidImage = 0,
        InvalidImage = 1,
        RecordedImage = 2,
        ValidMask = 3,
        InvalidMask = 4,
    };

    // Contract `overlay_modes` (ABI 15). Mirrors frontend::OverlayMode.
    enum class OverlayMode : std::uint32_t
    {
        None = 0,
        AllContour = 1,
        OuterInnerColorCoded = 2,
        AllMask = 3,
        FilteredMask = 4,
    };

    struct DatasetInfo
    {
        bool present{false};
        std::uint64_t count{0};
        int height{0};
        int width{0};
        int channels{0};
    };

    // Everything a shell shows about the open file without reading pixels.
    struct ReviewMetadata
    {
        bool fileOpen{false};
        std::string filePath;
        bool recordingFile{false};
        std::uint64_t startTimeNs{0};
        std::uint64_t endTimeNs{0};
        std::uint64_t totalValid{0};
        std::uint64_t totalInvalid{0};
        // Recording files: frames filtered (empty) at record time.
        std::uint64_t filteredFrames{0};
        services::ProcessingService::Roi roi{};
        bool hasBackground{false};
        bool hasCoreIdentity{false};
        std::string coreVersion;
        std::string coreSource;
        std::string coreReleaseTag;
        DatasetInfo validImages;
        DatasetInfo invalidImages;
        DatasetInfo validMasks;
        DatasetInfo invalidMasks;
        DatasetInfo recordedImages;
        // Multi-image series: experiment files carry a 4D valid-series
        // dataset; recording files a per-frame window of `multiImageCount`.
        bool hasSeries{false};
        std::uint64_t seriesCount{0};
        bool multiImageEnabled{false};
        std::uint64_t multiImageCount{1};
        // Run accounting (issue #367); absent for legacy files.
        bool hasAccounting{false};
        recording::RecordingAccountingSnapshot accounting;
        // TD-17: the pixel-to-micron factor the file was recorded with (run
        // snapshot `pixel_to_micron`), else the host-supplied fallback.
        double pixelToMicron{0.0};
        bool pixelToMicronFromFile{false};
        // The recorded processing config's ring-ratio thresholds (histogram
        // range); the ProcessingConfig defaults when the file has none.
        bool hasRecordedConfig{false};
        double ringRatioMin{15.0};
        double ringRatioMax{25.0};
        // Stored KDE records (JSON as written; empty when absent).
        std::string kdeAnalysisJson;
        std::string kdeLiveJson;
        // Run identity from snapshot v3 and the annotation (#717 B1a); absent
        // members for a legacy file.
        RunIdentity runIdentity;
    };

    // One metrics row: the full FilterResult the Qt HdfMetricsModel shows.
    struct MetricRow
    {
        std::uint64_t frameIndex{0};
        std::uint64_t timestampNs{0};
        bool valid{false};
        bool targetGroup{false};
        bool touchesBorder{false};
        bool hasSingleInnerContour{false};
        bool inRange{false};
        bool inChannel{true};
        int innerContourCount{0};
        int objectId{-1};
        int objectCount{0};
        int trackId{-1};
        std::uint64_t trackFirstFrame{0};
        std::uint64_t trackLastFrame{0};
        int trackObservationCount{0};
        double bboxX{0.0};
        double bboxY{0.0};
        double bboxWidth{0.0};
        double bboxHeight{0.0};
        double centroidX{0.0};
        double centroidY{0.0};
        double area{0.0}; // px²
        double areaUm2{0.0};
        double deformability{0.0};
        double areaRatio{0.0};
        double ringRatio{0.0};
        double laplacianVariance{0.0};
        double youngsModulus{0.0};
        double brightnessQ1{0.0};
        double brightnessQ2{0.0};
        double brightnessQ3{0.0};
        double brightnessQ4{0.0};
    };

    // One linked-records query (#717 B3b): the records of a set that share a
    // track id, in set order. `positions` are their indices in the set (the
    // index fetch_review_frame and metricsPage take), parallel to `rows`;
    // `total` counts every match, so `truncated` = total > rows.size().
    struct TrackRows
    {
        std::vector<std::uint64_t> positions;
        std::vector<MetricRow> rows;
        std::uint64_t total{0};
        bool truncated{false};
    };

    // One record reprocessed (#717 B3b): the stored record next to what the
    // review session's processing kernel finds in the same frame now. Never
    // written anywhere. Both are in image coordinates (computeProcessedFrame,
    // whatever the ROI); `match` is the reprocessed object whose centroid is
    // nearest the stored one, inside the stored bounding box.
    struct ReprocessResult
    {
        ReprocessSettings settings{ReprocessSettings::Recorded};
        // The recorded config, ROI and background were used (false for
        // Default, and for Recorded on a file that records no config).
        bool usedRecordedSettings{false};
        // Which inputs were used, in words, e.g. "recorded settings (config,
        // ROI, background)" or "default settings: the file records none".
        std::string settingsLabel;
        // The kernel that reprocessed it (the review session's bundled core,
        // not necessarily the one that recorded the file).
        std::string coreVersion;
        std::string coreSource;
        MetricRow stored;
        std::vector<MetricRow> reprocessed; // at most kReviewReprocessMaxObjects
        std::optional<std::size_t> match;   // index into `reprocessed`
        // The kernel found another number of objects than the stored record's
        // object count.
        bool objectCountDiffers{false};
        bool truncated{false};
    };

    // A packed 8-bit image: Mono8 (channels 1) or RGB (channels 3), row
    // stride = width * channels.
    struct ReviewImage
    {
        std::uint64_t frameIndex{0};
        std::uint64_t width{0};
        std::uint64_t height{0};
        std::uint32_t channels{1};
        std::vector<std::uint8_t> data;
    };

    // `count` square tiles of `size` px, letterboxed, packed one after the
    // other (tile k starts at k * size * size * channels).
    struct ThumbnailStrip
    {
        bool valid{false};
        std::uint64_t offset{0};
        std::uint64_t count{0};
        std::uint32_t size{0};
        std::uint32_t channels{1};
        std::vector<std::uint8_t> data;
    };

    // Columnar valid-set scatter (rows with validation.isValid only, in
    // valid-set order): what both Charts views plot.
    struct ScatterData
    {
        double pixelToMicron{0.0};
        std::vector<std::uint64_t> frameIndex;  // recorded ProcessedFrame::index
        std::vector<std::uint64_t> validPosition; // 0-based position in the valid set
        std::vector<double> areaUm2;
        std::vector<double> deformability;
        std::vector<std::uint8_t> targetGroup;
        std::vector<double> ringRatio; // histogram input (Qt bins ringRatio > 0)
    };

} // namespace backend::review
