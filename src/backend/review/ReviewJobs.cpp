#include "backend/review/ReviewJobs.h"
#include "backend/review/ExportEngine.h"
#include "backend/review/Reanalysis.h"
#include "backend/app/WallClock.h"

#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/MonitoringDensity.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewSession.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <thread>

namespace backend::review
{

    namespace
    {
        namespace fs = std::filesystem;

        struct Outcome
        {
            ReviewJobState state{ReviewJobState::Failed};
            std::string message;
        };

        std::uint64_t nowNs()
        {
            return backend::app::WallClock::nowNs();
        }

        recording::HdfExportSeriesRange toServiceRange(const ExportSeriesRange &r)
        {
            recording::HdfExportSeriesRange out;
            out.exportSeries = r.exportSeries;
            out.startInclusive = static_cast<std::size_t>(r.startInclusive);
            out.endInclusive = r.endInclusive == static_cast<std::uint64_t>(-1)
                                   ? static_cast<std::size_t>(-1)
                                   : static_cast<std::size_t>(r.endInclusive);
            return out;
        }
    } // namespace

    ReviewJobLane laneOf(ReviewJobKind kind)
    {
        switch (kind)
        {
        case ReviewJobKind::ExportMetrics:
        case ReviewJobKind::ExportAll:
        case ReviewJobKind::BatchExport:
        case ReviewJobKind::ExportCharts:
        case ReviewJobKind::Export:
            return ReviewJobLane::Export;
        case ReviewJobKind::RegenerateMasks:
        case ReviewJobKind::ComputeCore:
        case ReviewJobKind::Density:
        case ReviewJobKind::Reanalysis:
            break;
        }
        return ReviewJobLane::Processing;
    }

    ReviewJobsOptions ReviewJobsOptions::forReviewBridge()
    {
        ReviewJobsOptions options;
        options.concurrentLanes = false; // #722: one job of any kind at a time
        options.laneRelease = ReviewLaneRelease::BeforeTerminal;
        return options;
    }

    ReviewJobsOptions ReviewJobsOptions::forFacade(ReviewJobHost host)
    {
        ReviewJobsOptions options;
        options.concurrentLanes = true; // one export and one reanalysis at once, as before #512
        options.laneRelease = ReviewLaneRelease::AfterTerminal;
        options.host = std::move(host);
        return options;
    }

    namespace
    {
        constexpr const char *kAnotherJobRunning = "Another review job is still running";
        constexpr const char *kIdleStatus = "{\"state\":\"idle\"}";
        constexpr std::size_t kLaneCount = 2;

        std::size_t laneIndex(ReviewJobLane lane) { return static_cast<std::size_t>(lane); }

        // Joins a finished worker; one that is the calling thread (a start or
        // shutdown from inside its own terminal event) is detached instead.
        void reap(std::thread &worker)
        {
            if (!worker.joinable()) return;
            if (worker.get_id() == std::this_thread::get_id()) worker.detach();
            else worker.join();
        }
    } // namespace

    struct ReviewJobs::Impl : std::enable_shared_from_this<ReviewJobs::Impl>
    {
        using CancelFlag = std::shared_ptr<std::atomic<bool>>;

        // A job as the lane runner sees it.
        struct Job
        {
            ReviewJobKind kind{ReviewJobKind::ExportMetrics};
            // The host's Started message (the job's source); ReviewJobs' own
            // Started carries none.
            std::string startedMessage;
            // The refusal while this job's own lane is busy.
            std::string busyMessage{kAnotherJobRunning};
            // On the starting thread once the id is known, before the worker
            // exists (the JSON jobs' first status snapshot).
            std::function<void(std::uint64_t id)> onBegin;
            std::function<Outcome(std::uint64_t id, const CancelFlag &cancel)> body;
            // The status snapshot when the worker could not start or the body
            // threw (the JSON jobs).
            std::function<void(std::uint64_t id, const std::string &error)> onFailure;
            // ReviewJobs' own jobs: a set cancel flag turns any outcome but
            // Completed into Cancelled. The JSON jobs report the engine's
            // outcome as it is (the facade's rule before S7).
            bool cancelOverridesOutcome{true};
        };

        struct StartResult
        {
            std::uint64_t id{0};
            bool started{false};
            std::string error;
            std::exception_ptr failure; // the worker could not start (thrown again by the native starts)
        };

        struct Lane
        {
            bool busy{false};     // claimed: being started or running
            bool starting{false}; // claimed, worker not yet created (shutdown waits)
            std::thread::id starter;
            std::uint64_t id{0};
            CancelFlag cancel;
            std::thread worker; // the last job's worker, joined by the next start or shutdown
            std::string status{kIdleStatus}; // the JSON job's snapshot (export lane: review_export_json; processing: review_reanalysis_json)
        };

        ReviewSession &session;
        services::ProcessingService *processing;
        ReviewJobSink sink;
        const ReviewJobsOptions options;

        mutable std::mutex mutex;
        std::condition_variable laneChanged;
        std::array<Lane, kLaneCount> lanes;
        std::uint64_t nextId{1};
        std::string computedCoreJson;
        std::string computedCorePath; // file computedCoreJson belongs to
        DensityResult density;

