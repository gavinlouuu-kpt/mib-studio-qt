// review_jobs_test (plan 2026-10-01-standalone-review-app, PR 1b)
//
// backend::review::ReviewJobs — the review jobs every shell shares:
//  - export metrics: tracked operation (Started → Completed), output equals
//    a direct HdfExportService run byte-for-byte, factor = recorded (TD-17);
//  - export all: folder under the root with metrics + TIFFs + the chart
//    snapshot the shell handed over; cancel leaves no ".partial-" residue;
//  - batch: continues after a missing file, summary names the failure;
//  - regenerate masks (whole file, recorded config/ROI/background): output
//    opens in a ReviewSession with the image count, recorded indices kept;
//  - compute core: full-run record JSON with the recorded factor;
//  - density: one level per scatter point, no computed record when the
//    file has one, a computed record on a legacy file; grid path agrees
//    with the direct path within one level on a 4000-point population;
//  - single flight: a second start while one runs is refused.
// Watchdog guarded.

#include "backend/review/ReviewJobs.h"
#include "backend/review/ReviewSession.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/processing/KdeCoreRecord.h"
#include "backend/processing/MonitoringDensity.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace backend::review;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace
{
    constexpr int kH = 24, kW = 32;

    cv::Mat pattern(uint64_t index, int offset = 0)
    {
        cv::Mat m(kH, kW, CV_8UC1);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7 + offset) % 200);
        return m;
    }

    cv::Mat ringMask()
    {
        cv::Mat m = cv::Mat::zeros(kH, kW, CV_8UC1);
        cv::circle(m, cv::Point(kW / 2, kH / 2), 9, cv::Scalar(255), -1);
        cv::circle(m, cv::Point(kW / 2, kH / 2), 4, cv::Scalar(0), -1);
        return m;
    }

    ProcessedFrame makeFrame(uint64_t idx, bool valid, double area, double deform)
    {
        ProcessedFrame f;
        f.index = idx;
        f.timestampNs = 5000 + (idx + 1) * 1000ULL;
        f.originalImage = pattern(idx, valid ? 0 : 50);
        f.processedImage = ringMask();
        f.validation.isValid = valid;
        f.validation.objectId = static_cast<int>(idx);
        f.validation.objectCount = 1;
        f.validation.area = area;
        f.validation.deformability = deform;
        f.validation.hasSingleInnerContour = true;
        return f;
    }

    void writeExperiment(const fs::path &path, int validN, bool withRecord)
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> ax(700, 60), ay(0.05, 0.01), bx(1300, 70), by(0.12, 0.015);
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < validN; ++i)
            valid.push_back(i % 3 == 0 ? makeFrame(static_cast<uint64_t>(i * 2), true, bx(rng), by(rng))
                                       : makeFrame(static_cast<uint64_t>(i * 2), true, ax(rng), ay(rng)));
        for (int i = 0; i < 3; ++i) invalid.push_back(makeFrame(static_cast<uint64_t>(i * 2 + 1), false, 50, 0.3));
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path.string()), "open fixture");
        MIB_REQUIRE(hdf5.initializeDatasets(), "init datasets");
        MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append");
        backend::services::ProcessingConfig cfg;
        backend::services::ProcessingService::Roi roi{2, 2, 28, 20};
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi, &valid.front().originalImage), "info");
        MIB_REQUIRE(hdf5.writeRunSnapshotJson("{\"pixel_to_micron\":0.5}", "{}"), "snapshot");
        if (withRecord)
        {
            backend::monitoring::KdeCoreRecord r;
            r.provisional = false;
            r.source = "full-run";
            r.cellCount = static_cast<uint64_t>(validN);
            MIB_REQUIRE(hdf5.writeKdeAnalysisJson(backend::monitoring::toJson(r)), "record");
        }
        hdf5.closeFile();
    }

    // Collects job events; waits for the terminal state of an operation.
    struct Sink
    {
        std::mutex m;
        std::condition_variable cv;
        std::vector<ReviewJobEvent> events;
        void operator()(const ReviewJobEvent &e)
        {
            std::scoped_lock lock(m);
            events.push_back(e);
            cv.notify_all();
        }
        bool terminal(uint64_t id, ReviewJobEvent &out, int timeoutMs = 60000)
        {
            std::unique_lock lock(m);
            return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                for (const auto &e : events)
                    if (e.operationId == id && (e.state == ReviewJobState::Completed || e.state == ReviewJobState::Failed ||
                                                e.state == ReviewJobState::Cancelled))
                    {
                        out = e;
                        return true;
                    }
                return false;
            });
        }
        bool started(uint64_t id)
        {
            std::scoped_lock lock(m);
            for (const auto &e : events)
                if (e.operationId == id && e.state == ReviewJobState::Started) return true;
            return false;
        }
    };

    std::string readAll(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool noPartials(const fs::path &root)
    {
        if (!fs::exists(root)) return true;
        for (const auto &e : fs::recursive_directory_iterator(root))
            if (e.path().filename().string().find(".partial-") != std::string::npos) return false;
        return true;
    }
} // namespace

