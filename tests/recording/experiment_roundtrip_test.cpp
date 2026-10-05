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
#include <utility>
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
    // Contract-3 (unet-cells) members; must round-trip too.
    f.validation.brightnessMean = 100.25 + static_cast<double>(idx);
    f.validation.brightnessVariance = 30.5 + static_cast<double>(idx);
    f.validation.contourArea = 512.5 + static_cast<double>(idx);
    f.validation.pixelCount = 600 + static_cast<int>(idx);
    f.validation.blemishCount = 3 + static_cast<int>(idx);
    f.validation.degenerateContour = idx == 1;
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
        cfg.processing_contract_version = 2;
        cfg.bg_subtract_threshold = 11;
        cfg.enable_ring_ratio_check = false;
        cfg.enable_laplacian_variance_check = true;
        cfg.laplacian_variance_min = 5.0;
        cfg.laplacian_variance_max = 500.0;
        cfg.auto_roi_from_background = true;
        cfg.channel_band_y = 30;
        cfg.channel_band_h = 50;
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
        MIB_EXPECT(coreOut.contractVersion == 1,
                   "a plugin core's declared contract is recorded as-is");

        ProcessingConfig cfgOut;
        MIB_REQUIRE(r.readRecordedProcessingConfig(cfgOut), "recorded processing config reads back");
        MIB_EXPECT(cfgOut.processing_contract_version == 2 && cfgOut.bg_subtract_threshold == 11,
                   "declared contract and difference threshold round-trip");
        MIB_EXPECT(!cfgOut.enable_ring_ratio_check && cfgOut.enable_laplacian_variance_check &&
                       near(cfgOut.laplacian_variance_min, 5.0) &&
                       near(cfgOut.laplacian_variance_max, 500.0),
                   "ring and Laplacian gates round-trip");
        MIB_EXPECT(cfgOut.auto_roi_from_background && cfgOut.channel_band_y == 30 &&
                       cfgOut.channel_band_h == 50,
                   "channel band round-trips");

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
            MIB_EXPECT(near(meta[0].validation.brightnessMean, 100.25) &&
                           near(meta[1].validation.brightnessVariance, 31.5) &&
                           near(meta[1].validation.contourArea, 513.5) &&
                           meta[0].validation.pixelCount == 600 &&
                           meta[1].validation.blemishCount == 4 &&
                           !meta[0].validation.degenerateContour &&
                           meta[1].validation.degenerateContour,
                       "Contract-3 cell members round-trip through HDF5");
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

    // A recording made before Contract 3 has no cell members in its metadata
    // compound. Rewrite /valid_frames/metadata without them: the reader must
    // keep the "not present" defaults (NaN brightness, zero counts).
    {
        const char* cellMembers[] = {"brightness_mean", "brightness_variance", "contourArea",
                                     "pixelCount", "blemishCount", "degenerateContour"};
        hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open raw HDF handle for the pre-Contract-3 fixture");
        hid_t dset = H5Dopen2(file, "/valid_frames/metadata", H5P_DEFAULT);
        MIB_REQUIRE(dset >= 0, "open valid metadata");
        hid_t fileType = H5Dget_type(dset);
        hid_t space = H5Dget_space(dset);
        const hssize_t rows = H5Sget_simple_extent_npoints(space);
        // Pack every other member into an older compound, in file order.
        size_t size = 0;
        const int members = H5Tget_nmembers(fileType);
        std::vector<std::pair<std::string, hid_t>> keep;
        for (int i = 0; i < members; ++i) {
            char* name = H5Tget_member_name(fileType, static_cast<unsigned>(i));
            bool cell = false;
            for (const char* c : cellMembers) cell = cell || std::string(name) == c;
            if (!cell) {
                hid_t mt = H5Tget_native_type(H5Tget_member_type(fileType, static_cast<unsigned>(i)),
                                              H5T_DIR_ASCEND);
                keep.emplace_back(name, mt);
                size += H5Tget_size(mt);
            }
            H5free_memory(name);
        }
        MIB_REQUIRE(static_cast<int>(keep.size()) == members - 6, "the file has all six cell members");
        hid_t oldType = H5Tcreate(H5T_COMPOUND, size);
        size_t offset = 0;
        for (const auto& [name, mt] : keep) {
            H5Tinsert(oldType, name.c_str(), offset, mt);
            offset += H5Tget_size(mt);
        }
        std::vector<unsigned char> buf(size * static_cast<size_t>(rows));
        MIB_REQUIRE(H5Dread(dset, oldType, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data()) >= 0,
                    "read metadata in the older layout");
        H5Dclose(dset);
        MIB_REQUIRE(H5Ldelete(file, "/valid_frames/metadata", H5P_DEFAULT) >= 0, "drop metadata");
        const hsize_t dims[1] = {static_cast<hsize_t>(rows)};
        hid_t oldSpace = H5Screate_simple(1, dims, nullptr); // the live one is extendible
        hid_t oldSet = H5Dcreate2(file, "/valid_frames/metadata", oldType, oldSpace, H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT);
        H5Sclose(oldSpace);
        MIB_REQUIRE(oldSet >= 0 && H5Dwrite(oldSet, oldType, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                            buf.data()) >= 0,
                    "write metadata without the cell members");
        H5Dclose(oldSet);
        for (auto& [name, mt] : keep) H5Tclose(mt);
        H5Tclose(oldType);
        H5Sclose(space);
        H5Tclose(fileType);
        H5Fclose(file);

        Hdf5Service reader;
        MIB_REQUIRE(reader.loadFile(path), "reload the pre-Contract-3 fixture");
        std::vector<ProcessedFrame> meta;
        MIB_REQUIRE(reader.readValidMetadata(meta), "read pre-Contract-3 metadata");
        MIB_EXPECT(meta.size() == 2 && near(meta[1].validation.laplacianVariance, 13.5) &&
                       near(meta[0].validation.area, 100.0),
                   "older metadata keeps its own members");
        MIB_EXPECT(meta.size() == 2 && std::isnan(meta[0].validation.brightnessMean) &&
                       std::isnan(meta[1].validation.brightnessVariance) &&
                       meta[0].validation.contourArea == 0.0 && meta[1].validation.pixelCount == 0 &&
                       meta[1].validation.blemishCount == 0 &&
                       !meta[1].validation.degenerateContour,
                   "a file without cell members reads them as not present");
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
        ProcessingConfig legacyConfig;
        legacyConfig.processing_contract_version = 7; // sentinel: no attribute to read
        MIB_EXPECT(legacy.readRecordedProcessingConfig(legacyConfig) &&
                       legacyConfig.processing_contract_version == 7,
                   "attributes a legacy file lacks keep the caller's values");
        legacy.closeFile();
    }

    if (mib::test::exitCode() == 0) {
        std::printf("experiment save path round-trips (frames, metadata, ROI, config)\n");
    }
    return mib::test::exitCode();
}