        Impl(ReviewSession &s, services::ProcessingService *p, ReviewJobSink k, ReviewJobsOptions o)
            : session(s), processing(p), sink(std::move(k)), options(std::move(o)) {}

        void emit(const ReviewJobEvent &e)
        {
            if (sink)
            {
                try { sink(e); } catch (...) {}
            }
        }

        void setStatus(ReviewJobLane lane, std::string status)
        {
            std::scoped_lock lock(mutex);
            lanes[laneIndex(lane)].status = std::move(status);
        }

        std::string status(ReviewJobLane lane) const
        {
            std::scoped_lock lock(mutex);
            return lanes[laneIndex(lane)].status;
        }

        void releaseLane(std::size_t lane)
        {
            std::scoped_lock lock(mutex);
            lanes[lane].busy = false;
            lanes[lane].starting = false;
            laneChanged.notify_all();
        }

        // Claims the job's lane (refused while it, or under single-job policy
        // any lane, is busy), takes the operation (the host's or a new id),
        // reports Started, then starts the worker. Never holds the mutex while
        // calling the host or the sink, so either may call back in.
        StartResult start(Job jobIn)
        {
            auto job = std::make_shared<Job>(std::move(jobIn));
            const std::size_t lane = laneIndex(laneOf(job->kind));
            StartResult result;
            std::thread previous;
            CancelFlag flag;
            std::uint64_t id = 0;
            const bool hosted = static_cast<bool>(options.host.beginOperation);
            {
                std::scoped_lock lock(mutex);
                for (std::size_t i = 0; i < kLaneCount; ++i)
                {
                    if (!lanes[i].busy) continue;
                    if (i == lane)
                    {
                        result.error = job->busyMessage;
                        return result;
                    }
                    if (!options.concurrentLanes)
                    {
                        result.error = kAnotherJobRunning;
                        return result;
                    }
                }
                Lane &l = lanes[lane];
                l.busy = true;
                l.starting = true;
                l.starter = std::this_thread::get_id();
                l.cancel.reset();
                l.id = 0;
                previous = std::move(l.worker);
                if (!hosted)
                {
                    id = nextId++;
                    flag = std::make_shared<std::atomic<bool>>(false);
                    l.id = id;
                    l.cancel = flag;
                }
            }
            // The lane's previous job has reported its terminal state (or is
            // reporting it: BeforeTerminal); join it before this one starts.
            reap(previous);
            if (hosted)
            {
                try
                {
                    id = options.host.beginOperation(job->kind, job->startedMessage, flag);
                }
                catch (...)
                {
                    releaseLane(lane);
                    throw;
                }
                if (!flag) flag = std::make_shared<std::atomic<bool>>(false);
                std::scoped_lock lock(mutex);
                lanes[lane].id = id;
                lanes[lane].cancel = flag;
            }
            else
            {
                emit(ReviewJobEvent{id, job->kind, ReviewJobState::Started, 0, 0, {}});
            }
            try
            {
                if (job->onBegin) job->onBegin(id);
                std::scoped_lock lock(mutex);
                // The worker owns a reference to the runner state (this Impl):
                // a shutdown from inside one of its events detaches it (it
                // cannot join itself), and the ReviewJobs object may then be
                // destroyed while the event's callback is still running; the
                // sink, the lanes and the mutex it touches afterwards stay alive
                // until it ends (#742 review).
                lanes[lane].worker = std::thread([self = shared_from_this(), lane, id, flag, job]() {
                    self->run(lane, id, flag, *job);
                });
                lanes[lane].starting = false;
                laneChanged.notify_all();
            }
            catch (...)
            {
                // No worker (std::thread threw, or the first snapshot did): the
                // lane is free again, and the operation gets its one terminal
                // state here.
                result.failure = std::current_exception();
                result.error = "unknown error";
                try { std::rethrow_exception(result.failure); }
                catch (const std::exception &e) { result.error = e.what(); }
                catch (...) {}
                releaseLane(lane);
                if (job->onFailure) try { job->onFailure(id, result.error); } catch (...) {}
                emit(ReviewJobEvent{id, job->kind, ReviewJobState::Failed, 0, 0, result.error});
                result.id = id;
                return result;
            }
            result.id = id;
            result.started = true;
            return result;
        }

        void run(std::size_t lane, std::uint64_t id, const CancelFlag &flag, const Job &job)
        {
            Outcome outcome;
            try
            {
                outcome = job.body(id, flag);
            }
            catch (const std::exception &e)
            {
                outcome = Outcome{ReviewJobState::Failed, std::string("job failed: ") + e.what()};
                if (job.onFailure) try { job.onFailure(id, outcome.message); } catch (...) {}
            }
            catch (...)
            {
                outcome = Outcome{ReviewJobState::Failed, "job failed: unknown error"};
                if (job.onFailure) try { job.onFailure(id, outcome.message); } catch (...) {}
            }
            if (job.cancelOverridesOutcome && flag->load() && outcome.state != ReviewJobState::Completed)
            {
                outcome.state = ReviewJobState::Cancelled;
            }
            const bool releaseFirst = options.laneRelease == ReviewLaneRelease::BeforeTerminal;
            if (releaseFirst) releaseLane(lane);
            emit(ReviewJobEvent{id, job.kind, outcome.state, 0, 0, outcome.message});
            if (!releaseFirst) releaseLane(lane);
        }

