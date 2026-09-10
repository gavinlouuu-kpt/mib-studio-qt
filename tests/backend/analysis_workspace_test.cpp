#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <hdf5.h>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

int main() {
    using Hdf = backend::services::Hdf5Service;
    using Dataset = Hdf::MetadataDataset;
    namespace bridge = backend::bridge;
    mib::test::Watchdog watchdog;
    mib::test::TempDir tmp("analysis_workspace");
    const auto source = tmp / "source.h5";
    {
        Hdf writer;
        MIB_REQUIRE(writer.openFile(source.string()), "create fixture");
        MIB_REQUIRE(writer.initializeRecordingDatasets(), "recorded datasets");
        std::vector<cv::Mat> images;
        std::vector<Hdf::RecordingFrameMeta> metadata;
        for (uint64_t i = 0; i < 9; ++i) {
            images.emplace_back(3, 4, CV_8UC1, cv::Scalar(i + 30));
            metadata.push_back({100 + i, 1000 + i, 4, 3});
        }
        MIB_REQUIRE(writer.appendRecordingFrames(images, metadata), "write recording");
        MIB_REQUIRE(writer.writeRecordingInfo(1000, 1009, 9, 0), "recording info");
        backend::recording::RecordingAccountingSnapshot accounting;
        accounting.admitted = accounting.processed = 9;
        MIB_REQUIRE(writer.writeRunAccounting(accounting), "terminal run accounting");
        writer.closeFile();
    }
    const auto legacy = tmp / "legacy.h5";
    std::filesystem::copy_file(source, legacy);
    {
        hid_t file = H5Fopen(legacy.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        hid_t group = H5Gopen2(file, "/recording_info", H5P_DEFAULT);
        H5Adelete(group, "accounting_schema_version");
        H5Gclose(group);
        H5Fclose(file);
    }
    const auto original = bytes(source);
    Hdf reader;
    MIB_REQUIRE(reader.loadFile(source.string()), "read-only source");
    const auto handles = reader.openObjectCountForDiagnostics();
    std::vector<backend::services::ProcessedFrame> page;
    uint64_t total = 0;
    for (int repeat = 0; repeat < 20; ++repeat) {
        MIB_REQUIRE(reader.readMetadataPage(Dataset::Recorded, 4, 2, page, total), "middle page");
        MIB_EXPECT(total == 9 && page.size() == 2, "bounded row count");
        MIB_EXPECT(page[0].index == 104 && page[1].timestampNs == 1005, "exact source identities");
        MIB_EXPECT(page[0].originalImage.empty() && page[0].processedImage.empty(), "no pixels");
        MIB_REQUIRE(reader.readMetadataPage(Dataset::Recorded, 0, 0, page, total), "count only");
        MIB_EXPECT(total == 9 && page.empty(), "count-only has no row allocation");
        MIB_REQUIRE(reader.readMetadataPage(Dataset::Recorded, 8, 4096, page, total), "last page");
        MIB_EXPECT(page.size() == 1 && page[0].index == 108, "clamped last page");
        MIB_REQUIRE(reader.readMetadataPage(Dataset::Recorded, UINT64_MAX, 2, page, total),
                    "overflow offset");
        MIB_EXPECT(page.empty() && total == 9, "no offset+count overflow");
        MIB_EXPECT(!reader.readMetadataPage(Dataset::Recorded, 0, 4097, page, total),
                   "reject oversized request");
        MIB_EXPECT(page.empty() && total == 0, "failed output cleared");
        MIB_EXPECT(!reader.readMetadataPage(Dataset::Invalid, 0, 1, page, total),
                   "missing dataset fails");
    }
    MIB_EXPECT(reader.openObjectCountForDiagnostics() == handles,
               "repeated pages leak no HDF5 handles");
    reader.closeFile();

    watchdog.mark("analysis startup and command denial");
    backend::AppBackend backend;
    bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((tmp / "app").string(), backend::ApplicationMode::AnalysisOnly),
                "analysis startup");
    MIB_EXPECT(!backend.isCameraConfigured(), "no default/mock camera selected");
    MIB_EXPECT(!facade.initialize((tmp / "app").string()), "mode cannot escalate to instrument");
    const std::vector<bridge::BackendCommand> forbidden = {bridge::CameraCommand{},
                                                           bridge::RecordingCommand{},
                                                           bridge::ProcessingSettingsCommand{},
                                                           bridge::ExperimentCommand{},
                                                           bridge::TriggerCommand{},
                                                           bridge::PumpCommand{},
                                                           bridge::AutofocusCommand{},
                                                           bridge::MonitoringCommand{},
                                                           bridge::PlaybackSeekCommand{}};
    for (const auto& command : forbidden)
        MIB_EXPECT(!facade.dispatch(command).ok, "instrument command denied by C++");
    bridge::BackendCameraDiscovery discovery;
    MIB_EXPECT(!facade.fetchCameraDiscovery(discovery), "no hardware discovery");
    bridge::BackendAutofocusStatus autofocus;
    MIB_EXPECT(!facade.fetchAutofocusStatus(autofocus), "no autofocus thread/controller");
    MIB_EXPECT(!facade.dispatch(bridge::RecordingLoadCommand{legacy.string(), true}).ok,
               "unverified finalization is unavailable in analysis-only mode");
    MIB_REQUIRE(facade.dispatch(bridge::RecordingLoadCommand{source.string(), true}).ok,
                "open recording");
    bridge::BackendReviewMetadata metadata;
    MIB_REQUIRE(facade.fetchReviewMetadata(metadata), "review provenance");
    MIB_EXPECT(metadata.accountingAvailable && metadata.accountingReconciled &&
                   metadata.completionState == 0,
               "completion is sourced from persisted accounting");
    std::vector<bridge::MonitoringObjectRow> rows;
    MIB_REQUIRE(facade.fetchReviewMetricsPage(true, 4, 2, rows, total), "facade bounded page");
    MIB_EXPECT(total == 9 && rows.size() == 2, "facade count");
    MIB_EXPECT(!facade.fetchReviewMetricsPage(true, 0, UINT64_MAX, rows, total),
               "facade rejects unbounded page");
    bridge::BackendFrame frame;
    MIB_REQUIRE(facade.fetchReviewImage(bridge::ReviewImageDataset::RecordedImage, 8, frame),
                "sparse last image");
    MIB_EXPECT(frame.data.size() == 12 && frame.data[0] == 38, "exact source pixels");
    // Disconnection/replacement invalidates the still-open OS file handle;
    // cached pixels/metrics must not substitute for an unavailable mount.
#ifndef _WIN32
    // POSIX permits unlink/rename of an open file; Windows/NAS disconnection
    // needs the separate mapped-drive qualification, not this simulation.
    const auto moved = tmp / "moved.h5";
    std::filesystem::rename(source, moved);
    MIB_EXPECT(!facade.fetchReviewMetricsPage(true, 0, 1, rows, total),
               "disconnected source fails");
    MIB_EXPECT(rows.empty() && total == 0, "no stale rows after disconnect");
    std::filesystem::rename(moved, source);
#endif
    const auto modified = std::filesystem::last_write_time(source);
    std::filesystem::last_write_time(source, modified + std::chrono::seconds(2));
    MIB_EXPECT(!facade.fetchReviewImage(bridge::ReviewImageDataset::RecordedImage, 0, frame),
               "changed source fails");
    MIB_EXPECT(frame.data.empty(), "no stale pixels");
    std::filesystem::last_write_time(source, modified);
    facade.shutdown();
    MIB_EXPECT(bytes(source) == original, "source immutable after review");

    // Processed scientific columns and legacy full-read output remain identical.
    const auto processed = tmp / "processed.h5";
    {
        Hdf writer;
        MIB_REQUIRE(writer.openFile(processed.string()) && writer.initializeDatasets(),
                    "processed fixture");
        std::vector<backend::services::ProcessedFrame> frames(5);
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i].index = 900 + i;
            frames[i].timestampNs = 8000 + i;
            frames[i].originalImage = cv::Mat(2, 2, CV_8UC1, cv::Scalar(11));
            frames[i].processedImage = cv::Mat(2, 2, CV_8UC1, cv::Scalar(255));
            frames[i].validation.area = 12.5 + i;
            frames[i].validation.deformability = 0.25;
            frames[i].validation.objectId = static_cast<int>(i);
            frames[i].validation.trackId = 100 + i;
            frames[i].validation.isValid = true;
        }
        MIB_REQUIRE(writer.appendFrames(frames, {}), "processed metadata");
    }
    MIB_REQUIRE(reader.loadFile(processed.string()), "processed read-only open");
    std::vector<backend::services::ProcessedFrame> full;
    MIB_REQUIRE(reader.readValidMetadata(full), "legacy projection");
    MIB_REQUIRE(reader.readMetadataPage(Dataset::Valid, 2, 2, page, total), "processed page");
    MIB_EXPECT(total == 5 && page.size() == 2, "processed page bound");
    MIB_EXPECT(page[0].index == full[2].index &&
                   page[0].validation.area == full[2].validation.area &&
                   page[1].validation.trackId == full[3].validation.trackId &&
                   page[1].validation.objectId == full[3].validation.objectId,
               "scientific projection parity");
    reader.closeFile();

    // A 100-million-row extent is cheap on disk; requesting the final row
    // must not allocate an array proportional to this extent.
    const auto huge = tmp / "huge.h5";
    {
        hid_t file = H5Fcreate(huge.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        hid_t group = H5Gcreate2(file, "/recorded_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        hsize_t extent[] = {100000000}, chunk[] = {128};
        hid_t space = H5Screate_simple(1, extent, nullptr);
        hid_t properties = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(properties, 1, chunk);
        struct Entry {
            uint64_t index, timestamp;
        } entry{123456789, 987654321};
        hid_t type = H5Tcreate(H5T_COMPOUND, sizeof(Entry));
        H5Tinsert(type, "index", HOFFSET(Entry, index), H5T_NATIVE_UINT64);
        H5Tinsert(type, "timestampNs", HOFFSET(Entry, timestamp), H5T_NATIVE_UINT64);
        hid_t dataset =
            H5Dcreate2(group, "metadata", type, space, H5P_DEFAULT, properties, H5P_DEFAULT);
        hsize_t start[] = {99999999}, count[] = {1};
        H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t memory = H5Screate_simple(1, count, nullptr);
        MIB_REQUIRE(H5Dwrite(dataset, type, memory, space, H5P_DEFAULT, &entry) >= 0,
                    "sparse final row");
        H5Sclose(memory);
        H5Dclose(dataset);
        H5Tclose(type);
        H5Pclose(properties);
        H5Sclose(space);
        H5Gclose(group);
        H5Fclose(file);
    }
    MIB_REQUIRE(reader.loadFile(huge.string()), "sparse source open");
    MIB_REQUIRE(reader.readMetadataPage(Dataset::Recorded, 99999999, 1, page, total),
                "sparse final page");
    MIB_EXPECT(total == 100000000 && page.size() == 1 && page[0].index == 123456789 &&
                   page[0].timestampNs == 987654321,
               "bounded sparse metadata identities");
    reader.closeFile();

    // Malformed input shape must fail before passing a one-element dims array
    // to HDF5 (rank > 1 used to overwrite stack memory).
    const auto bad = tmp / "bad.h5";
    hid_t file = H5Fcreate(bad.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t group = H5Gcreate2(file, "/valid_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims[] = {2, 2};
    hid_t space = H5Screate_simple(2, dims, nullptr);
    hid_t dataset = H5Dcreate2(group, "metadata", H5T_NATIVE_UINT64, space, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT);
    H5Dclose(dataset);
    H5Sclose(space);
    H5Gclose(group);
    H5Fclose(file);
    MIB_REQUIRE(reader.loadFile(bad.string()), "open malformed fixture");
    MIB_EXPECT(!reader.readMetadataPage(Dataset::Valid, 0, 1, page, total),
               "malformed rank rejected");
    reader.closeFile();
    return mib::test::exitCode();
}
