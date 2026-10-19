// facade_review_session_test (#512 S5, plan 2026-10-18-one-review-implementation)
//
// The facade's review surface on its own ReviewSession:
//  - characterization: load (result, RecordingStatusEvent, operation and
//    error events), metadata, metrics pages, images, run accounting
//    ("review"), charts JSON and the export's default source equal the
//    pre-slice facade code, kept here as the oracle (it read a file loaded
//    into AppBackend::hdf5(); the oracle reads its own Hdf5Service with the
//    same calls), on experiment, legacy, recording, empty-set,
//    unreadable-metadata and non-HDF5 fixtures, including failed loads that
//    replace an open file;
//  - the writer's handle (AppBackend::hdf5()) is untouched by load and close:
//    a load beside an open writer handle is refused only by the guards (the
//    raw-recording and experiment guards are exercised with a live camera in
//    backend.facade_boundary), and the writer keeps writing afterwards;
//  - decision D1 (a): while a review file is open the readiness gate
//    `storage.hdf5` fails with the same id, status, reason and remediation as
//    a file in the shared handle, and Start is refused with the same message;
//    after Close File (or a failed load, or shutdown) the gate passes again;
//    a metadata read that throws after the session's reader opened (a
//    malformed background) leaves the D1 flag equal to the session's state
//    (the load fails closed: open() is all or nothing);
//  - charts never pair one file's identity with another's rows while loads
//    replace the file on another thread (one session snapshot per request).
// Watchdog guarded.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/processing/IProcessingKernel.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/review/ReviewCharts.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <hdf5.h>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace backend::bridge;
using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace
{
    // ---- fixtures --------------------------------------------------------------
    cv::Mat pattern(uint64_t index, int offset = 0)
    {
        cv::Mat m(24, 32, CV_8UC1);
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x)
                m.at<uint8_t>(y, x) = static_cast<uint8_t>((x * 3 + y * 5 + static_cast<int>(index) * 7 + offset) % 200);
        return m;
    }

    cv::Mat ringMask()
    {
        cv::Mat m = cv::Mat::zeros(24, 32, CV_8UC1);
        cv::circle(m, cv::Point(16, 12), 9, cv::Scalar(255), -1);
        cv::circle(m, cv::Point(16, 12), 4, cv::Scalar(0), -1);
        return m;
    }

    ProcessedFrame makeFrame(uint64_t idx, bool valid, bool passes, bool series)
    {
        ProcessedFrame f;
        f.index = idx;
        f.timestampNs = (idx + 1) * 1000ULL;
        f.originalImage = pattern(idx, valid ? 0 : 50);
        f.processedImage = ringMask();
        auto &v = f.validation;
        v.isValid = passes;
        v.isTargetGroup = valid && idx % 4 == 0;
        v.hasSingleInnerContour = true;
        v.objectId = static_cast<int>(idx);
        v.objectCount = 1;
        v.trackId = static_cast<int>(idx / 2);
        v.centroidX = 3.25 + static_cast<double>(idx);
        v.centroidY = 7.5 - static_cast<double>(idx);
        v.area = 100.0 + static_cast<double>(idx);
        v.deformability = 0.1 + 0.01 * static_cast<double>(idx);
        v.areaRatio = 1.01 + 0.001 * static_cast<double>(idx);
        v.ringRatio = 18.0 + 0.37 * static_cast<double>(idx);
        v.youngsModulus = 1.5 + 0.1 * static_cast<double>(idx);
        v.analysisPixelToMicronFactor = 0.25;
        if (series)
            for (int s = 0; s < 3; ++s) f.seriesImages.push_back(pattern(idx, 100 + s * 17));
        return f;
    }

    void writeExperiment(const fs::path &path, int nValid, int nInvalid, bool extras)
    {
        std::vector<ProcessedFrame> valid, invalid;
        for (int i = 0; i < nValid; ++i) valid.push_back(makeFrame(uint64_t(i * 2), true, i != 3 && i != 7, extras));
        for (int i = 0; i < nInvalid; ++i) invalid.push_back(makeFrame(uint64_t(i * 2 + 1), false, false, false));
        Hdf5Service h;
        MIB_REQUIRE(h.openFile(path.string()) && h.initializeDatasets(), "fixture open");
        MIB_REQUIRE(h.appendFrames(valid, invalid), "fixture frames");
        backend::services::ProcessingConfig cfg;
        cfg.ring_ratio_min = 12.0;
        cfg.ring_ratio_max = 30.0;
        const cv::Mat bg(24, 32, CV_8UC1, cv::Scalar(42));
        backend::processing::ProcessingCoreIdentity core;
        core.version = "9.9.9";
        core.source = "test";
        core.releaseTag = "tag-x";
        MIB_REQUIRE(h.writeExperimentInfo(1000, 5000, valid.size(), invalid.size(), cfg, {4, 2, 20, 16},
                                          extras ? &bg : nullptr, extras ? &core : nullptr),
                    "fixture experiment info");
        if (extras)
        {
            MIB_REQUIRE(h.writeRunSnapshotJson("{\"schema\":1,\"pixel_to_micron\":0.25}", "{}"), "fixture snapshot");
            backend::recording::RecordingAccountingSnapshot a;
            a.admitted = 14;
            a.processed = 10;
            a.scientificallyRejected = 4;
            a.persistenceAdmitted = 14;
            a.persistenceCommitted = 14;
            a.completion = backend::recording::RunCompletionState::Complete;
            a.reconciled = true;
            MIB_REQUIRE(h.writeRunAccounting(a), "fixture accounting");
        }
        h.closeFile();
    }

    void writeRecording(const fs::path &path, int n, int multi)
    {
        Hdf5Service h;
        MIB_REQUIRE(h.openFile(path.string()) && h.initializeRecordingDatasets(), "recording fixture open");
        std::vector<cv::Mat> images;
        std::vector<Hdf5Service::RecordingFrameMeta> meta;
        for (int i = 0; i < n; ++i)
        {
            images.push_back(pattern(uint64_t(i)));
            meta.push_back({uint64_t(i), uint64_t(i) * 100, 32, 24});
        }
        MIB_REQUIRE(h.appendRecordingFrames(images, meta), "recording fixture frames");
        MIB_REQUIRE(h.writeRecordingInfo(10, 20, n, 2, multi > 1, multi), "recording fixture info");
        h.closeFile();
    }

    // A background dataset whose height does not fit an int: cv::Mat throws
    // while the session reads the metadata, after its reader has opened.
    void addMalformedBackground(const fs::path &path)
    {
        const hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open background fault fixture");
        hsize_t dims[3] = {1, hsize_t{1} << 31, 1};
        hsize_t chunk[3] = {1, 1024, 1};
        const hid_t space = H5Screate_simple(3, dims, nullptr);
        const hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_chunk(dcpl, 3, chunk);
        const hid_t ds = H5Dcreate2(file, "/experiment_info/background", H5T_NATIVE_UINT8, space, H5P_DEFAULT, dcpl,
                                    H5P_DEFAULT);
        MIB_REQUIRE(ds >= 0, "malformed background dataset");
        H5Dclose(ds);
        H5Pclose(dcpl);
        H5Sclose(space);
        H5Fclose(file);
    }

    void makeValidMetadataUnreadable(const fs::path &path)
    {
        const hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "open unreadable fixture");
        MIB_REQUIRE(H5Ldelete(file, "/valid_frames/metadata", H5P_DEFAULT) >= 0, "delete valid metadata");
        hsize_t dims[1] = {3};
        const hid_t space = H5Screate_simple(1, dims, nullptr);
        const hid_t ds = H5Dcreate2(file, "/valid_frames/metadata", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT);
        const int data[3] = {1, 2, 3};
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
        H5Dclose(ds);
        H5Sclose(space);
        H5Fclose(file);
    }

    // ---- the oracle: the pre-slice facade code (develop ee37c3fd) ---------------
    // BackendFacade.cpp handleRecordingLoadCommand / fetchReviewMetadata /
    // fetchReviewMetricsPage / fetchReviewImage / fetchRunAccountingJson
    // ("review") and ReviewCharts.cpp fetchReviewChartsJson, with the shared
    // handle replaced by the oracle's own reader and loadedRecordingPath_ by
    // `path`.
    struct Oracle
    {
        Hdf5Service hdf5;
        std::string path;
        bool metricsLoaded{false};
        std::vector<ProcessedFrame> validMeta, invalidMeta;

        bool load(const std::string &filePath, bool readMetadata, RecordingStatusEvent &event)
        {
            path.clear();
            metricsLoaded = false;
            validMeta.clear();
            invalidMeta.clear();
            if (hdf5.isFileOpen()) hdf5.closeFile();
            if (!hdf5.loadFile(filePath))
            {
                event = RecordingStatusEvent{RecordingState::Error, filePath, false, 0, 0, 0, 0};
                return false;
            }
            event = RecordingStatusEvent{};
            event.state = RecordingState::Loaded;
            event.filePath = filePath;
            if (readMetadata)
            {
                event.recordingFile = hdf5.isRecordingFile();
                if (event.recordingFile)
                {
                    std::uint64_t startTimeNs = 0, endTimeNs = 0, totalFrames = 0, filteredFrames = 0;
                    if (hdf5.readRecordingInfo(startTimeNs, endTimeNs, totalFrames, filteredFrames))
                    {
                        event.framesWritten = totalFrames;
                        event.framesFiltered = filteredFrames;
                    }
                    std::vector<ProcessedFrame> frames;
                    if (hdf5.readRecordingMetadata(frames)) event.loadedValidFrames = frames.size();
                }
                else
                {
                    std::vector<ProcessedFrame> validFrames, invalidFrames;
                    if (hdf5.readValidMetadata(validFrames)) event.loadedValidFrames = validFrames.size();
                    if (hdf5.readInvalidMetadata(invalidFrames)) event.loadedInvalidFrames = invalidFrames.size();
                }
            }
            path = filePath;
            return true;
        }

        void close()
        {
            hdf5.closeFile();
            path.clear();
            metricsLoaded = false;
            validMeta.clear();
            invalidMeta.clear();
        }

        BackendReviewMetadata metadata()
        {
            BackendReviewMetadata out;
            out.fileOpen = hdf5.isFileOpen();
            if (!out.fileOpen) return out;
            out.filePath = path;
            if (out.filePath.empty())
            {
                out.fileOpen = false;
                return out;
            }
            out.recordingFile = hdf5.isRecordingFile();
            if (out.recordingFile)
            {
                std::uint64_t total = 0, filtered = 0;
                hdf5.readRecordingInfo(out.startTimeNs, out.endTimeNs, total, filtered);
                out.totalValid = total;
            }
            else
            {
                std::size_t totalValid = 0, totalInvalid = 0;
                hdf5.readExperimentInfo(out.startTimeNs, out.endTimeNs, totalValid, totalInvalid, &out.roi);
                out.totalValid = totalValid;
                out.totalInvalid = totalInvalid;
            }
            backend::processing::ProcessingCoreIdentity identity;
            if (hdf5.readProcessingCoreIdentity(identity))
            {
                out.hasCoreIdentity = true;
                out.coreVersion = identity.version;
                out.coreSource = identity.source;
                out.coreReleaseTag = identity.releaseTag;
            }
            {
                cv::Mat bg;
                out.hasBackground = hdf5.readBackgroundImage(bg) && !bg.empty();
            }
            auto fillInfo = [this](const char *p, BackendReviewDatasetInfo &info) {
                std::size_t count = 0;
                info.present = hdf5.getDatasetInfo(p, count, info.height, info.width, info.channels);
                info.count = count;
            };
            fillInfo("/valid_frames/images", out.validImages);
            fillInfo("/invalid_frames/images", out.invalidImages);
            fillInfo("/valid_frames/masks", out.validMasks);
            fillInfo("/invalid_frames/masks", out.invalidMasks);
            fillInfo("/recorded_frames/images", out.recordedImages);
            return out;
        }

        bool page(bool valid, std::uint64_t offset, std::uint64_t count, std::vector<MonitoringObjectRow> &rows,
                  std::uint64_t &totalOut)
        {
            if (!hdf5.isFileOpen()) return false;
            if (!metricsLoaded)
            {
                validMeta.clear();
                invalidMeta.clear();
                if (hdf5.isRecordingFile())
                    hdf5.readRecordingMetadata(validMeta);
                else
                {
                    hdf5.readValidMetadata(validMeta);
                    hdf5.readInvalidMetadata(invalidMeta);
                }
                metricsLoaded = true;
            }
            const auto &source = valid ? validMeta : invalidMeta;
            totalOut = source.size();
            rows.clear();
            if (offset >= source.size()) return true;
            const std::uint64_t end = std::min<std::uint64_t>(source.size(), offset + count);
            for (std::uint64_t i = offset; i < end; ++i)
            {
                const auto &v = source[i].validation;
                MonitoringObjectRow row;
                row.frameIndex = source[i].index;
                row.timestampNs = source[i].timestampNs;
                row.valid = v.isValid;
                row.targetGroup = v.isTargetGroup;
                row.objectId = v.objectId;
                row.objectCount = v.objectCount;
                row.trackId = v.trackId;
                row.centroidX = v.centroidX;
                row.centroidY = v.centroidY;
                row.area = v.area;
                row.deformability = v.deformability;
                row.areaRatio = v.areaRatio;
                row.ringRatio = v.ringRatio;
                row.youngsModulus = v.youngsModulus;
                row.pixelToMicronFactor = v.analysisPixelToMicronFactor;
                rows.push_back(row);
            }
            return true;
        }

        bool image(ReviewImageDataset dataset, std::uint64_t index, BackendFrame &out)
        {
            if (!hdf5.isFileOpen()) return false;
            const char *p = nullptr;
            switch (dataset)
            {
            case ReviewImageDataset::ValidImage: p = "/valid_frames/images"; break;
            case ReviewImageDataset::InvalidImage: p = "/invalid_frames/images"; break;
            case ReviewImageDataset::RecordedImage: p = "/recorded_frames/images"; break;
            case ReviewImageDataset::ValidMask: p = "/valid_frames/masks"; break;
            case ReviewImageDataset::InvalidMask: p = "/invalid_frames/masks"; break;
            }
            if (!p) return false;
            cv::Mat image;
            if (!hdf5.readImageByIndex(p, index, image) || image.empty()) return false;
            cv::Mat gray;
            if (image.channels() == 1)
                gray = image;
            else
                cv::extractChannel(image, gray, 0);
            out = BackendFrame{};
            out.frameIndex = index;
            out.width = static_cast<std::uint64_t>(gray.cols);
            out.height = static_cast<std::uint64_t>(gray.rows);
            out.pixelFormat = 0;
            out.strideBytes = static_cast<std::size_t>(gray.cols);
            out.data.resize(static_cast<std::size_t>(gray.cols) * gray.rows);
            for (int row = 0; row < gray.rows; ++row)
                std::memcpy(out.data.data() + static_cast<std::size_t>(row) * gray.cols, gray.ptr(row),
                            static_cast<std::size_t>(gray.cols));
            return true;
        }

        static nlohmann::json accountingToJson(const backend::recording::RecordingAccountingSnapshot &a)
        {
            namespace recording = backend::recording;
            return {
                {"completion", static_cast<int>(a.completion)},
                {"completion_name", recording::toString(a.completion)},
                {"completion_reason", a.completionReason},
                {"reconciled", a.reconciled},
                {"admitted", a.admitted},
                {"empty", a.empty},
                {"processed", a.processed},
                {"scientifically_rejected", a.scientificallyRejected},
                {"processing_failed", a.processingFailed},
                {"store_overwritten", a.storeOverwritten},
                {"store_not_committed", a.storeNotCommitted},
                {"store_malformed", a.storeMalformed},
                {"cancelled_by_policy", a.cancelledByPolicy},
                {"pending_at_stop", a.pendingAtStop},
                {"sequence_gaps", a.sequenceGaps},
                {"objects_detected", a.objectsDetected},
                {"persistence_admitted", a.persistenceAdmitted},
                {"persistence_committed", a.persistenceCommitted},
                {"persistence_failed", a.persistenceFailed},
                {"persistence_pending_at_stop", a.persistencePendingAtStop},
                {"fatal_error", a.fatalError},
                {"fatal_message", a.fatalMessage},
                {"malformed_warn_fraction", recording::kMalformedWarnFraction},
            };
        }

        std::string accounting()
        {
            const std::string source = "review";
            if (path.empty() || !hdf5.isFileOpen())
                return nlohmann::json{{"available", false}, {"source", source}, {"error", "no file is open for review"}}.dump();
            backend::recording::RecordingAccountingSnapshot a;
            nlohmann::json j;
            const bool recorded = !hdf5.isRecordingFile() && hdf5.readRunAccounting(a);
            j = recorded ? accountingToJson(a) : nlohmann::json::object();
            j["available"] = true;
            j["recorded"] = recorded;
            j["source"] = source;
            j["file_path"] = path;
            return j.dump();
        }

        std::string charts(double factor, double ringMin, double ringMax) const
        {
            try
            {
                const std::string source = path;
                if (source.empty()) throw std::runtime_error("No HDF file open");
                Hdf5Service reader;
                if (!reader.loadFile(source) || reader.isRecordingFile())
                    throw std::runtime_error("Charts require an experiment HDF file");
                std::vector<ProcessedFrame> rows;
                const auto present = reader.metadataDatasetPresent(true);
                if (!present || (*present && !reader.readValidMetadata(rows)))
                    throw std::runtime_error("Cannot read valid-frame metrics");
                return backend::review::reviewChartsJson(rows, source, factor, ringMin, ringMax);
            }
            catch (const std::exception &error)
            {
                return backend::review::reviewChartsErrorJson(error.what());
            }
        }
    };

    // ---- comparisons -------------------------------------------------------------
    bool sameInfo(const BackendReviewDatasetInfo &a, const BackendReviewDatasetInfo &b)
    {
        return a.present == b.present && a.count == b.count && a.height == b.height && a.width == b.width &&
               a.channels == b.channels;
    }

    bool sameMetadata(const BackendReviewMetadata &a, const BackendReviewMetadata &b)
    {
        return a.fileOpen == b.fileOpen && a.recordingFile == b.recordingFile && a.startTimeNs == b.startTimeNs &&
               a.endTimeNs == b.endTimeNs && a.totalValid == b.totalValid && a.totalInvalid == b.totalInvalid &&
               a.roi.x == b.roi.x && a.roi.y == b.roi.y && a.roi.w == b.roi.w && a.roi.h == b.roi.h &&
               a.hasBackground == b.hasBackground && a.hasCoreIdentity == b.hasCoreIdentity &&
               a.coreVersion == b.coreVersion && a.coreSource == b.coreSource && a.coreReleaseTag == b.coreReleaseTag &&
               sameInfo(a.validImages, b.validImages) && sameInfo(a.invalidImages, b.invalidImages) &&
               sameInfo(a.validMasks, b.validMasks) && sameInfo(a.invalidMasks, b.invalidMasks) &&
               sameInfo(a.recordedImages, b.recordedImages) && a.filePath == b.filePath;
    }

    bool sameRows(const std::vector<MonitoringObjectRow> &a, const std::vector<MonitoringObjectRow> &b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const auto &x = a[i];
            const auto &y = b[i];
            // Bitwise equality for the doubles (NaN-safe, no tolerance).
            auto eq = [](double p, double q) { return std::memcmp(&p, &q, sizeof p) == 0; };
            if (x.frameIndex != y.frameIndex || x.timestampNs != y.timestampNs || x.valid != y.valid ||
                x.targetGroup != y.targetGroup || x.objectId != y.objectId || x.objectCount != y.objectCount ||
                x.trackId != y.trackId || !eq(x.centroidX, y.centroidX) || !eq(x.centroidY, y.centroidY) ||
                !eq(x.area, y.area) || !eq(x.deformability, y.deformability) || !eq(x.areaRatio, y.areaRatio) ||
                !eq(x.ringRatio, y.ringRatio) || !eq(x.youngsModulus, y.youngsModulus) ||
                !eq(x.pixelToMicronFactor, y.pixelToMicronFactor))
                return false;
        }
        return true;
    }

    bool sameFrame(const BackendFrame &a, const BackendFrame &b)
    {
        return a.captureSession == b.captureSession && a.storeGeneration == b.storeGeneration &&
               a.frameIndex == b.frameIndex && a.timestampNs == b.timestampNs && a.width == b.width &&
               a.height == b.height && a.pixelFormat == b.pixelFormat && a.strideBytes == b.strideBytes &&
               a.data == b.data;
    }

    struct Events
    {
        std::mutex m;
        std::vector<RecordingStatusEvent> recording;
        std::vector<OperationStatusEvent> loadOps;
        std::vector<BackendErrorEvent> errors;
        void clear()
        {
            std::lock_guard<std::mutex> lk(m);
            recording.clear();
            loadOps.clear();
            errors.clear();
        }
    };

    const backend::app::ReadinessGate *hdf5Gate(const backend::app::ExperimentReadinessSnapshot &r)
    {
        return r.gate("storage.hdf5");
    }

    bool sameGate(const backend::app::ReadinessGate *a, const backend::app::ReadinessGate *b)
    {
        return a && b && a->id == b->id && a->status == b->status && a->reason == b->reason &&
               a->remediation == b->remediation && a->detail == b->detail;
    }
} // namespace