        // One of ReviewJobs' own jobs: refusals as `error`, a worker that
        // could not start thrown as before S7.
        std::uint64_t startOwn(ReviewJobKind kind, std::function<Outcome(std::uint64_t, const CancelFlag &)> body,
                               std::string *error)
        {
            Job job;
            job.kind = kind;
            job.body = std::move(body);
            const StartResult r = start(std::move(job));
            if (r.failure) std::rethrow_exception(r.failure);
            if (r.id == 0 && error) *error = r.error;
            return r.id;
        }

        void progress(std::uint64_t id, ReviewJobKind kind, std::uint64_t done, std::uint64_t total,
                      const std::string &message = {})
        {
            emit(ReviewJobEvent{id, kind, ReviewJobState::Progress, done, total, message});
        }

        // An export's progress as a job event ("<phase>[: <current output>]").
        void exportProgress(std::uint64_t id, ReviewJobKind kind, const recording::HdfExportProgress &p)
        {
            progress(id, kind, p.completed, p.total,
                     std::string(recording::toString(p.phase)) + (p.currentOutput.empty() ? "" : ": " + p.currentOutput));
        }

        // One export through the engine (backend/review/ExportEngine.h), the
        // job's cancel flag applied at each progress report.
        Outcome runExport(std::uint64_t id, ReviewJobKind kind, const recording::HdfExportRequest &request,
                          const std::shared_ptr<std::atomic<bool>> &flag)
        {
            ExportRunOptions options;
            options.polling = ExportCancelPolling::AtProgress;
            const auto result = runExportFile(
                request, flag, [&](const recording::HdfExportProgress &p) { exportProgress(id, kind, p); }, options);
            return exportOutcome(result);
        }

        static Outcome exportOutcome(const recording::HdfExportResult &result)
        {
            switch (result.status)
            {
            case recording::HdfExportStatus::Completed:
                return Outcome{ReviewJobState::Completed, result.finalPath};
            case recording::HdfExportStatus::Cancelled:
                return Outcome{ReviewJobState::Cancelled,
                               result.retainedPartialPath.empty() ? "partial output was discarded"
                                                                   : "partial output kept at " + result.retainedPartialPath};
            default:
                return Outcome{ReviewJobState::Failed, result.error.empty() ? "export failed" : result.error};
            }
        }
    };

    ReviewJobs::ReviewJobs(ReviewSession &session, services::ProcessingService *processing, ReviewJobSink sink,
                           ReviewJobsOptions options)
        : impl_(std::make_shared<Impl>(session, processing, std::move(sink), std::move(options)))
    {
    }

    ReviewJobs::~ReviewJobs() { shutdown(); }

    void ReviewJobs::shutdown()
    {
        std::vector<std::thread> workers;
        {
            std::unique_lock lock(impl_->mutex);
            // A job between claim and worker creation is waited for (unless
            // this thread is the one starting it), so its worker is joined too.
            impl_->laneChanged.wait(lock, [&] {
                for (const auto &l : impl_->lanes)
                    if (l.starting && l.starter != std::this_thread::get_id()) return false;
                return true;
            });
            for (auto &l : impl_->lanes)
            {
                if (l.cancel) l.cancel->store(true);
                workers.push_back(std::move(l.worker));
            }
        }
        for (auto &worker : workers) reap(worker);
    }

    bool ReviewJobs::cancel(std::uint64_t operationId)
    {
        std::scoped_lock lock(impl_->mutex);
        for (auto &l : impl_->lanes)
        {
            if (l.busy && l.id == operationId && l.cancel)
            {
                l.cancel->store(true);
                return true;
            }
        }
        return false;
    }

    bool ReviewJobs::busy() const
    {
        std::scoped_lock lock(impl_->mutex);
        for (const auto &l : impl_->lanes)
            if (l.busy) return true;
        return false;
    }

    bool ReviewJobs::busy(ReviewJobLane lane) const
    {
        std::scoped_lock lock(impl_->mutex);
        return impl_->lanes[laneIndex(lane)].busy;
    }

    std::string ReviewJobs::exportStatusJson() const { return impl_->status(ReviewJobLane::Export); }
    std::string ReviewJobs::reanalysisStatusJson() const { return impl_->status(ReviewJobLane::Processing); }

    ReviewJobStart ReviewJobs::startExportJson(const ExportBatchRequest &batch)
    {
        Impl::Job job;
        job.kind = ReviewJobKind::Export;
        job.startedMessage = batch.request.sourcePath;
        job.busyMessage = "An export is already active";
        job.cancelOverridesOutcome = false;
        Impl *impl = impl_.get();
        job.onBegin = [impl](std::uint64_t id) { impl->setStatus(ReviewJobLane::Export, exportValidatingStatusJson(id)); };
        job.onFailure = [impl](std::uint64_t id, const std::string &error) {
            impl->setStatus(ReviewJobLane::Export, exportStartFailedStatusJson(id, error));
        };
        job.body = [impl, request = batch.request, sources = batch.sources](std::uint64_t id, const Impl::CancelFlag &flag) {
            const auto outcome = runExportBatch(
                request, sources, flag,
                [&](std::size_t index, const recording::HdfExportProgress &progress, const std::vector<ExportFileOutcome> &done) {
                    impl->setStatus(ReviewJobLane::Export, exportProgressStatusJson(id, sources, index, progress, done));
                    impl->progress(id, ReviewJobKind::Export, progress.completed, progress.total);
                });
            const auto state = outcome.cancelled ? ReviewJobState::Cancelled
                               : outcome.failed  ? ReviewJobState::Failed
                                                 : ReviewJobState::Completed;
            impl->setStatus(ReviewJobLane::Export, exportTerminalStatusJson(id, outcome));
            return Outcome{state, state == ReviewJobState::Completed ? outcome.last.finalPath : outcome.error};
        };
        const auto r = impl_->start(std::move(job));
        return ReviewJobStart{r.id, r.started, r.error};
    }

