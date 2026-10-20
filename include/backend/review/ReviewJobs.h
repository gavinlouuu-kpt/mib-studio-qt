// Review jobs — the long-running work behind the review surface (plan
// 2026-10-01-standalone-review-app, ADR 0014): metrics / full / batch
// exports over HdfExportService, mask regeneration over
// ProcessingService::processBatch, the full-run KDE core contour and the
// review density estimate. Qt-free; lives in mib_review_core.
//
// Operation semantics follow ADR 0004: every job has an id, emits Started,
// bounded Progress and exactly one terminal state (Completed / Failed /
// Cancelled), observes its cancel flag, and never publishes a partial
// output. Jobs open their own readers on the session's file path so they
// never block the session's interactive reads.
//
// Lanes (#512 S7, decision D7): every job runs on one of two lanes, exports
// and processing, single flight per lane (a second start on a busy lane is
// refused). Whether the two lanes may run at the same time is the host's
// policy (`ReviewJobsOptions`): MIB Studio's facade runs one export and one
// reanalysis at once, as it did before #512; YOFO Review's bridge keeps one
// job of any kind at a time (the Qt tab's rule) until #722 makes concurrent
// HDF5 use safe. A host may also own the operations (`ReviewJobHost`): the
// facade's ids, cancel flags, operation kinds and shutdown cancellation.
#pragma once

#include "backend/recording/HdfExportService.h"
#include "backend/review/ReviewTypes.h"

