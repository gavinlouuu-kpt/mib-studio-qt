// experiment_roundtrip_test
//
// Round-trip coverage for the experiment save path (distinct from recording
// mode): write frames + metadata + experiment info + config JSON, close, reload,
// and verify totals, ROI, per-frame metadata, and image pixels survive. Guards
// against silent data loss/corruption in the primary "save data" capability.

#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>
#include <hdf5.h>

#include <cmath>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;

namespace {

ProcessedFrame makeFrame(uint64_t idx, unsigned char value, bool valid,
                         double area, double deform)
{
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(value));
    f.processedImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(valid ? 255 : 0));
    f.validation.isValid = valid;
    f.validation.objectId = static_cast<int>(idx);
    f.validation.objectCount = 1;
    f.validation.area = area;
    f.validation.deformability = deform;
    // Contract-2 per-object focus metric; must round-trip through HDF5.
    f.validation.laplacianVariance = 12.5 + static_cast<double>(idx);
    return f;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int main()
{
    mib::test::TempDir td("mib_experiment_roundtrip");
    const std::string path = (td / "experiment.h5").string();

    std::vector<ProcessedFrame> valid{makeFrame(0, 40, true, 100.0, 0.20),
                                      makeFrame(1, 80, true, 150.0, 0.30)};
    std::vector<ProcessedFrame> invalid{makeFrame(2, 120, false, 10.0, 0.90)};

    // --- Write via the incremental append path (as used during a run) ---
    {
        Hdf5Service hdf5;
        MIB_REQUIRE(hdf5.openFile(path), "openFile experiment");
        MIB_REQUIRE(hdf5.initializeDatasets(), "initializeDatasets");
        MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "appendFrames");

        ProcessingConfig cfg;
        ProcessingService::Roi roi{1, 2, 6, 7};
        backend::processing::ProcessingCoreIdentity core;
        core.version = "2.3.4";
        core.contractVersion = 1;
        core.engineAbiVersion = 1;
        core.artifactSha256 = std::string(64, 'a');
        core.releaseTag = "mib-processing-v2.3.4";
        core.manifestSha256 = std::string(64, 'b');
        core.source = "plugin";
        core.buildId = "fixture-build";
        core.runtimeFingerprint = "fixture-runtime";
        MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, valid.size(),
                                             invalid.size(), cfg, roi, nullptr, &core),
                    "writeExperimentInfo");
        MIB_EXPECT(hdf5.writeConfigJson("{\"pixel_to_micron\":0.4886}"),
                   "writeConfigJson");
        hdf5.closeFile();
    }

    // --- Reload and verify ---
    {
        Hdf5Service r;
        MIB_REQUIRE(r.loadFile(path), "reload experiment");

        uint64_t start = 0, end = 0;
        size_t totalValid = 0, totalInvalid = 0;
        ProcessingService::Roi roiOut{};
        MIB_REQUIRE(r.readExperimentInfo(start, end, totalValid, totalInvalid, &roiOut),
                    "readExperimentInfo");
        MIB_EXPECT(start == 1000 && end == 4000, "experiment times round-trip");
        MIB_EXPECT(totalValid == 2 && totalInvalid == 1, "frame totals round-trip");
        MIB_EXPECT(roiOut.x == 1 && roiOut.y == 2 && roiOut.w == 6 && roiOut.h == 7,
                   "ROI round-trips");
        backend::processing::ProcessingCoreIdentity coreOut;
        MIB_REQUIRE(r.readProcessingCoreIdentity(coreOut), "processing core identity round-trip");
        MIB_EXPECT(coreOut.version == "2.3.4" && coreOut.source == "plugin",
                   "processing core version/source round-trip");
        MIB_EXPECT(coreOut.artifactSha256 == std::string(64, 'a') &&
                       coreOut.manifestSha256 == std::string(64, 'b'),
                   "processing core digests round-trip");

        std::vector<ProcessedFrame> meta;
        MIB_REQUIRE(r.readValidMetadata(meta), "readValidMetadata");
        MIB_EXPECT(meta.size() == 2, "valid metadata count");
        if (meta.size() == 2) {
            MIB_EXPECT(meta[0].index == 0 && meta[1].index == 1, "indices round-trip");
            MIB_EXPECT(near(meta[0].validation.area, 100.0), "area[0] round-trips");
            MIB_EXPECT(near(meta[1].validation.deformability, 0.30),
                       "deformability[1] round-trips");
            MIB_EXPECT(near(meta[0].validation.laplacianVariance, 12.5) &&
                           near(meta[1].validation.laplacianVariance, 13.5),
                       "laplacian variance round-trips through HDF5");
        }

        std::vector<ProcessedFrame> full;
        MIB_REQUIRE(r.readValidFrames(full), "readValidFrames");
        MIB_EXPECT(full.size() == 2, "valid frame count");
        if (!full.empty() && !full[0].originalImage.empty()) {
            MIB_EXPECT(full[0].originalImage.cols == 10 && full[0].originalImage.rows == 8,
                       "image dimensions round-trip");
            MIB_EXPECT(full[0].originalImage.at<unsigned char>(0, 0) == 40,
                       "image pixel value round-trips");
        } else {
            MIB_EXPECT(false, "valid frames carry image payloads");
        }
        r.closeFile();
    }

    // Legacy/external HDF writers may use fixed-length strings. Replacing one
    // provenance attribute exercises the bounded fixed-string reader path
    // (it must not treat fixed bytes as a heap-allocated char pointer).
    {
        hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open raw HDF handle for fixed-string fixture");
        hid_t group = H5Gopen2(file, "/experiment_info", H5P_DEFAULT);
        MIB_REQUIRE(group >= 0, "open experiment_info for fixed-string fixture");
        MIB_REQUIRE(H5Adelete(group, "processing_core_version") >= 0,
                    "remove variable processing_core_version");
        hid_t space = H5Screate(H5S_SCALAR);
        hid_t type = H5Tcopy(H5T_C_S1);
        H5Tset_size(type, 16);
        H5Tset_strpad(type, H5T_STR_NULLTERM);
        hid_t attribute = H5Acreate2(group, "processing_core_version", type, space,
                                     H5P_DEFAULT, H5P_DEFAULT);
        const char fixedVersion[16] = "2.3.4-fixed";
        MIB_REQUIRE(attribute >= 0 && H5Awrite(attribute, type, fixedVersion) >= 0,
                    "write fixed processing_core_version");
        H5Aclose(attribute);
        H5Tclose(type);
        H5Sclose(space);
        H5Gclose(group);
        H5Fclose(file);

        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reload fixed-string provenance fixture");
        backend::processing::ProcessingCoreIdentity core;
        MIB_REQUIRE(reader.readProcessingCoreIdentity(core),
                    "fixed-string provenance is read safely");
        MIB_EXPECT(core.version == "2.3.4-fixed", "fixed string is decoded without overread");
        reader.closeFile();
    }

    {
        const std::string legacyPath = (td / "legacy.h5").string();
        hid_t file = H5Fcreate(legacyPath.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "create legacy provenance fixture");
        hid_t group = H5Gcreate2(file, "/experiment_info", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT);
        MIB_REQUIRE(group >= 0, "create legacy experiment_info group");
        H5Gclose(group);
        H5Fclose(file);
        Hdf5Service legacy;
        MIB_REQUIRE(legacy.loadFile(legacyPath), "load legacy provenance fixture");
        backend::processing::ProcessingCoreIdentity missing;
        MIB_EXPECT(!legacy.readProcessingCoreIdentity(missing),
                   "legacy file without core attributes is reported explicitly");
        legacy.closeFile();
    }

    if (mib::test::exitCode() == 0) {
        std::printf("experiment save path round-trips (frames, metadata, ROI, config)\n");
    }
    return mib::test::exitCode();
}