    ReviewJobStart ReviewJobs::startReanalysisJson(const ReanalysisRequest &request, services::ProcessingService &kernel)
    {
        Impl::Job job;
        job.kind = ReviewJobKind::Reanalysis;
        job.startedMessage = request.sourcePath;
        job.busyMessage = "Reanalysis already active";
        job.cancelOverridesOutcome = false;
        Impl *impl = impl_.get();
        job.onBegin = [impl](std::uint64_t id) { impl->setStatus(ReviewJobLane::Processing, reanalysisLoadingStatusJson(id)); };
        job.onFailure = [impl](std::uint64_t id, const std::string &error) {
            impl->setStatus(ReviewJobLane::Processing, reanalysisStartFailedStatusJson(id, error));
        };
        job.body = [impl, request, kernel = &kernel](std::uint64_t id, const Impl::CancelFlag &flag) {
            const auto outcome = runReanalysis(request, *kernel, flag, [&](std::uint64_t done, std::uint64_t total) {
                impl->setStatus(ReviewJobLane::Processing, reanalysisProgressStatusJson(id, done, total));
                impl->progress(id, ReviewJobKind::Reanalysis, done, total);
            });
            impl->setStatus(ReviewJobLane::Processing, reanalysisTerminalStatusJson(id, request, outcome));
            return Outcome{outcome.published   ? ReviewJobState::Completed
                           : outcome.cancelled ? ReviewJobState::Cancelled
                                               : ReviewJobState::Failed,
                           outcome.error};
        };
        const auto r = impl_->start(std::move(job));
        return ReviewJobStart{r.id, r.started, r.error};
    }

    bool ReviewJobsTestAccess::waitIdle(const ReviewJobs &jobs, std::chrono::milliseconds timeout)
    {
        auto &impl = *jobs.impl_;
        std::unique_lock lock(impl.mutex);
        return impl.laneChanged.wait_for(lock, timeout, [&] {
            for (const auto &l : impl.lanes)
                if (l.busy) return false;
            return true;
        });
    }

    bool ReviewJobsTestAccess::waitIdle(const ReviewJobs &jobs, ReviewJobLane lane, std::chrono::milliseconds timeout)
    {
        auto &impl = *jobs.impl_;
        std::unique_lock lock(impl.mutex);
        return impl.laneChanged.wait_for(lock, timeout, [&] { return !impl.lanes[laneIndex(lane)].busy; });
    }

    std::uint64_t ReviewJobsTestAccess::start(ReviewJobs &jobs, ReviewJobKind kind, Body body, std::string *error,
                                              bool cancelOverridesOutcome, const std::string &busyMessage)
    {
        ReviewJobs::Impl::Job job;
        job.kind = kind;
        if (!busyMessage.empty()) job.busyMessage = busyMessage;
        job.cancelOverridesOutcome = cancelOverridesOutcome;
        ReviewJobs::Impl *impl = jobs.impl_.get();
        job.body = [impl, kind, body = std::move(body)](std::uint64_t id, const ReviewJobs::Impl::CancelFlag &flag) {
            const auto result = body(id, flag, [&](std::uint64_t done, std::uint64_t total) {
                impl->progress(id, kind, done, total);
            });
            return Outcome{result.state, result.message};
        };
        const auto r = impl->start(std::move(job));
        if (r.failure) std::rethrow_exception(r.failure);
        if (r.id == 0 && error) *error = r.error;
        return r.id;
    }

    std::string ReviewJobs::computedCoreJson() const
    {
        const std::string current = impl_->session.filePath();
        std::scoped_lock lock(impl_->mutex);
        return !current.empty() && impl_->computedCorePath == current ? impl_->computedCoreJson : std::string{};
    }

    DensityResult ReviewJobs::density() const
    {
        const std::string current = impl_->session.filePath();
        std::scoped_lock lock(impl_->mutex);
        if (current.empty() || impl_->density.sourcePath != current) return DensityResult{};
        return impl_->density;
    }

    std::uint64_t ReviewJobs::startExportMetrics(const std::string &outputPath, std::string *error)
    {
        const std::string source = impl_->session.filePath();
        if (source.empty())
        {
            if (error) *error = "No review file open";
            return 0;
        }
        if (outputPath.empty())
        {
            if (error) *error = "Export output path is empty";
            return 0;
        }
        const double factor = impl_->session.pixelToMicron();
        return impl_->startOwn(ReviewJobKind::ExportMetrics, [impl = impl_.get(), source, outputPath, factor](std::uint64_t id, auto flag) {
            recording::HdfExportRequest request;
            request.sourcePath = source;
            request.outputRoot = fs::path(outputPath).parent_path().string();
            request.format = recording::HdfExportFormat::MetricsCsv;
            request.conversionFactor = factor;
            request.explicitDestination = outputPath;
            return impl->runExport(id, ReviewJobKind::ExportMetrics, request, flag);
        }, error);
    }

