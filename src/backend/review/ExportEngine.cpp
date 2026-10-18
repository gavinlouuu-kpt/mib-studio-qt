// Export engine (#512 slice 3): request parsing, the batch worker and the
// status JSON moved verbatim from BackendFacade::submitReviewExportJson; the
// per-file run of ReviewJobs (cancel at progress) moved from ReviewJobs.cpp.
// See ExportEngine.h.
#include "backend/review/ExportEngine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace backend::review
{

    namespace
    {
        nlohmann::json resultsJson(const std::vector<ExportFileOutcome> &files)
        {
            nlohmann::json results = nlohmann::json::array();
            for (const auto &file : files)
            {
                const auto &result = file.result;
                results.push_back(nlohmann::json{{"source_path", file.sourcePath},
                                                 {"state", recording::toString(result.status)},
                                                 {"final_path", result.finalPath},
                                                 {"retained_partial_path", result.retainedPartialPath},
                                                 {"error", result.error}});
            }
            return results;
        }
    } // namespace

    bool parseExportRequest(const std::string &json, const ExportDefaults &defaults, ExportBatchRequest &out,
                            std::string &error)
    {
        using namespace backend::recording;
        HdfExportRequest request;
        std::vector<std::string> sources;
        try
        {
            const auto input = nlohmann::json::parse(json);
            request.outputRoot = input.at("output_root").get<std::string>();
            if (input.contains("source_paths"))
            {
                sources = input.at("source_paths").get<std::vector<std::string>>();
                if (sources.empty() || sources.size() > 256 ||
                    std::any_of(sources.begin(), sources.end(), [](const auto &p) { return p.empty(); }))
                    throw std::invalid_argument("Batch requires 1 to 256 nonempty source paths");
            }
            request.explicitDestination = input.value("explicit_destination", std::string{});
            if (sources.size() > 1 && !request.explicitDestination.empty())
                throw std::invalid_argument("Batch exports use generated destinations, not an explicit destination");
            if (request.outputRoot.empty() && !request.explicitDestination.empty())
                request.outputRoot = ".";
            const auto format = input.value("format", std::string{"all"});
            if (format != "metrics_csv" && format != "images" && format != "all" && format != "charts")
                throw std::invalid_argument("Unknown export format");
            request.format = format == "metrics_csv" ? HdfExportFormat::MetricsCsv
                             : format == "images"    ? HdfExportFormat::Images
                             : format == "charts"    ? HdfExportFormat::Charts
                                                     : HdfExportFormat::All;
            request.generateReviewCharts = true;
            request.chartRingMin = defaults.chartRingMin;
            request.chartRingMax = defaults.chartRingMax;
            request.chartIsoelasticOverlays = input.value("isoelastic_overlays", true);
            const auto frames = input.value("frames", std::string{"both"});
            if (frames != "valid" && frames != "invalid" && frames != "both")
                throw std::invalid_argument("Unknown frame selection");
            request.frames = frames == "valid"   ? HdfExportFrames::Valid
                             : frames == "invalid" ? HdfExportFrames::Invalid
                                                   : HdfExportFrames::Both;
            if (input.contains("series"))
            {
                const auto &series = input.at("series");
                request.series.exportSeries = series.value("enabled", true);
                const auto index = [&](const char *name) {
                    if (!series.at(name).is_number_integer() || series.at(name) < 0)
                        throw std::invalid_argument("Series indices must be nonnegative integers");
                    return series.at(name).get<size_t>();
                };
                if (series.contains("start")) request.series.startInclusive = index("start");
                if (series.contains("end"))
                {
                    request.series.endInclusive = index("end");
                    if (request.series.endInclusive < request.series.startInclusive)
                        throw std::invalid_argument("Series end must not precede start");
                }
            }
            request.conversionFactor = input.value("conversion_factor", defaults.conversionFactor);
            if (!std::isfinite(request.conversionFactor) || request.conversionFactor <= 0)
                throw std::invalid_argument("Conversion factor must be finite and positive");
            request.keepPartialOnFailure = input.value("keep_partial_on_failure", false);
            if (request.outputRoot.empty() && request.explicitDestination.empty())
                throw std::invalid_argument("Export destination is empty");
        }
        catch (const std::exception &e)
        {
            error = e.what();
            return false;
        }
        out.request = std::move(request);
        out.sources = std::move(sources);
        return true;
    }

    recording::HdfExportResult runExportFile(const recording::HdfExportRequest &request,
                                             const ExportCancelFlag &cancel,
                                             const recording::HdfExportProgressFn &onProgress,
                                             const ExportRunOptions &options)
    {
        recording::HdfExportService service;
        if (options.imageWriterForTests) service.setImageWriterForTests(options.imageWriterForTests);
        if (options.polling == ExportCancelPolling::Direct)
        {
            const recording::HdfExportCancelToken token =
                cancel ? recording::HdfExportCancelToken(cancel) : recording::HdfExportCancelToken();
            return service.run(request, token, onProgress);
        }
        recording::HdfExportCancelToken token;
        return service.run(request, token, [&](const recording::HdfExportProgress &p) {
            if (cancel && cancel->load()) token.cancel();
            if (onProgress) onProgress(p);
        });
    }

    std::size_t ExportBatchOutcome::completedCount() const
    {
        return static_cast<std::size_t>(std::count_if(files.begin(), files.end(), [](const ExportFileOutcome &f) {
            return f.attempted && f.result.status == recording::HdfExportStatus::Completed;
        }));
    }

    ExportBatchOutcome runExportBatch(const recording::HdfExportRequest &request,
                                      const std::vector<std::string> &sources, const ExportCancelFlag &cancel,
                                      const ExportBatchProgressFn &onProgress, const ExportRunOptions &options)
    {
        using recording::HdfExportStatus;
        ExportBatchOutcome outcome;
        outcome.sourceCount = sources.size();
        for (std::size_t index = 0; index < sources.size(); ++index)
        {
            if (cancel && cancel->load(std::memory_order_acquire)) break;
            recording::HdfExportRequest fileRequest = request;
            fileRequest.sourcePath = sources[index];
            std::string skipped;
            if (options.prepare && !options.prepare(index, fileRequest, skipped))
            {
                ExportFileOutcome file;
                file.sourcePath = fileRequest.sourcePath;
                file.attempted = false;
                file.result.status = HdfExportStatus::Failed;
                file.result.error = skipped;
                outcome.files.push_back(std::move(file));
                outcome.failed = true;
                continue;
            }
            outcome.last = runExportFile(fileRequest, cancel,
                                         [&](const recording::HdfExportProgress &progress) {
                                             if (onProgress) onProgress(index, progress, outcome.files);
                                         },
                                         options);
            outcome.failed = outcome.failed || outcome.last.status == HdfExportStatus::Failed;
            outcome.files.push_back(ExportFileOutcome{fileRequest.sourcePath, true, outcome.last});
            if (outcome.last.status == HdfExportStatus::Cancelled) break;
        }
        // A cancellation accepted after a commit cannot undo that commit. If
        // every file completed, report completion; otherwise retain each outcome.
        outcome.cancelled = outcome.files.size() < sources.size() || outcome.last.status == HdfExportStatus::Cancelled;
        outcome.error = outcome.cancelled ? std::string("Batch cancelled; completed files remain published")
                        : outcome.failed  ? std::string("One or more exports failed; see per-file results")
                                          : std::string{};
        return outcome;
    }

    std::string exportValidatingStatusJson(std::uint64_t operationId)
    {
        return nlohmann::json{{"operation_id", std::to_string(operationId)},
                              {"state", "running"},
                              {"phase", "validating"},
                              {"completed", "0"},
                              {"total", "0"}}
            .dump();
    }

    std::string exportProgressStatusJson(std::uint64_t operationId, const std::vector<std::string> &sources,
                                         std::size_t index, const recording::HdfExportProgress &progress,
                                         const std::vector<ExportFileOutcome> &done)
    {
        return nlohmann::json{{"operation_id", std::to_string(operationId)},
                              {"job_id", progress.jobId},
                              {"state", "running"},
                              {"phase", recording::toString(progress.phase)},
                              {"completed", std::to_string(progress.completed)},
                              {"total", std::to_string(progress.total)},
                              {"source_path", sources[index]},
                              {"file_index", index + 1},
                              {"file_count", sources.size()},
                              {"results", resultsJson(done)},
                              {"current_output", progress.currentOutput}}
            .dump();
    }

    std::string exportTerminalStatusJson(std::uint64_t operationId, const ExportBatchOutcome &outcome)
    {
        const auto &result = outcome.last;
        return nlohmann::json{{"operation_id", std::to_string(operationId)},
                              {"job_id", result.jobId},
                              {"state", outcome.cancelled ? "cancelled" : outcome.failed ? "failed" : "completed"},
                              {"final_path", result.finalPath},
                              {"retained_partial_path", result.retainedPartialPath},
                              {"images_exported", std::to_string(result.imagesExported)},
                              {"images_failed", std::to_string(result.imagesFailed)},
                              {"metrics_written", result.metricsWritten},
                              {"file_count", outcome.sourceCount},
                              {"results", resultsJson(outcome.files)},
                              {"warnings", result.warnings},
                              {"error", outcome.sourceCount == 1 ? result.error : outcome.error}}
            .dump();
    }

    std::string exportStartFailedStatusJson(std::uint64_t operationId, const std::string &error)
    {
        return nlohmann::json{{"operation_id", std::to_string(operationId)}, {"state", "failed"}, {"error", error}}
            .dump();
    }

} // namespace backend::review
