// hdf_export_service_test (issue #344)
//
// Native Qt-free HdfExportService:
//  - round-trip: fixture with valid/invalid/series/metadata -> All export;
//    CSV rows, TIFF names/pixels, series naming, chart images, source hash;
//  - transactional output: cancellation in every phase and an injected
//    write failure never publish a normal-looking folder, leave no
//    ".partial-" residue (unless retention is requested, then a visible
//    manifest);
//  - fault injection: missing source, file-as-parent output, existing
//    destination;
//  - bounded name lookup with thousands of prior exports;
//  - repeated runs (MIB_EXPORT_SOAK_CYCLES, default 8): HDF5 open-object
//    count returns to baseline, output manifests identical, timing bounded.
// Watchdog guarded.

#include "backend/recording/HdfExportService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace backend::recording;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace {

cv::Mat pattern(uint64_t index, int h, int w, int offset = 0)
{
    cv::Mat m(h, w, CV_8UC1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7 + offset) % 251);
    return m;
}

ProcessedFrame makeFrame(uint64_t idx, bool valid, int h, int w, int series)
{
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = pattern(idx, h, w, valid ? 0 : 50);
    f.processedImage = cv::Mat(h, w, CV_8UC1, cv::Scalar(valid ? 255 : 0));
    f.validation.isValid = valid;
    f.validation.objectId = static_cast<int>(idx);
    f.validation.objectCount = 1;
    f.validation.area = 100.0 + idx;
    f.validation.deformability = 0.25;
    f.validation.brightness.q1 = 1.5;
    for (int s = 0; valid && s < series; ++s) f.seriesImages.push_back(pattern(idx, h, w, 100 + s * 17));
    return f;
}

std::string writeFixture(const fs::path& path, int validN, int invalidN, int series, int h, int w)
{
    std::vector<ProcessedFrame> valid, invalid;
    for (int i = 0; i < validN; ++i) valid.push_back(makeFrame(static_cast<uint64_t>(i * 2), true, h, w, series));
    for (int i = 0; i < invalidN; ++i) invalid.push_back(makeFrame(static_cast<uint64_t>(i * 2 + 1), false, h, w, 0));
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path.string()), "open fixture");
    MIB_REQUIRE(hdf5.initializeDatasets(), "init datasets");
    MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append frames");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, w, h};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, roi), "experiment info");
    hdf5.closeFile();
    return path.string();
}

uint64_t fnv1a(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    uint64_t h = 1469598103934665603ULL;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) { h ^= static_cast<uint8_t>(buf[i]); h *= 1099511628211ULL; }
    }
    return h;
}

std::map<std::string, uint64_t> manifest(const fs::path& dir)
{
    std::map<std::string, uint64_t> m;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) m[fs::relative(e.path(), dir).string()] = fnv1a(e.path());
    return m;
}

bool noPartials(const fs::path& root)
{
    if (!fs::exists(root)) return true;
    for (const auto& e : fs::directory_iterator(root))
        if (e.path().filename().string().find(".partial-") != std::string::npos) return false;
    return true;
}

} // namespace