    std::uint64_t ReviewJobs::startExportAll(const ExportAllRequest &req, std::string *error)
    {
        const std::string source = impl_->session.filePath();
        if (source.empty())
        {
            if (error) *error = "No review file open";
            return 0;
        }
        if (req.outputRoot.empty())
        {
            if (error) *error = "Export root is empty";
            return 0;
        }
        const double factor = impl_->session.pixelToMicron();
        return impl_->startOwn(ReviewJobKind::ExportAll, [impl = impl_.get(), source, req, factor](std::uint64_t id, auto flag) {
            recording::HdfExportRequest request;
            request.sourcePath = source;
            request.outputRoot = req.outputRoot;
            request.format = recording::HdfExportFormat::All;
            request.conversionFactor = factor;
            request.series = toServiceRange(req.series);
            for (const auto &chart : req.charts)
            {
                if (chart.name.empty() || chart.encoded.empty()) continue;
                cv::Mat image = cv::imdecode(chart.encoded, cv::IMREAD_COLOR);
                if (!image.empty()) request.supplementalImages[chart.name] = image;
            }
            return impl->runExport(id, ReviewJobKind::ExportAll, request, flag);
        }, error);
    }

    std::uint64_t ReviewJobs::startBatchExport(const BatchExportRequest &req, std::string *error)
    {
        if (req.sources.empty())
        {
            if (error) *error = "No source files";
            return 0;
        }
        if (req.outputRoot.empty())
        {
            if (error) *error = "Export root is empty";
            return 0;
        }
        // Each source uses its own recorded factor (TD-17); files without one
        // use the host fallback, as when they are opened.
        const double fallback = impl_->session.fallbackPixelToMicron();
        return impl_->startOwn(ReviewJobKind::BatchExport, [impl = impl_.get(), req, fallback](std::uint64_t id, auto flag) {
            recording::HdfExportRequest base;
            base.outputRoot = req.outputRoot;
            base.conversionFactor = fallback;
            base.series = toServiceRange(req.series);
            ExportRunOptions options;
            options.polling = ExportCancelPolling::AtProgress;
            options.prepare = [&](std::size_t i, recording::HdfExportRequest &request, std::string &skipped) {
                const std::string &source = req.sources[i];
                impl->progress(id, ReviewJobKind::BatchExport, i, req.sources.size(), source);
                bool recordingFile = false;
                {
                    services::Hdf5Service probe;
                    if (probe.loadFile(source))
                    {
                        recordingFile = probe.isRecordingFile();
                        if (const double f = ReviewSession::recordedPixelToMicron(probe); f > 0.0) request.conversionFactor = f;
                        probe.closeFile();
                    }
                }
                if (req.metricsOnly && recordingFile)
                {
                    // As the Qt tab: recording files are refused, not attempted.
                    skipped = "recording files do not contain metrics";
                    return false;
                }
                if (req.metricsOnly)
                {
                    request.format = recording::HdfExportFormat::MetricsCsv;
                    request.explicitDestination =
                        recording::HdfExportService::nextAvailableName(
                            req.outputRoot, recording::HdfExportService::sourceBaseName(source) + "_metrics.csv",
                            recording::HdfExportService::sourceBaseName(source) + "_metrics_", ".csv");
                }
                else
                {
                    request.format = recordingFile ? recording::HdfExportFormat::Images : recording::HdfExportFormat::All;
                }
                return true;
            };
            const auto batch = runExportBatch(
                base, req.sources, flag,
                [&](std::size_t, const recording::HdfExportProgress &p, const std::vector<ExportFileOutcome> &) {
                    impl->exportProgress(id, ReviewJobKind::BatchExport, p);
                },
                options);
            const std::size_t exported = batch.completedCount();
            if (batch.cancelled) return Outcome{ReviewJobState::Cancelled, "cancelled after " + std::to_string(exported) + " file(s)"};
            std::vector<std::string> failures;
            for (const auto &file : batch.files)
            {
                if (file.attempted && file.result.status == recording::HdfExportStatus::Completed) continue;
                failures.push_back(fs::path(file.sourcePath).filename().string() + ": " +
                                   (file.attempted ? Impl::exportOutcome(file.result).message : file.result.error));
            }
            std::ostringstream summary;
            summary << "exported " << exported << " of " << req.sources.size() << " file(s)";
            if (!failures.empty())
            {
                summary << "; failed: ";
                for (std::size_t i = 0; i < failures.size(); ++i) summary << (i ? "; " : "") << failures[i];
            }
            return Outcome{ReviewJobState::Completed, summary.str()};
        }, error);
    }

