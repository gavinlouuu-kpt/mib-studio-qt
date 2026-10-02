#include "backend/review/ReviewJobs.h"

#include "backend/processing/BatchMaskSources.h"
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
#include <chrono>
#include <cmath>
#include <filesystem>
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
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
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

    struct ReviewJobs::Impl
    {
        ReviewSession &session;
        services::ProcessingService *processing;
        ReviewJobSink sink;

        mutable std::mutex mutex;
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<std::uint64_t> nextId{1};
        std::uint64_t currentId{0};
        ReviewJobKind currentKind{ReviewJobKind::ExportMetrics};
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        std::string computedCoreJson;
        std::string computedCorePath; // file computedCoreJson belongs to
        DensityResult density;

        Impl(ReviewSession &s, services::ProcessingService *p, ReviewJobSink k)
            : session(s), processing(p), sink(std::move(k)) {}

        void emit(const ReviewJobEvent &e)
        {
            if (sink)
            {
                try { sink(e); } catch (...) {}
            }
        }

        // Start `job` on the worker; refuses while one runs (single-flight).
        template <typename Job>
        std::uint64_t start(ReviewJobKind kind, Job job, std::string *error)
        {
            std::scoped_lock lock(mutex);
            if (running.load())
            {
                if (error) *error = "Another review job is still running";
                return 0;
            }
            if (worker.joinable()) worker.join();
            const std::uint64_t id = nextId.fetch_add(1);
            auto flag = std::make_shared<std::atomic<bool>>(false);
            cancelFlag = flag;
            currentId = id;
            currentKind = kind;
            running.store(true);
            emit(ReviewJobEvent{id, kind, ReviewJobState::Started, 0, 0, {}});
            worker = std::thread([this, id, kind, flag, job = std::move(job)]() mutable {
                Outcome outcome;
                try
                {
                    outcome = job(id, flag);
                }
                catch (const std::exception &e)
                {
                    outcome = Outcome{ReviewJobState::Failed, std::string("job failed: ") + e.what()};
                }
                catch (...)
                {
                    outcome = Outcome{ReviewJobState::Failed, "job failed: unknown error"};
                }
                if (flag->load() && outcome.state != ReviewJobState::Completed)
                {
                    outcome.state = ReviewJobState::Cancelled;
                }
                running.store(false);
                emit(ReviewJobEvent{id, kind, outcome.state, 0, 0, outcome.message});
            });
            return id;
        }

        void progress(std::uint64_t id, ReviewJobKind kind, std::uint64_t done, std::uint64_t total,
                      const std::string &message = {})
        {
            emit(ReviewJobEvent{id, kind, ReviewJobState::Progress, done, total, message});
        }

        // One HdfExportService run with cancel + progress plumbing.
        Outcome runExport(std::uint64_t id, ReviewJobKind kind, const recording::HdfExportRequest &request,
                          const std::shared_ptr<std::atomic<bool>> &flag, recording::HdfExportResult *resultOut = nullptr)
        {
            recording::HdfExportService service;
            recording::HdfExportCancelToken token;
            const auto result = service.run(request, token, [&](const recording::HdfExportProgress &p) {
                if (flag->load()) token.cancel();
                progress(id, kind, p.completed, p.total,
                         std::string(recording::toString(p.phase)) + (p.currentOutput.empty() ? "" : ": " + p.currentOutput));
            });
            if (resultOut) *resultOut = result;
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

    ReviewJobs::ReviewJobs(ReviewSession &session, services::ProcessingService *processing, ReviewJobSink sink)
        : impl_(std::make_unique<Impl>(session, processing, std::move(sink)))
    {
    }

    ReviewJobs::~ReviewJobs() { shutdown(); }

    void ReviewJobs::shutdown()
    {
        std::thread worker;
        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->cancelFlag) impl_->cancelFlag->store(true);
            worker = std::move(impl_->worker);
        }
        if (worker.joinable()) worker.join();
    }

    bool ReviewJobs::cancel(std::uint64_t operationId)
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->running.load() || impl_->currentId != operationId || !impl_->cancelFlag) return false;
        impl_->cancelFlag->store(true);
        return true;
    }

    bool ReviewJobs::busy() const { return impl_->running.load(); }

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
        return impl_->start(ReviewJobKind::ExportMetrics, [this, source, outputPath, factor](std::uint64_t id, auto flag) {
            recording::HdfExportRequest request;
            request.sourcePath = source;
            request.outputRoot = fs::path(outputPath).parent_path().string();
            request.format = recording::HdfExportFormat::MetricsCsv;
            request.conversionFactor = factor;
            request.explicitDestination = outputPath;
            return impl_->runExport(id, ReviewJobKind::ExportMetrics, request, flag);
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
        return impl_->start(ReviewJobKind::ExportAll, [this, source, req, factor](std::uint64_t id, auto flag) {
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
            return impl_->runExport(id, ReviewJobKind::ExportAll, request, flag);
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
        const double factor = impl_->session.pixelToMicron();
        return impl_->start(ReviewJobKind::BatchExport, [this, req, factor](std::uint64_t id, auto flag) {
            std::uint64_t exported = 0;
            std::vector<std::string> failures;
            for (std::size_t i = 0; i < req.sources.size(); ++i)
            {
                if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled after " + std::to_string(exported) + " file(s)"};
                const std::string &source = req.sources[i];
                impl_->progress(id, ReviewJobKind::BatchExport, i, req.sources.size(), source);
                recording::HdfExportRequest request;
                request.sourcePath = source;
                request.outputRoot = req.outputRoot;
                request.conversionFactor = factor;
                request.series = toServiceRange(req.series);
                bool recordingFile = false;
                {
                    services::Hdf5Service probe;
                    if (probe.loadFile(source))
                    {
                        recordingFile = probe.isRecordingFile();
                        probe.closeFile();
                    }
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
                recording::HdfExportResult result;
                const Outcome one = impl_->runExport(id, ReviewJobKind::BatchExport, request, flag, &result);
                if (one.state == ReviewJobState::Completed) ++exported;
                else if (one.state == ReviewJobState::Cancelled) return Outcome{ReviewJobState::Cancelled, "cancelled after " + std::to_string(exported) + " file(s)"};
                else failures.push_back(fs::path(source).filename().string() + ": " + one.message);
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
        return impl_->start(ReviewJobKind::ExportCharts, [this, req](std::uint64_t id, auto flag) {
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
                impl_->progress(id, ReviewJobKind::ExportCharts, i, total, chart.name);
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
            impl_->progress(id, ReviewJobKind::ExportCharts, total, total);
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
        const bool recordingFile = impl_->session.isRecordingFile();
        return impl_->start(ReviewJobKind::RegenerateMasks, [this, req, source, recordingFile](std::uint64_t id, auto flag) {
            using namespace services::batch_masks;
            std::vector<cv::Mat> images;
            std::vector<std::string> names;
            std::vector<std::string> errors;
            std::vector<std::uint64_t> sourceIndex;
            std::vector<std::uint64_t> sourceTimestamp;
            services::ProcessingConfig config = req.config;
            services::ProcessingService::Roi roi = req.roi;
            cv::Mat background;
            bool fromHdf5 = false;

            if (req.source == RegenerateSource::Avi || req.source == RegenerateSource::Folder)
            {
                const bool ok = req.source == RegenerateSource::Avi ? loadFromAvi(req.sourcePath, images, names, errors)
                                                                    : loadFromFolder(req.sourcePath, images, names, errors);
                if (!ok || images.empty())
                    return Outcome{ReviewJobState::Failed, errors.empty() ? "no images loaded from " + req.sourcePath : errors.front()};
            }
            else
            {
                fromHdf5 = true;
                services::Hdf5Service reader;
                if (!reader.loadFile(source)) return Outcome{ReviewJobState::Failed, "cannot open " + source};
                if (req.useRecordedConfig)
                {
                    services::ProcessingConfig recorded;
                    if (reader.readRecordedProcessingConfig(recorded)) config = recorded;
                    std::uint64_t t0 = 0, t1 = 0;
                    std::size_t nv = 0, ni = 0;
                    if (!recordingFile) reader.readExperimentInfo(t0, t1, nv, ni, &roi);
                    reader.readBackgroundImage(background);
                }
                struct Part { const char *path; bool valid; };
                std::vector<Part> parts;
                if (recordingFile) parts.push_back({"/recorded_frames/images", true});
                else if (req.source == RegenerateSource::CurrentInvalid) parts.push_back({"/invalid_frames/images", false});
                else if (req.source == RegenerateSource::CurrentValid) parts.push_back({"/valid_frames/images", true});
                else { parts.push_back({"/valid_frames/images", true}); parts.push_back({"/invalid_frames/images", false}); }
                const bool ranged = req.source != RegenerateSource::WholeFile;
                for (const auto &part : parts)
                {
                    std::size_t count = 0;
                    int h = 0, w = 0, c = 0;
                    if (!reader.getDatasetInfo(part.path, count, h, w, c) || count == 0) continue;
                    std::size_t start = ranged ? static_cast<std::size_t>(std::min<std::uint64_t>(req.startIndex, count)) : 0;
                    std::size_t n = ranged && req.count > 0 ? static_cast<std::size_t>(std::min<std::uint64_t>(req.count, count - start)) : count - start;
                    if (n == 0) continue;
                    std::vector<cv::Mat> gray;
                    if (!loadFromHdf5(reader, part.path, start, n, gray)) return Outcome{ReviewJobState::Failed, std::string("failed to read ") + part.path};
                    for (std::size_t k = 0; k < gray.size(); ++k)
                    {
                        services::ProcessedFrame meta;
                        const bool haveMeta = impl_->session.frameMeta(part.valid, start + k, meta);
                        sourceIndex.push_back(haveMeta ? meta.index : start + k);
                        sourceTimestamp.push_back(haveMeta ? meta.timestampNs : 0);
                    }
                    images.insert(images.end(), gray.begin(), gray.end());
                    if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled while loading"};
                }
                reader.closeFile();
                if (images.empty()) return Outcome{ReviewJobState::Failed, "no images in the selected range"};
            }
            if (background.empty() && req.synthesizeBackground)
            {
                // Median of the first ≤ 32 frames: the least-changing pixels.
                const std::size_t n = std::min<std::size_t>(images.size(), 32);
                std::vector<cv::Mat> stack(images.begin(), images.begin() + static_cast<std::ptrdiff_t>(n));
                background = cv::Mat(images.front().size(), CV_8UC1);
                std::vector<std::uint8_t> column(n);
                for (int y = 0; y < background.rows; ++y)
                    for (int x = 0; x < background.cols; ++x)
                    {
                        for (std::size_t k = 0; k < n; ++k) column[k] = stack[k].at<std::uint8_t>(y, x);
                        std::nth_element(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(n / 2), column.end());
                        background.at<std::uint8_t>(y, x) = column[n / 2];
                    }
            }
            impl_->progress(id, ReviewJobKind::RegenerateMasks, 0, images.size(), "processing");
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled before processing"};
            backend::processing::ProcessingCoreIdentity identity;
            const std::size_t total = images.size();
            auto results = impl_->processing->processBatch(
                images, config, background, roi,
                [&](const services::ProcessingService::BatchProgress &p) {
                    if ((p.done % 25) == 0 || p.done == p.total)
                        impl_->progress(id, ReviewJobKind::RegenerateMasks, p.done, total, "processing");
                },
                &identity);
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled after processing; nothing written"};
            if (fromHdf5)
            {
                const std::uint64_t base = sourceTimestamp.empty() ? 0 : sourceTimestamp.front();
                for (std::size_t k = 0; k < results.size() && k < sourceIndex.size(); ++k)
                {
                    results[k].index = sourceIndex[k];
                    results[k].timestampNs = sourceTimestamp[k] >= base ? sourceTimestamp[k] - base : 0;
                }
            }
            std::size_t valid = 0;
            for (const auto &f : results) if (f.validation.isValid) ++valid;
            impl_->progress(id, ReviewJobKind::RegenerateMasks, total, total, "saving");
            const bool ok = saveMasksToHdf5(results, req.outputPath, config, roi.x, roi.y, roi.w, roi.h, background,
                                            fromHdf5, &identity);
            if (!ok) return Outcome{ReviewJobState::Failed, "HDF5 write failed: " + req.outputPath};
            std::ostringstream msg;
            msg << req.outputPath << " (" << results.size() << " images: " << valid << " valid, " << (results.size() - valid) << " invalid)";
            return Outcome{ReviewJobState::Completed, msg.str()};
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
        return impl_->start(ReviewJobKind::ComputeCore, [this, fraction](std::uint64_t, auto flag) {
            const std::string path = impl_->session.filePath();
            const ScatterData sc = impl_->session.scatter();
            if (impl_->session.filePath() != path) return Outcome{ReviewJobState::Failed, "the file changed while reading"};
            std::vector<backend::monitoring::DensityPoint> points;
            points.reserve(sc.areaUm2.size());
            for (std::size_t i = 0; i < sc.areaUm2.size(); ++i) points.push_back({sc.areaUm2[i], sc.deformability[i]});
            if (points.size() < 3) return Outcome{ReviewJobState::Failed, "fewer than three valid cells"};
            auto record = backend::monitoring::computeFullRunCoreRecord(points, fraction, sc.pixelToMicron);
            record.cellCount = points.size();
            record.computedAtNs = nowNs();
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled"};
            const std::string json = backend::monitoring::toJson(record);
            {
                std::scoped_lock lock(impl_->mutex);
                impl_->computedCoreJson = json;
                impl_->computedCorePath = path;
            }
            return Outcome{ReviewJobState::Completed, "core contour computed from " + std::to_string(record.cellCount) + " cells"};
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
        return impl_->start(ReviewJobKind::Density, [this, r](std::uint64_t, auto flag) {
            {
                std::scoped_lock lock(impl_->mutex);
                impl_->density = DensityResult{}; // not ready while this one runs
            }
            const std::string path = impl_->session.filePath();
            const ScatterData sc = impl_->session.scatter();
            const ReviewMetadata meta = impl_->session.metadata();
            if (impl_->session.filePath() != path) return Outcome{ReviewJobState::Failed, "the file changed while reading"};
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
                record.cellCount = points.size();
                record.computedAtNs = nowNs();
                result.computedRecordJson = backend::monitoring::toJson(record);
            }
            if (flag->load()) return Outcome{ReviewJobState::Cancelled, "cancelled"};
            result.ready = true;
            result.sourcePath = path;
            {
                std::scoped_lock lock(impl_->mutex);
                impl_->density = result;
                if (!result.computedRecordJson.empty())
                {
                    impl_->computedCoreJson = result.computedRecordJson;
                    impl_->computedCorePath = path;
                }
            }
            return Outcome{ReviewJobState::Completed, "density estimated for " + std::to_string(points.size()) + " cells"};
        }, error);
    }

} // namespace backend::review
