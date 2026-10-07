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
#include "backend/recording/FcsWriter.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <hdf5.h>
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
    // The same physical object may be observed repeatedly; FCS must retain
    // every detection rather than deduplicating this frame-local ID.
    f.validation.objectId = 7;
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

    // Single-stream files remain valid CSV inputs, even with Both selected.
    for (bool validOnly : {true, false}) {
        auto r = request(HdfExportFormat::MetricsCsv);
        r.sourcePath = writeFixture(td.path() / (validOnly ? "valid-only.h5" : "invalid-only.h5"),
                                    validOnly ? 1 : 0, validOnly ? 0 : 1, 0, kH, kW);
        const auto exported = service.run(r, HdfExportCancelToken{});
        MIB_EXPECT(exported.completed(), "single-stream CSV completes: " + exported.error);
    }
    // An empty selected stream still owns its schema, independent of other streams.
    {
        const auto path = td.path() / "empty-selected.h5";
        writeFixture(path, 1, 1, 0, kH, kW);
        const hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open empty selection fixture");
        const hid_t valid = H5Dopen2(file, "/valid_frames/metadata", H5P_DEFAULT);
        const hsize_t zero = 0;
        MIB_REQUIRE(valid >= 0 && H5Dset_extent(valid, &zero) >= 0, "empty valid metadata");
        H5Dclose(valid);
        MIB_REQUIRE(H5Ldelete(file, "/invalid_frames/metadata", H5P_DEFAULT) >= 0, "replace invalid schema");
        const hid_t type = H5Tcreate(H5T_COMPOUND, sizeof(uint64_t));
        H5Tinsert(type, "timestampNs", 0, H5T_NATIVE_UINT64);
        const hid_t space = H5Screate_simple(1, &zero, nullptr);
        const hid_t invalid = H5Dcreate2(file, "/invalid_frames/metadata", type, space,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        MIB_REQUIRE(invalid >= 0, "create distinct empty invalid schema");
        H5Dclose(invalid);
        H5Sclose(space);
        H5Tclose(type);
        H5Fclose(file);
        auto r = request(HdfExportFormat::Fcs);
        r.sourcePath = path.string();
        const auto exported = service.run(r, HdfExportCancelToken{});
        MIB_REQUIRE(exported.completed(), "empty selected FCS completes: " + exported.error);
        std::ifstream fcs(fs::path(exported.finalPath) / "empty-selected.fcs", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(fcs)), {});
        MIB_EXPECT(bytes.find("Area_px2") != std::string::npos, "empty valid FCS retains valid schema");
    }

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
        MIB_EXPECT(row.rfind("Valid,0,1000,7,1,-1,0,0,0,0.250,100.00,", 0) == 0, "csv row format: " + row);
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

    // ---- FCS 3.1 detection export --------------------------------------------
    {
        const fs::path fcsOut = td.path() / "fcs-out";
        auto fcs = request(HdfExportFormat::Fcs);
        fcs.outputRoot = fcsOut.string();
        const auto valid = service.run(fcs, HdfExportCancelToken{});
        MIB_REQUIRE(valid.completed(), "FCS default export completes: " + valid.error);
        MIB_EXPECT(valid.validCount == kValid && valid.invalidCount == 0, "FCS defaults to valid detections");
        const fs::path folder(valid.finalPath);
        const fs::path fcsPath = folder / "cell run.v1.fcs";
        const fs::path mapPath = folder / "cell run.v1_event_map.csv";
        MIB_EXPECT(fs::is_regular_file(fcsPath) && fs::is_regular_file(mapPath), "FCS transaction outputs");
        std::ifstream map(mapPath);
        std::string line;
        std::getline(map, line);
        MIB_EXPECT(line == "fcs_event_index,source_frame_index,object_id,timestamp_ns,event_mode", "FCS event map header");
        std::getline(map, line);
        MIB_EXPECT(line.find("0,0,7,1000,detection") != std::string::npos, "FCS exact event map row");

        // Explicit FCS destinations publish one sibling event map alongside
        // the requested file.  Both names are a no-replace pair.
        auto explicitFcs = request(HdfExportFormat::Fcs);
        explicitFcs.outputRoot = fcsOut.string();
        explicitFcs.explicitDestination = (fcsOut / "chosen.fcs").string();
        const auto explicitResult = service.run(explicitFcs, HdfExportCancelToken{});
        const fs::path explicitMap = fcsOut / "chosen_event_map.csv";
        MIB_EXPECT(explicitResult.completed() && explicitResult.finalPath == explicitFcs.explicitDestination,
                   "explicit FCS destination completes at the requested file");
        MIB_EXPECT(fs::is_regular_file(explicitFcs.explicitDestination) && fs::is_regular_file(explicitMap),
                   "explicit FCS publishes the paired event map");
        auto pairTaken = explicitFcs;
        MIB_EXPECT(service.run(pairTaken, HdfExportCancelToken{}).status == HdfExportStatus::Failed,
                   "existing explicit FCS pair is refused");
        fs::remove(explicitMap);
        MIB_EXPECT(service.run(pairTaken, HdfExportCancelToken{}).status == HdfExportStatus::Failed,
                   "existing explicit FCS file is refused when map is absent");
        fs::remove(explicitFcs.explicitDestination);
        auto mapTaken = explicitFcs;
        { std::ofstream occupied(explicitMap); occupied << "occupied\n"; }
        MIB_EXPECT(service.run(mapTaken, HdfExportCancelToken{}).status == HdfExportStatus::Failed,
                   "existing explicit event map is refused when FCS is absent");
        fs::remove(explicitMap);

        auto cancelledCommit = explicitFcs;
        cancelledCommit.explicitDestination = (fcsOut / "cancelled-commit.fcs").string();
        HdfExportCancelToken commitCancel;
        const auto cancelledCommitResult = service.run(
            cancelledCommit, commitCancel, [&](const HdfExportProgress& p) {
                if (p.phase == HdfExportPhase::Committing) commitCancel.cancel();
            });
        MIB_EXPECT(cancelledCommitResult.status == HdfExportStatus::Cancelled &&
                       !fs::exists(fcsOut / "cancelled-commit.fcs") &&
                       !fs::exists(fcsOut / "cancelled-commit_event_map.csv"),
                   "cancellation after committing callback publishes no FCS pair");

        auto commitFault = explicitFcs;
        commitFault.explicitDestination = (fcsOut / "commit-fault.fcs").string();
        const auto commitFaultResult = service.run(
            commitFault, HdfExportCancelToken{}, [&](const HdfExportProgress& p) {
                if (p.phase == HdfExportPhase::Committing) {
                    std::ofstream occupied(commitFault.explicitDestination);
                    occupied << "competing output\n";
                }
            });
        MIB_EXPECT(commitFaultResult.status == HdfExportStatus::Failed &&
                       fs::is_regular_file(commitFault.explicitDestination) &&
                       !fs::exists(fcsOut / "commit-fault_event_map.csv"),
                   "FCS publication rolls back the sidecar when the file target is taken");

        auto both = request(HdfExportFormat::Fcs);
        both.outputRoot = fcsOut.string();
        both.fcsFrames = HdfExportFrames::Both;
        const auto bothResult = service.run(both, HdfExportCancelToken{});
        MIB_EXPECT(bothResult.completed() && bothResult.validCount == kValid && bothResult.invalidCount == kInvalid,
                   "FCS both selection");
        auto invalid = request(HdfExportFormat::Fcs);
        invalid.outputRoot = fcsOut.string();
        invalid.fcsFrames = HdfExportFrames::Invalid;
        invalid.conversionFactor = 0.5;
        const auto invalidResult = service.run(invalid, HdfExportCancelToken{});
        MIB_EXPECT(invalidResult.completed() && invalidResult.validCount == 0 && invalidResult.invalidCount == kInvalid,
                   "FCS invalid selection and custom calibration");

        auto cancelled = request(HdfExportFormat::Fcs);
        cancelled.outputRoot = (td.path() / "fcs-cancelled").string();
        HdfExportCancelToken cancelToken;
        cancelToken.cancel();
        const auto cancelledResult = service.run(cancelled, cancelToken);
        MIB_EXPECT(cancelledResult.status == HdfExportStatus::Cancelled &&
                       noPartials(cancelled.outputRoot),
                   "FCS cancellation publishes no output");
        auto badCalibration = request(HdfExportFormat::Fcs);
        badCalibration.outputRoot = (td.path() / "fcs-bad-calibration").string();
        badCalibration.conversionFactor = std::numeric_limits<double>::quiet_NaN();
        const auto badCalibrationResult = service.run(badCalibration, HdfExportCancelToken{});
        MIB_EXPECT(badCalibrationResult.status == HdfExportStatus::Failed &&
                       badCalibrationResult.error.find("finite and positive") != std::string::npos,
                   "FCS rejects non-finite calibration before output");

        const auto makeMapFault = [&](bool retain) {
            auto requestWithFault = request(HdfExportFormat::Fcs);
            requestWithFault.outputRoot = (td.path() / (retain ? "fcs-map-retain" : "fcs-map-fault")).string();
            requestWithFault.keepPartialOnFailure = retain;
            return requestWithFault;
        };
        HdfExportProgressFn injectMapFault = [](const HdfExportProgress& progress) {
            if (progress.phase == HdfExportPhase::Metrics && !progress.currentOutput.empty()) {
                const fs::path fcsPath(progress.currentOutput);
                std::error_code ec;
                fs::create_directory(fcsPath.parent_path() /
                                         (fcsPath.stem().string() + "_event_map.csv"), ec);
            }
        };
        const auto mapFault = service.run(makeMapFault(false), HdfExportCancelToken{}, injectMapFault);
        MIB_EXPECT(mapFault.status == HdfExportStatus::Failed && mapFault.finalPath.empty() &&
                       noPartials(td.path() / "fcs-map-fault"),
                   "FCS event-map open failure is transactional");
        const auto retained = service.run(makeMapFault(true), HdfExportCancelToken{}, injectMapFault);
        MIB_EXPECT(retained.status == HdfExportStatus::Failed && !retained.retainedPartialPath.empty() &&
                       fs::exists(fs::path(retained.retainedPartialPath) / "export-failure.json") &&
                       fs::exists(fs::path(retained.retainedPartialPath) / "cell run.v1.fcs"),
                   "FCS map failure can retain a manifest and partial FCS");
        fs::remove_all(retained.retainedPartialPath);

        auto cancelDuringFcs = request(HdfExportFormat::Fcs);
        cancelDuringFcs.outputRoot = (td.path() / "fcs-cancel-during-write").string();
        HdfExportCancelToken fcsCancel;
        const auto cancelledAfterPartial = service.run(
            cancelDuringFcs, fcsCancel, [&](const HdfExportProgress& progress) {
                if (progress.phase == HdfExportPhase::Metrics) fcsCancel.cancel();
            });
        MIB_EXPECT(cancelledAfterPartial.status == HdfExportStatus::Cancelled &&
                       noPartials(cancelDuringFcs.outputRoot),
                   "FCS cancellation after partial creation cleans up");
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
        // Compare the median of the last three rounds, not the single last round:
        // one descheduled round on a shared runner (TSan, Windows: 135 ms against a
        // 35 ms median, #517) is noise, while a per-round slowdown from leaked
        // state still lifts all three late rounds above the bound.
        if (cycles >= 8) {
            std::vector<double> early(durations.begin() + 1, durations.begin() + 5);
            std::sort(early.begin(), early.end());
            const double median = (early[1] + early[2]) / 2.0;
            std::vector<double> late(durations.end() - 3, durations.end());
            std::sort(late.begin(), late.end());
            const double lateMedian = late[1];
            std::fprintf(stderr, "soak: cycles=%d first=%.1fms median(2-5)=%.1fms median(last 3)=%.1fms last=%.1fms\n",
                         cycles, durations[0], median, lateMedian, durations.back());
            MIB_EXPECT(lateMedian <= std::max(1.25 * median, median + 20.0),
                       "median of the last 3 rounds <= 1.25x median of rounds 2-5");
        }
        MIB_EXPECT(fnv1a(source) == sourceHash, "source untouched after soak");
    }

    return mib::test::exitCode();
}