    bool ReviewJobs::validChartName(const std::string &name)
    {
        if (name.empty() || name.size() > 128 || name.front() == '.') return false;
        if (name.find_first_of("/\\:") != std::string::npos) return false;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto endsWith = [&](const std::string &suffix) {
            return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return endsWith(".tiff") || endsWith(".tif") || endsWith(".png");
    }

    std::uint64_t ReviewJobs::startExportCharts(const ExportChartsRequest &req, std::string *error)
    {
        if (req.outputDir.empty())
        {
            if (error) *error = "Export directory is empty";
            return 0;
        }
        if (req.charts.empty())
        {
            if (error) *error = "No charts to export";
            return 0;
        }
        for (const auto &chart : req.charts)
        {
            if (!validChartName(chart.name))
            {
                if (error) *error = "Invalid chart file name: " + chart.name;
                return 0;
            }
        }
        return impl_->startOwn(ReviewJobKind::ExportCharts, [impl = impl_.get(), req](std::uint64_t id, auto flag) {
            const fs::path dir(req.outputDir);
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) return Outcome{ReviewJobState::Failed, "not a directory: " + req.outputDir};
            std::vector<fs::path> temps;
            const auto discard = [&]() {
                for (const auto &t : temps) fs::remove(t, ec);
            };
            const std::uint64_t total = req.charts.size();
            for (std::size_t i = 0; i < req.charts.size(); ++i)
            {
                const auto &chart = req.charts[i];
                if (flag->load())
                {
                    discard();
                    return Outcome{ReviewJobState::Cancelled, "cancelled; partial charts discarded"};
                }
                impl->progress(id, ReviewJobKind::ExportCharts, i, total, chart.name);
                const cv::Mat image = cv::imdecode(chart.encoded, cv::IMREAD_COLOR);
                if (image.empty())
                {
                    discard();
                    return Outcome{ReviewJobState::Failed, "could not decode chart " + chart.name};
                }
                // Same extension so OpenCV picks the same encoder.
                const fs::path final = dir / chart.name;
                const fs::path temp = dir / ("." + final.stem().string() + ".partial" + final.extension().string());
                temps.push_back(temp);
                if (!cv::imwrite(temp.string(), image))
                {
                    discard();
                    return Outcome{ReviewJobState::Failed, "could not write " + final.string()};
                }
            }
            for (std::size_t i = 0; i < req.charts.size(); ++i)
            {
                fs::rename(temps[i], dir / req.charts[i].name, ec);
                if (ec)
                {
                    discard();
                    return Outcome{ReviewJobState::Failed, "could not publish " + req.charts[i].name + ": " + ec.message()};
                }
            }
            impl->progress(id, ReviewJobKind::ExportCharts, total, total);
            return Outcome{ReviewJobState::Completed,
                           "exported " + std::to_string(total) + " chart(s) to " + req.outputDir};
        }, error);
    }

    std::uint64_t ReviewJobs::startRegenerateMasks(const RegenerateMasksRequest &req, std::string *error)
    {
        if (!impl_->processing)
        {
            if (error) *error = "No processing service in this build";
            return 0;
        }
        if (req.outputPath.empty())
        {
            if (error) *error = "Output path is empty";
            return 0;
        }
        const bool needsFile = req.source == RegenerateSource::CurrentValid || req.source == RegenerateSource::CurrentInvalid ||
                               req.source == RegenerateSource::WholeFile;
        const std::string source = impl_->session.filePath();
        if (needsFile && source.empty())
        {
            if (error) *error = "No review file open";
            return 0;
        }
        if (!needsFile && req.sourcePath.empty())
        {
            if (error) *error = "Source path is empty";
            return 0;
        }
        // D6 (#512 S8): the reanalysis engine writes a partial file and
        // publishes without replacing, so an existing output is refused here
        // (and a file created while the job runs fails the job, untouched).
        std::error_code exists;
        if (fs::exists(fs::path(req.outputPath), exists))
        {
            if (error) *error = kReanalysisOutputExists;
            return 0;
        }
        const bool recordingFile = impl_->session.isRecordingFile();
        return impl_->startOwn(ReviewJobKind::RegenerateMasks, [impl = impl_.get(), req, source, recordingFile](std::uint64_t id, auto flag) {
            // YOFO Review's regenerate on the shared reanalysis engine
            // (backend/review/Reanalysis.h) with its D6 options: timestamps
            // rebased to the first selected frame, a median-of-32 synthetic
            // background, the whole file as valid rows then invalid rows,
            // colour HDF frames converted to gray.
            ReanalysisRequest r;
            r.outputPath = req.outputPath;
            r.timestamps = ReanalysisTimestamps::Rebase;
            r.syntheticBackgroundKind = ReanalysisSyntheticBackground::MedianOf32;
            r.frameOrder = ReanalysisFrameOrder::Datasets;
            // No budget: the job loads the whole selection, as before.
            r.maxFrames = std::numeric_limits<std::uint64_t>::max();
            r.maxInputBytes = std::numeric_limits<std::uint64_t>::max();
            r.syntheticBackground = req.synthesizeBackground;
            r.config = req.config;
            r.roi = req.roi;
            if (req.source == RegenerateSource::Avi || req.source == RegenerateSource::Folder)
            {
                r.sourceKind = req.source == RegenerateSource::Avi ? "avi" : "folder";
                r.sourcePath = req.sourcePath;
                r.overrideRoi = true;
            }
            else
            {
                r.sourceKind = "hdf";
                r.sourcePath = source;
                r.convertColorToGray = true; // as the pre-S8 reader (loadFromHdf5) did
                if (req.source == RegenerateSource::WholeFile) r.dataset = "all";
                else if (recordingFile) r.dataset = "/recorded_frames/images";
                else if (req.source == RegenerateSource::CurrentInvalid) r.dataset = "/invalid_frames/images";
                else r.dataset = "/valid_frames/images";
                // The recorded config (the engine reads the recorded ROI and
                // background itself) and a range clamped to the set.
                services::Hdf5Service reader;
                if (!reader.loadFile(source)) return Outcome{ReviewJobState::Failed, "cannot open " + source};
                if (req.useRecordedConfig)
                {
                    services::ProcessingConfig recorded;
                    if (reader.readRecordedProcessingConfig(recorded)) r.config = recorded;
                }
                if (req.source != RegenerateSource::WholeFile)
                {
                    std::size_t available = 0;
                    int h = 0, w = 0, c = 0;
                    if (!reader.getDatasetInfo(r.dataset, available, h, w, c)) available = 0;
                    r.start = req.startIndex;
                    r.count = req.count;
                    if (req.count > 0 && req.startIndex < available)
                        r.count = std::min<std::uint64_t>(req.count, available - req.startIndex);
                }
                reader.closeFile();
                // The recorded ROI applies to experiment files only; recording
                // files and "not recorded" use the request's.
                r.overrideRoi = !req.useRecordedConfig || recordingFile;
                r.clearBackground = !req.useRecordedConfig;
            }
            const auto onProgress = [&](std::uint64_t done, std::uint64_t total) {
                if ((done % 25) == 0 || done == total)
                    impl->progress(id, ReviewJobKind::RegenerateMasks, done, total, "processing");
                if (done == total) impl->progress(id, ReviewJobKind::RegenerateMasks, total, total, "saving");
            };
            const auto outcome = runReanalysis(r, *impl->processing, flag, onProgress);
            if (!outcome.warning.empty()) spdlog::warn("Regenerate masks {}: {}", req.outputPath, outcome.warning);
            const std::string kept =
                outcome.retainedPartialPath.empty() ? "" : "; partial output kept at " + outcome.retainedPartialPath;
            if (outcome.published)
            {
                std::ostringstream msg;
                msg << req.outputPath << " (" << outcome.produced << " images: " << outcome.valid << " valid, "
                    << (outcome.produced - outcome.valid) << " invalid)";
                return Outcome{ReviewJobState::Completed, msg.str()};
            }
            if (outcome.cancelled)
                return Outcome{ReviewJobState::Cancelled, kept.empty() ? "nothing written" : kept.substr(2)};
            return Outcome{ReviewJobState::Failed, outcome.error + kept};
        }, error);
    }

