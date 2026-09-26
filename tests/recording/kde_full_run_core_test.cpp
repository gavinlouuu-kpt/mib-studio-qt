// kde_full_run_core_test
//
// Authoritative (full-run) KDE core record and the post-run update path:
//  - computeFullRunCoreRecord: non-provisional, deterministic for a given
//    population (fixed-seed subsample above 5000 points), non-finite points
//    excluded and counted, the contour encloses ~90% of the WHOLE population
//    (not only the subsample), too few cells give no level and no contour;
//  - Hdf5Service::openFileForUpdate adds the analysis record to a finished
//    experiment without truncating it (frames, experiment info and the live
//    record survive), and refuses a missing file, a file read-only on disk,
//    and a second open while the service is already open.

#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/KdeCoreRecord.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace mon = backend::monitoring;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace {

std::vector<mon::DensityPoint> population(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> ax(180, 25), ay(0.05, 0.01), bx(330, 30), by(0.11, 0.015);
    std::vector<mon::DensityPoint> pts;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 3 == 0)
            pts.push_back({bx(rng), by(rng)});
        else
            pts.push_back({ax(rng), ay(rng)});
    }
    return pts;
}

ProcessedFrame frame(uint64_t idx) {
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(50));
    f.processedImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = true;
    f.validation.area = 100.0 + static_cast<double>(idx);
    f.validation.deformability = 0.05;
    return f;
}

} // namespace

int main() {
    // ---- the computation --------------------------------------------------------
    {
        auto pts = population(20000, 7);
        for (int i = 0; i < 17; ++i)
            pts.push_back({std::nan(""), 0.1});
        const auto a = mon::computeFullRunCoreRecord(pts, 0.9, 0.4886);
        const auto b = mon::computeFullRunCoreRecord(pts, 0.9, 0.4886);
        MIB_EXPECT(!a.provisional && a.source == "full-run" && a.bandwidthFactor == 1.0,
                   "full-run record is authoritative");
        MIB_EXPECT(a.populationCount == mon::kFullRunMaxPoints && a.excludedPoints == 17,
                   "large runs are subsampled to the cap; non-finite points excluded and counted");
        MIB_EXPECT(mon::toJson(a) == mon::toJson(b),
                   "same population -> identical record (fixed seed)");
        MIB_EXPECT(a.cellCount == 4500, "90% of the 5000 estimated points form the core");
        MIB_EXPECT(!a.contours.empty(), "a contour is traced");
        std::vector<mon::DensityPoint> finite(pts.begin(), pts.begin() + 20000);
        const double inside = mon::fractionInside(finite, a.contours);
        std::fprintf(stderr, "full-run: %zu loop(s), %.3f of all 20000 points inside\n",
                     a.contours.size(), inside);
        MIB_EXPECT(inside >= 0.85 && inside <= 0.95,
                   "the contour encloses ~90% of the whole population");
        MIB_EXPECT(a.x0 < 150 && a.x1 > 360 && a.gridNx == 128, "grid spans the padded data range");

        const auto small = mon::computeFullRunCoreRecord({{1, 1}, {2, 2}}, 0.9, 0.5);
        MIB_EXPECT(std::isnan(small.level) && small.contours.empty() && small.populationCount == 2,
                   "too few cells: no level, no contour, count kept");
        const auto viaCodec = mon::fromJson(mon::toJson(a));
        MIB_EXPECT(viaCodec && !viaCodec->provisional &&
                       viaCodec->contours.size() == a.contours.size(),
                   "the full-run record round-trips through the codec");
    }

    // ---- update an existing experiment -------------------------------------
    mib::test::TempDir td("mib_kde_full_run");
    const std::string path = (td / "experiment.h5").string();
    const std::string liveJson = mon::toJson(mon::KdeCoreRecord{});
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "create");
        MIB_REQUIRE(hdf5.initializeDatasets(), "datasets");
        MIB_REQUIRE(hdf5.appendFrames({frame(0), frame(1), frame(2)}, {}), "frames");
        backend::services::ProcessingConfig cfg;
        backend::services::ProcessingService::Roi roi{0, 0, 10, 8};
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, 3, 0, cfg, roi, nullptr, nullptr), "info");
        MIB_REQUIRE(hdf5.writeKdeLiveJson(liveJson), "live record");
        hdf5.closeFile();
    }
    const auto full = mon::computeFullRunCoreRecord(population(600, 3), 0.9, 0.4886);
    const std::string fullJson = mon::toJson(full);
    {
        Hdf5Service updater;
        MIB_REQUIRE(updater.openFileForUpdate(path), "open finished experiment for update");
        MIB_EXPECT(!updater.openFileForUpdate(path),
                   "a second open on the same service is refused");
        MIB_REQUIRE(updater.writeKdeAnalysisJson(fullJson), "write analysis record");
        updater.closeFile();
    }
    {
        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reopen");
        uint64_t s = 0, e = 0;
        size_t v = 0, inv = 0;
        MIB_EXPECT(reader.readExperimentInfo(s, e, v, inv) && v == 3 && s == 1000 && e == 4000,
                   "update did not truncate: experiment info intact");
        std::vector<ProcessedFrame> frames;
        MIB_EXPECT(reader.readValidMetadata(frames) && frames.size() == 3,
                   "update did not truncate: frames intact");
        std::string a, l;
        MIB_EXPECT(reader.readKdeAnalysisJson(a) && a == fullJson, "analysis record stored");
        MIB_EXPECT(reader.readKdeLiveJson(l) && l == liveJson, "live record untouched");
        reader.closeFile();
    }

    // ---- refusals ---------------------------------------------------------------
    {
        Hdf5Service updater;
        MIB_EXPECT(!updater.openFileForUpdate((td / "missing.h5").string()),
                   "missing file refused (never created)");
        MIB_EXPECT(!std::filesystem::exists(td / "missing.h5"), "refusal created nothing");
    }
    {
        namespace fs = std::filesystem;
        fs::permissions(path,
                        fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        // Root (containers, devcontainer) bypasses permission bits, so only
        // assert the refusal where the OS itself refuses writes to the file.
        const bool osRefusesWrite =
            !std::fstream(path, std::ios::in | std::ios::out | std::ios::binary).is_open();
        Hdf5Service updater;
        const bool opened = updater.openFileForUpdate(path);
        if (opened) updater.closeFile();
        fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
        if (osRefusesWrite)
            MIB_EXPECT(!opened, "file read-only on disk refused");
        else
            std::printf("NOTE: write bits not enforced for this user (root?); "
                        "read-only refusal not checked\n");
    }

    return mib::test::exitCode();
}