int main()
{
    mib::test::Watchdog wd(240);
    mib::test::TempDir td("review_jobs");
    const fs::path experiment = td.path() / "run.h5";
    const fs::path legacy = td.path() / "legacy.h5";
    writeExperiment(experiment, 30, true);
    writeExperiment(legacy, 12, false);

    ReviewSession session;
    MIB_REQUIRE(session.open(experiment.string()), "open");
    backend::services::ProcessingService processing;
    Sink sink;
    ReviewJobs jobs(session, &processing, std::ref(sink));
    std::string err;
    ReviewJobEvent done;

    // ---- refusals -----------------------------------------------------------
    MIB_EXPECT(jobs.startExportMetrics("", &err) == 0 && !err.empty(), "empty path refused");
    MIB_EXPECT(jobs.startBatchExport(BatchExportRequest{}, &err) == 0, "empty batch refused");
    MIB_EXPECT(!jobs.cancel(12345), "cancel unknown");

    // ---- export metrics = direct service run ---------------------------------
    {
        const fs::path csv = td.path() / "metrics.csv";
        const uint64_t id = jobs.startExportMetrics(csv.string(), &err);
        MIB_REQUIRE(id != 0, "start export: " + err);
        MIB_EXPECT(jobs.busy() || sink.started(id), "started");
        MIB_REQUIRE(sink.terminal(id, done), "export terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "export completed: " + done.message);
        MIB_EXPECT(done.kind == ReviewJobKind::ExportMetrics, "kind");
        MIB_EXPECT(fs::exists(csv), "csv exists");

        backend::recording::HdfExportRequest direct;
        direct.sourcePath = experiment.string();
        direct.outputRoot = td.path().string();
        direct.format = backend::recording::HdfExportFormat::MetricsCsv;
        direct.conversionFactor = 0.5; // recorded factor (TD-17)
        direct.explicitDestination = (td.path() / "direct.csv").string();
        backend::recording::HdfExportService service;
        backend::recording::HdfExportCancelToken token;
        const auto r = service.run(direct, token);
        MIB_REQUIRE(r.status == backend::recording::HdfExportStatus::Completed, "direct run");
        MIB_EXPECT(readAll(csv) == readAll(td.path() / "direct.csv"), "job output equals direct run");
        MIB_EXPECT(readAll(csv).find("Frame Type") != std::string::npos, "csv header");
    }
    wd.mark("export metrics");

    // ---- export all with a chart snapshot -------------------------------------
    {
        ExportAllRequest req;
        req.outputRoot = (td.path() / "all").string();
        cv::Mat chart(20, 30, CV_8UC3, cv::Scalar(10, 20, 30));
        ChartSnapshot snap;
        snap.name = "scatter_plot.tiff";
        std::vector<uchar> png;
        cv::imencode(".png", chart, png);
        snap.encoded.assign(png.begin(), png.end());
        req.charts.push_back(snap);
        const uint64_t id = jobs.startExportAll(req, &err);
        MIB_REQUIRE(id != 0, "start export all: " + err);
        MIB_REQUIRE(sink.terminal(id, done), "export all terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "export all completed: " + done.message);
        const fs::path folder(done.message);
        MIB_EXPECT(fs::is_directory(folder), "output folder " + done.message);
        MIB_EXPECT(fs::exists(folder / "metrics.csv"), "metrics in folder");
        MIB_EXPECT(fs::exists(folder / "scatter_plot.tiff"), "chart written");
        int tiffs = 0;
        for (const auto &e : fs::recursive_directory_iterator(folder))
            if (e.path().extension() == ".tiff") ++tiffs;
        MIB_EXPECT(tiffs >= 30, "image tiffs exported");
        MIB_EXPECT(noPartials(td.path() / "all"), "no partial residue");
    }
    wd.mark("export all");

    // ---- cancel ----------------------------------------------------------------
    {
        ExportAllRequest req;
        req.outputRoot = (td.path() / "cancel").string();
        const uint64_t id = jobs.startExportAll(req, &err);
        MIB_REQUIRE(id != 0, "start for cancel: " + err);
        MIB_EXPECT(jobs.cancel(id), "cancel accepted");
        MIB_REQUIRE(sink.terminal(id, done), "cancel terminal");
        MIB_EXPECT(done.state == ReviewJobState::Cancelled || done.state == ReviewJobState::Completed, "cancel state");
        MIB_EXPECT(noPartials(td.path() / "cancel"), "cancel leaves no partial");
    }
    wd.mark("cancel");

    // ---- batch continues after a failure ----------------------------------------
    {
        BatchExportRequest req;
        req.sources = {experiment.string(), (td.path() / "missing.h5").string(), legacy.string()};
        req.outputRoot = (td.path() / "batch").string();
        req.metricsOnly = true;
        const uint64_t id = jobs.startBatchExport(req, &err);
        MIB_REQUIRE(id != 0, "start batch: " + err);
        MIB_REQUIRE(sink.terminal(id, done), "batch terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "batch completed");
        MIB_EXPECT(done.message.find("exported 2 of 3") != std::string::npos, "batch summary: " + done.message);
        MIB_EXPECT(done.message.find("missing.h5") != std::string::npos, "batch names the failure");
        MIB_EXPECT(fs::exists(td.path() / "batch" / "run_metrics.csv"), "batch output run");
        MIB_EXPECT(fs::exists(td.path() / "batch" / "legacy_metrics.csv"), "batch output legacy");
    }
    wd.mark("batch");

    // ---- regenerate masks (whole file, recorded config) --------------------------
    {
        RegenerateMasksRequest req;
        req.source = RegenerateSource::WholeFile;
        req.outputPath = (td.path() / "regenerated.h5").string();
        const uint64_t id = jobs.startRegenerateMasks(req, &err);
        MIB_REQUIRE(id != 0, "start regenerate: " + err);
        MIB_REQUIRE(sink.terminal(id, done, 120000), "regenerate terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "regenerate completed: " + done.message);
        ReviewSession out;
        MIB_REQUIRE(out.open(req.outputPath), "open regenerated");
        MIB_EXPECT(out.frameCount(true) + out.frameCount(false) == 33, "regenerated frame count");
        ProcessedFrame first;
        MIB_REQUIRE(out.frameCount(true) + out.frameCount(false) > 0 && (out.frameMeta(true, 0, first) || out.frameMeta(false, 0, first)), "first frame");
        MIB_EXPECT(first.timestampNs < 100000, "timestamps normalised to the first frame");
    }
    wd.mark("regenerate");

    // ---- compute core -----------------------------------------------------------
    {
        const uint64_t id = jobs.startComputeCore(0.9, &err);
        MIB_REQUIRE(id != 0, "start core: " + err);
        MIB_REQUIRE(sink.terminal(id, done), "core terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "core completed: " + done.message);
        const auto record = backend::monitoring::fromJson(jobs.computedCoreJson());
        MIB_REQUIRE(record.has_value(), "core json parses");
        MIB_EXPECT(!record->provisional && record->source == "full-run" && record->cellCount == 30, "core record");
        MIB_EXPECT(std::fabs(record->pixelToMicron - 0.5) < 1e-12, "core factor is the recorded one");
        MIB_EXPECT(record->computedAtNs > 0, "computedAt set");
    }
    wd.mark("core");

    // ---- density: stored record → no computed record; levels parallel ------------
    {
        DensityRequest req;
        const uint64_t id = jobs.startDensity(req, &err);
        MIB_REQUIRE(id != 0, "start density: " + err);
        MIB_REQUIRE(sink.terminal(id, done), "density terminal");
        MIB_EXPECT(done.state == ReviewJobState::Completed, "density completed: " + done.message);
        const DensityResult d = jobs.density();
        MIB_EXPECT(d.ready && d.levels.size() == session.scatter().areaUm2.size(), "levels parallel to scatter");
        MIB_EXPECT(d.computedRecordJson.empty(), "stored record → nothing computed");
        bool inRange = true;
        int maxLevel = 0;
        for (auto l : d.levels) { inRange = inRange && l < 8; maxLevel = std::max<int>(maxLevel, l); }
        MIB_EXPECT(inRange && maxLevel == 7, "densest point reaches the top level");
    }
    {
        ReviewSession ls;
        MIB_REQUIRE(ls.open(legacy.string()), "open legacy");
        Sink s2;
        ReviewJobs j2(ls, nullptr, std::ref(s2));
        const uint64_t id = j2.startDensity(DensityRequest{}, &err);
        MIB_REQUIRE(id != 0, "legacy density: " + err);
        MIB_REQUIRE(s2.terminal(id, done), "legacy density terminal");
        MIB_EXPECT(!j2.density().computedRecordJson.empty(), "legacy file gets a computed record");
        MIB_EXPECT(j2.startRegenerateMasks(RegenerateMasksRequest{}, &err) == 0, "no processing service → refused");
    }
    wd.mark("density");

    // ---- grid path ≈ direct path (4000 points, two clusters) ----------------------
    {
        std::mt19937 rng(3);
        std::normal_distribution<double> ax(200, 20), ay(0.05, 0.01), bx(400, 25), by(0.12, 0.015);
        std::vector<backend::monitoring::DensityPoint> pts;
        for (int i = 0; i < 4000; ++i)
            pts.push_back(i % 2 ? backend::monitoring::DensityPoint{bx(rng), by(rng)} : backend::monitoring::DensityPoint{ax(rng), ay(rng)});
        const auto direct = ReviewJobs::densityAtPoints(pts, 1.0, 100000, 1);
        const auto grid = ReviewJobs::densityAtPoints(pts, 1.0, 2000, 1);
        MIB_REQUIRE(direct.size() == pts.size() && grid.size() == pts.size(), "sizes");
        std::size_t within = 0;
        double rms = 0.0;
        for (std::size_t i = 0; i < pts.size(); ++i)
        {
            rms += (direct[i] - grid[i]) * (direct[i] - grid[i]);
            if (std::abs(ReviewJobs::levelForDensity(direct[i], 8) - ReviewJobs::levelForDensity(grid[i], 8)) <= 1) ++within;
        }
        rms = std::sqrt(rms / pts.size());
        MIB_EXPECT(rms < 0.08, "grid vs direct RMS " + std::to_string(rms));
        MIB_EXPECT(within >= pts.size() * 95 / 100, "levels within one for " + std::to_string(within) + " points");
        MIB_EXPECT(ReviewJobs::levelForDensity(1.0, 8) == 7 && ReviewJobs::levelForDensity(0.0, 8) == 0 &&
                       ReviewJobs::levelForDensity(std::nan(""), 8) == 0,
                   "level bucketing");
    }
    wd.mark("grid");

    // ---- single flight --------------------------------------------------------------
    {
        ExportAllRequest req;
        req.outputRoot = (td.path() / "flight").string();
        const uint64_t a = jobs.startExportAll(req, &err);
        MIB_REQUIRE(a != 0, "first job");
        const uint64_t b = jobs.startExportMetrics((td.path() / "x.csv").string(), &err);
        MIB_EXPECT(b == 0 && err.find("still running") != std::string::npos, "second job refused while running");
        MIB_REQUIRE(sink.terminal(a, done), "first terminal");
    }

    jobs.shutdown();
    return mib::test::exitCode();
}
