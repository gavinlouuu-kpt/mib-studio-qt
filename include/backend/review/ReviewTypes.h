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

#include <cstdint>
#include <string>
#include <vector>

namespace backend::review
{

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
