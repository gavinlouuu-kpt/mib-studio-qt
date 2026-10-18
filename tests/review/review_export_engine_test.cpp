// review_export_engine_test (#512 slice 3, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md)
//
// The export engine (backend/review/ExportEngine.h) moved out of
// BackendFacade into mib_review_core. This drives it directly — no
// AppBackend — so any shell can rely on it:
//  - request parsing: defaults, every field, and the refusals with the
//    messages the facade returns (contract `review_export_json`);
//  - round trip: a single All export and a metrics batch, source unchanged;
//  - the batch rule: continue after a failed source, stop on cancel (during
//    a source and before the first), a skipped source (`prepare`) recorded
//    as failed and not attempted, per-source requests that do not leak;
//  - fault injection: an image write fault, with and without
//    keep_partial_on_failure (the retained `.partial-` folder and its
//    export-failure.json manifest), a cancel with the partial kept;
//  - the two cancel polling modes (the facade's direct token, ReviewJobs'
//    copy at each progress report) and the phases each reports, and that
//    ReviewJobs' exports keep theirs;
//  - the status snapshots (validating, progress, terminal for single,
//    batch, cancelled and failed runs, start failure), byte for byte.
// The facade surface over it is covered by backend.export_bridge_facade,
// ReviewJobs' by review.jobs. Watchdog guarded.

#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include "backend/review/ExportEngine.h"
#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using backend::recording::HdfExportFormat;
using backend::recording::HdfExportFrames;
using backend::recording::HdfExportPhase;
using backend::recording::HdfExportProgress;
using backend::recording::HdfExportRequest;
using backend::recording::HdfExportStatus;
using backend::review::ExportBatchOutcome;
using backend::review::ExportBatchRequest;
using backend::review::ExportCancelPolling;
using backend::review::ExportDefaults;
using backend::review::ExportFileOutcome;
using backend::review::ExportRunOptions;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using nlohmann::json;

namespace
{
    constexpr int kValid = 6, kInvalid = 2;

    void writeFixture(const fs::path &path)
    {
        Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(), "fixture open");
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < kValid + kInvalid; ++i)
        {
            ProcessedFrame f;
            f.index = static_cast<std::uint64_t>(i);
            f.timestampNs = 1000 + static_cast<std::uint64_t>(i);
            f.originalImage = cv::Mat(24, 32, CV_8UC1, cv::Scalar(10 + 20 * i));
            f.processedImage = cv::Mat::zeros(24, 32, CV_8UC1);
            cv::circle(f.processedImage, {16, 12}, 6, cv::Scalar(255), -1);
            f.validation.isValid = i < kValid;
            f.validation.objectCount = 1;
            f.validation.area = 300 + 25 * i;
            f.validation.deformability = 0.02 * i;
            f.validation.ringRatio = 1.0 + 0.25 * i;
            (i < kValid ? valid : invalid).push_back(f);
        }
        MIB_REQUIRE(writer.appendFrames(valid, invalid), "fixture frames");
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 1007, kValid, kInvalid, {}, {0, 0, 32, 24}), "fixture info");
        writer.closeFile();
    }

    std::uint64_t hash(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::uint64_t value = 14695981039346656037ULL;
        char byte;
        while (in.get(byte)) value = (value ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
        return value;
    }

    bool noPartials(const fs::path &dir)
    {
        if (!fs::exists(dir)) return true;
        for (const auto &e : fs::recursive_directory_iterator(dir))
            if (e.path().filename().string().find(".partial-") != std::string::npos) return false;
        return true;
    }

    std::shared_ptr<std::atomic<bool>> flag(bool value = false)
    {
        return std::make_shared<std::atomic<bool>>(value);
    }

    // `text` with each key replaced by its value as a JSON string literal
    // (quotes included, so Windows paths are escaped as the formatter does).
    std::string fill(std::string text, const std::map<std::string, std::string> &values)
    {
        for (const auto &[key, value] : values)
        {
            const std::string literal = json(value).dump();
            for (std::size_t at; (at = text.find(key)) != std::string::npos;) text.replace(at, key.size(), literal);
        }
        return text;
    }

    struct Recorded
    {
        std::size_t index;
        HdfExportProgress progress;
        std::vector<ExportFileOutcome> done;
    };
} // namespace