#include <atomic>
#include <chrono>
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
    struct ExportBatchRequest; // backend/review/ExportEngine.h
    struct ReanalysisRequest;  // backend/review/Reanalysis.h

    // Contract `review_operation_kinds` (0-6). `Export` and `Reanalysis` are
    // the bridge contract's JSON jobs (`review_export_json`,
    // `review_reanalysis_json`) that MIB Studio's facade runs here since #512
    // S7. They are not in review-contract.json (slice S12 proposes them with
    // these values), and the review bridge never starts them.
    enum class ReviewJobKind : std::uint32_t
    {
        ExportMetrics = 0,
        ExportAll = 1,
        BatchExport = 2,
        RegenerateMasks = 3,
        ComputeCore = 4,
        Density = 5,
        ExportCharts = 6,
        Export = 7,
        Reanalysis = 8,
    };

    // D7 (#512): the two lanes of a session's jobs.
    enum class ReviewJobLane : std::uint32_t
    {
        Export = 0,     // Export Metrics / All / Batch / Charts, review_export_json
        Processing = 1, // Regenerate masks, core contour, density, review_reanalysis_json
    };

    ReviewJobLane laneOf(ReviewJobKind kind);

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

    // Receives every job event, on the thread that produced it (the starting
    // thread for Started and for a terminal reported at start, else the job's
    // worker), never under ReviewJobs' lock. Callbacks may call `shutdown()`
    // or `cancel()`, but must not destroy this ReviewJobs or its host. Hosts
    // deliver events to their consumers asynchronously (the review bridge and
    // the facade's bridge convert and enqueue; the shell drains the queue).
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
        std::uint64_t count{0}; // 0 = to the end (clamped to the set)
        // A new file. Runs on the reanalysis engine (backend/review/Reanalysis.h,
        // #512 S8): written as a partial file beside it and published without
        // replacing, so an existing output is refused at start and one created
        // while the job runs fails the job, untouched.
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

    // A host that tracks the jobs as its own operations (MIB Studio's facade:
    // BackendFacade::beginOperation / reportOperationProgress / finishOperation,
    // ADR 0004). Without one, ReviewJobs numbers its jobs from 1 and reports
    // Started through the sink as well (YOFO Review's bridge).
    struct ReviewJobHost
    {
        using CancelFlag = std::shared_ptr<std::atomic<bool>>;
        // Called on the starting thread once the job's lane is claimed and
        // before its worker exists: registers the operation, emits its
        // Started (`message`: the job's source) and returns its non-zero id
        // with the cancel flag the job observes (the host's cancel sets it).
        // The sink then receives the job's Progress and terminal events with
        // that id, never a Started. Like the sink, it may call `shutdown()` or
        // `cancel()` but must not destroy this ReviewJobs or its host.
        std::function<std::uint64_t(ReviewJobKind kind, const std::string &message, CancelFlag &cancel)>
            beginOperation;
    };

    // When a finished job's lane is free again, relative to its terminal event.
    enum class ReviewLaneRelease
    {
        // Before the terminal event: busy() is false when the sink receives it
        // (ReviewJobs before #512 S7; YOFO Review starts its next job on it).
        BeforeTerminal,
        // After the sink returned from the terminal event: a start from the
        // terminal event is refused (the facade's single-flight flags were
        // cleared after finishOperation before #512 S7).
        AfterTerminal,
    };

    // A host's job policy (D7 as amended 2026-10-19, #512 S7).
    struct ReviewJobsOptions
    {
        // true: the export lane and the processing lane run at the same time,
        // one job each. false: one job of any kind at a time.
        bool concurrentLanes{false};
        ReviewLaneRelease laneRelease{ReviewLaneRelease::BeforeTerminal};
        ReviewJobHost host;

        // YOFO Review's own session (the review bridge, crates/mib-bridge
        // review_shim.cpp): one job of any kind at a time, lane free before
        // the terminal event, ReviewJobs' own ids. It does not gain the second
        // lane until #722: HDF5 is built without thread safety and
        // Hdf5Service has no process-wide lock, so an export beside a
        // regenerate would call into the library from two threads at once.
        static ReviewJobsOptions forReviewBridge();
        // MIB Studio's facade (BackendFacade): one export and one reanalysis
        // at the same time, exactly as before #512 (listed as at risk in
        // #722), lane free after the terminal event, the facade's operations.
        static ReviewJobsOptions forFacade(ReviewJobHost host);
    };

    // A start of a JSON job (`startExportJson` / `startReanalysisJson`).
    struct ReviewJobStart
    {
        std::uint64_t operationId{0}; // 0 = refused before an operation existed
        // false with an id: the worker could not start (its terminal was
        // reported). true: the terminal comes through the sink, or was already
        // reported (Cancelled, without running) when a shutdown() overtook the
        // start.
        bool started{false};
        std::string error;
    };

    struct ReviewJobsTestAccess; // custom job bodies for the lane tests (defined in ReviewJobs.cpp)

    // Runs YOFO Review's and MIB Studio's review jobs on two lanes (export,
    // processing) under a host's policy (ReviewJobsOptions). Every operation
    // gets Started, then Progress, then exactly one terminal event.
    //
    // Callbacks (the sink, the host's beginOperation) may call `shutdown()` or
    // `cancel()`, but must not destroy this ReviewJobs or its host. Hosts
    // deliver events to their consumers asynchronously. Destroying the owner
    // from inside its own callback is out of contract (#742 review round 2).
    class ReviewJobs
    {
    public:
        // `session` outlives the runner; `processing` may be null (regenerate
        // masks then fails with a message). The default options are the review
        // bridge's (one job at a time).
        ReviewJobs(ReviewSession &session, services::ProcessingService *processing, ReviewJobSink sink,
                   ReviewJobsOptions options = ReviewJobsOptions::forReviewBridge());
        ~ReviewJobs();
        ReviewJobs(const ReviewJobs &) = delete;
        ReviewJobs &operator=(const ReviewJobs &) = delete;

        // Each returns the operation id (0 = refused; `error` says why: the
        // request's fault, or "Another review job is still running" while a
        // job runs that the lane policy does not let this one run beside).
        std::uint64_t startExportMetrics(const std::string &outputPath, std::string *error = nullptr);
        std::uint64_t startExportAll(const ExportAllRequest &request, std::string *error = nullptr);
        std::uint64_t startBatchExport(const BatchExportRequest &request, std::string *error = nullptr);
        std::uint64_t startExportCharts(const ExportChartsRequest &request, std::string *error = nullptr);
        std::uint64_t startRegenerateMasks(const RegenerateMasksRequest &request, std::string *error = nullptr);
        std::uint64_t startComputeCore(double coreFraction, std::string *error = nullptr);
        std::uint64_t startDensity(const DensityRequest &request, std::string *error = nullptr);

        // The bridge contract's JSON jobs, as the facade's `review_export_json`
        // and `review_reanalysis_json` ran them before #512 S7. The host parses
        // the request (`parseExportRequest` with its defaults and fills
        // `request.sourcePath` and `sources`; `parseReanalysisRequest`).
        // Export: one HdfExportService run per source, the cancel flag polled
        // by the service (`ExportCancelPolling::Direct`); refused with "An
        // export is already active" while the export lane is busy. Reanalysis:
        // `runReanalysis` on `kernel` (the facade: the live processing
        // service; it outlives the job); refused with "Reanalysis already
        // active" while the processing lane is busy. Progress events carry no
        // message; the terminal message is the final path (export completed)
        // or the engine's error. The lane's status snapshot is updated before
        // each event and read with `exportStatusJson()` /
        // `reanalysisStatusJson()` (the S2 / S3 formatters; `{"state":"idle"}`
        // before the first job).
        ReviewJobStart startExportJson(const ExportBatchRequest &request);
        ReviewJobStart startReanalysisJson(const ReanalysisRequest &request, services::ProcessingService &kernel);
        std::string exportStatusJson() const;
        std::string reanalysisStatusJson() const;

        // Cancels the running job with this id (either lane).
        bool cancel(std::uint64_t operationId);
        // A job runs on any lane / on `lane`.
        bool busy() const;
        bool busy(ReviewJobLane lane) const;

        // #717 B1a: the experiment library's identity probe
        // (ReviewSession::probeRuns), gated on the lanes. It holds the lanes'
        // lock for the whole call, so no job claims a lane while it runs, and
        // it is refused (false, `error`) while either lane is claimed: a job's
        // readers work outside the session's mutex, and HDF5 is not
        // thread-safe (#722). Synchronous; per-file errors are in `out`.
        bool probeRuns(const std::vector<std::string> &paths, std::vector<RunProbe> &out,
                       std::string *error = nullptr);
        // #717 B3b: one record reprocessed with the processing kernel, next
        // to its stored values (ReprocessResult). Never writes. The inputs
        // (stored row, image, recorded config / ROI / background) are read by
        // ReviewSession::reprocessInputs under the lanes' lock and the
        // session's mutex, like the probe; processBatch then runs outside
        // every lock (it touches no HDF5 and is thread-safe). Refused (false,
        // `error`) while either lane is claimed (a regenerate job uses the
        // kernel, and every job reads HDF5 outside the session's mutex), when
        // there is no processing service, and for what reprocessInputs refuses.
        bool reprocessRecord(bool valid, std::uint64_t index, ReprocessSettings settings, ReprocessResult &out,
                             std::string *error = nullptr);
        // The record the last ComputeCore / Density job produced for the file
        // the session holds now (JSON, empty when none or computed for
        // another file); the shell saves it through ReviewSession.
        std::string computedCoreJson() const;
        DensityResult density() const;
        // Cancels the running jobs (both lanes) and joins them. A job still
        // being started when it is called does not run: its start reports it
        // Cancelled (the JSON jobs: their engine's outcome for a run cancelled
        // before its first step). Called from outside the runner (the owner's
        // teardown), it returns only once every such start has returned, so
        // none is inside ReviewJobs, the host or the sink afterwards. Called
        // from a lane worker or from inside a callback it does not wait for
        // them (waiting there could deadlock). Starts stay possible afterwards.
        // Called by the destructor.
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
        friend struct ReviewJobsTestAccess;
        struct Impl;
        // Shared with the lane workers: a worker detached by a shutdown from
        // inside its own event keeps the runner state it still touches alive
        // after this object is gone. Job bodies reach the runner only through
        // it, never through `this`.
        std::shared_ptr<Impl> impl_;
    };

    // Test-only: a job with a caller-supplied body on `kind`'s lane, through
    // the same lanes, host hook, events and shutdown as the real jobs, so the
    // lane tests drive start / cancel / shutdown with handshakes instead of
    // HDF5 work. `cancelOverridesOutcome` as ReviewJobs' own jobs (true) or
    // the JSON jobs (false).
    struct ReviewJobsTestAccess
    {
        struct Result
        {
            ReviewJobState state{ReviewJobState::Completed};
            std::string message;
        };
        using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
        using Body = std::function<Result(std::uint64_t id, const std::shared_ptr<std::atomic<bool>> &cancel,
                                          const Progress &progress)>;
        static std::uint64_t start(ReviewJobs &jobs, ReviewJobKind kind, Body body, std::string *error = nullptr,
                                   bool cancelOverridesOutcome = true, const std::string &busyMessage = {});
        // Blocks until no lane (or `lane`) is busy, woken by the lane release
        // itself; false after `timeout`.
        static bool waitIdle(const ReviewJobs &jobs, std::chrono::milliseconds timeout);
        static bool waitIdle(const ReviewJobs &jobs, ReviewJobLane lane, std::chrono::milliseconds timeout);
        // Blocks until `lane` is claimed by a start, woken by the claim
        // itself; false after `timeout`.
        static bool waitBusy(const ReviewJobs &jobs, ReviewJobLane lane, std::chrono::milliseconds timeout);
        // Blocks until a shutdown() from outside is waiting for a start in
        // progress; false after `timeout`.
        static bool waitShutdownWaiting(const ReviewJobs &jobs, std::chrono::milliseconds timeout);
    };

} // namespace backend::review
