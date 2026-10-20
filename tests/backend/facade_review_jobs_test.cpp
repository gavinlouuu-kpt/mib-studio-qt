// facade_review_jobs_test (#512 S7, plan
// docs/exec-plans/active/2026-10-18-one-review-implementation.md)
//
// MIB Studio's facade runs `review_export_json` and `review_reanalysis_json`
// on ReviewJobs' lanes (ReviewJobsOptions::forFacade) instead of its own
// threads. This keeps the pre-S7 facade code as the oracle (`PreS7Jobs`
// below: the two submit bodies, the status snapshots, the single-flight flags
// and threads, and the operation registry with its shutdown cancellation,
// copied from BackendFacade.cpp / ReviewReanalysis.cpp at develop 88e9afbe)
// and drives both through the same scenarios with handshakes:
//  - an export and a reanalysis that complete, fail (missing source, batch
//    with a missing file) or are cancelled at their first progress event,
//    a metrics CSV to an explicit destination, charts;
//  - duplicates refused from inside the progress event ("An export is
//    already active", "Reanalysis already active"), and from inside the
//    terminal event (the flag was cleared after finishOperation);
//  - an export run to completion while a reanalysis is held at its first
//    progress event (one of each at the same time, D7);
//  - cancel through dispatch(OperationCommand) during the job and after it.
// Every operation event (kind, state, progress, total, message), the job's
// status snapshot at each event, every start / refusal / cancel result and
// the status snapshots after each job must be identical, normalized only for
// the temp directory, the export job ids and the operation ids.
// Then the facade alone, as a concurrency stress (handshakes keep the HDF5
// work of the two lanes from overlapping, #722): 12 cycles with both lanes
// held, duplicates refused on both, one lane cancelled (through
// dispatch(OperationCommand) or requestOperationCancel) while the other
// waits, both released in turn; and shutdown with both held: the reanalysis
// is cancelled with "Backend shutdown" once, the export reports its own
// terminal, shutdown returns after both and nothing is reported after it.
// Every operation sees Started first and exactly one terminal. Between jobs
// the test waits for the lane's own release (BackendFacadeTestAccess +
// ReviewJobsTestAccess::waitIdle; the oracle's: joining its worker), never a
// sleep: the lane is freed only after the terminal event returns. Watchdog
// guarded.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ExportEngine.h"
#include "backend/review/Reanalysis.h"
#include "backend/review/ReviewJobs.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace backend::bridge;
namespace br = backend::review;
namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;

    // ---- the oracle: the facade's review jobs before #512 S7 ------------------
    struct PreS7Jobs
    {
        // BackendFacade's operation registry (beginOperation ... cancelAllOperations).
        struct ActiveOperation
        {
            BackendOperationKind kind{BackendOperationKind::RecordingLoad};
            CancelFlag cancelRequested;
        };
        std::function<void(const OperationStatusEvent &)> sink;
        mutable std::mutex operationsMutex_;
        std::unordered_map<std::uint64_t, ActiveOperation> activeOperations_;
        std::atomic<std::uint64_t> nextOperationId_{1};

        void emitEvent(const OperationStatusEvent &e) const
        {
            if (sink) sink(e);
        }
        std::uint64_t beginOperation(BackendOperationKind kind, CancelFlag *cancelFlagOut, const std::string &message)
        {
            const std::uint64_t operationId = nextOperationId_.fetch_add(1, std::memory_order_relaxed);
            auto flag = std::make_shared<std::atomic<bool>>(false);
            {
                std::scoped_lock lock(operationsMutex_);
                activeOperations_[operationId] = ActiveOperation{kind, flag};
            }
            if (cancelFlagOut) *cancelFlagOut = flag;
            emitEvent(OperationStatusEvent{operationId, kind, BackendOperationState::Started, 0, 0, message});
            return operationId;
        }
        void reportOperationProgress(std::uint64_t operationId, std::uint64_t progress, std::uint64_t total,
                                     const std::string &message = {})
        {
            BackendOperationKind kind;
            {
                std::scoped_lock lock(operationsMutex_);
                const auto it = activeOperations_.find(operationId);
                if (it == activeOperations_.end()) return;
                kind = it->second.kind;
            }
            emitEvent(OperationStatusEvent{operationId, kind, BackendOperationState::Progress, progress, total, message});
        }
        void finishOperation(std::uint64_t operationId, BackendOperationState terminalState, const std::string &message = {})
        {
            BackendOperationKind kind;
            {
                std::scoped_lock lock(operationsMutex_);
                const auto it = activeOperations_.find(operationId);
                if (it == activeOperations_.end()) return;
                kind = it->second.kind;
                activeOperations_.erase(it);
            }
            emitEvent(OperationStatusEvent{operationId, kind, terminalState, 0, 0, message});
        }
        bool requestOperationCancel(std::uint64_t operationId)
        {
            std::scoped_lock lock(operationsMutex_);
            const auto it = activeOperations_.find(operationId);
            if (it == activeOperations_.end()) return false;
            it->second.cancelRequested->store(true, std::memory_order_relaxed);
            return true;
        }
        std::size_t activeOperationCount() const
        {
            std::scoped_lock lock(operationsMutex_);
            return activeOperations_.size();
        }
        BackendCommandResult handleOperationCommand(const OperationCommand &command)
        {
            if (requestOperationCancel(command.operationId))
                return {true, BackendCommandType::Operation, "Operation cancel requested", command.operationId};
            return {false, BackendCommandType::Operation, "Unknown or finished operation", command.operationId};
        }

        // The jobs (submitReviewExportJson after its parse; submitReviewReanalysisJson after its parse).
        backend::services::ProcessingService &kernel;
        mutable std::mutex exportMutex_;
        mutable std::mutex reanalysisMutex_;
        bool reanalysisActive_{false};
        std::thread reanalysisThread_;
        std::string reanalysisStatusJson_{"{\"state\":\"idle\"}"};
        bool exportActive_{false};
        std::thread exportThread_;
        std::string exportStatusJson_{"{\"state\":\"idle\"}"};

        explicit PreS7Jobs(backend::services::ProcessingService &k) : kernel(k) {}
        ~PreS7Jobs()
        {
            if (exportThread_.joinable()) exportThread_.join();
            if (reanalysisThread_.joinable()) reanalysisThread_.join();
        }

        BackendCommandResult submitExport(br::ExportBatchRequest parsed)
        {
            auto &request = parsed.request;
            auto &sources = parsed.sources;
            request.sourcePath = sources.front();
            {
                std::scoped_lock lock(exportMutex_);
                if (exportActive_) return {false, BackendCommandType::Review, "An export is already active"};
                exportActive_ = true;
            }
            if (exportThread_.joinable()) exportThread_.join();
            CancelFlag cancelFlag;
            const auto operationId = beginOperation(BackendOperationKind::Export, &cancelFlag, request.sourcePath);
            {
                std::scoped_lock lock(exportMutex_);
                exportStatusJson_ = br::exportValidatingStatusJson(operationId);
            }
            try
            {
                exportThread_ = std::thread([this, operationId, cancelFlag, request, sources]() {
                    const auto outcome = br::runExportBatch(
                        request, sources, cancelFlag,
                        [this, operationId, &sources](std::size_t index, const backend::recording::HdfExportProgress &progress,
                                                      const std::vector<br::ExportFileOutcome> &done) {
                            {
                                std::scoped_lock lock(exportMutex_);
                                exportStatusJson_ = br::exportProgressStatusJson(operationId, sources, index, progress, done);
                            }
                            reportOperationProgress(operationId, progress.completed, progress.total);
                        });
                    const auto state = outcome.cancelled ? BackendOperationState::Cancelled
                                       : outcome.failed  ? BackendOperationState::Failed
                                                         : BackendOperationState::Completed;
                    {
                        std::scoped_lock lock(exportMutex_);
                        exportStatusJson_ = br::exportTerminalStatusJson(operationId, outcome);
                    }
                    finishOperation(operationId, state,
                                    state == BackendOperationState::Completed ? outcome.last.finalPath : outcome.error);
                    std::scoped_lock lock(exportMutex_);
                    exportActive_ = false;
                });
            }
            catch (const std::exception &e)
            {
                {
                    std::scoped_lock lock(exportMutex_);
                    exportActive_ = false;
                    exportStatusJson_ = br::exportStartFailedStatusJson(operationId, e.what());
                }
                finishOperation(operationId, BackendOperationState::Failed, e.what());
                return {false, BackendCommandType::Review, e.what(), operationId};
            }
            return {true, BackendCommandType::Review, "Export started", operationId};
        }

        BackendCommandResult submitReanalysis(const br::ReanalysisRequest &request)
        {
            {
                std::scoped_lock lock(reanalysisMutex_);
                if (reanalysisActive_) return {false, BackendCommandType::Review, "Reanalysis already active"};
                reanalysisActive_ = true;
            }
            if (reanalysisThread_.joinable()) reanalysisThread_.join();
            CancelFlag cancel;
            const auto id = beginOperation(BackendOperationKind::Reanalysis, &cancel, request.sourcePath);
            {
                std::scoped_lock lock(reanalysisMutex_);
                reanalysisStatusJson_ = br::reanalysisLoadingStatusJson(id);
            }
            try
            {
                reanalysisThread_ = std::thread([this, id, cancel, request] {
                    const auto outcome = br::runReanalysis(request, kernel, cancel, [&](std::uint64_t done, std::uint64_t total) {
                        {
                            std::scoped_lock lock(reanalysisMutex_);
                            reanalysisStatusJson_ = br::reanalysisProgressStatusJson(id, done, total);
                        }
                        reportOperationProgress(id, done, total);
                    });
                    {
                        std::scoped_lock lock(reanalysisMutex_);
                        reanalysisStatusJson_ = br::reanalysisTerminalStatusJson(id, request, outcome);
                    }
                    finishOperation(id,
                                    outcome.published   ? BackendOperationState::Completed
                                    : outcome.cancelled ? BackendOperationState::Cancelled
                                                        : BackendOperationState::Failed,
                                    outcome.error);
                    std::scoped_lock lock(reanalysisMutex_);
                    reanalysisActive_ = false;
                });
            }
            catch (const std::exception &error)
            {
                {
                    std::scoped_lock lock(reanalysisMutex_);
                    reanalysisActive_ = false;
                    reanalysisStatusJson_ = br::reanalysisStartFailedStatusJson(id, error.what());
                }
                finishOperation(id, BackendOperationState::Failed, error.what());
                return {false, BackendCommandType::Review, error.what(), id};
            }
            return {true, BackendCommandType::Review, "Reanalysis started", id};
        }
        std::string exportStatus() const
        {
            std::scoped_lock lock(exportMutex_);
            return exportStatusJson_;
        }
        std::string reanalysisStatus() const
        {
            std::scoped_lock lock(reanalysisMutex_);
            return reanalysisStatusJson_;
        }
    };

    // ---- one surface over the facade and the oracle ---------------------------
    struct Driver
    {
        virtual ~Driver() = default;
        virtual BackendCommandResult submitExport(const std::string &json) = 0;
        virtual BackendCommandResult submitReanalysis(const std::string &json) = 0;
        virtual std::string exportStatus() const = 0;
        virtual std::string reanalysisStatus() const = 0;
        virtual bool cancel(std::uint64_t id) = 0;
        virtual BackendCommandResult dispatchCancel(std::uint64_t id) = 0;
        virtual std::size_t active() const = 0;
        // Returns once the job's lane is free for the next start.
        virtual void waitLaneIdle(bool reanalysis) = 0;
        // Operation events of kind Export / Reanalysis.
        virtual void setSink(std::function<void(const OperationStatusEvent &)> sink) = 0;
    };

    struct FacadeDriver : Driver
    {
        BackendFacade &facade;
        explicit FacadeDriver(BackendFacade &f) : facade(f) {}
        BackendCommandResult submitExport(const std::string &j) override { return facade.submitReviewExportJson(j); }
        BackendCommandResult submitReanalysis(const std::string &j) override { return facade.submitReviewReanalysisJson(j); }
        std::string exportStatus() const override { return facade.fetchReviewExportStatusJson(); }
        std::string reanalysisStatus() const override { return facade.fetchReviewReanalysisStatusJson(); }
        bool cancel(std::uint64_t id) override { return facade.requestOperationCancel(id); }
        BackendCommandResult dispatchCancel(std::uint64_t id) override
        {
            return facade.dispatch(OperationCommand{OperationCommandAction::Cancel, id});
        }
        std::size_t active() const override { return facade.activeOperationCount(); }
        // The lane's own release (after the terminal event), not a sleep.
        void waitLaneIdle(bool reanalysis) override
        {
            MIB_REQUIRE(br::ReviewJobsTestAccess::waitIdle(BackendFacadeTestAccess::reviewJobs(facade),
                                                          reanalysis ? br::ReviewJobLane::Processing : br::ReviewJobLane::Export,
                                                          std::chrono::seconds(60)),
                        "lane released");
        }
        void setSink(std::function<void(const OperationStatusEvent &)> sink) override
        {
            if (!sink)
            {
                facade.setEventSink({});
                return;
            }
            facade.setEventSink([sink](const BackendEvent &event) {
                const auto *op = std::get_if<OperationStatusEvent>(&event);
                if (op && (op->kind == BackendOperationKind::Export || op->kind == BackendOperationKind::Reanalysis)) sink(*op);
            });
        }
    };

    struct OracleDriver : Driver
    {
        PreS7Jobs jobs;
        backend::services::ProcessingService &processing;
        std::string loaded; // the facade's loaded review file
        OracleDriver(backend::services::ProcessingService &p, std::string source) : jobs(p), processing(p), loaded(std::move(source)) {}
        BackendCommandResult submitExport(const std::string &j) override
        {
            br::ExportBatchRequest parsed;
            const auto chartConfig = processing.getProcessingConfig();
            const br::ExportDefaults defaults{processing.getPixelToMicronFactor(), chartConfig.ring_ratio_min,
                                              chartConfig.ring_ratio_max};
            std::string error;
            if (!br::parseExportRequest(j, defaults, parsed, error)) return {false, BackendCommandType::Review, error};
            parsed.request.sourcePath = loaded;
            if (parsed.sources.empty()) parsed.sources.push_back(loaded);
            return jobs.submitExport(parsed);
        }
        BackendCommandResult submitReanalysis(const std::string &j) override
        {
            br::ReanalysisRequest request;
            std::string error;
            if (!br::parseReanalysisRequest(j, processing.getProcessingConfig(), request, error))
                return {false, BackendCommandType::Review, error};
            return jobs.submitReanalysis(request);
        }
        std::string exportStatus() const override { return jobs.exportStatus(); }
        std::string reanalysisStatus() const override { return jobs.reanalysisStatus(); }
        bool cancel(std::uint64_t id) override { return jobs.requestOperationCancel(id); }
        BackendCommandResult dispatchCancel(std::uint64_t id) override
        {
            return jobs.handleOperationCommand(OperationCommand{OperationCommandAction::Cancel, id});
        }
        std::size_t active() const override { return jobs.activeOperationCount(); }
        // A join handshake: the pre-S7 worker cleared its single-flight flag
        // just before its thread ended.
        void waitLaneIdle(bool reanalysis) override
        {
            std::thread &worker = reanalysis ? jobs.reanalysisThread_ : jobs.exportThread_;
            if (worker.joinable()) worker.join();
        }
        void setSink(std::function<void(const OperationStatusEvent &)> sink) override { jobs.sink = std::move(sink); }
    };

    // ---- trace ------------------------------------------------------------------
    const char *stateName(BackendOperationState s)
    {
        switch (s)
        {
        case BackendOperationState::Started: return "Started";
        case BackendOperationState::Progress: return "Progress";
        case BackendOperationState::Completed: return "Completed";
        case BackendOperationState::Failed: return "Failed";
        case BackendOperationState::Cancelled: return "Cancelled";
        case BackendOperationState::TimedOut: return "TimedOut";
        }
        return "?";
    }
    bool terminalState(BackendOperationState s)
    {
        return s == BackendOperationState::Completed || s == BackendOperationState::Failed ||
               s == BackendOperationState::Cancelled || s == BackendOperationState::TimedOut;
    }

    struct Trace
    {
        std::string dir;
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> lines;
        std::map<std::uint64_t, int> ordinal;
        std::map<std::uint64_t, std::vector<BackendOperationState>> states;
        int violations{0};

        std::string nid(std::uint64_t id)
        {
            if (id == 0) return "0";
            auto it = ordinal.find(id);
            if (it == ordinal.end()) it = ordinal.emplace(id, static_cast<int>(ordinal.size()) + 1).first;
            return "#" + std::to_string(it->second);
        }
        // Caller holds `m`.
        std::string norm(std::string s)
        {
            // The directory appears raw in messages and JSON-escaped in the
            // status snapshots, where a Windows path's backslashes are doubled.
            const std::string quoted = json(dir).dump();
            const std::string escaped = quoted.substr(1, quoted.size() - 2);
            for (const std::string &needle : {escaped, dir})
            {
                for (std::size_t pos; (pos = s.find(needle)) != std::string::npos;) s.replace(pos, needle.size(), "<DIR>");
            }
            s = std::regex_replace(s, std::regex(R"(\.partial-[A-Za-z0-9_-]+)"), ".partial-<J>");
            s = std::regex_replace(s, std::regex(R"re("job_id":"[^"]*")re"), "\"job_id\":\"<J>\"");
            std::string out;
            std::smatch match;
            const std::regex idre(R"re("operation_id":"(\d+)")re");
            auto begin = s.cbegin();
            while (std::regex_search(begin, s.cend(), match, idre))
            {
                out.append(begin, match[0].first);
                out += "\"operation_id\":\"" + nid(std::stoull(match[1].str())) + "\"";
                begin = match[0].second;
            }
            out.append(begin, s.cend());
            return out;
        }
        void line(const std::string &l)
        {
            std::scoped_lock lock(m);
            lines.push_back(norm(l));
        }
        std::size_t reserve()
        {
            std::scoped_lock lock(m);
            lines.emplace_back("<pending>");
            return lines.size() - 1;
        }
        void fill(std::size_t slot, const std::string &prefix, const BackendCommandResult &r)
        {
            std::scoped_lock lock(m);
            lines[slot] = prefix + "ok=" + (r.ok ? "1" : "0") + " msg=" + norm(r.message) + " op=" + nid(r.operationId);
        }
        void event(const OperationStatusEvent &op, const std::string &status)
        {
            std::scoped_lock lock(m);
            auto &seq = states[op.operationId];
            if (op.state == BackendOperationState::Started ? !seq.empty() : (seq.empty() || terminalState(seq.back())))
                ++violations;
            seq.push_back(op.state);
            lines.push_back(norm(std::string("EV ") + (op.kind == BackendOperationKind::Export ? "Export " : "Reanalysis ") +
                                 nid(op.operationId) + " " + stateName(op.state) + " p=" + std::to_string(op.progress) + "/" +
                                 std::to_string(op.total) + " msg=" + op.message + " | status=" + status));
            cv.notify_all();
        }
        bool waitTerminal(std::uint64_t id)
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(60), [&] {
                const auto it = states.find(id);
                return it != states.end() && !it->second.empty() && terminalState(it->second.back());
            });
        }
        BackendOperationState last(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            return states[id].back();
        }
        int terminals(std::uint64_t id)
        {
            std::scoped_lock lock(m);
            int n = 0;
            for (auto s : states[id]) n += terminalState(s) ? 1 : 0;
            return n;
        }
    };

    BackendCommandResult started(Trace &t, const std::string &prefix, const std::function<BackendCommandResult()> &fn)
    {
        const auto slot = t.reserve(); // before the call: the worker's first events come after it
        const auto r = fn();
        t.fill(slot, prefix, r);
        return r;
    }

    struct Latch
    {
        std::mutex m;
        std::condition_variable cv;
        bool open{false};
        bool waiting{false};
        void wait()
        {
            std::unique_lock lock(m);
            waiting = true;
            cv.notify_all();
            cv.wait(lock, [&] { return open; });
        }
        bool waitUntilWaiting()
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::seconds(60), [&] { return waiting; });
        }
        void release()
        {
            std::scoped_lock lock(m);
            open = true;
            cv.notify_all();
        }
    };

    void writeFixture(const fs::path &path, int n)
    {
        backend::services::Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(path.string()), "fixture open");
        MIB_REQUIRE(writer.initializeDatasets(), "fixture datasets");
        std::vector<backend::services::ProcessedFrame> frames(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            auto &f = frames[static_cast<std::size_t>(i)];
            f.index = static_cast<std::uint64_t>(i);
            f.timestampNs = 1000 + static_cast<std::uint64_t>(i);
            f.originalImage = cv::Mat(32, 32, CV_8UC1, cv::Scalar(i * 5));
            f.processedImage = f.originalImage.clone();
            f.validation.isValid = true;
            f.validation.objectCount = 1;
            f.validation.area = 100 + 7 * i;
            f.validation.deformability = 0.01 * (i % 9) + 0.02;
            f.validation.ringRatio = 1.0 + 0.05 * i;
        }
        MIB_REQUIRE(writer.appendFrames(frames, {}), "fixture write");
        MIB_REQUIRE(writer.writeExperimentInfo(1000, 1000 + n - 1, n, 0, {}, {0, 0, 32, 32}), "fixture info");
        writer.closeFile();
    }

    // The scenarios both drivers run; `dir` holds the driver's own fixture and outputs.
    std::vector<std::string> runScenarios(Driver &d, const fs::path &dir, const fs::path &source)
    {
        Trace t;
        t.dir = dir.string();
        std::mutex hookMutex;
        std::function<void(const OperationStatusEvent &)> hook;
        d.setSink([&](const OperationStatusEvent &op) {
            t.event(op, op.kind == BackendOperationKind::Export ? d.exportStatus() : d.reanalysisStatus());
            std::function<void(const OperationStatusEvent &)> h;
            {
                std::scoped_lock lock(hookMutex);
                h = hook;
            }
            if (h) h(op);
        });
        auto setHook = [&](std::function<void(const OperationStatusEvent &)> h) {
            std::scoped_lock lock(hookMutex);
            hook = std::move(h);
        };
        // After a job's terminal event, wait for its lane's release: both
        // drivers free the lane only after the terminal event returned (the
        // pre-S7 code cleared its flag after finishOperation), so the next
        // start must not race it. The status was written before the event.
        auto settle = [&](std::uint64_t id, bool reanalysis) {
            MIB_REQUIRE(t.waitTerminal(id), "terminal event");
            d.waitLaneIdle(reanalysis);
            t.line("STATUS export=" + d.exportStatus());
            t.line("STATUS reanalysis=" + d.reanalysisStatus());
        };
        const json all{{"output_root", dir.string()}, {"format", "all"}};
        int outN = 0;
        auto reanalysisRequest = [&](const fs::path &src) {
            return json{{"source_path", src.string()},
                        {"output_path", (dir / ("re-" + std::to_string(outN++) + ".h5")).string()},
                        {"dataset", "all"},
                        {"start", 0},
                        {"count", 0}}
                .dump();
        };
        auto recordResult = [&](const std::string &prefix, const BackendCommandResult &r) {
            const auto slot = t.reserve();
            t.fill(slot, prefix, r);
        };

        t.line("== idle");
        t.line("STATUS export=" + d.exportStatus());
        t.line("STATUS reanalysis=" + d.reanalysisStatus());

        t.line("== export all completes");
        settle(started(t, "START ", [&] { return d.submitExport(all.dump()); }).operationId, false);

        t.line("== export cancelled at its first progress; duplicate from the event");
        {
            std::atomic<bool> once{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Export && op.state == BackendOperationState::Progress && !once.exchange(true))
                {
                    recordResult("DUP ", d.submitExport(all.dump()));
                    t.line(std::string("CANCEL ") + (d.cancel(op.operationId) ? "1" : "0"));
                }
            });
            const auto r = started(t, "START ", [&] { return d.submitExport(all.dump()); });
            settle(r.operationId, false);
            setHook({});
            t.line(std::string("CANCEL-AFTER ") + (d.cancel(r.operationId) ? "1" : "0"));
        }

        t.line("== metrics CSV to an explicit destination");
        settle(started(t, "START ",
                       [&] {
                           return d.submitExport(json{{"format", "metrics_csv"},
                                                      {"output_root", dir.string()},
                                                      {"explicit_destination", (dir / "m.csv").string()}}
                                                     .dump());
                       })
                   .operationId,
               false);

        t.line("== batch with a missing file");
        settle(started(t, "START ",
                       [&] {
                           return d.submitExport(json{{"format", "metrics_csv"},
                                                      {"output_root", dir.string()},
                                                      {"source_paths", json::array({source.string(), (dir / "missing.h5").string(),
                                                                                    source.string()})}}
                                                     .dump());
                       })
                   .operationId,
               false);

        t.line("== charts");
        settle(started(t, "START ", [&] { return d.submitExport(json{{"format", "charts"}, {"output_root", dir.string()}}.dump()); })
                   .operationId,
               false);

        t.line("== reanalysis completes");
        settle(started(t, "START ", [&] { return d.submitReanalysis(reanalysisRequest(source)); }).operationId, true);

        t.line("== reanalysis cancelled at its first progress; duplicate from the event");
        {
            std::atomic<bool> once{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Reanalysis && op.state == BackendOperationState::Progress &&
                    !once.exchange(true))
                {
                    recordResult("DUP ", d.submitReanalysis(reanalysisRequest(source)));
                    t.line(std::string("CANCEL ") + (d.cancel(op.operationId) ? "1" : "0"));
                }
            });
            settle(started(t, "START ", [&] { return d.submitReanalysis(reanalysisRequest(source)); }).operationId, true);
            setHook({});
        }

        t.line("== reanalysis of a missing source");
        settle(started(t, "START ", [&] { return d.submitReanalysis(reanalysisRequest(dir / "missing.h5")); }).operationId, true);

        t.line("== an export while a reanalysis is held at its first progress");
        {
            Latch hold;
            std::atomic<bool> once{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Reanalysis && op.state == BackendOperationState::Progress &&
                    !once.exchange(true))
                    hold.wait();
            });
            const auto re = started(t, "START reanalysis ", [&] { return d.submitReanalysis(reanalysisRequest(source)); });
            MIB_REQUIRE(hold.waitUntilWaiting(), "reanalysis held");
            recordResult("DUP reanalysis ", d.submitReanalysis(reanalysisRequest(source)));
            const auto ex = started(t, "START export ", [&] { return d.submitExport(all.dump()); });
            MIB_REQUIRE(t.waitTerminal(ex.operationId), "export beside the reanalysis");
            d.waitLaneIdle(false);
            t.line("STATUS export=" + d.exportStatus());
            t.line("STATUS reanalysis=" + d.reanalysisStatus());
            recordResult("DUP reanalysis ", d.submitReanalysis(reanalysisRequest(source)));
            hold.release();
            settle(re.operationId, true);
            setHook({});
        }

        t.line("== cancel through dispatch(OperationCommand)");
        {
            std::atomic<bool> once{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Export && op.state == BackendOperationState::Progress && !once.exchange(true))
                    recordResult("DISPATCH-CANCEL ", d.dispatchCancel(op.operationId));
            });
            const auto r = started(t, "START ", [&] { return d.submitExport(all.dump()); });
            settle(r.operationId, false);
            setHook({});
            recordResult("DISPATCH-CANCEL-AFTER ", d.dispatchCancel(r.operationId));
        }

        t.line("== a start from inside the terminal event");
        {
            std::atomic<bool> once{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Export && terminalState(op.state) && !once.exchange(true))
                    recordResult("RESUBMIT ", d.submitExport(all.dump()));
            });
            settle(started(t, "START ", [&] { return d.submitExport(all.dump()); }).operationId, false);
            std::atomic<bool> once2{false};
            setHook([&](const OperationStatusEvent &op) {
                if (op.kind == BackendOperationKind::Reanalysis && terminalState(op.state) && !once2.exchange(true))
                    recordResult("RESUBMIT ", d.submitReanalysis(reanalysisRequest(source)));
            });
            settle(started(t, "START ", [&] { return d.submitReanalysis(reanalysisRequest(source)); }).operationId, true);
            setHook({});
        }
        d.setSink({});
        std::scoped_lock lock(t.m);
        MIB_EXPECT(t.violations == 0, "Started first, nothing after a terminal");
        for (const auto &[id, seq] : t.states)
        {
            int terminals = 0;
            for (auto s : seq) terminals += terminalState(s) ? 1 : 0;
            MIB_EXPECT(terminals == 1, "one terminal per operation");
        }
        return t.lines;
    }
} // namespace