int main()
{
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", "mock");
    _putenv_s("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator");
#else
    setenv("MIB_CAMERA_MODE", "mock", 1);
    setenv("MIB_DISABLED_SERVICES", "auto_update,autofocus,trigger,syringe_pump,pulse_generator", 1);
#endif
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("facade_review_session");
    const fs::path full = td.path() / "full.h5";
    const fs::path legacy = td.path() / "legacy.h5";
    const fs::path emptyValid = td.path() / "empty_valid.h5";
    const fs::path unreadable = td.path() / "unreadable.h5";
    const fs::path recMulti = td.path() / "rec_multi.h5";
    const fs::path recSingle = td.path() / "rec_single.h5";
    const fs::path notHdf5 = td.path() / "not_hdf5.h5";
    writeExperiment(full, 10, 4, true);
    writeExperiment(legacy, 6, 0, false);
    writeExperiment(emptyValid, 0, 3, false);
    writeExperiment(unreadable, 5, 2, true);
    makeValidMetadataUnreadable(unreadable);
    writeRecording(recMulti, 9, 3);
    writeRecording(recSingle, 5, 1);
    {
        std::ofstream(notHdf5) << "not an hdf5 file";
    }
    wd.mark("fixtures");

    backend::AppBackend backend;
    BackendFacade facade(backend);
    Events events;
    facade.setEventSink([&](const BackendEvent &event) {
        std::lock_guard<std::mutex> lk(events.m);
        if (const auto *r = std::get_if<RecordingStatusEvent>(&event)) events.recording.push_back(*r);
        if (const auto *op = std::get_if<OperationStatusEvent>(&event))
            if (op->kind == BackendOperationKind::RecordingLoad) events.loadOps.push_back(*op);
        if (const auto *e = std::get_if<BackendErrorEvent>(&event)) events.errors.push_back(*e);
    });
    MIB_REQUIRE(facade.initialize((td.path() / "data").string()), "facade initialize");
    const std::string runOut = (td.path() / "run_out.h5").string();
    const auto factor = backend.processing().getPixelToMicronFactor();
    const auto cfg = backend.processing().getProcessingConfig();

    // The gate a file in the shared handle produces (the pre-slice path of a
    // loaded review file), captured from the real coordinator: the review
    // source must reproduce it field for field.
    backend::app::ReadinessGate sharedHandleGate;
    {
        const fs::path writer = td.path() / "shared_probe.h5";
        MIB_REQUIRE(backend.hdf5().openFile(writer.string()), "open the shared handle");
        const auto r = backend.experiment().evaluateReadiness(runOut);
        MIB_REQUIRE(hdf5Gate(r) != nullptr, "storage.hdf5 gate present");
        sharedHandleGate = *hdf5Gate(r);
        backend.hdf5().closeFile();
        MIB_EXPECT(sharedHandleGate.status == backend::app::GateStatus::Fail &&
                       sharedHandleGate.reason == "an HDF5 file is already open" &&
                       sharedHandleGate.remediation == "finish or close the current file first" &&
                       sharedHandleGate.detail.empty(),
                   "the shared-handle gate is the documented one");
        const auto pass = backend.experiment().evaluateReadiness(runOut);
        MIB_EXPECT(hdf5Gate(pass) && hdf5Gate(pass)->status == backend::app::GateStatus::Pass, "gate passes when closed");
    }
    wd.mark("shared handle gate");

    // ---- characterization against the oracle -------------------------------------
    Oracle oracle;
    auto checkSurface = [&](const std::string &label) {
        BackendReviewMetadata got;
        MIB_REQUIRE(facade.fetchReviewMetadata(got), label + ": metadata");
        MIB_EXPECT(sameMetadata(got, oracle.metadata()), label + ": metadata equals the pre-slice facade");
        for (const bool valid : {true, false})
        {
            const std::uint64_t n = valid ? (got.recordingFile ? got.totalValid : got.validImages.count)
                                          : got.invalidImages.count;
            const std::vector<std::pair<std::uint64_t, std::uint64_t>> pages{
                {0, 5}, {2, 3}, {0, 100000}, {n ? n - 1 : 0, 10}, {n, 5}, {1000000, 1}, {1, 0}};
            for (const auto &[offset, count] : pages)
            {
                std::vector<MonitoringObjectRow> rows, want;
                std::uint64_t total = 777, wantTotal = 777;
                const bool ok = facade.fetchReviewMetricsPage(valid, offset, count, rows, total);
                const bool wantOk = oracle.page(valid, offset, count, want, wantTotal);
                MIB_EXPECT(ok == wantOk && total == wantTotal && sameRows(rows, want),
                           label + ": page valid=" + std::to_string(valid) + " offset=" + std::to_string(offset) +
                               " count=" + std::to_string(count));
            }
        }
        const BackendReviewDatasetInfo *infos[5] = {&got.validImages, &got.invalidImages, &got.recordedImages,
                                                    &got.validMasks, &got.invalidMasks};
        for (int ds = 0; ds < 5; ++ds)
        {
            const std::uint64_t n = infos[ds]->count;
            for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}, n ? n - 1 : 0, n, n + 1})
            {
                BackendFrame image, want;
                const bool ok = facade.fetchReviewImage(static_cast<ReviewImageDataset>(ds), index, image);
                const bool wantOk = oracle.image(static_cast<ReviewImageDataset>(ds), index, want);
                MIB_EXPECT(ok == wantOk && (!ok || sameFrame(image, want)),
                           label + ": image dataset " + std::to_string(ds) + " index " + std::to_string(index));
            }
        }
        MIB_EXPECT(facade.fetchRunAccountingJson("review") == oracle.accounting(), label + ": review accounting");
        MIB_EXPECT(facade.fetchReviewChartsJson() == oracle.charts(factor, cfg.ring_ratio_min, cfg.ring_ratio_max),
                   label + ": charts JSON");
    };

    auto loadBoth = [&](const fs::path &path, bool readMetadata, const std::string &label) {
        events.clear();
        RecordingLoadCommand load;
        load.filePath = path.string();
        load.readMetadata = readMetadata;
        const auto result = facade.dispatch(load);
        RecordingStatusEvent want;
        const bool wantOk = oracle.load(path.string(), readMetadata, want);
        MIB_EXPECT(result.ok == wantOk && result.command == BackendCommandType::RecordingLoad &&
                       result.message == (wantOk ? "Recording loaded" : "Recording load failed") &&
                       result.operationId != 0,
                   label + ": load result");
        std::lock_guard<std::mutex> lk(events.m);
        MIB_EXPECT(events.recording.size() == 1, label + ": one RecordingStatusEvent");
        if (!events.recording.empty())
        {
            const auto &e = events.recording.front();
            MIB_EXPECT(e.state == want.state && e.filePath == want.filePath && e.recordingFile == want.recordingFile &&
                           e.framesWritten == want.framesWritten && e.framesFiltered == want.framesFiltered &&
                           e.loadedValidFrames == want.loadedValidFrames &&
                           e.loadedInvalidFrames == want.loadedInvalidFrames,
                       label + ": RecordingStatusEvent equals the pre-slice facade");
        }
        MIB_EXPECT(events.loadOps.size() == 2 && events.loadOps[0].state == BackendOperationState::Started &&
                       events.loadOps[0].operationId == result.operationId &&
                       events.loadOps[1].state ==
                           (wantOk ? BackendOperationState::Completed : BackendOperationState::Failed) &&
                       events.loadOps[1].message == (wantOk ? path.string() : std::string("Recording load failed")),
                   label + ": load operation events");
        MIB_EXPECT(events.errors.size() == (wantOk ? 0u : 1u) &&
                       (wantOk || (events.errors[0].source == BackendErrorSource::Recording &&
                                   events.errors[0].command == BackendCommandType::RecordingLoad &&
                                   events.errors[0].message == "Recording load failed")),
                   label + ": load error event");
        MIB_EXPECT(backend.reviewSourceOpen() == wantOk, label + ": D1 flag follows the open review source");
        return wantOk;
    };

    // Nothing loaded yet.
    checkSurface("no file");
    {
        const nlohmann::json request{{"output_root", (td.path() / "export_none").string()}, {"format", "metrics_csv"}};
        const auto refused = facade.submitReviewExportJson(request.dump());
        MIB_EXPECT(!refused.ok && refused.message == "No recording loaded to export", "export refused without a file");
    }
    for (const auto &[path, label] :
         std::vector<std::pair<fs::path, std::string>>{{full, "full"},
                                                        {legacy, "legacy"},
                                                        {emptyValid, "empty valid set"},
                                                        {unreadable, "unreadable valid metadata"},
                                                        {recMulti, "recording multi-image"},
                                                        {recSingle, "recording"}})
    {
        wd.mark(label.c_str());
        MIB_EXPECT(loadBoth(path, true, label), label + ": loads");
        checkSurface(label);
        // D1 (a): the review source fails the gate exactly as the shared handle did.
        const auto r = backend.experiment().evaluateReadiness(runOut);
        MIB_EXPECT(sameGate(hdf5Gate(r), &sharedHandleGate), label + ": storage.hdf5 identical to the shared-handle gate");
        MIB_EXPECT(!backend.hdf5().isFileOpen(), label + ": the shared handle stays closed");
    }
    MIB_EXPECT(loadBoth(full, false, "full without metadata"), "readMetadata=false loads");
    checkSurface("full without metadata");

    // The export's default source is the loaded file.
    {
        MIB_REQUIRE(loadBoth(full, true, "export source"), "load for export");
        const auto root = td.path() / "export_full";
        fs::create_directories(root);
        const nlohmann::json request{{"output_root", root.string()}, {"format", "metrics_csv"}};
        const auto started = facade.submitReviewExportJson(request.dump());
        MIB_REQUIRE(started.ok, "export of the loaded file starts: " + started.message);
        nlohmann::json status;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            status = nlohmann::json::parse(facade.fetchReviewExportStatusJson());
            if (status.value("operation_id", "") == std::to_string(started.operationId) && status["state"] != "running" &&
                facade.activeOperationCount() == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        MIB_EXPECT(status["state"] == "completed" && status["results"].size() == 1 &&
                       status["results"][0]["source_path"] == full.string(),
                   "export default source is the session's file: " + status.dump());
    }

    // Failed loads replace (close) the open file, as the shared handle did.
    for (const auto &[bad, label] : std::vector<std::pair<fs::path, std::string>>{
             {td.path() / "missing.h5", "missing"}, {notHdf5, "not HDF5"}, {fs::path(), "empty path"}})
    {
        wd.mark(label.c_str());
        MIB_REQUIRE(loadBoth(legacy, true, label + " (before)"), "good load before a bad one");
        MIB_EXPECT(!loadBoth(bad, true, label), label + ": refused");
        checkSurface(label + " after the failed load");
        const auto r = backend.experiment().evaluateReadiness(runOut);
        MIB_EXPECT(hdf5Gate(r) && hdf5Gate(r)->status == backend::app::GateStatus::Pass,
                   label + ": gate passes once the failed load closed the file");
    }
    wd.mark("failed loads");

    // ---- D1 when a read throws after the reader opened (#512 S5 review) -----------
    // Before the fix the exception left the session's reader open while the
    // flag was never set, and the coordinator mutex was released: Start or raw
    // recording could then open a writer beside it. The flag must equal the
    // session's state on every exit; open() rolls back, so the load fails
    // closed and Start is not held by a reader that is gone.
    {
        const fs::path bad = td.path() / "bad_background.h5";
        writeExperiment(bad, 4, 1, false);
        addMalformedBackground(bad);
        for (const bool afterGoodLoad : {false, true})
        {
            const std::string label = afterGoodLoad ? "throwing read after a good load" : "throwing read from closed";
            wd.mark(label.c_str());
            if (afterGoodLoad)
                MIB_REQUIRE(loadBoth(full, true, label + " (before)"), "good load first");
            else
                MIB_REQUIRE(facade.closeReview().ok && !backend.reviewSourceOpen(), "start closed");
            oracle.close();
            events.clear();
            RecordingLoadCommand load;
            load.filePath = bad.string();
            BackendCommandResult result;
            bool threw = false;
            try
            {
                result = facade.dispatch(load);
            }
            catch (...)
            {
                threw = true;
            }
            BackendReviewMetadata meta;
            MIB_REQUIRE(facade.fetchReviewMetadata(meta), label + ": metadata");
            MIB_EXPECT(backend.reviewSourceOpen() == meta.fileOpen,
                       label + ": the D1 flag equals the session's open state");
            const auto r = backend.experiment().evaluateReadiness(runOut);
            backend::app::ExperimentStartRequest request;
            request.outputPath = runOut;
            request.readinessGeneration = r.generation;
            const auto start = backend.experiment().start(request);
            const std::string writerConflict =
                "stop raw recording and close the existing HDF5 file before starting an experiment";
            if (meta.fileOpen)
            {
                MIB_EXPECT(sameGate(hdf5Gate(r), &sharedHandleGate) && start.message == writerConflict,
                           label + ": an open reader keeps Start refused");
            }
            MIB_EXPECT(!threw && !result.ok && result.message == "Recording load failed" && !meta.fileOpen,
                       label + ": the load fails closed (open() all or nothing)");
            MIB_EXPECT(hdf5Gate(r) && hdf5Gate(r)->status == backend::app::GateStatus::Pass &&
                           start.message != writerConflict && !start.started(),
                       label + ": nothing holds Start once the reader is closed");
            std::lock_guard<std::mutex> lk(events.m);
            MIB_EXPECT(events.recording.size() == 1 && events.recording[0].state == RecordingState::Error &&
                           events.loadOps.size() == 2 && events.loadOps[1].state == BackendOperationState::Failed,
                       label + ": failed-load events");
        }
    }
    wd.mark("throwing read");

    // ---- charts never mix two files while loads replace them (#512 S5 review) ------
    // One session snapshot per request: every charts JSON is one of the three
    // whole answers (full's, legacy's, the recording refusal), never full's
    // identity with legacy's rows or a recording slipping past the check.
    {
        auto chartsOf = [&](const fs::path &path) {
            RecordingLoadCommand c;
            c.filePath = path.string();
            MIB_REQUIRE(facade.dispatch(c).ok, "load " + path.string());
            return facade.fetchReviewChartsJson();
        };
        const std::string fullCharts = chartsOf(full);
        const std::string legacyCharts = chartsOf(legacy);
        const std::string recordingCharts = chartsOf(recMulti);
        MIB_REQUIRE(nlohmann::json::parse(fullCharts).value("rows", "") == "10" &&
                        nlohmann::json::parse(legacyCharts).value("rows", "") == "6" &&
                        recordingCharts == backend::review::reviewChartsErrorJson("Charts require an experiment HDF file"),
                    "the three answers");
        MIB_REQUIRE(facade.dispatch(RecordingLoadCommand{full.string(), true}).ok, "start from full");
        // Handshake: the loader starts its loop only after the first fetch, so
        // at least one fetch runs however the threads are scheduled.
        std::atomic<bool> go{false};
        std::atomic<bool> done{false};
        std::thread loader([&] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i)
                for (const fs::path *p : {&legacy, &recMulti, &full})
                    facade.dispatch(RecordingLoadCommand{p->string(), true});
            done = true;
        });
        int fetched = 0;
        int mixed = 0;
        do
        {
            const auto charts = facade.fetchReviewChartsJson();
            ++fetched;
            if (charts != fullCharts && charts != legacyCharts && charts != recordingCharts) ++mixed;
            go = true;
        } while (!done.load());
        loader.join();
        std::fprintf(stderr, "charts interleave: %d fetches, %d mixed\n", fetched, mixed);
        MIB_EXPECT(fetched >= 1 && mixed == 0, "every charts answer belongs to one file");
        events.clear();
    }
    wd.mark("charts interleave");

    // ---- D1 (a): Start refused while a review file is open, same message ---------
    {
        MIB_REQUIRE(loadBoth(full, true, "D1"), "load for D1");
        backend::app::ExperimentStartRequest request;
        request.outputPath = runOut;
        request.readinessGeneration = backend.experiment().readinessGeneration();
        const auto refused = backend.experiment().start(request);
        MIB_EXPECT(refused.outcome == backend::app::ExperimentStartOutcome::NotReady &&
                       refused.message ==
                           "stop raw recording and close the existing HDF5 file before starting an experiment",
                   "Start refused with the pre-slice message: " + refused.message);
        MIB_EXPECT(sameGate(hdf5Gate(refused.readiness), &sharedHandleGate), "Start's readiness carries the same gate");
        MIB_EXPECT(!fs::exists(runOut), "no output created by the refused Start");

        const auto closed = facade.closeReview();
        MIB_EXPECT(closed.ok && closed.message == "Review file closed" &&
                       closed.command == BackendCommandType::RecordingLoad,
                   "close review");
        oracle.close();
        MIB_EXPECT(!backend.reviewSourceOpen(), "close lifts the D1 flag");
        checkSurface("closed");
        const auto again = facade.closeReview();
        MIB_EXPECT(again.ok && again.message == "Review file closed", "closing twice is fine");
        const auto r = backend.experiment().evaluateReadiness(runOut);
        MIB_EXPECT(hdf5Gate(r) && hdf5Gate(r)->status == backend::app::GateStatus::Pass &&
                       hdf5Gate(r)->reason.empty(),
                   "gate passes after Close File");
        request.readinessGeneration = r.generation;
        const auto after = backend.experiment().start(request);
        MIB_EXPECT(after.message != refused.message, "Start is no longer refused for the review file: " + after.message);
        MIB_EXPECT(!after.started(), "no camera session here: Start is refused by its own gates");
    }
    wd.mark("D1");

    // ---- the writer's handle is untouched by load and close ------------------------
    // A writer handle open with the experiment idle and no raw recording (the
    // guards pass): load and close succeed and never close or replace it.
    {
        const fs::path writerPath = td.path() / "writer.h5";
        auto &writer = backend.hdf5();
        MIB_REQUIRE(writer.openFile(writerPath.string()) && writer.initializeDatasets(), "open the writer handle");
        MIB_EXPECT(loadBoth(full, true, "beside a writer"), "load is refused only by the guards");
        MIB_EXPECT(writer.isFileOpen(), "load leaves the writer handle open");
        BackendReviewMetadata meta;
        MIB_EXPECT(facade.fetchReviewMetadata(meta) && meta.fileOpen && meta.filePath == full.string() &&
                       meta.totalValid == 10,
                   "review reads the review file, not the writer's");
        MIB_EXPECT(loadBoth(recMulti, true, "replace beside a writer"), "replacing the review file");
        MIB_EXPECT(writer.isFileOpen(), "replacing the review file leaves the writer open");
        MIB_EXPECT(facade.closeReview().ok, "close beside a writer");
        MIB_EXPECT(writer.isFileOpen(), "close leaves the writer handle open");
        MIB_EXPECT(!backend.reviewSourceOpen(), "flag cleared, writer still counts for the gate");
        const auto r = backend.experiment().evaluateReadiness(runOut);
        MIB_EXPECT(sameGate(hdf5Gate(r), &sharedHandleGate), "the writer handle still fails storage.hdf5");
        // The same writer keeps writing: frames appended after load/close land in its file.
        std::vector<ProcessedFrame> frames{makeFrame(0, true, true, false), makeFrame(2, true, true, false)};
        MIB_EXPECT(writer.appendFrames(frames, {}), "writer appends after load/close");
        writer.closeFile();
        Hdf5Service reader;
        std::vector<ProcessedFrame> written;
        MIB_EXPECT(reader.loadFile(writerPath.string()) && reader.readValidMetadata(written) && written.size() == 2,
                   "the writer's file holds the appended frames");
        reader.closeFile();
    }
    wd.mark("writer untouched");

    // ---- shutdown closes the review source and lifts the flag ----------------------
    MIB_REQUIRE(loadBoth(full, true, "before shutdown"), "load before shutdown");
    facade.shutdown();
    MIB_EXPECT(!backend.reviewSourceOpen(), "shutdown lifts the D1 flag");
    {
        BackendReviewMetadata meta;
        MIB_EXPECT(!facade.fetchReviewMetadata(meta), "no review pulls after shutdown");
    }
    return mib::test::exitCode();
}