int main()
{
    mib::test::Watchdog wd(120);
    mib::test::TempDir td("review_export_engine");
    const fs::path source = td.path() / "run.h5";
    const fs::path other = td.path() / "other.h5";
    writeFixture(source);
    writeFixture(other);
    const auto sourceHash = hash(source);
    const fs::path missing = td.path() / "missing.h5";
    const ExportDefaults defaults{0.375, 1.5, 4.5};
    std::string error;

    // ---- parsing ---------------------------------------------------------------
    {
        ExportBatchRequest parsed;
        MIB_REQUIRE(backend::review::parseExportRequest(json{{"output_root", "out"}}.dump(), defaults, parsed, error),
                    "minimal request parses: " + error);
        const auto &r = parsed.request;
        MIB_EXPECT(parsed.sources.empty() && r.sourcePath.empty(), "sources left to the host");
        MIB_EXPECT(r.outputRoot == "out" && r.explicitDestination.empty(), "root");
        MIB_EXPECT(r.format == HdfExportFormat::All && r.frames == HdfExportFrames::Both, "format all, frames both");
        MIB_EXPECT(r.conversionFactor == 0.375, "host factor is the default");
        MIB_EXPECT(r.generateReviewCharts && r.chartRingMin == 1.5 && r.chartRingMax == 4.5, "host chart range");
        MIB_EXPECT(r.chartIsoelasticOverlays && !r.keepPartialOnFailure, "isoelastic on, partial discarded");
        MIB_EXPECT(r.series.exportSeries && r.series.startInclusive == 0 &&
                       r.series.endInclusive == static_cast<std::size_t>(-1),
                   "every series image");
    }
    {
        ExportBatchRequest parsed;
        const json every{{"output_root", ""},
                         {"explicit_destination", "pinned.csv"},
                         {"source_paths", {source.string()}},
                         {"format", "metrics_csv"},
                         {"frames", "invalid"},
                         {"series", {{"enabled", false}, {"start", 2}, {"end", 5}}},
                         {"conversion_factor", 0.25},
                         {"keep_partial_on_failure", true},
                         {"isoelastic_overlays", false}};
        MIB_REQUIRE(backend::review::parseExportRequest(every.dump(), defaults, parsed, error), "every field: " + error);
        const auto &r = parsed.request;
        MIB_EXPECT(parsed.sources == std::vector<std::string>{source.string()}, "source_paths");
        MIB_EXPECT(r.outputRoot == "." && r.explicitDestination == "pinned.csv", "explicit destination, root '.'");
        MIB_EXPECT(r.format == HdfExportFormat::MetricsCsv && r.frames == HdfExportFrames::Invalid, "format, frames");
        MIB_EXPECT(!r.series.exportSeries && r.series.startInclusive == 2 && r.series.endInclusive == 5, "series");
        MIB_EXPECT(r.conversionFactor == 0.25 && r.keepPartialOnFailure && !r.chartIsoelasticOverlays, "flags");
        const std::map<std::string, HdfExportFormat> formats{
            {"images", HdfExportFormat::Images}, {"charts", HdfExportFormat::Charts}, {"all", HdfExportFormat::All}};
        for (const auto &[name, format] : formats)
        {
            MIB_EXPECT(backend::review::parseExportRequest(json{{"output_root", "o"}, {"format", name}}.dump(), defaults,
                                                           parsed, error) &&
                           parsed.request.format == format,
                       "format " + name);
        }
        MIB_EXPECT(backend::review::parseExportRequest(json{{"output_root", "o"}, {"frames", "valid"}}.dump(), defaults,
                                                       parsed, error) &&
                       parsed.request.frames == HdfExportFrames::Valid,
                   "frames valid");
    }
    {
        const std::vector<std::string> many(257, source.string());
        const std::vector<std::pair<json, std::string>> refusals{
            {json{{"output_root", "o"}, {"source_paths", json::array()}}, "Batch requires 1 to 256 nonempty source paths"},
            {json{{"output_root", "o"}, {"source_paths", {source.string(), ""}}}, "Batch requires 1 to 256 nonempty source paths"},
            {json{{"output_root", "o"}, {"source_paths", many}}, "Batch requires 1 to 256 nonempty source paths"},
            {json{{"output_root", "o"}, {"source_paths", {source.string(), other.string()}}, {"explicit_destination", "x"}},
             "Batch exports use generated destinations, not an explicit destination"},
            {json{{"output_root", "o"}, {"format", "fcs"}}, "Unknown export format"},
            {json{{"output_root", "o"}, {"frames", "some"}}, "Unknown frame selection"},
            {json{{"output_root", "o"}, {"series", {{"start", -1}}}}, "Series indices must be nonnegative integers"},
            {json{{"output_root", "o"}, {"series", {{"end", 1.5}}}}, "Series indices must be nonnegative integers"},
            {json{{"output_root", "o"}, {"series", {{"start", 5}, {"end", 2}}}}, "Series end must not precede start"},
            {json{{"output_root", "o"}, {"conversion_factor", 0}}, "Conversion factor must be finite and positive"},
            {json{{"output_root", "o"}, {"conversion_factor", -1.0}}, "Conversion factor must be finite and positive"},
            {json{{"output_root", ""}}, "Export destination is empty"},
        };
        for (const auto &[request, message] : refusals)
        {
            ExportBatchRequest parsed;
            error.clear();
            MIB_EXPECT(!backend::review::parseExportRequest(request.dump(), defaults, parsed, error) && error == message,
                       "refused '" + message + "', got '" + error + "' for " + request.dump());
        }
        ExportBatchRequest parsed;
        MIB_EXPECT(!backend::review::parseExportRequest("{bad", defaults, parsed, error) &&
                       error.rfind("[json.exception.parse_error", 0) == 0,
                   "malformed JSON: " + error);
        MIB_EXPECT(!backend::review::parseExportRequest(json{{"format", "all"}}.dump(), defaults, parsed, error) &&
                       error.rfind("[json.exception.out_of_range", 0) == 0,
                   "output_root required: " + error);
    }
    wd.mark("parsing");

    // ---- status snapshots that carry no run ---------------------------------------
    MIB_EXPECT(backend::review::exportValidatingStatusJson(7) ==
                   R"({"completed":"0","operation_id":"7","phase":"validating","state":"running","total":"0"})",
               "validating snapshot: " + backend::review::exportValidatingStatusJson(7));
    MIB_EXPECT(backend::review::exportStartFailedStatusJson(7, "boom") ==
                   R"({"error":"boom","operation_id":"7","state":"failed"})",
               "start-failed snapshot");

    HdfExportRequest base;
    base.outputRoot = (td.path() / "single").string();
    base.conversionFactor = 0.5;
    base.generateReviewCharts = true;

    // ---- single All export: round trip + the progress and terminal snapshots ------
    {
        std::vector<Recorded> seen;
        const auto outcome = backend::review::runExportBatch(
            base, {source.string()}, flag(),
            [&](std::size_t index, const HdfExportProgress &p, const std::vector<ExportFileOutcome> &done) {
                seen.push_back({index, p, done});
            });
        MIB_REQUIRE(outcome.files.size() == 1 && outcome.last.status == HdfExportStatus::Completed,
                    "single export completed: " + outcome.last.error);
        MIB_EXPECT(!outcome.cancelled && !outcome.failed && outcome.error.empty() && outcome.completedCount() == 1,
                   "single outcome");
        const fs::path final = outcome.last.finalPath;
        MIB_EXPECT(fs::exists(final / "metrics.csv") && fs::exists(final / "scatter_plot.tiff") &&
                       fs::exists(final / "histogram.tiff"),
                   "metrics and backend-rendered charts published");
        MIB_EXPECT(outcome.last.imagesExported == kValid + kInvalid, "every image exported");
        MIB_EXPECT(noPartials(td.path() / "single"), "no partial left");
        MIB_EXPECT(hash(source) == sourceHash, "source unchanged");
        MIB_REQUIRE(!seen.empty() && seen.front().progress.phase == HdfExportPhase::Validating, "progress reported");
        const auto first = backend::review::exportProgressStatusJson(7, {source.string()}, 0, seen.front().progress,
                                                                     seen.front().done);
        MIB_EXPECT(first == fill(R"({"completed":"0","current_output":"","file_count":1,"file_index":1,"job_id":@J,)"
                                 R"("operation_id":"7","phase":"validating","results":[],"source_path":@S,)"
                                 R"("state":"running","total":"0"})",
                                 {{"@J", outcome.last.jobId}, {"@S", source.string()}}),
                   "first progress snapshot: " + first);
        const auto terminal = backend::review::exportTerminalStatusJson(7, outcome);
        MIB_EXPECT(terminal ==
                       fill(R"({"error":"","file_count":1,"final_path":@F,"images_exported":"8","images_failed":"0",)"
                            R"("job_id":@J,"metrics_written":true,"operation_id":"7","results":[{"error":"",)"
                            R"("final_path":@F,"retained_partial_path":"","source_path":@S,"state":"completed"}],)"
                            R"("retained_partial_path":"","state":"completed","warnings":[]})",
                            {{"@F", final.string()}, {"@J", outcome.last.jobId}, {"@S", source.string()}}),
                   "single terminal snapshot: " + terminal);
    }
    wd.mark("single");

    // ---- metrics batch: a missing middle source fails, the rest still run ---------
    {
        HdfExportRequest metrics = base;
        metrics.outputRoot = (td.path() / "batch").string();
        metrics.format = HdfExportFormat::MetricsCsv;
        const std::vector<std::string> sources{source.string(), missing.string(), source.string()};
        std::vector<Recorded> seen;
        const auto outcome = backend::review::runExportBatch(
            metrics, sources, flag(),
            [&](std::size_t index, const HdfExportProgress &p, const std::vector<ExportFileOutcome> &done) {
                seen.push_back({index, p, done});
            });
        MIB_REQUIRE(outcome.files.size() == 3, "every source attempted");
        MIB_EXPECT(outcome.files[0].result.status == HdfExportStatus::Completed &&
                       outcome.files[1].result.status == HdfExportStatus::Failed &&
                       outcome.files[2].result.status == HdfExportStatus::Completed,
                   "continue on failure");
        MIB_EXPECT(outcome.failed && !outcome.cancelled && outcome.completedCount() == 2, "batch failed, not cancelled");
        MIB_EXPECT(outcome.error == "One or more exports failed; see per-file results", "batch message");
        MIB_EXPECT(outcome.files[0].result.finalPath != outcome.files[2].result.finalPath &&
                       fs::path(outcome.files[2].result.finalPath).filename() == "run_metrics_2.csv",
                   "duplicate names get the _N suffix");
        // The first progress of the last source carries the two before it.
        const auto it = std::find_if(seen.begin(), seen.end(), [](const Recorded &r) { return r.index == 2; });
        MIB_REQUIRE(it != seen.end() && it->done.size() == 2, "progress of source 3 carries sources 1-2");
        const auto progress = backend::review::exportProgressStatusJson(9, sources, it->index, it->progress, it->done);
        const auto &r0 = outcome.files[0].result;
        const auto &r1 = outcome.files[1].result;
        MIB_EXPECT(progress ==
                       fill(R"({"completed":"0","current_output":"","file_count":3,"file_index":3,"job_id":@J,)"
                            R"("operation_id":"9","phase":"validating","results":[{"error":"","final_path":@F0,)"
                            R"("retained_partial_path":"","source_path":@S,"state":"completed"},{"error":@E1,)"
                            R"("final_path":"","retained_partial_path":"","source_path":@M,"state":"failed"}],)"
                            R"("source_path":@S,"state":"running","total":"0"})",
                            {{"@J", outcome.last.jobId}, {"@F0", r0.finalPath}, {"@E1", r1.error},
                             {"@S", source.string()}, {"@M", missing.string()}}),
                   "batch progress snapshot: " + progress);
        const auto terminal = backend::review::exportTerminalStatusJson(9, outcome);
        MIB_EXPECT(terminal ==
                       fill(R"({"error":"One or more exports failed; see per-file results","file_count":3,)"
                            R"("final_path":@F2,"images_exported":"0","images_failed":"0","job_id":@J,)"
                            R"("metrics_written":true,"operation_id":"9","results":[{"error":"","final_path":@F0,)"
                            R"("retained_partial_path":"","source_path":@S,"state":"completed"},{"error":@E1,)"
                            R"("final_path":"","retained_partial_path":"","source_path":@M,"state":"failed"},)"
                            R"({"error":"","final_path":@F2,"retained_partial_path":"","source_path":@S,)"
                            R"("state":"completed"}],"retained_partial_path":"","state":"failed","warnings":[]})",
                            {{"@J", outcome.last.jobId}, {"@F0", r0.finalPath}, {"@F2", outcome.last.finalPath},
                             {"@E1", r1.error}, {"@S", source.string()}, {"@M", missing.string()}}),
                   "batch terminal snapshot: " + terminal);
        MIB_EXPECT(hash(source) == sourceHash, "batch source unchanged");
    }
    wd.mark("batch");

    // ---- cancel during the second source (direct token) ----------------------------
    {
        HdfExportRequest all = base;
        all.outputRoot = (td.path() / "cancel").string();
        const std::vector<std::string> sources{other.string(), source.string(), other.string()};
        auto cancel = flag();
        const auto outcome = backend::review::runExportBatch(
            all, sources, cancel,
            [&](std::size_t index, const HdfExportProgress &p, const std::vector<ExportFileOutcome> &) {
                if (index == 1 && p.phase == HdfExportPhase::Metadata) cancel->store(true);
            });
        MIB_REQUIRE(outcome.files.size() == 2, "the third source is not reached");
        MIB_EXPECT(outcome.files[0].result.status == HdfExportStatus::Completed &&
                       outcome.files[1].result.status == HdfExportStatus::Cancelled,
                   "first published, second cancelled");
        MIB_EXPECT(outcome.cancelled && outcome.error == "Batch cancelled; completed files remain published",
                   "batch cancelled");
        MIB_EXPECT(fs::exists(outcome.files[0].result.finalPath), "completed file stays published");
        MIB_EXPECT(noPartials(td.path() / "cancel"), "cancelled partial removed");
        const auto terminal = backend::review::exportTerminalStatusJson(11, outcome);
        MIB_EXPECT(terminal ==
                       fill(R"({"error":"Batch cancelled; completed files remain published","file_count":3,)"
                            R"("final_path":"","images_exported":"0","images_failed":"0","job_id":@J,)"
                            R"("metrics_written":false,"operation_id":"11","results":[{"error":"","final_path":@F0,)"
                            R"("retained_partial_path":"","source_path":@O,"state":"completed"},)"
                            R"({"error":"export cancelled","final_path":"","retained_partial_path":"",)"
                            R"("source_path":@S,"state":"cancelled"}],"retained_partial_path":"",)"
                            R"("state":"cancelled","warnings":[]})",
                            {{"@J", outcome.last.jobId}, {"@F0", outcome.files[0].result.finalPath},
                             {"@O", other.string()}, {"@S", source.string()}}),
                   "cancelled terminal snapshot: " + terminal);
    }
    // ---- a cancel seen before the first source: nothing runs -----------------------
    {
        bool called = false;
        const auto outcome = backend::review::runExportBatch(
            base, {source.string()}, flag(true),
            [&](std::size_t, const HdfExportProgress &, const std::vector<ExportFileOutcome> &) { called = true; });
        MIB_EXPECT(outcome.files.empty() && outcome.cancelled && !called, "cancelled before start");
        // Single source: the terminal error is the (absent) file's, as the facade has always reported it.
        MIB_EXPECT(backend::review::exportTerminalStatusJson(12, outcome) ==
                       R"({"error":"","file_count":1,"final_path":"","images_exported":"0","images_failed":"0",)"
                       R"("job_id":"","metrics_written":false,"operation_id":"12","results":[],)"
                       R"("retained_partial_path":"","state":"cancelled","warnings":[]})",
                   "cancel-before-start snapshot: " + backend::review::exportTerminalStatusJson(12, outcome));
    }
    wd.mark("cancel");

    // ---- failed single export: the terminal error is the file's --------------------
    {
        HdfExportRequest bad = base;
        bad.outputRoot = source.string(); // a file, not a directory
        const auto outcome = backend::review::runExportBatch(bad, {source.string()}, flag(), {});
        MIB_REQUIRE(outcome.files.size() == 1 && outcome.failed && !outcome.cancelled, "fault fails");
        const auto terminal = backend::review::exportTerminalStatusJson(13, outcome);
        MIB_EXPECT(terminal ==
                       fill(R"({"error":@E,"file_count":1,"final_path":"","images_exported":"0","images_failed":"0",)"
                            R"("job_id":@J,"metrics_written":false,"operation_id":"13","results":[{"error":@E,)"
                            R"("final_path":"","retained_partial_path":"","source_path":@S,"state":"failed"}],)"
                            R"("retained_partial_path":"","state":"failed","warnings":[]})",
                            {{"@E", "output path exists and is not a directory: " + source.string()},
                             {"@J", outcome.last.jobId}, {"@S", source.string()}}),
                   "failed terminal snapshot: " + terminal);
        MIB_EXPECT(hash(source) == sourceHash, "fault leaves the source unchanged");
    }
    // ---- image write fault, with and without keep_partial_on_failure ----------------
    for (const bool keep : {false, true})
    {
        HdfExportRequest images = base;
        images.outputRoot = (td.path() / (keep ? "fault-kept" : "fault")).string();
        images.format = HdfExportFormat::Images;
        images.keepPartialOnFailure = keep;
        ExportRunOptions options;
        int writes = 0;
        options.imageWriterForTests = [&](const std::string &, const cv::Mat &) { return ++writes < 3; };
        const auto outcome = backend::review::runExportBatch(images, {source.string()}, flag(), {}, options);
        MIB_REQUIRE(outcome.files.size() == 1 && outcome.last.status == HdfExportStatus::Failed,
                    "write fault fails the export");
        MIB_EXPECT(outcome.last.finalPath.empty(), "a fault never publishes");
        if (keep)
        {
            const fs::path partial = outcome.last.retainedPartialPath;
            MIB_EXPECT(partial.filename().string().find(".partial-") != std::string::npos, "kept partial is named so");
            MIB_EXPECT(fs::exists(partial / "export-failure.json"), "kept partial has its failure manifest");
        }
        else
        {
            MIB_EXPECT(outcome.last.retainedPartialPath.empty() && noPartials(images.outputRoot), "partial removed");
        }
    }
    // ---- cancel with the partial kept ------------------------------------------------
    {
        HdfExportRequest kept = base;
        kept.outputRoot = (td.path() / "cancel-kept").string();
        kept.keepPartialOnFailure = true;
        auto cancel = flag();
        const auto outcome = backend::review::runExportBatch(
            kept, {source.string()}, cancel,
            [&](std::size_t, const HdfExportProgress &p, const std::vector<ExportFileOutcome> &) {
                if (p.phase == HdfExportPhase::Metadata) cancel->store(true);
            });
        MIB_REQUIRE(outcome.cancelled && outcome.last.status == HdfExportStatus::Cancelled, "kept cancel");
        const fs::path partial = outcome.last.retainedPartialPath;
        MIB_EXPECT(fs::exists(partial / "export-failure.json"), "cancelled partial kept with its manifest");
        const auto terminal = json::parse(backend::review::exportTerminalStatusJson(14, outcome));
        MIB_EXPECT(terminal["retained_partial_path"] == partial.string() &&
                       terminal["results"][0]["retained_partial_path"] == partial.string(),
                   "snapshot names the kept partial");
    }
    wd.mark("faults");

    // ---- cancel polling: the facade's direct token vs ReviewJobs' copy at progress --
    for (const auto polling : {ExportCancelPolling::Direct, ExportCancelPolling::AtProgress})
    {
        HdfExportRequest metrics = base;
        metrics.sourcePath = source.string();
        metrics.outputRoot = (td.path() / "polling").string();
        metrics.format = HdfExportFormat::MetricsCsv;
        auto cancel = flag();
        std::vector<HdfExportPhase> phases;
        ExportRunOptions options;
        options.polling = polling;
        const auto result = backend::review::runExportFile(
            metrics, cancel,
            [&](const HdfExportProgress &p) {
                phases.push_back(p.phase);
                cancel->store(true); // cancel inside the first report
            },
            options);
        MIB_EXPECT(result.status == HdfExportStatus::Cancelled, "cancelled in either mode");
        const std::vector<HdfExportPhase> expected =
            polling == ExportCancelPolling::Direct
                ? std::vector<HdfExportPhase>{HdfExportPhase::Validating}
                : std::vector<HdfExportPhase>{HdfExportPhase::Validating, HdfExportPhase::Metadata,
                                              HdfExportPhase::Cleanup};
        std::string seen;
        for (const auto phase : phases) seen += std::string(seen.empty() ? "" : ",") + backend::recording::toString(phase);
        MIB_EXPECT(phases == expected, std::string(polling == ExportCancelPolling::Direct
                                                       ? "direct: stops at the check after validation"
                                                       : "at progress: stops after the next report") +
                                           " (" + seen + ")");
        MIB_EXPECT(noPartials(metrics.outputRoot), "no partial after a polling cancel");
    }
    wd.mark("polling");

    // ---- prepare: per-source requests, a skipped source ------------------------------
    {
        HdfExportRequest all = base;
        all.outputRoot = (td.path() / "prepared").string();
        ExportRunOptions options;
        std::vector<std::string> prepared;
        options.prepare = [&](std::size_t index, HdfExportRequest &request, std::string &skip) {
            prepared.push_back(request.sourcePath);
            MIB_EXPECT(request.format == HdfExportFormat::All, "each source starts from the batch request");
            if (index == 1)
            {
                skip = "skipped by the host";
                return false;
            }
            if (index == 2) request.format = HdfExportFormat::MetricsCsv;
            return true;
        };
        const std::vector<std::string> sources{source.string(), other.string(), source.string()};
        const auto outcome = backend::review::runExportBatch(all, sources, flag(), {}, options);
        MIB_EXPECT(prepared == sources, "prepare sees every source in order");
        MIB_REQUIRE(outcome.files.size() == 3, "every source reached");
        MIB_EXPECT(!outcome.files[1].attempted && outcome.files[1].result.status == HdfExportStatus::Failed &&
                       outcome.files[1].result.error == "skipped by the host",
                   "skipped source recorded as failed");
        MIB_EXPECT(fs::is_directory(outcome.files[0].result.finalPath) &&
                       fs::path(outcome.files[2].result.finalPath).extension() == ".csv",
                   "per-source format");
        MIB_EXPECT(outcome.failed && !outcome.cancelled && outcome.completedCount() == 2, "skip fails the batch");
        MIB_EXPECT(outcome.last.finalPath == outcome.files[2].result.finalPath, "last = last attempted");
    }
    wd.mark("prepare");

    // ---- ReviewJobs' exports run through the engine and keep cancel-at-progress -----
    {
        using backend::review::ReviewJobEvent;
        using backend::review::ReviewJobState;
        backend::review::ReviewSession session;
        MIB_REQUIRE(session.open(source.string()), "session opens the fixture");
        std::mutex m;
        std::condition_variable cv;
        std::vector<ReviewJobEvent> events;
        std::unique_ptr<backend::review::ReviewJobs> jobs;
        jobs = std::make_unique<backend::review::ReviewJobs>(session, nullptr, [&](const ReviewJobEvent &e) {
            bool cancelNow = false;
            {
                std::scoped_lock lock(m);
                events.push_back(e);
                cancelNow = e.state == ReviewJobState::Progress && events.size() == 2; // the first report
            }
            if (cancelNow) jobs->cancel(e.operationId);
            cv.notify_all();
        });
        std::string startError;
        const auto id = jobs->startExportMetrics((td.path() / "jobs.csv").string(), &startError);
        MIB_REQUIRE(id != 0, "ReviewJobs export starts: " + startError);
        std::unique_lock lock(m);
        MIB_REQUIRE(cv.wait_for(lock, std::chrono::seconds(60), [&] {
            return !events.empty() && events.back().state != ReviewJobState::Started &&
                   events.back().state != ReviewJobState::Progress;
        }), "ReviewJobs export terminal");
        std::vector<std::string> messages;
        for (const auto &e : events) messages.push_back(e.message.substr(0, e.message.find(':')));
        MIB_EXPECT((messages == std::vector<std::string>{"", "validating", "metadata", "cleanup", "partial output was discarded"}) &&
                       events.back().state == ReviewJobState::Cancelled,
                   "a cancel takes effect after the next report, then the partial is discarded");
        lock.unlock();
        jobs->shutdown();
        bool residue = fs::exists(td.path() / "jobs.csv");
        for (const auto &e : fs::directory_iterator(td.path()))
            residue = residue || e.path().filename().string().rfind(".jobs.csv.partial-", 0) == 0;
        MIB_EXPECT(!residue, "nothing published, no partial left");
    }
    wd.mark("review jobs");

    MIB_EXPECT(hash(source) == sourceHash, "source unchanged at the end");
    return mib::test::exitCode();
}
