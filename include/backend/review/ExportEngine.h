// ExportEngine — HDF5 review exports over HdfExportService: the engine behind
// the backend bridge's `review_export_json` / `review_export_status_json`
// (MIB Studio, YOFO Studio) and ReviewJobs' Export Metrics, Export All and
// Batch export (YOFO Review), moved out of BackendFacade into mib_review_core
// so every review shell shares it (#512 slice 3, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md).
//
// Qt-free, built on mib_processing only. `runExportBatch` runs on the
// caller's thread: one HdfExportService run per source, in order, each with
// its own read-only reader. A failed source does not stop the batch; a
// cancelled one does, and a cancel seen before a source starts stops the
// batch before it. The host owns single flight, the operation id, progress
// events and the status snapshot (the facade and ReviewJobs keep all of
// that). Request parsing and the status JSON follow bridge-contract.json
// `review_export_json` exactly; the facade's messages are these.
#pragma once

#include "backend/recording/HdfExportService.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backend::review
{

    // The host's values for what a `review_export_json` request leaves out.
    struct ExportDefaults
    {
        double conversionFactor{0.4886}; // when the request has no `conversion_factor` (facade: the live factor)
        double chartRingMin{0.0};        // chart histogram range (facade: the live processing config)
        double chartRingMax{10.0};
    };

    struct ExportBatchRequest
    {
        // Every field but `sourcePath`, shared by every source.
        recording::HdfExportRequest request;
        // `source_paths` (1-256); empty = the host's loaded file.
        std::vector<std::string> sources;
    };

    // Parses and validates a `review_export_json` request. The request's
    // `sourcePath` stays empty: the host fills it (the facade: the loaded
    // file unless `source_paths` is given). On failure returns false and
    // `error` is the message the bridge returns.
    bool parseExportRequest(const std::string &json, const ExportDefaults &defaults, ExportBatchRequest &out,
                            std::string &error);

    using ExportCancelFlag = std::shared_ptr<std::atomic<bool>>;

    // How the cancel flag reaches HdfExportService.
    enum class ExportCancelPolling
    {
        // The service polls the flag itself before every artifact and frame
        // (the facade's `requestOperationCancel`).
        Direct,
        // The flag is copied into a per-file token at each progress report,
        // so a cancel takes effect at the service's next check after the next
        // report (ReviewJobs' behaviour before #512).
        AtProgress,
    };

    struct ExportRunOptions
    {
        ExportCancelPolling polling{ExportCancelPolling::Direct};
        // Batch only: called for source `index` after the batch's cancel
        // check and before the source runs, with the batch request whose
        // `sourcePath` is that source; may adjust it (factor, format,
        // destination). Returning false skips the source: it is recorded as
        // failed with `error`, not attempted, and the batch continues.
        std::function<bool(std::size_t index, recording::HdfExportRequest &request, std::string &error)> prepare;
        // Test seam: HdfExportService::setImageWriterForTests.
        recording::HdfExportImageWriter imageWriterForTests;
    };

    // One HdfExportService run. Never throws for job failures.
    recording::HdfExportResult runExportFile(const recording::HdfExportRequest &request,
                                             const ExportCancelFlag &cancel,
                                             const recording::HdfExportProgressFn &onProgress,
                                             const ExportRunOptions &options = {});

    struct ExportFileOutcome
    {
        std::string sourcePath;
        bool attempted{true}; // false: skipped by `ExportRunOptions::prepare`
        recording::HdfExportResult result;
    };

    struct ExportBatchOutcome
    {
        std::size_t sourceCount{0};
        // Every source reached, in order (attempted or skipped); the list ends
        // at a cancellation.
        std::vector<ExportFileOutcome> files;
        // The last attempted source's result (default-constructed when none ran).
        recording::HdfExportResult last;
        // Not every source was reached, or the last attempted one was cancelled.
        // A cancel accepted after the last commit cannot undo it: a batch whose
        // every source completed is not cancelled.
        bool cancelled{false};
        bool failed{false}; // a source failed or was skipped
        // The batch message of `review_export_json` ("" when completed).
        std::string error;
        std::size_t completedCount() const;
    };

    // Progress of source `index` (0-based); `done` holds the sources before it.
    using ExportBatchProgressFn = std::function<void(std::size_t index, const recording::HdfExportProgress &progress,
                                                     const std::vector<ExportFileOutcome> &done)>;

    // One export per source, continue on failure, stop on cancel.
    // Never throws for job failures.
    ExportBatchOutcome runExportBatch(const recording::HdfExportRequest &request,
                                      const std::vector<std::string> &sources, const ExportCancelFlag &cancel,
                                      const ExportBatchProgressFn &onProgress, const ExportRunOptions &options = {});
    // What `runExportBatch` returns when the flag is set before its first
    // source (no file touched): the review job that a shutdown overtook
    // before it ran (ReviewJobs, #742 review round 2).
    ExportBatchOutcome exportBatchCancelledBeforeStart(std::size_t sourceCount);

    // Status snapshots of `review_export_json` (operation ids and counters as
    // decimal strings, `file_index` / `file_count` as numbers).
    std::string exportValidatingStatusJson(std::uint64_t operationId);
    std::string exportProgressStatusJson(std::uint64_t operationId, const std::vector<std::string> &sources,
                                         std::size_t index, const recording::HdfExportProgress &progress,
                                         const std::vector<ExportFileOutcome> &done);
    std::string exportTerminalStatusJson(std::uint64_t operationId, const ExportBatchOutcome &outcome);
    std::string exportStartFailedStatusJson(std::uint64_t operationId, const std::string &error);

} // namespace backend::review
