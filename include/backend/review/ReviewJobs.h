// Review jobs — the long-running work behind the review surface (plan
// 2026-10-01-standalone-review-app, ADR 0008): metrics / full / batch
// exports over HdfExportService, mask regeneration over
// ProcessingService::processBatch, the full-run KDE core contour and the
// review density estimate. Qt-free; lives in mib_review_core.
//
// Operation semantics follow ADR 0004: every job has an id, emits Started,
// bounded Progress and exactly one terminal state (Completed / Failed /
// Cancelled), observes its cancel flag, and never publishes a partial
// output. Jobs open their own readers on the session's file path so they
// never block the session's interactive reads. One job runs at a time per
// runner (the Qt tab's rule); a second start while one runs is refused.
#pragma once

#include "backend/recording/HdfExportService.h"
#include "backend/review/ReviewTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace backend::services { class ProcessingService; }
namespace backend::monitoring { struct DensityPoint; }

namespace backend::review
{
    class ReviewSession;

    // Contract `review_operation_kinds`.
    enum class ReviewJobKind : std::uint32_t
    {
        ExportMetrics = 0,
        ExportAll = 1,
        BatchExport = 2,
        RegenerateMasks = 3,
        ComputeCore = 4,
        Density = 5,
        ExportCharts = 6,
    };

    // Contract `operation_states`.
    enum class ReviewJobState : std::uint32_t
    {
        Started = 0,
        Progress = 1,
        Completed = 2,
        Failed = 3,
        Cancelled = 4,
        TimedOut = 5,
    };

    struct ReviewJobEvent
    {
        std::uint64_t operationId{0};
        ReviewJobKind kind{ReviewJobKind::ExportMetrics};
        ReviewJobState state{ReviewJobState::Started};
        std::uint64_t progress{0};
        std::uint64_t total{0};
        std::string message; // terminal: output path / summary / error
    };

    using ReviewJobSink = std::function<void(const ReviewJobEvent &)>;

    struct ExportSeriesRange
    {
        bool exportSeries{true};
        std::uint64_t startInclusive{0};
        std::uint64_t endInclusive{static_cast<std::uint64_t>(-1)};
    };

    // Chart snapshots the shell rendered (PNG or TIFF bytes), written next
    // to the images as `name` (the Qt tab uses "scatter_plot.tiff" and
    // "ring_width_histogram.tiff").
    struct ChartSnapshot
    {
        std::string name;
        std::vector<std::uint8_t> encoded;
    };

    struct ExportAllRequest
    {
        std::string outputRoot;
        ExportSeriesRange series;
        std::vector<ChartSnapshot> charts;
    };

    // Export Charts: the shell's snapshots written as files into an existing
    // directory (Qt: scatter_plot.tiff, ring_width_histogram.tiff). All or
    // nothing — written under temporary names, renamed once every one
    // encoded.
    struct ExportChartsRequest
    {
        std::string outputDir;
        std::vector<ChartSnapshot> charts;
    };

    struct BatchExportRequest
    {
        std::vector<std::string> sources;
        std::string outputRoot;
        bool metricsOnly{false};
        ExportSeriesRange series;
    };

    enum class RegenerateSource : std::uint32_t
    {
        CurrentValid = 0,   // the open file's valid (or recorded) images, [start, start+count)
        CurrentInvalid = 1, // the open file's invalid images
        WholeFile = 2,      // every image of the open file (recording: all; experiment: valid + invalid)
        Avi = 3,            // `sourcePath` is an AVI
        Folder = 4,         // `sourcePath` is a folder of TIFF/PNG/JPEG
    };

    struct RegenerateMasksRequest
    {
        RegenerateSource source{RegenerateSource::WholeFile};
        std::string sourcePath;
        std::uint64_t startIndex{0};
        std::uint64_t count{0}; // 0 = to the end
        std::string outputPath;
        // Processing inputs. When `useRecordedConfig` the open file's recorded
        // config, ROI and background are used (the reanalysis default); else
        // the given ones.
        bool useRecordedConfig{true};
        services::ProcessingConfig config{};
        services::ProcessingService::Roi roi{};
        bool synthesizeBackground{false};
    };

    struct DensityRequest
    {
        double bandwidthFactor{1.0};
        double coreFraction{0.9};
        int levels{8};
        bool wantCoreRecord{true}; // compute a full-run record when the file has none
    };

    // Result of the last density job (parallel to ReviewSession::scatter()).
    struct DensityResult
    {
        bool ready{false};
        // The file the levels belong to; density() returns an empty result
        // once the session holds another file.
        std::string sourcePath;
        std::vector<std::uint8_t> levels;
        int levelCount{8};
        double bandwidthFactor{1.0};
        double coreFraction{0.9};
        std::string computedRecordJson; // empty when the file has a stored full-run record
    };

    class ReviewJobs
    {
    public:
        // `session` outlives the runner; `processing` may be null (regenerate
        // masks then fails with a message).
        ReviewJobs(ReviewSession &session, services::ProcessingService *processing, ReviewJobSink sink);
        ~ReviewJobs();
        ReviewJobs(const ReviewJobs &) = delete;
        ReviewJobs &operator=(const ReviewJobs &) = delete;

        // Each returns the operation id (0 = refused; `error` says why).
        std::uint64_t startExportMetrics(const std::string &outputPath, std::string *error = nullptr);
        std::uint64_t startExportAll(const ExportAllRequest &request, std::string *error = nullptr);
        std::uint64_t startBatchExport(const BatchExportRequest &request, std::string *error = nullptr);
        std::uint64_t startExportCharts(const ExportChartsRequest &request, std::string *error = nullptr);
        std::uint64_t startRegenerateMasks(const RegenerateMasksRequest &request, std::string *error = nullptr);
        std::uint64_t startComputeCore(double coreFraction, std::string *error = nullptr);
        std::uint64_t startDensity(const DensityRequest &request, std::string *error = nullptr);

        bool cancel(std::uint64_t operationId);
        bool busy() const;
        // The record the last ComputeCore / Density job produced for the file
        // the session holds now (JSON, empty when none or computed for
        // another file); the shell saves it through ReviewSession.
        std::string computedCoreJson() const;
        DensityResult density() const;
        // Cancels the running job and joins it. Called by the destructor.
        void shutdown();

        // Pure helpers, exposed for tests.
        static int levelForDensity(double density, int levels);
        // A chart file name the export accepts: a bare file name (no
        // directory parts) ending in .tif / .tiff / .png.
        static bool validChartName(const std::string &name);
        static std::vector<double> densityAtPoints(const std::vector<backend::monitoring::DensityPoint> &points,
                                                   double bandwidthFactor, std::size_t gridAbove,
                                                   std::uint32_t seed);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace backend::review