int main()
{
    mib::test::Watchdog wd(240);
    mib::test::TempDir td("hdf_export_service");
    constexpr int kValid = 12, kInvalid = 6, kSeries = 3, kH = 16, kW = 24;
    const fs::path source = td.path() / "cell run.v1.h5";
    writeFixture(source, kValid, kInvalid, kSeries, kH, kW);
    const uint64_t sourceHash = fnv1a(source);
    const fs::path out = td.path() / "out";
    HdfExportService service;
    const long long baselineObjects = Hdf5Service::globalOpenObjectCountForDiagnostics();

    auto request = [&](HdfExportFormat fmt) {
        HdfExportRequest r;
        r.sourcePath = source.string();
        r.outputRoot = out.string();
        r.format = fmt;
        r.supplementalImages["scatter_plot.tiff"] = cv::Mat(20, 30, CV_8UC3, cv::Scalar(1, 2, 3));
        r.supplementalImages["ring_width_histogram.tiff"] = cv::Mat(20, 30, CV_8UC3, cv::Scalar(4, 5, 6));
        return r;
    };

    // ---- 1. Round trip --------------------------------------------------------
    {
        wd.mark("roundtrip");
        std::vector<HdfExportPhase> phases;
        const auto r = service.run(request(HdfExportFormat::All), HdfExportCancelToken{},
                                   [&](const HdfExportProgress& p) { phases.push_back(p.phase); });
        MIB_REQUIRE(r.completed(), "all export completes: " + r.error);
        MIB_EXPECT(r.finalPath == (out / "cell run.v1").string(), "source-derived folder");
        MIB_EXPECT(r.imagesExported == kValid + kInvalid && r.seriesExported == kValid * kSeries && r.chartsExported == 2,
                   "image counts");
        MIB_EXPECT(r.validCount == kValid && r.invalidCount == kInvalid && r.metricsWritten, "metrics counts");
        const fs::path folder(r.finalPath);
        cv::Mat v = cv::imread((folder / "valid_frame_000004.tiff").string(), cv::IMREAD_UNCHANGED);
        MIB_EXPECT(!v.empty() && cv::countNonZero(v != pattern(4, kH, kW)) == 0, "valid pixels round-trip");
        cv::Mat inv = cv::imread((folder / "invalid_frame_000003.tiff").string(), cv::IMREAD_UNCHANGED);
        MIB_EXPECT(!inv.empty() && cv::countNonZero(inv != pattern(3, kH, kW, 50)) == 0, "invalid pixels round-trip");
        cv::Mat s = cv::imread((folder / "valid_frame_000002_series_02.tiff").string(), cv::IMREAD_UNCHANGED);
        MIB_EXPECT(!s.empty() && cv::countNonZero(s != pattern(2, kH, kW, 117)) == 0, "series pixels + 1-based naming");
        MIB_EXPECT(fs::exists(folder / "scatter_plot.tiff") && fs::exists(folder / "ring_width_histogram.tiff"), "charts");
        std::ifstream csv(folder / "metrics.csv");
        std::string header, row;
        std::getline(csv, header);
        std::getline(csv, row);
        MIB_EXPECT(header.rfind("Frame Type,Index,Timestamp,Object Id,Object Count,Track Id", 0) == 0, "csv header");
        MIB_EXPECT(row.rfind("Valid,0,1000,0,1,-1,0,0,0,0.250,100.00,", 0) == 0, "csv row format: " + row);
        MIB_EXPECT(std::count(phases.begin(), phases.end(), HdfExportPhase::Committing) == 1, "commit phase reported");
        MIB_EXPECT(noPartials(out), "no partial residue");
        MIB_EXPECT(fnv1a(source) == sourceHash, "source untouched");
        // Metrics-only + images-only + frame selection.
        const auto csvOnly = service.run(request(HdfExportFormat::MetricsCsv), HdfExportCancelToken{});
        MIB_EXPECT(csvOnly.completed() && csvOnly.finalPath == (out / "cell run.v1_metrics.csv").string(), "csv path");
        auto imgReq = request(HdfExportFormat::Images);
        imgReq.frames = HdfExportFrames::Invalid;
        const auto imgOnly = service.run(imgReq, HdfExportCancelToken{});
        MIB_EXPECT(imgOnly.completed() && imgOnly.imagesExported == kInvalid && imgOnly.seriesExported == 0 &&
                       !imgOnly.metricsWritten && imgOnly.finalPath == (out / "cell run.v1_2").string(),
                   "images-only invalid selection uses _2 folder");
        auto rangeReq = request(HdfExportFormat::Images);
        rangeReq.frames = HdfExportFrames::Valid;
        rangeReq.series.startInclusive = 1;
        rangeReq.series.endInclusive = 1;
        const auto ranged = service.run(rangeReq, HdfExportCancelToken{});
        MIB_EXPECT(ranged.completed() && ranged.seriesExported == kValid, "series range");
    }

    // ---- 2. Cancellation in every phase -----------------------------------------
    {
        wd.mark("cancel");
        struct Case { const char* name; std::function<bool(const std::string&)> trigger; };
        const std::vector<Case> cases{
            {"valid", [](const std::string& p) { return p.find("valid_frame_000004.tiff") != std::string::npos; }},
            {"series", [](const std::string& p) { return p.find("_series_02") != std::string::npos; }},
            {"invalid", [](const std::string& p) { return p.find("invalid_frame_000003") != std::string::npos; }},
            {"charts", [](const std::string& p) { return p.find("scatter_plot") != std::string::npos; }},
        };
        for (const auto& c : cases) {
            HdfExportCancelToken token;
            HdfExportService s2;
            s2.setImageWriterForTests([&](const std::string& path, const cv::Mat& img) {
                if (c.trigger(path)) token.cancel();
                return cv::imwrite(path, img);
            });
            const auto r = s2.run(request(HdfExportFormat::All), token);
            MIB_EXPECT(r.status == HdfExportStatus::Cancelled, std::string("cancelled during ") + c.name);
            MIB_EXPECT(r.finalPath.empty() && !fs::exists(out / "cell run.v1_4"), std::string("nothing published: ") + c.name);
            MIB_EXPECT(noPartials(out), std::string("partial removed: ") + c.name);
        }
        HdfExportCancelToken pre;
        pre.cancel();
        const auto r = service.run(request(HdfExportFormat::MetricsCsv), pre);
        MIB_EXPECT(r.status == HdfExportStatus::Cancelled && noPartials(out), "cancel before open");
    }

    // ---- 3. Faults --------------------------------------------------------------
    {
        wd.mark("faults");
        HdfExportService failing;
        int writes = 0;
        failing.setImageWriterForTests([&](const std::string& path, const cv::Mat& img) {
            return ++writes == 5 ? false : cv::imwrite(path, img);
        });
        const auto r = failing.run(request(HdfExportFormat::Images), HdfExportCancelToken{});
        MIB_EXPECT(r.status == HdfExportStatus::Failed && r.error.find("failed to write image") != std::string::npos,
                   "write failure fails the job");
        MIB_EXPECT(noPartials(out) && !fs::exists(out / "cell run.v1_4"), "failed job discarded");
        auto keep = request(HdfExportFormat::Images);
        keep.keepPartialOnFailure = true;
        writes = 0;
        const auto kept = failing.run(keep, HdfExportCancelToken{});
        MIB_EXPECT(kept.status == HdfExportStatus::Failed && !kept.retainedPartialPath.empty() &&
                       fs::exists(fs::path(kept.retainedPartialPath) / "export-failure.json") &&
                       fs::path(kept.retainedPartialPath).filename().string().rfind(".cell run.v1", 0) == 0,
                   "retained partial is visibly partial");
        fs::remove_all(kept.retainedPartialPath);

        auto missing = request(HdfExportFormat::All);
        missing.sourcePath = (td.path() / "missing.h5").string();
        MIB_EXPECT(service.run(missing, HdfExportCancelToken{}).status == HdfExportStatus::Failed, "missing source");
        const fs::path blocker = td.path() / "blocker.txt";
        { std::ofstream f(blocker); f << "x"; }
        auto badRoot = request(HdfExportFormat::All);
        badRoot.outputRoot = (blocker / "sub").string();
        const auto br = service.run(badRoot, HdfExportCancelToken{});
        MIB_EXPECT(br.status == HdfExportStatus::Failed && br.error.find("output directory") != std::string::npos,
                   "file-as-parent output");
        auto explicitTaken = request(HdfExportFormat::All);
        explicitTaken.explicitDestination = (out / "cell run.v1").string();
        MIB_EXPECT(service.run(explicitTaken, HdfExportCancelToken{}).status == HdfExportStatus::Failed,
                   "explicit destination that exists is refused");
        MIB_EXPECT(noPartials(out), "faults leave no partials");
        MIB_EXPECT(fnv1a(source) == sourceHash, "source untouched after faults");
    }

    // ---- 4. Bounded name lookup ----------------------------------------------
    {
        wd.mark("names");
        const fs::path many = td.path() / "many";
        fs::create_directories(many / "sample");
        for (int i = 2; i <= 1500; ++i) fs::create_directories(many / ("sample_" + std::to_string(i)));
        fs::create_directories(many / "sample_9999_notes");
        const auto t0 = std::chrono::steady_clock::now();
        const auto chosen = HdfExportService::nextAvailableName(many.string(), "sample", "sample_", "");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(chosen == (many / "sample_1501").string(), "max suffix + 1: " + chosen);
        MIB_EXPECT(ms < 500.0, "single listing is fast");
        MIB_EXPECT(HdfExportService::nextAvailableName(many.string(), "a_metrics.csv", "a_metrics_", ".csv") ==
                       (many / "a_metrics.csv").string(), "first name when unused");
    }

    // ---- 5. Repeated runs: object counts, manifests, timing ---------------------
    {
        wd.mark("soak");
        int cycles = 8;
        if (const char* env = std::getenv("MIB_EXPORT_SOAK_CYCLES")) cycles = std::max(3, std::atoi(env));
        const fs::path soakOut = td.path() / "soak";
        std::map<std::string, uint64_t> first;
        std::vector<double> durations;
        bool objectsStable = true;
        for (int n = 0; n < cycles; ++n) {
            auto r = request(HdfExportFormat::All);
            r.outputRoot = soakOut.string();
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = service.run(r, HdfExportCancelToken{});
            durations.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            MIB_REQUIRE(res.completed(), "soak round " + std::to_string(n) + ": " + res.error);
            const auto m = manifest(res.finalPath);
            if (n == 0) first = m;
            else if (m != first) { MIB_EXPECT(false, "manifest differs in round " + std::to_string(n)); }
            const long long objects = Hdf5Service::globalOpenObjectCountForDiagnostics();
            if (objects != baselineObjects) {
                objectsStable = false;
                std::fprintf(stderr, "round %d: open HDF5 objects %lld (baseline %lld)\n", n, objects, baselineObjects);
            }
            fs::remove_all(res.finalPath);
        }
        MIB_EXPECT(objectsStable, "HDF5 open-object count returns to baseline after every job");
        if (cycles >= 6) {
            std::vector<double> early(durations.begin() + 1, durations.begin() + 5);
            std::sort(early.begin(), early.end());
            const double median = (early[1] + early[2]) / 2.0;
            std::fprintf(stderr, "soak: cycles=%d first=%.1fms median(2-5)=%.1fms last=%.1fms\n", cycles, durations[0],
                         median, durations.back());
            MIB_EXPECT(durations.back() <= std::max(1.25 * median, median + 20.0), "last round <= 1.25x median of rounds 2-5");
        }
        MIB_EXPECT(fnv1a(source) == sourceHash, "source untouched after soak");
    }

    return mib::test::exitCode();
}