    std::uint64_t ReviewJobs::startComputeCore(double coreFraction, std::string *error)
    {
        if (!impl_->session.isOpen() || impl_->session.isRecordingFile())
        {
            if (error) *error = "No experiment file open";
            return 0;
        }
        const double fraction = std::clamp(std::isfinite(coreFraction) ? coreFraction : 0.9, 0.05, 1.0);
        return impl_->startOwn(ReviewJobKind::ComputeCore, [impl = impl_.get(), fraction](std::uint64_t, auto flag) {
            const std::string path = impl->session.filePath();
            const ScatterData sc = impl->session.scatter();
            if (impl->session.filePath() != path) return Outcome{ReviewJobState::Failed, "the file changed while reading"};
            std::vector<backend::monitoring::DensityPoint> points;
            points.reserve(sc.areaUm2.size());
            for (std::size_t i = 0; i < sc.areaUm2.size(); ++i) points.push_back({sc.areaUm2[i], sc.deformability[i]});
            if (points.size() < 3) return Outcome{ReviewJobState::Failed, "fewer than three valid cells"};
            // cellCount stays the in-core count computeFullRunCoreRecord sets (the
            // Qt tab's meaning); populationCount carries the run's size.
            auto record = backend::monitoring::computeFullRunCoreRecord(points, fraction, sc.pixelToMicron);
            record.computedAtNs = nowNs();
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled"};
            const std::string json = backend::monitoring::toJson(record);
            {
                std::scoped_lock lock(impl->mutex);
                impl->computedCoreJson = json;
                impl->computedCorePath = path;
            }
            return Outcome{ReviewJobState::Completed, "Core " + std::to_string(std::lround(record.coreFraction * 100.0)) + "%: " +
                                                              std::to_string(record.cellCount) + " of " + std::to_string(record.populationCount) +
                                                              " cells, " + std::to_string(record.contours.size()) + " loop(s)"};
        }, error);
    }

    int ReviewJobs::levelForDensity(double density, int levels)
    {
        if (levels < 1) return 0;
        if (!std::isfinite(density)) return 0;
        const int level = static_cast<int>(std::clamp(density, 0.0, 1.0) * levels);
        return std::min(level, levels - 1);
    }