int main()
{
    mib::test::Watchdog watchdog(120);
    mib::test::TempDir root("facade_review_jobs");
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", "mock");
#else
    setenv("MIB_CAMERA_MODE", "mock", 1);
#endif
#ifdef _WIN32
    const fs::path oracleDir = root / "oracle";
    const fs::path facadeDir = root / "facade";
#else
    // A backslash is JSON-escaped in the status snapshots, as every Windows
    // path is, so the POSIX lanes check the trace normalization too (the MSVC
    // lane does not run on every PR).
    const fs::path oracleDir = root / "ora\\cle";
    const fs::path facadeDir = root / "fac\\ade";
#endif
    fs::create_directories(oracleDir);
    fs::create_directories(facadeDir);
    writeFixture(oracleDir / "fixture.h5", 24);
    writeFixture(facadeDir / "fixture.h5", 24);

    backend::AppBackend backend;
    BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((root / "data").string()), "initialize");
    RecordingLoadCommand load;
    load.filePath = (facadeDir / "fixture.h5").string();
    MIB_REQUIRE(facade.dispatch(load).ok, "load fixture");

    // ---- the oracle and the facade, scenario for scenario ---------------------
    std::vector<std::string> expected, actual;
    {
        OracleDriver oracle(backend.processing(), (oracleDir / "fixture.h5").string());
        expected = runScenarios(oracle, oracleDir, oracleDir / "fixture.h5");
    }
    watchdog.mark("oracle");
    {
        FacadeDriver driver(facade);
        actual = runScenarios(driver, facadeDir, facadeDir / "fixture.h5");
    }
    watchdog.mark("facade");
    MIB_EXPECT(expected.size() > 100, "the scenarios produced a trace: " + std::to_string(expected.size()));
    MIB_EXPECT(expected.size() == actual.size(),
               "trace length " + std::to_string(actual.size()) + " vs pre-S7 " + std::to_string(expected.size()));
    for (std::size_t i = 0; i < std::min(expected.size(), actual.size()); ++i)
    {
        if (expected[i] != actual[i])
        {
            MIB_EXPECT(false, "line " + std::to_string(i) + "\n  pre-S7: " + expected[i] + "\n  facade: " + actual[i]);
            break;
        }
    }

    // ---- the facade alone: both lanes held, cancel one, release in turn --------
    {
        Trace t;
        t.dir = facadeDir.string();
        std::mutex hookMutex;
        Latch *holdExport = nullptr;
        Latch *holdReanalysis = nullptr;
        std::atomic<std::uint64_t> heldExport{0}, heldReanalysis{0};
        facade.setEventSink([&](const BackendEvent &event) {
            const auto *op = std::get_if<OperationStatusEvent>(&event);
            if (!op || (op->kind != BackendOperationKind::Export && op->kind != BackendOperationKind::Reanalysis)) return;
            t.event(*op, "");
            if (op->state != BackendOperationState::Progress) return;
            Latch *latch = nullptr;
            {
                std::scoped_lock lock(hookMutex);
                if (op->kind == BackendOperationKind::Export && holdExport && heldExport.exchange(op->operationId) == 0)
                    latch = holdExport;
                if (op->kind == BackendOperationKind::Reanalysis && holdReanalysis &&
                    heldReanalysis.exchange(op->operationId) == 0)
                    latch = holdReanalysis;
            }
            if (latch) latch->wait();
        });
        int outN = 0;
        const json all{{"output_root", facadeDir.string()}, {"format", "all"}};
        auto reanalysis = [&] {
            return json{{"source_path", (facadeDir / "fixture.h5").string()},
                        {"output_path", (facadeDir / ("stress-" + std::to_string(outN++) + ".h5")).string()},
                        {"dataset", "all"}}
                .dump();
        };
        auto arm = [&](Latch *e, Latch *r) {
            std::scoped_lock lock(hookMutex);
            holdExport = e;
            holdReanalysis = r;
            heldExport = 0;
            heldReanalysis = 0;
        };
        // Both lanes released (after their terminal events), woken by the
        // release itself: no operation count, no sleep.
        br::ReviewJobs &lanes = BackendFacadeTestAccess::reviewJobs(facade);
        auto idle = [&] {
            MIB_REQUIRE(br::ReviewJobsTestAccess::waitIdle(lanes, std::chrono::seconds(60)), "both lanes released");
            MIB_EXPECT(facade.activeOperationCount() == 0, "no operation left");
        };
        for (int cycle = 0; cycle < 12; ++cycle)
        {
            Latch le, lr;
            arm(&le, &lr);
            const auto ex = facade.submitReviewExportJson(all.dump());
            MIB_REQUIRE(ex.ok, "cycle export started: " + ex.message);
            // The lane's snapshot belongs to the new job once the start returns
            // (validating, or its first progress if the worker got there first).
            MIB_EXPECT(json::parse(facade.fetchReviewExportStatusJson())["operation_id"] == std::to_string(ex.operationId),
                       "export snapshot is the new job's when the start returns");
            MIB_REQUIRE(le.waitUntilWaiting(), "export held");
            // The reanalysis loads its source while the export waits outside HDF5.
            const auto re = facade.submitReviewReanalysisJson(reanalysis());
            MIB_REQUIRE(re.ok, "cycle reanalysis beside the export (D7): " + re.message);
            MIB_EXPECT(json::parse(facade.fetchReviewReanalysisStatusJson())["operation_id"] == std::to_string(re.operationId),
                       "reanalysis snapshot is the new job's when the start returns");
            MIB_REQUIRE(lr.waitUntilWaiting(), "reanalysis held");
            const auto dupE = facade.submitReviewExportJson(all.dump());
            const auto dupR = facade.submitReviewReanalysisJson(reanalysis());
            MIB_EXPECT(!dupE.ok && dupE.message == "An export is already active" && dupE.operationId == 0, "export lane refused");
            MIB_EXPECT(!dupR.ok && dupR.message == "Reanalysis already active" && dupR.operationId == 0, "processing lane refused");
            MIB_EXPECT(facade.activeOperationCount() == 2, "two operations at once");
            const bool cancelExport = (cycle % 3) != 1;
            const bool cancelReanalysis = (cycle % 3) != 0;
            // One lane at a time runs HDF5 code: the first released, then the other.
            if (cancelExport)
            {
                const auto c = cycle % 2 ? facade.dispatch(OperationCommand{OperationCommandAction::Cancel, ex.operationId})
                                         : BackendCommandResult{facade.requestOperationCancel(ex.operationId),
                                                                BackendCommandType::Operation, "", ex.operationId};
                MIB_EXPECT(c.ok, "export cancel accepted");
            }
            le.release();
            MIB_REQUIRE(t.waitTerminal(ex.operationId), "export terminal");
            MIB_EXPECT(t.last(ex.operationId) ==
                           (cancelExport ? BackendOperationState::Cancelled : BackendOperationState::Completed),
                       "export outcome in cycle " + std::to_string(cycle));
            MIB_EXPECT(t.terminals(re.operationId) == 0, "the held reanalysis is untouched");
            const auto exportStatus = json::parse(facade.fetchReviewExportStatusJson());
            MIB_EXPECT(exportStatus["operation_id"] == std::to_string(ex.operationId) &&
                           exportStatus["state"] == (cancelExport ? "cancelled" : "completed"),
                       "export status is its lane's");
            MIB_EXPECT(json::parse(facade.fetchReviewReanalysisStatusJson())["state"] == "running",
                       "reanalysis status still running");
            if (cancelReanalysis) MIB_EXPECT(facade.requestOperationCancel(re.operationId), "reanalysis cancel accepted");
            lr.release();
            MIB_REQUIRE(t.waitTerminal(re.operationId), "reanalysis terminal");
            MIB_EXPECT(t.last(re.operationId) ==
                           (cancelReanalysis ? BackendOperationState::Cancelled : BackendOperationState::Completed),
                       "reanalysis outcome in cycle " + std::to_string(cycle));
            MIB_EXPECT(!facade.requestOperationCancel(re.operationId) && !facade.requestOperationCancel(ex.operationId),
                       "finished operations cannot be cancelled");
            idle();
            arm(nullptr, nullptr);
        }
        watchdog.mark("cycles");

        // ---- shutdown with both lanes held --------------------------------------
        {
            Latch le, lr;
            std::atomic<bool> shutdownCancel{false};
            std::thread releaser;
            std::mutex releaserMutex;
            arm(&le, &lr);
            const auto ex = facade.submitReviewExportJson(all.dump());
            MIB_REQUIRE(ex.ok && le.waitUntilWaiting(), "shutdown: export held");
            const auto re = facade.submitReviewReanalysisJson(reanalysis());
            MIB_REQUIRE(re.ok && lr.waitUntilWaiting(), "shutdown: reanalysis held");
            // cancelAllOperations reports the reanalysis cancelled ("Backend
            // shutdown") on the shutdown thread; then the held jobs are let go
            // in turn (the reanalysis first, the export once the reanalysis
            // lane is released).
            facade.setEventSink([&](const BackendEvent &event) {
                const auto *op = std::get_if<OperationStatusEvent>(&event);
                if (!op || (op->kind != BackendOperationKind::Export && op->kind != BackendOperationKind::Reanalysis)) return;
                t.event(*op, op->message);
                if (op->operationId == re.operationId && op->state == BackendOperationState::Cancelled &&
                    !shutdownCancel.exchange(true))
                {
                    std::scoped_lock lock(releaserMutex);
                    releaser = std::thread([&] {
                        lr.release();
                        // The reanalysis worker is done (lane released) before
                        // the export resumes its HDF5 work.
                        MIB_EXPECT(br::ReviewJobsTestAccess::waitIdle(lanes, br::ReviewJobLane::Processing,
                                                                      std::chrono::seconds(60)),
                                   "the reanalysis lane is released during shutdown");
                        le.release();
                    });
                }
            });
            facade.shutdown();
            {
                std::scoped_lock lock(releaserMutex);
                if (releaser.joinable()) releaser.join();
            }
            MIB_EXPECT(shutdownCancel.load(), "shutdown cancelled the reanalysis");
            MIB_EXPECT(t.terminals(re.operationId) == 1 && t.last(re.operationId) == BackendOperationState::Cancelled,
                       "the reanalysis has one terminal: Backend shutdown");
            MIB_EXPECT(t.terminals(ex.operationId) == 1, "the export reported its own terminal before shutdown returned");
            const auto exportState = t.last(ex.operationId);
            MIB_EXPECT(exportState == BackendOperationState::Cancelled || exportState == BackendOperationState::Completed,
                       "the export ended (cancelled by shutdown unless it finished first)");
            const auto exportStatus = json::parse(facade.fetchReviewExportStatusJson());
            MIB_EXPECT(exportStatus["operation_id"] == std::to_string(ex.operationId) && exportStatus["state"] != "running",
                       "export status terminal");
            MIB_EXPECT(json::parse(facade.fetchReviewReanalysisStatusJson())["state"] == "cancelled",
                       "the reanalysis worker saw its flag and wrote a cancelled status");
            MIB_EXPECT(facade.activeOperationCount() == 0, "no operation outlives shutdown");
            std::size_t after = 0;
            {
                std::scoped_lock lock(t.m);
                after = t.lines.size();
            }
            // Every lane worker was joined by shutdown(): nothing can report any more.
            MIB_EXPECT(!facade.submitReviewExportJson(all.dump()).ok, "no export after shutdown");
            {
                std::scoped_lock lock(t.m);
                MIB_EXPECT(t.lines.size() == after, "nothing reported after shutdown returned");
            }
            facade.setEventSink({});
        }
        std::scoped_lock lock(t.m);
        MIB_EXPECT(t.violations == 0, "facade cycles: Started first, nothing after a terminal");
    }
    return mib::test::exitCode();
}