    std::vector<double> ReviewJobs::densityAtPoints(const std::vector<backend::monitoring::DensityPoint> &points,
                                                    double bandwidthFactor, std::size_t gridAbove, std::uint32_t seed)
    {
        namespace mon = backend::monitoring;
        if (points.size() <= gridAbove) return mon::normalizedDensity(points, bandwidthFactor);

        // Fixed-seed subsample → bandwidth + grid → bilinear interpolation,
        // normalised by the grid maximum (the scatter plan's > 5000 rule).
        std::vector<mon::DensityPoint> sample;
        sample.reserve(gridAbove);
        std::vector<std::size_t> order(points.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::mt19937 rng(seed);
        std::shuffle(order.begin(), order.end(), rng);
        for (std::size_t i = 0; i < gridAbove; ++i)
            if (mon::isFinitePoint(points[order[i]])) sample.push_back(points[order[i]]);
        const mon::DensityBandwidth bw = mon::silvermanBandwidth(sample, bandwidthFactor);
        double x0 = points.front().x, x1 = x0, y0 = points.front().y, y1 = y0;
        for (const auto &p : points)
        {
            if (!mon::isFinitePoint(p)) continue;
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
        const double padX = x1 > x0 ? 0.1 * (x1 - x0) : 1.0;
        const double padY = y1 > y0 ? 0.1 * (y1 - y0) : 0.01;
        x0 -= padX; x1 += padX; y0 -= padY; y1 += padY;
        const double rawMax = mon::rawKdeMaximum(sample, bw);
        const mon::DensityGrid grid = mon::gaussianKdeGrid(sample, bw, rawMax, x0, x1, y0, y1, 256, 128);
        std::vector<double> out(points.size(), 0.0);
        if (grid.nx < 2 || grid.ny < 2) return out;
        double gridMax = 0.0;
        for (double v : grid.value) gridMax = std::max(gridMax, v);
        if (!(gridMax > 0.0)) return out;
        const double dx = (grid.x1 - grid.x0) / (grid.nx - 1);
        const double dy = (grid.y1 - grid.y0) / (grid.ny - 1);
        for (std::size_t k = 0; k < points.size(); ++k)
        {
            const auto &p = points[k];
            if (!mon::isFinitePoint(p)) continue;
            const double fx = std::clamp((p.x - grid.x0) / dx, 0.0, static_cast<double>(grid.nx - 1));
            const double fy = std::clamp((p.y - grid.y0) / dy, 0.0, static_cast<double>(grid.ny - 1));
            const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
            const int i1 = std::min(i0 + 1, grid.nx - 1), j1 = std::min(j0 + 1, grid.ny - 1);
            const double tx = fx - i0, ty = fy - j0;
            const double v = (1 - tx) * (1 - ty) * grid.at(i0, j0) + tx * (1 - ty) * grid.at(i1, j0) +
                             (1 - tx) * ty * grid.at(i0, j1) + tx * ty * grid.at(i1, j1);
            out[k] = std::clamp(v / gridMax, 0.0, 1.0);
        }
        return out;
    }

    std::uint64_t ReviewJobs::startDensity(const DensityRequest &req, std::string *error)
    {
        if (!impl_->session.isOpen() || impl_->session.isRecordingFile())
        {
            if (error) *error = "No experiment file open";
            return 0;
        }
        DensityRequest r = req;
        r.bandwidthFactor = std::clamp(std::isfinite(r.bandwidthFactor) ? r.bandwidthFactor : 1.0, 0.2, 5.0);
        r.coreFraction = std::clamp(std::isfinite(r.coreFraction) ? r.coreFraction : 0.9, 0.05, 1.0);
        r.levels = std::clamp(r.levels, 1, 64);
        return impl_->startOwn(ReviewJobKind::Density, [impl = impl_.get(), r](std::uint64_t, auto flag) {
            {
                std::scoped_lock lock(impl->mutex);
                impl->density = DensityResult{}; // not ready while this one runs
            }
            const std::string path = impl->session.filePath();
            const ScatterData sc = impl->session.scatter();
            const ReviewMetadata meta = impl->session.metadata();
            if (impl->session.filePath() != path) return Outcome{ReviewJobState::Failed, "the file changed while reading"};
            std::vector<backend::monitoring::DensityPoint> points;
            points.reserve(sc.areaUm2.size());
            for (std::size_t i = 0; i < sc.areaUm2.size(); ++i) points.push_back({sc.areaUm2[i], sc.deformability[i]});
            DensityResult result;
            result.levelCount = r.levels;
            result.bandwidthFactor = r.bandwidthFactor;
            result.coreFraction = r.coreFraction;
            const std::vector<double> density =
                densityAtPoints(points, r.bandwidthFactor, backend::monitoring::kFullRunMaxPoints,
                                backend::monitoring::kFullRunSampleSeed);
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled"};
            result.levels.reserve(density.size());
            for (double d : density) result.levels.push_back(static_cast<std::uint8_t>(levelForDensity(d, r.levels)));
            if (r.wantCoreRecord && meta.kdeAnalysisJson.empty() && points.size() >= 3)
            {
                auto record = backend::monitoring::computeFullRunCoreRecord(points, r.coreFraction, sc.pixelToMicron);
                record.computedAtNs = nowNs();
                result.computedRecordJson = backend::monitoring::toJson(record);
            }
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled"};
            result.ready = true;
            result.sourcePath = path;
            {
                std::scoped_lock lock(impl->mutex);
                impl->density = result;
                if (!result.computedRecordJson.empty())
                {
                    impl->computedCoreJson = result.computedRecordJson;
                    impl->computedCorePath = path;
                }
            }
            return Outcome{ReviewJobState::Completed, "density estimated for " + std::to_string(points.size()) + " cells"};
        }, error);
    }

} // namespace backend::review
