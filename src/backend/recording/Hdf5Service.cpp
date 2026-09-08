#include "backend/recording/Hdf5Service.h"
#include "backend/diagnostics/CrashStateMirror.h"
#include "backend/processing/ProcessingService.h"

#include <spdlog/spdlog.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

// HDF5 C API (more widely available than C++ API)
#include <hdf5.h>
#include <hdf5_hl.h>

#include <vector>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace backend::services
{
    namespace
    {
        PerformanceTraceFn g_performanceTraceHook;

        void tracePerformance(std::string_view name, std::string_view operation,
                               double durationMs, std::string_view jsonData)
        {
            if (g_performanceTraceHook)
                g_performanceTraceHook(name, operation, durationMs, jsonData);
        }
    }

    void setHdf5PerformanceTraceHook(PerformanceTraceFn fn)
    {
        g_performanceTraceHook = std::move(fn);
    }

    struct Hdf5Service::Impl
    {
        hid_t fileId_{H5I_INVALID_HID};
        std::string filePath_;
        bool isOpen_{false};
        bool writable_{false};
        bool datasetsInitialized_{false};
        hsize_t validFramesWritten_{0};
        hsize_t invalidFramesWritten_{0};
        hsize_t seriesImagesWritten_{0}; // tracks /valid_frames/series_images row count

        // Time-interval flush state: append paths flush at most once per
        // flushInterval_ instead of every batch, so hot-path I/O stays cheap.
        std::chrono::steady_clock::time_point lastIntervalFlush_{};
        std::chrono::milliseconds flushInterval_{5000};

        ~Impl()
        {
            if (fileId_ != H5I_INVALID_HID)
            {
                if (writable_)
                {
                    H5Fflush(fileId_, H5F_SCOPE_GLOBAL);
                }
                H5Fclose(fileId_);
                fileId_ = H5I_INVALID_HID;
            }
        }
    };

    namespace
    {
        // File-access property list used for all opens. Sets strong close so a
        // leaked HDF5 ID cannot prevent finalization, and disables HDF5's
        // byte-range file locking: that lock is unreliable on network/NAS shares
        // (and on Windows can block concurrent access). This app owns the file
        // exclusively, so disabling it is safe. Returns H5P_DEFAULT on failure
        // (caller must not close H5P_DEFAULT).
        hid_t createFileAccessPropertyList()
        {
            hid_t fileAccessId = H5Pcreate(H5P_FILE_ACCESS);
            if (fileAccessId < 0)
            {
                SPDLOG_WARN("Failed to create HDF5 file access property list; using defaults");
                return H5P_DEFAULT;
            }

            if (H5Pset_fclose_degree(fileAccessId, H5F_CLOSE_STRONG) < 0)
            {
                SPDLOG_WARN("Failed to set HDF5 strong close degree; using default close behavior");
                H5Pclose(fileAccessId);
                return H5P_DEFAULT;
            }

            // H5Pset_file_locking was added after the HDF5 1.10.5 baseline
            // used by manylinux_2_28. Keep portable wheel builds compatible;
            // older runtimes retain their default locking behavior.
#if H5_VERSION_GE(1, 10, 7)
            H5Pset_file_locking(fileAccessId, /*use_file_locking=*/0, /*ignore_when_disabled=*/1);
#endif

            return fileAccessId;
        }

        void closePropertyList(hid_t propertyListId)
        {
            if (propertyListId != H5P_DEFAULT && propertyListId != H5I_INVALID_HID)
            {
                H5Pclose(propertyListId);
            }
        }

        struct ProcessedFrameMetadataRecord
        {
            uint64_t index;
            uint64_t timestampNs;
            int32_t objectId;
            int32_t objectCount;
            int32_t trackId;
            uint64_t trackFirstFrame;
            uint64_t trackLastFrame;
            int32_t trackObservationCount;
            double bboxX;
            double bboxY;
            double bboxWidth;
            double bboxHeight;
            double centroidX;
            double centroidY;
            double deformability;
            double area;
            double areaRatio;
            double ringRatio;
            uint8_t isValid;
            uint8_t touchesBorder;
            uint8_t hasSingleInnerContour;
            uint8_t inRange;
            int32_t innerContourCount;
            double brightness_q1;
            double brightness_q2;
            double brightness_q3;
            double brightness_q4;
            double youngsModulus;
            uint8_t isTargetGroup;
        };

        hid_t createProcessedFrameMetadataType(bool includeBaseFields = true,
                                               bool includeObjectFields = true,
                                               bool includeTrackingFields = true)
        {
            hid_t compTypeId = H5Tcreate(H5T_COMPOUND, sizeof(ProcessedFrameMetadataRecord));
            if (includeBaseFields)
            {
                H5Tinsert(compTypeId, "index", HOFFSET(ProcessedFrameMetadataRecord, index), H5T_NATIVE_UINT64);
                H5Tinsert(compTypeId, "timestampNs", HOFFSET(ProcessedFrameMetadataRecord, timestampNs), H5T_NATIVE_UINT64);
                H5Tinsert(compTypeId, "deformability", HOFFSET(ProcessedFrameMetadataRecord, deformability), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "area", HOFFSET(ProcessedFrameMetadataRecord, area), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "areaRatio", HOFFSET(ProcessedFrameMetadataRecord, areaRatio), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "ringRatio", HOFFSET(ProcessedFrameMetadataRecord, ringRatio), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "isValid", HOFFSET(ProcessedFrameMetadataRecord, isValid), H5T_NATIVE_UINT8);
                H5Tinsert(compTypeId, "touchesBorder", HOFFSET(ProcessedFrameMetadataRecord, touchesBorder), H5T_NATIVE_UINT8);
                H5Tinsert(compTypeId, "hasSingleInnerContour", HOFFSET(ProcessedFrameMetadataRecord, hasSingleInnerContour), H5T_NATIVE_UINT8);
                H5Tinsert(compTypeId, "inRange", HOFFSET(ProcessedFrameMetadataRecord, inRange), H5T_NATIVE_UINT8);
                H5Tinsert(compTypeId, "innerContourCount", HOFFSET(ProcessedFrameMetadataRecord, innerContourCount), H5T_NATIVE_INT32);
                H5Tinsert(compTypeId, "brightness_q1", HOFFSET(ProcessedFrameMetadataRecord, brightness_q1), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "brightness_q2", HOFFSET(ProcessedFrameMetadataRecord, brightness_q2), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "brightness_q3", HOFFSET(ProcessedFrameMetadataRecord, brightness_q3), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "brightness_q4", HOFFSET(ProcessedFrameMetadataRecord, brightness_q4), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "youngsModulus", HOFFSET(ProcessedFrameMetadataRecord, youngsModulus), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "isTargetGroup", HOFFSET(ProcessedFrameMetadataRecord, isTargetGroup), H5T_NATIVE_UINT8);
            }
            if (includeObjectFields)
            {
                H5Tinsert(compTypeId, "objectId", HOFFSET(ProcessedFrameMetadataRecord, objectId), H5T_NATIVE_INT32);
                H5Tinsert(compTypeId, "objectCount", HOFFSET(ProcessedFrameMetadataRecord, objectCount), H5T_NATIVE_INT32);
            }
            if (includeTrackingFields)
            {
                H5Tinsert(compTypeId, "trackId", HOFFSET(ProcessedFrameMetadataRecord, trackId), H5T_NATIVE_INT32);
                H5Tinsert(compTypeId, "trackFirstFrame", HOFFSET(ProcessedFrameMetadataRecord, trackFirstFrame), H5T_NATIVE_UINT64);
                H5Tinsert(compTypeId, "trackLastFrame", HOFFSET(ProcessedFrameMetadataRecord, trackLastFrame), H5T_NATIVE_UINT64);
                H5Tinsert(compTypeId, "trackObservationCount", HOFFSET(ProcessedFrameMetadataRecord, trackObservationCount), H5T_NATIVE_INT32);
                H5Tinsert(compTypeId, "bboxX", HOFFSET(ProcessedFrameMetadataRecord, bboxX), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "bboxY", HOFFSET(ProcessedFrameMetadataRecord, bboxY), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "bboxWidth", HOFFSET(ProcessedFrameMetadataRecord, bboxWidth), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "bboxHeight", HOFFSET(ProcessedFrameMetadataRecord, bboxHeight), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "centroidX", HOFFSET(ProcessedFrameMetadataRecord, centroidX), H5T_NATIVE_DOUBLE);
                H5Tinsert(compTypeId, "centroidY", HOFFSET(ProcessedFrameMetadataRecord, centroidY), H5T_NATIVE_DOUBLE);
            }
            return compTypeId;
        }

        ProcessedFrameMetadataRecord makeMetadataRecord(const ProcessedFrame &frame)
        {
            ProcessedFrameMetadataRecord md{};
            md.index = frame.index;
            md.timestampNs = frame.timestampNs;
            md.objectId = frame.validation.objectId;
            md.objectCount = frame.validation.objectCount;
            md.trackId = frame.validation.trackId;
            md.trackFirstFrame = frame.validation.trackFirstFrame;
            md.trackLastFrame = frame.validation.trackLastFrame;
            md.trackObservationCount = frame.validation.trackObservationCount;
            md.bboxX = frame.validation.bboxX;
            md.bboxY = frame.validation.bboxY;
            md.bboxWidth = frame.validation.bboxWidth;
            md.bboxHeight = frame.validation.bboxHeight;
            md.centroidX = frame.validation.centroidX;
            md.centroidY = frame.validation.centroidY;
            md.deformability = frame.validation.deformability;
            md.area = frame.validation.area;
            md.areaRatio = frame.validation.areaRatio;
            md.ringRatio = frame.validation.ringRatio;
            md.isValid = frame.validation.isValid ? 1 : 0;
            md.touchesBorder = frame.validation.touchesBorder ? 1 : 0;
            md.hasSingleInnerContour = frame.validation.hasSingleInnerContour ? 1 : 0;
            md.inRange = frame.validation.inRange ? 1 : 0;
            md.innerContourCount = frame.validation.innerContourCount;
            md.brightness_q1 = frame.validation.brightness.q1;
            md.brightness_q2 = frame.validation.brightness.q2;
            md.brightness_q3 = frame.validation.brightness.q3;
            md.brightness_q4 = frame.validation.brightness.q4;
            md.youngsModulus = frame.validation.youngsModulus;
            md.isTargetGroup = frame.validation.isTargetGroup ? 1 : 0;
            return md;
        }
    } // namespace

    Hdf5Service::Hdf5Service() : impl_(std::make_unique<Impl>())
    {
    }

    Hdf5Service::~Hdf5Service()
    {
        closeFile();
    }

    bool Hdf5Service::initialize(const std::string &rootDir)
    {
        SPDLOG_INFO("Hdf5Service initialized at {}", rootDir);
        return true;
    }

    bool Hdf5Service::openFile(const std::string &filePath)
    {
        if (impl_->isOpen_)
        {
            SPDLOG_WARN("HDF5 file already open: {}", impl_->filePath_);
            return false;
        }

        // Ensure the destination directory exists. Users frequently point the
        // save dialog at a folder that does not exist yet (e.g. a freshly chosen
        // path on a second/external/network drive). H5Fcreate cannot create
        // intermediate directories and only reports a generic failure, which is
        // why a save that works in an existing folder fails on a new one. Create
        // the parent tree here and surface a specific, actionable error.
        try
        {
            const std::filesystem::path parent =
                std::filesystem::path(filePath).parent_path();
            if (!parent.empty() && !std::filesystem::is_directory(parent))
            {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec && !std::filesystem::is_directory(parent))
                {
                    SPDLOG_ERROR("Cannot create destination folder for HDF5 file "
                                 "'{}': {} (code {}). Check the drive is connected "
                                 "and writable.",
                                 filePath, ec.message(), ec.value());
                    return false;
                }
            }
        }
        catch (const std::exception &ex)
        {
            SPDLOG_ERROR("Invalid HDF5 destination path '{}': {}", filePath, ex.what());
            return false;
        }

        // Create file, overwriting if it exists. The FAPL sets strong close (so a
        // leaked HDF5 ID cannot prevent finalization) and disables file locking
        // (unreliable on NAS/CIFS).
        const hid_t fileAccessId = createFileAccessPropertyList();
        impl_->fileId_ = H5Fcreate(filePath.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fileAccessId);
        closePropertyList(fileAccessId);
        if (impl_->fileId_ < 0)
        {
            SPDLOG_ERROR("Failed to create HDF5 file '{}'. Verify the drive is "
                         "available, the path is writable, the name is valid, and "
                         "there is enough free space.",
                         filePath);
            return false;
        }

        impl_->filePath_ = filePath;
        impl_->isOpen_ = true;
        impl_->writable_ = true;
        impl_->datasetsInitialized_ = false;
        // Configure interval flush (env override, default 5 s). Measure the
        // first interval from open so the first batch does not flush.
        impl_->flushInterval_ = std::chrono::milliseconds(5000);
        if (const char* env = std::getenv("MIB_HDF5_FLUSH_INTERVAL_MS"))
        {
            char* end = nullptr;
            const long parsed = std::strtol(env, &end, 10);
            if (end != env && parsed >= 0)
            {
                impl_->flushInterval_ = std::chrono::milliseconds(parsed);
            }
        }
        impl_->lastIntervalFlush_ = std::chrono::steady_clock::now();
        impl_->validFramesWritten_ = 0;
        impl_->invalidFramesWritten_ = 0;
        impl_->seriesImagesWritten_ = 0;
        {
            auto& m = backend::diagnostics::CrashStateMirror::instance();
            m.hdf5.fileOpen.store(true);
            m.hdf5.appendedValid.store(0);
            m.hdf5.appendedInvalid.store(0);
            m.setHdf5Path(filePath);
        }
        SPDLOG_INFO("HDF5 file opened: {}", filePath);
        return true;
    }

    void Hdf5Service::closeFile()
    {
        if (impl_->fileId_ != H5I_INVALID_HID && impl_->isOpen_)
        {
            bool flushOk = true;
            if (impl_->writable_)
            {
                const auto flushStart = std::chrono::steady_clock::now();
                const herr_t flushStatus = H5Fflush(impl_->fileId_, H5F_SCOPE_GLOBAL);
                const double flushMs = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - flushStart)
                                           .count();
                flushOk = flushStatus >= 0;
                if (flushOk)
                {
                    SPDLOG_INFO("HDF5 final flush completed: {} ({:.3f} ms)",
                                impl_->filePath_, flushMs);
                }
                else
                {
                    SPDLOG_ERROR("HDF5 final flush failed for {} after {:.3f} ms",
                                 impl_->filePath_, flushMs);
                }
            }

            const auto openObjects = H5Fget_obj_count(impl_->fileId_, H5F_OBJ_ALL);
            if (openObjects > 1)
            {
                SPDLOG_WARN("HDF5 file {} has {} open objects before close; strong close will finalize them",
                            impl_->filePath_, static_cast<long long>(openObjects));
            }

            // stop-lag diagnostic: H5Fclose implicitly flushes dirty buffers,
            // which on large multi-image payloads can dominate the shutdown
            // time. Time it explicitly so it shows up in the log.
            const auto t0 = std::chrono::steady_clock::now();
            herr_t status = H5Fclose(impl_->fileId_);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            if (status < 0)
            {
                SPDLOG_ERROR("Error closing HDF5 file (after {:.3f} ms)", ms);
            }
            else
            {
                SPDLOG_INFO("HDF5 file closed: {} (H5Fclose took {:.3f} ms)",
                            impl_->filePath_, ms);
            }
            {
                std::ostringstream data;
                data << "{\"status\":" << status
                     << ",\"final_flush_ok\":" << (flushOk ? "true" : "false")
                     << ",\"open_objects_before_close\":" << openObjects << "}";
                tracePerformance("hdf5.close_file", "hdf5.close", ms, data.str());
            }
            impl_->fileId_ = H5I_INVALID_HID;
            impl_->isOpen_ = false;
            impl_->writable_ = false;
            {
                auto& m = backend::diagnostics::CrashStateMirror::instance();
                m.hdf5.fileOpen.store(false);
                m.clearHdf5Path();
            }
        }
    }

    bool Hdf5Service::flush()
    {
        if (!isFileOpen())
            return false;
        herr_t s = H5Fflush(impl_->fileId_, H5F_SCOPE_GLOBAL);
        if (s < 0)
        {
            SPDLOG_ERROR("H5Fflush failed for {}", impl_->filePath_);
            return false;
        }
        SPDLOG_DEBUG("H5Fflush completed for {}", impl_->filePath_);
        return true;
    }

    // Flush at most once per impl_->flushInterval_ so per-batch append paths
    // keep the recorder thread off synchronous I/O most of the time. A crash
    // loses at most one interval's worth of buffered frames; clean stop still
    // gets a final flush + strong close in closeFile(). An interval of 0 forces
    // a flush on every call (used by tests).
    bool Hdf5Service::maybeIntervalFlush()
    {
        if (!isFileOpen())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - impl_->lastIntervalFlush_ < impl_->flushInterval_)
            return true; // skip: within interval
        impl_->lastIntervalFlush_ = now;
        return flush();
    }

    bool Hdf5Service::isFileOpen() const
    {
        return impl_->isOpen_ && impl_->fileId_ != H5I_INVALID_HID;
    }

    static bool writeImageDataset(hid_t fileId, const std::string &datasetPath,
                                  const std::vector<cv::Mat> &images)
    {
        if (images.empty())
            return true;

        // Get dimensions from first image
        const cv::Mat &firstImg = images[0];
        int height = firstImg.rows;
        int width = firstImg.cols;
        int channels = firstImg.channels();

        // Create dataspace
        hsize_t dims[4];
        int ndims;
        if (channels == 1)
        {
            ndims = 3;
            dims[0] = images.size();
            dims[1] = static_cast<hsize_t>(height);
            dims[2] = static_cast<hsize_t>(width);
        }
        else
        {
            ndims = 4;
            dims[0] = images.size();
            dims[1] = static_cast<hsize_t>(height);
            dims[2] = static_cast<hsize_t>(width);
            dims[3] = static_cast<hsize_t>(channels);
        }

        // Create dataspace with unlimited first dimension for extensibility
        hsize_t maxDims[4];
        if (channels == 1)
        {
            maxDims[0] = H5S_UNLIMITED;
            maxDims[1] = static_cast<hsize_t>(height);
            maxDims[2] = static_cast<hsize_t>(width);
        }
        else
        {
            maxDims[0] = H5S_UNLIMITED;
            maxDims[1] = static_cast<hsize_t>(height);
            maxDims[2] = static_cast<hsize_t>(width);
            maxDims[3] = static_cast<hsize_t>(channels);
        }

        hid_t dataspaceId = H5Screate_simple(ndims, dims, maxDims);
        if (dataspaceId < 0)
        {
            SPDLOG_ERROR("Failed to create dataspace for {}", datasetPath);
            return false;
        }

        // Create chunked dataset property for extensibility
        hid_t propId = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunkDims[4];
        if (channels == 1)
        {
            chunkDims[0] = std::min(static_cast<hsize_t>(100), dims[0]); // Chunk size of 100 frames or less
            chunkDims[1] = dims[1];
            chunkDims[2] = dims[2];
            H5Pset_chunk(propId, 3, chunkDims);
        }
        else
        {
            chunkDims[0] = std::min(static_cast<hsize_t>(100), dims[0]);
            chunkDims[1] = dims[1];
            chunkDims[2] = dims[2];
            chunkDims[3] = dims[3];
            H5Pset_chunk(propId, 4, chunkDims);
        }

        // Create dataset with chunked property
        hid_t datasetId = H5Dcreate2(fileId, datasetPath.c_str(), H5T_NATIVE_UINT8, dataspaceId,
                                     H5P_DEFAULT, propId, H5P_DEFAULT);
        H5Pclose(propId);
        if (datasetId < 0)
        {
            H5Sclose(dataspaceId);
            SPDLOG_ERROR("Failed to create dataset {}", datasetPath);
            return false;
        }

        // Prepare data buffer (size_t casts: int*int*int would overflow in
        // int arithmetic before widening, undersizing the buffer)
        size_t frameSize = static_cast<size_t>(height) * static_cast<size_t>(width) *
                           static_cast<size_t>(channels);
        std::vector<uint8_t> buffer(images.size() * frameSize);
        size_t offset = 0;
        for (const auto &img : images)
        {
            if (img.rows != height || img.cols != width || img.channels() != channels)
            {
                SPDLOG_WARN("Image size mismatch, skipping");
                continue;
            }
            if (img.isContinuous())
            {
                std::memcpy(buffer.data() + offset, img.data, img.total() * img.elemSize());
                offset += img.total() * img.elemSize();
            }
            else
            {
                for (int r = 0; r < img.rows; r++)
                {
                    std::memcpy(buffer.data() + offset, img.ptr(r), img.cols * img.elemSize());
                    offset += img.cols * img.elemSize();
                }
            }
        }

        // Write data
        herr_t status = H5Dwrite(datasetId, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());
        if (status < 0)
        {
            SPDLOG_ERROR("Failed to write image dataset {}", datasetPath);
            H5Dclose(datasetId);
            H5Sclose(dataspaceId);
            return false;
        }

        H5Dclose(datasetId);
        H5Sclose(dataspaceId);
        SPDLOG_DEBUG("Wrote {} images to {} ({}x{}x{})", images.size(), datasetPath, height, width, channels);
        return true;
    }

    static bool writeMetadataDataset(hid_t fileId, const std::string &datasetPath,
                                     const std::vector<ProcessedFrame> &frames)
    {
        if (frames.empty())
            return true;

        hid_t compTypeId = createProcessedFrameMetadataType();

        // Create dataspace with unlimited first dimension for extensibility
        hsize_t dims[1] = {frames.size()};
        hsize_t maxDims[1] = {H5S_UNLIMITED};
        hid_t dataspaceId = H5Screate_simple(1, dims, maxDims);
        if (dataspaceId < 0)
        {
            H5Tclose(compTypeId);
            SPDLOG_ERROR("Failed to create dataspace for metadata");
            return false;
        }

        // Create chunked dataset property for extensibility
        hid_t propId = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunkDims[1] = {std::min(static_cast<hsize_t>(1000), dims[0])}; // Chunk size of 1000 entries or less
        H5Pset_chunk(propId, 1, chunkDims);

        // Create dataset with chunked property
        hid_t datasetId = H5Dcreate2(fileId, datasetPath.c_str(), compTypeId, dataspaceId,
                                     H5P_DEFAULT, propId, H5P_DEFAULT);
        H5Pclose(propId);
        if (datasetId < 0)
        {
            H5Sclose(dataspaceId);
            H5Tclose(compTypeId);
            SPDLOG_ERROR("Failed to create metadata dataset {}", datasetPath);
            return false;
        }

        // Prepare data
        std::vector<ProcessedFrameMetadataRecord> metadata;
        metadata.reserve(frames.size());
        for (const auto &frame : frames)
        {
            metadata.push_back(makeMetadataRecord(frame));
        }

        // Write data
        herr_t status = H5Dwrite(datasetId, compTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, metadata.data());
        if (status < 0)
        {
            SPDLOG_ERROR("Failed to write metadata dataset {}", datasetPath);
            H5Dclose(datasetId);
            H5Sclose(dataspaceId);
            H5Tclose(compTypeId);
            return false;
        }

        H5Dclose(datasetId);
        H5Sclose(dataspaceId);
        H5Tclose(compTypeId);
        SPDLOG_DEBUG("Wrote {} metadata entries to {}", frames.size(), datasetPath);
        return true;
    }

    bool Hdf5Service::saveFrames(const std::vector<ProcessedFrame> &validFrames,
                                 const std::vector<ProcessedFrame> &invalidFrames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Create groups
        hid_t validGroupId = H5Gcreate2(impl_->fileId_, "/valid_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (validGroupId >= 0)
            H5Gclose(validGroupId);

        hid_t invalidGroupId = H5Gcreate2(impl_->fileId_, "/invalid_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (invalidGroupId >= 0)
            H5Gclose(invalidGroupId);

        // Write valid frames
        if (!validFrames.empty())
        {
            std::vector<cv::Mat> validImages, validMasks;
            for (const auto &frame : validFrames)
            {
                validImages.push_back(frame.originalImage);
                validMasks.push_back(frame.processedImage);
            }
            if (!writeImageDataset(impl_->fileId_, "/valid_frames/images", validImages))
                return false;
            if (!writeImageDataset(impl_->fileId_, "/valid_frames/masks", validMasks))
                return false;
            if (!writeMetadataDataset(impl_->fileId_, "/valid_frames/metadata", validFrames))
                return false;
        }

        // Write invalid frames
        if (!invalidFrames.empty())
        {
            std::vector<cv::Mat> invalidImages, invalidMasks;
            for (const auto &frame : invalidFrames)
            {
                invalidImages.push_back(frame.originalImage);
                invalidMasks.push_back(frame.processedImage);
            }
            if (!writeImageDataset(impl_->fileId_, "/invalid_frames/images", invalidImages))
                return false;
            if (!writeImageDataset(impl_->fileId_, "/invalid_frames/masks", invalidMasks))
                return false;
            if (!writeMetadataDataset(impl_->fileId_, "/invalid_frames/metadata", invalidFrames))
                return false;
        }

        SPDLOG_INFO("Saved {} valid frames and {} invalid frames to HDF5",
                    validFrames.size(), invalidFrames.size());
        return true;
    }

    // Helper function to append images to an existing dataset
    static bool appendImageDataset(hid_t fileId, const std::string &datasetPath,
                                   const std::vector<cv::Mat> &images, hsize_t &currentSize)
    {
        if (images.empty())
            return true;

        // Get dimensions from first image
        const cv::Mat &firstImg = images[0];
        int height = firstImg.rows;
        int width = firstImg.cols;
        int channels = firstImg.channels();

        // Open existing dataset
        hid_t datasetId = H5Dopen2(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("Failed to open dataset {} for appending", datasetPath);
            return false;
        }

        // Get current dataspace and dimensions
        hid_t filespaceId = H5Dget_space(datasetId);
        int ndims = H5Sget_simple_extent_ndims(filespaceId);
        hsize_t currentDims[4];
        H5Sget_simple_extent_dims(filespaceId, currentDims, nullptr);
        H5Sclose(filespaceId);

        // The dataset extent was fixed by the first-ever batch; a
        // mid-recording dimension change (ROI/resolution) cannot be appended.
        // Fail with a precise message instead of a cryptic H5Dwrite error.
        if (currentDims[1] != static_cast<hsize_t>(height) ||
            currentDims[2] != static_cast<hsize_t>(width) ||
            (ndims == 4 && currentDims[3] != static_cast<hsize_t>(channels)))
        {
            SPDLOG_ERROR("appendImageDataset {}: batch dimensions {}x{}x{} do not match "
                         "dataset extent {}x{}x{} (frame size changed mid-recording?)",
                         datasetPath, height, width, channels,
                         currentDims[1], currentDims[2], ndims == 4 ? currentDims[3] : 1);
            H5Dclose(datasetId);
            return false;
        }

        // Extend dataset once for the whole batch
        hsize_t newDims[4];
        if (channels == 1)
        {
            newDims[0] = currentDims[0] + images.size();
            newDims[1] = currentDims[1];
            newDims[2] = currentDims[2];
        }
        else
        {
            newDims[0] = currentDims[0] + images.size();
            newDims[1] = currentDims[1];
            newDims[2] = currentDims[2];
            newDims[3] = currentDims[3];
        }
        herr_t status = H5Dset_extent(datasetId, newDims);
        if (status < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to extend dataset {}", datasetPath);
            return false;
        }

        // Write each frame as its own hyperslab to avoid assembling a large contiguous buffer
        const size_t imageSizeBytes = static_cast<size_t>(height) * width * channels;
        std::vector<uint8_t> scratch; // allocated only if needed

        for (size_t i = 0; i < images.size(); ++i)
        {
            const cv::Mat &img = images[i];
            if (img.rows != height || img.cols != width || img.channels() != channels)
            {
                SPDLOG_ERROR("Image dimensions mismatch in appendImageDataset");
                H5Dclose(datasetId);
                return false;
            }

            filespaceId = H5Dget_space(datasetId);
            hsize_t start[4] = {currentDims[0] + static_cast<hsize_t>(i), 0, 0, 0};
            hsize_t count[4];
            if (channels == 1)
            {
                count[0] = 1;
                count[1] = static_cast<hsize_t>(height);
                count[2] = static_cast<hsize_t>(width);
            }
            else
            {
                count[0] = 1;
                count[1] = static_cast<hsize_t>(height);
                count[2] = static_cast<hsize_t>(width);
                count[3] = static_cast<hsize_t>(channels);
            }
            H5Sselect_hyperslab(filespaceId, H5S_SELECT_SET, start, nullptr, count, nullptr);

            hid_t memspaceId = H5Screate_simple(channels == 1 ? 3 : 4, count, nullptr);

            const void *srcPtr = nullptr;
            if (img.isContinuous())
            {
                srcPtr = img.data;
            }
            else
            {
                if (scratch.size() < imageSizeBytes) scratch.resize(imageSizeBytes);
                size_t off = 0;
                const size_t rowBytes = static_cast<size_t>(img.cols) * img.elemSize();
                for (int r = 0; r < img.rows; ++r)
                {
                    std::memcpy(scratch.data() + off, img.ptr(r), rowBytes);
                    off += rowBytes;
                }
                srcPtr = scratch.data();
            }

            status = H5Dwrite(datasetId, H5T_NATIVE_UINT8, memspaceId, filespaceId, H5P_DEFAULT, srcPtr);
            H5Sclose(memspaceId);
            H5Sclose(filespaceId);
            if (status < 0)
            {
                SPDLOG_ERROR("Failed to append frame {} to dataset {}", i, datasetPath);
                H5Dclose(datasetId);
                return false;
            }
        }

        H5Dclose(datasetId);

        // Update tracked size to match actual dataset extent after successful write
        currentSize = currentDims[0] + images.size();
        return true;
    }

    // Helper function to append metadata to an existing dataset
    static bool appendMetadataDataset(hid_t fileId, const std::string &datasetPath,
                                      const std::vector<ProcessedFrame> &frames, hsize_t &currentSize)
    {
        if (frames.empty())
            return true;

        // Open existing dataset
        hid_t datasetId = H5Dopen2(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("Failed to open metadata dataset {} for appending", datasetPath);
            return false;
        }

        hid_t memTypeId = createProcessedFrameMetadataType();
        if (memTypeId < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to create metadata memory type for {}", datasetPath);
            return false;
        }

        // Extend dataset once for the whole batch
        hid_t filespaceId = H5Dget_space(datasetId);
        hsize_t currentDims[1];
        H5Sget_simple_extent_dims(filespaceId, currentDims, nullptr);
        H5Sclose(filespaceId);

        hsize_t newDims[1] = {currentDims[0] + frames.size()};
        herr_t status = H5Dset_extent(datasetId, newDims);
        if (status < 0)
        {
            H5Tclose(memTypeId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to extend metadata dataset {}", datasetPath);
            return false;
        }

        // Write each metadata entry individually
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const auto &frame = frames[i];
            ProcessedFrameMetadataRecord md = makeMetadataRecord(frame);

            filespaceId = H5Dget_space(datasetId);
            hsize_t start[1] = {currentDims[0] + static_cast<hsize_t>(i)};
            hsize_t count[1] = {1};
            H5Sselect_hyperslab(filespaceId, H5S_SELECT_SET, start, nullptr, count, nullptr);
            hid_t memspaceId = H5Screate_simple(1, count, nullptr);

            status = H5Dwrite(datasetId, memTypeId, memspaceId, filespaceId, H5P_DEFAULT, &md);
            H5Sclose(memspaceId);
            H5Sclose(filespaceId);
            if (status < 0)
            {
                SPDLOG_ERROR("Failed to append metadata entry {} to dataset {}", i, datasetPath);
                H5Tclose(memTypeId);
                H5Dclose(datasetId);
                return false;
            }
        }

        H5Tclose(memTypeId);
        H5Dclose(datasetId);

        // Update tracked size to match actual dataset extent after successful write
        currentSize = currentDims[0] + frames.size();
        return true;
    }

    // Write a new 4D series_images dataset (N, seriesCount, H, W)
    static bool writeSeriesImageDataset(hid_t fileId, const std::string &datasetPath,
                                        const std::vector<ProcessedFrame> &frames)
    {
        // Collect only frames that have series images
        std::vector<const ProcessedFrame *> seriesFrames;
        for (const auto &f : frames) {
            if (!f.seriesImages.empty()) seriesFrames.push_back(&f);
        }
        if (seriesFrames.empty()) return true;

        const size_t seriesCount = seriesFrames[0]->seriesImages.size();
        const int height = seriesFrames[0]->seriesImages[0].rows;
        const int width = seriesFrames[0]->seriesImages[0].cols;

        // 4D: (N, seriesCount, H, W)
        hsize_t dims[4] = {seriesFrames.size(), seriesCount,
                           static_cast<hsize_t>(height), static_cast<hsize_t>(width)};
        hsize_t maxDims[4] = {H5S_UNLIMITED, seriesCount,
                              static_cast<hsize_t>(height), static_cast<hsize_t>(width)};

        hid_t dataspaceId = H5Screate_simple(4, dims, maxDims);
        if (dataspaceId < 0) {
            SPDLOG_ERROR("Failed to create dataspace for {}", datasetPath);
            return false;
        }

        hid_t propId = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunkDims[4] = {std::min(static_cast<hsize_t>(10), dims[0]),
                                seriesCount,
                                static_cast<hsize_t>(height),
                                static_cast<hsize_t>(width)};
        H5Pset_chunk(propId, 4, chunkDims);

        hid_t datasetId = H5Dcreate2(fileId, datasetPath.c_str(), H5T_NATIVE_UINT8, dataspaceId,
                                      H5P_DEFAULT, propId, H5P_DEFAULT);
        H5Pclose(propId);
        if (datasetId < 0) {
            H5Sclose(dataspaceId);
            SPDLOG_ERROR("Failed to create dataset {}", datasetPath);
            return false;
        }

        // Write each record's series as a hyperslab
        const size_t seriesFrameBytes = static_cast<size_t>(height) * width;
        std::vector<uint8_t> scratch;

        for (size_t n = 0; n < seriesFrames.size(); ++n) {
            const auto &frame = *seriesFrames[n];
            for (size_t s = 0; s < seriesCount && s < frame.seriesImages.size(); ++s) {
                const cv::Mat &img = frame.seriesImages[s];
                hid_t fileSpace = H5Dget_space(datasetId);
                hsize_t start[4] = {static_cast<hsize_t>(n), static_cast<hsize_t>(s), 0, 0};
                hsize_t count[4] = {1, 1, static_cast<hsize_t>(height), static_cast<hsize_t>(width)};
                H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, count, nullptr);
                hid_t memSpace = H5Screate_simple(4, count, nullptr);

                const void *srcPtr = nullptr;
                if (img.isContinuous()) {
                    srcPtr = img.data;
                } else {
                    if (scratch.size() < seriesFrameBytes) scratch.resize(seriesFrameBytes);
                    size_t off = 0;
                    for (int r = 0; r < img.rows; ++r) {
                        std::memcpy(scratch.data() + off, img.ptr(r), img.cols);
                        off += img.cols;
                    }
                    srcPtr = scratch.data();
                }

                herr_t status = H5Dwrite(datasetId, H5T_NATIVE_UINT8, memSpace, fileSpace, H5P_DEFAULT, srcPtr);
                H5Sclose(memSpace);
                H5Sclose(fileSpace);
                if (status < 0) {
                    SPDLOG_ERROR("Failed to write series image [{},{}] to {}", n, s, datasetPath);
                    H5Dclose(datasetId);
                    H5Sclose(dataspaceId);
                    return false;
                }
            }
        }

        H5Dclose(datasetId);
        H5Sclose(dataspaceId);
        SPDLOG_DEBUG("Wrote {} series records ({}x{}x{}) to {}", seriesFrames.size(), seriesCount, height, width, datasetPath);
        return true;
    }

    // Append to existing 4D series_images dataset
    static bool appendSeriesImageDataset(hid_t fileId, const std::string &datasetPath,
                                         const std::vector<ProcessedFrame> &frames, hsize_t &currentSize)
    {
        std::vector<const ProcessedFrame *> seriesFrames;
        for (const auto &f : frames) {
            if (!f.seriesImages.empty()) seriesFrames.push_back(&f);
        }
        if (seriesFrames.empty()) return true;

        const size_t seriesCount = seriesFrames[0]->seriesImages.size();
        const int height = seriesFrames[0]->seriesImages[0].rows;
        const int width = seriesFrames[0]->seriesImages[0].cols;

        hid_t datasetId = H5Dopen2(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0) {
            SPDLOG_ERROR("Failed to open series dataset {} for appending", datasetPath);
            return false;
        }

        // Get current extent
        hid_t fileSpace = H5Dget_space(datasetId);
        hsize_t currentDims[4];
        H5Sget_simple_extent_dims(fileSpace, currentDims, nullptr);
        H5Sclose(fileSpace);

        // Dataset layout is {N, series, H, W}; its extent was fixed by the
        // first batch. Reject a mid-recording dimension change loudly.
        if (currentDims[2] != static_cast<hsize_t>(height) ||
            currentDims[3] != static_cast<hsize_t>(width)) {
            SPDLOG_ERROR("appendSeriesImageDataset {}: batch dimensions {}x{} do not match "
                         "dataset extent {}x{} (frame size changed mid-recording?)",
                         datasetPath, height, width, currentDims[2], currentDims[3]);
            H5Dclose(datasetId);
            return false;
        }

        // Extend first dimension
        hsize_t newDims[4] = {currentDims[0] + seriesFrames.size(), currentDims[1], currentDims[2], currentDims[3]};
        herr_t status = H5Dset_extent(datasetId, newDims);
        if (status < 0) {
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to extend series dataset {}", datasetPath);
            return false;
        }

        std::vector<uint8_t> scratch;
        const size_t seriesFrameBytes = static_cast<size_t>(height) * width;

        // stop-lag diagnostic: this nested loop issues N*seriesCount
        // H5Dwrite calls, each with its own hyperslab setup. It is the
        // leading suspect for the stop-lag when multi-image is enabled.
        const auto tLoopStart = std::chrono::steady_clock::now();
        size_t writesDone = 0;

        for (size_t n = 0; n < seriesFrames.size(); ++n) {
            const auto &frame = *seriesFrames[n];
            for (size_t s = 0; s < seriesCount && s < frame.seriesImages.size(); ++s) {
                const cv::Mat &img = frame.seriesImages[s];
                // The scratch copy below is sized height*width but walks
                // img.rows/img.cols — a larger series image would overflow
                // the heap, a smaller one would write a garbage hyperslab.
                if (img.rows != height || img.cols != width || img.channels() != 1) {
                    SPDLOG_ERROR("appendSeriesImageDataset {}: series image [{},{}] is "
                                 "{}x{}x{} but the dataset expects {}x{}x1; batch aborted",
                                 datasetPath, n, s, img.rows, img.cols, img.channels(),
                                 height, width);
                    H5Dclose(datasetId);
                    return false;
                }
                fileSpace = H5Dget_space(datasetId);
                hsize_t start[4] = {currentDims[0] + static_cast<hsize_t>(n), static_cast<hsize_t>(s), 0, 0};
                hsize_t count[4] = {1, 1, static_cast<hsize_t>(height), static_cast<hsize_t>(width)};
                H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, count, nullptr);
                hid_t memSpace = H5Screate_simple(4, count, nullptr);

                const void *srcPtr = nullptr;
                if (img.isContinuous()) {
                    srcPtr = img.data;
                } else {
                    if (scratch.size() < seriesFrameBytes) scratch.resize(seriesFrameBytes);
                    size_t off = 0;
                    for (int r = 0; r < img.rows; ++r) {
                        std::memcpy(scratch.data() + off, img.ptr(r), img.cols);
                        off += img.cols;
                    }
                    srcPtr = scratch.data();
                }

                status = H5Dwrite(datasetId, H5T_NATIVE_UINT8, memSpace, fileSpace, H5P_DEFAULT, srcPtr);
                H5Sclose(memSpace);
                H5Sclose(fileSpace);
                if (status < 0) {
                    SPDLOG_ERROR("Failed to append series image [{},{}] to {}", n, s, datasetPath);
                    H5Dclose(datasetId);
                    return false;
                }
                ++writesDone;
            }
        }

        const double loopMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - tLoopStart)
                                  .count();
        const double avgMs = writesDone > 0 ? loopMs / static_cast<double>(writesDone) : 0.0;
        SPDLOG_INFO("stop-lag/hdf5: appendSeriesImageDataset wrote {} images "
                    "({} frames x {} series, {}x{}) in {:.3f} ms (avg {:.3f} ms/write)",
                    writesDone, seriesFrames.size(), seriesCount, height, width,
                    loopMs, avgMs);

        H5Dclose(datasetId);
        currentSize = currentDims[0] + seriesFrames.size();
        return true;
    }

    bool Hdf5Service::initializeDatasets()
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        if (impl_->datasetsInitialized_)
        {
            return true; // Already initialized
        }

        // Create groups
        hid_t validGroupId = H5Gcreate2(impl_->fileId_, "/valid_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (validGroupId >= 0)
            H5Gclose(validGroupId);

        hid_t invalidGroupId = H5Gcreate2(impl_->fileId_, "/invalid_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (invalidGroupId >= 0)
            H5Gclose(invalidGroupId);

        impl_->datasetsInitialized_ = true;
        SPDLOG_DEBUG("HDF5 datasets initialized (will be created on first append)");
        return true;
    }

    bool Hdf5Service::appendFrames(const std::vector<ProcessedFrame> &validFrames,
                                   const std::vector<ProcessedFrame> &invalidFrames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // stop-lag diagnostic: time each major section (valid images/masks/
        // metadata, series images, invalid frames). Useful for isolating
        // which part of the write dominates, especially under multi-image.
        using lag_clock = std::chrono::steady_clock;
        const auto tAppendStart = lag_clock::now();
        auto elapsedMs = [](lag_clock::time_point t0) {
            return std::chrono::duration<double, std::milli>(lag_clock::now() - t0).count();
        };
        double msValidImages = 0.0, msSeries = 0.0, msInvalid = 0.0;
        size_t seriesFramesCount = 0;

        // Initialize datasets if needed
        if (!impl_->datasetsInitialized_)
        {
            initializeDatasets();
        }

        // Append valid frames
        if (!validFrames.empty())
        {
            const auto tVS = lag_clock::now();
            // Create datasets if they don't exist
            if (impl_->validFramesWritten_ == 0)
            {
                std::vector<cv::Mat> validImages, validMasks;
                for (const auto &frame : validFrames)
                {
                    validImages.push_back(frame.originalImage);
                    validMasks.push_back(frame.processedImage);
                }
                if (!writeImageDataset(impl_->fileId_, "/valid_frames/images", validImages))
                    return false;
                if (!writeImageDataset(impl_->fileId_, "/valid_frames/masks", validMasks))
                    return false;
                if (!writeMetadataDataset(impl_->fileId_, "/valid_frames/metadata", validFrames))
                    return false;
                impl_->validFramesWritten_ = validFrames.size();
            }
            else
            {
                // Append to existing datasets
                std::vector<cv::Mat> validImages, validMasks;
                for (const auto &frame : validFrames)
                {
                    validImages.push_back(frame.originalImage);
                    validMasks.push_back(frame.processedImage);
                }
                if (!appendImageDataset(impl_->fileId_, "/valid_frames/images", validImages, impl_->validFramesWritten_))
                    return false;
                if (!appendImageDataset(impl_->fileId_, "/valid_frames/masks", validMasks, impl_->validFramesWritten_))
                    return false;
                if (!appendMetadataDataset(impl_->fileId_, "/valid_frames/metadata", validFrames, impl_->validFramesWritten_))
                    return false;
            }
            msValidImages = elapsedMs(tVS);

            // Write multi-image series data if any frames have series images
            bool hasSeriesImages = false;
            for (const auto &frame : validFrames) {
                if (!frame.seriesImages.empty()) {
                    hasSeriesImages = true;
                    ++seriesFramesCount;
                }
            }
            if (hasSeriesImages) {
                const auto tSeries = lag_clock::now();
                if (impl_->seriesImagesWritten_ == 0) {
                    if (!writeSeriesImageDataset(impl_->fileId_, "/valid_frames/series_images", validFrames)) {
                        SPDLOG_WARN("Failed to write series_images dataset (non-fatal)");
                    } else {
                        // Count how many frames had series data
                        for (const auto &f : validFrames) {
                            if (!f.seriesImages.empty()) ++impl_->seriesImagesWritten_;
                        }
                    }
                } else {
                    if (!appendSeriesImageDataset(impl_->fileId_, "/valid_frames/series_images", validFrames, impl_->seriesImagesWritten_)) {
                        SPDLOG_WARN("Failed to append series_images dataset (non-fatal)");
                    }
                }
                msSeries = elapsedMs(tSeries);
            }
        }

        // Append invalid frames
        if (!invalidFrames.empty())
        {
            const auto tInv = lag_clock::now();
            if (impl_->invalidFramesWritten_ == 0)
            {
                std::vector<cv::Mat> invalidImages, invalidMasks;
                for (const auto &frame : invalidFrames)
                {
                    invalidImages.push_back(frame.originalImage);
                    invalidMasks.push_back(frame.processedImage);
                }
                if (!writeImageDataset(impl_->fileId_, "/invalid_frames/images", invalidImages))
                    return false;
                if (!writeImageDataset(impl_->fileId_, "/invalid_frames/masks", invalidMasks))
                    return false;
                if (!writeMetadataDataset(impl_->fileId_, "/invalid_frames/metadata", invalidFrames))
                    return false;
                impl_->invalidFramesWritten_ = invalidFrames.size();
            }
            else
            {
                std::vector<cv::Mat> invalidImages, invalidMasks;
                for (const auto &frame : invalidFrames)
                {
                    invalidImages.push_back(frame.originalImage);
                    invalidMasks.push_back(frame.processedImage);
                }
                if (!appendImageDataset(impl_->fileId_, "/invalid_frames/images", invalidImages, impl_->invalidFramesWritten_))
                    return false;
                if (!appendImageDataset(impl_->fileId_, "/invalid_frames/masks", invalidMasks, impl_->invalidFramesWritten_))
                    return false;
                if (!appendMetadataDataset(impl_->fileId_, "/invalid_frames/metadata", invalidFrames, impl_->invalidFramesWritten_))
                    return false;
            }
            msInvalid = elapsedMs(tInv);
        }

        const double msTotal = elapsedMs(tAppendStart);
        SPDLOG_INFO("stop-lag/hdf5: appendFrames total {:.3f} ms "
                    "(valid={} [{:.3f} ms], seriesFrames={} [{:.3f} ms], "
                    "invalid={} [{:.3f} ms])",
                    msTotal,
                    validFrames.size(), msValidImages,
                    seriesFramesCount, msSeries,
                    invalidFrames.size(), msInvalid);
        {
            std::ostringstream data;
            data << "{\"valid\":" << validFrames.size()
                 << ",\"invalid\":" << invalidFrames.size()
                 << ",\"series_frames\":" << seriesFramesCount
                 << ",\"valid_ms\":" << msValidImages
                 << ",\"series_ms\":" << msSeries
                 << ",\"invalid_ms\":" << msInvalid
                 << "}";
            tracePerformance("hdf5.append_frames", "hdf5.write", msTotal, data.str());
        }

        if (!validFrames.empty() || !invalidFrames.empty())
        {
            if (!maybeIntervalFlush())
            {
                SPDLOG_WARN("appendFrames: post-write flush failed");
            }
        }

        return true;
    }

    bool Hdf5Service::writeExperimentInfo(uint64_t startTimeNs, uint64_t endTimeNs,
                                          size_t totalValidFrames, size_t totalInvalidFrames,
                                          const ProcessingConfig& processingConfig,
                                          const ProcessingService::Roi& roi,
                                          const cv::Mat* background,
                                          const backend::processing::ProcessingCoreIdentity* processingCore)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Create group
        hid_t infoGroupId = H5Gcreate2(impl_->fileId_, "/experiment_info", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (infoGroupId < 0)
        {
            SPDLOG_ERROR("Failed to create experiment_info group");
            return false;
        }

        // Create scalar dataspace for attributes
        hid_t scalarSpaceId = H5Screate(H5S_SCALAR);
        if (scalarSpaceId < 0)
        {
            H5Gclose(infoGroupId);
            SPDLOG_ERROR("Failed to create scalar dataspace");
            return false;
        }

        // Write attributes
        hid_t attr1 = H5Acreate2(infoGroupId, "start_time_ns", H5T_NATIVE_UINT64, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr1 >= 0)
        {
            H5Awrite(attr1, H5T_NATIVE_UINT64, &startTimeNs);
            H5Aclose(attr1);
        }

        hid_t attr2 = H5Acreate2(infoGroupId, "end_time_ns", H5T_NATIVE_UINT64, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr2 >= 0)
        {
            H5Awrite(attr2, H5T_NATIVE_UINT64, &endTimeNs);
            H5Aclose(attr2);
        }

        uint64_t validCount = totalValidFrames;
        hid_t attr3 = H5Acreate2(infoGroupId, "total_valid_frames", H5T_NATIVE_UINT64, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr3 >= 0)
        {
            H5Awrite(attr3, H5T_NATIVE_UINT64, &validCount);
            H5Aclose(attr3);
        }

        uint64_t invalidCount = totalInvalidFrames;
        hid_t attr4 = H5Acreate2(infoGroupId, "total_invalid_frames", H5T_NATIVE_UINT64, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr4 >= 0)
        {
            H5Awrite(attr4, H5T_NATIVE_UINT64, &invalidCount);
            H5Aclose(attr4);
        }

        // Write processing configuration attributes
        int32_t gaussianBlurSize = static_cast<int32_t>(processingConfig.gaussian_blur_size);
        hid_t attr5 = H5Acreate2(infoGroupId, "processing_config_gaussian_blur_size", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr5 >= 0)
        {
            H5Awrite(attr5, H5T_NATIVE_INT32, &gaussianBlurSize);
            H5Aclose(attr5);
        }

        int32_t bgSubtractThreshold = static_cast<int32_t>(processingConfig.bg_subtract_threshold);
        hid_t attr6 = H5Acreate2(infoGroupId, "processing_config_bg_subtract_threshold", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr6 >= 0)
        {
            H5Awrite(attr6, H5T_NATIVE_INT32, &bgSubtractThreshold);
            H5Aclose(attr6);
        }

        int32_t morphKernelSize = static_cast<int32_t>(processingConfig.morph_kernel_size);
        hid_t attr7 = H5Acreate2(infoGroupId, "processing_config_morph_kernel_size", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr7 >= 0)
        {
            H5Awrite(attr7, H5T_NATIVE_INT32, &morphKernelSize);
            H5Aclose(attr7);
        }

        int32_t morphIterations = static_cast<int32_t>(processingConfig.morph_iterations);
        hid_t attr8 = H5Acreate2(infoGroupId, "processing_config_morph_iterations", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr8 >= 0)
        {
            H5Awrite(attr8, H5T_NATIVE_INT32, &morphIterations);
            H5Aclose(attr8);
        }

        int32_t areaThresholdMin = static_cast<int32_t>(processingConfig.area_threshold_min);
        hid_t attr9 = H5Acreate2(infoGroupId, "processing_config_area_threshold_min", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr9 >= 0)
        {
            H5Awrite(attr9, H5T_NATIVE_INT32, &areaThresholdMin);
            H5Aclose(attr9);
        }

        int32_t areaThresholdMax = static_cast<int32_t>(processingConfig.area_threshold_max);
        hid_t attr10 = H5Acreate2(infoGroupId, "processing_config_area_threshold_max", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr10 >= 0)
        {
            H5Awrite(attr10, H5T_NATIVE_INT32, &areaThresholdMax);
            H5Aclose(attr10);
        }

        uint8_t enableBorderCheck = processingConfig.enable_border_check ? 1 : 0;
        hid_t attr11 = H5Acreate2(infoGroupId, "processing_config_enable_border_check", H5T_NATIVE_UINT8, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr11 >= 0)
        {
            H5Awrite(attr11, H5T_NATIVE_UINT8, &enableBorderCheck);
            H5Aclose(attr11);
        }

        uint8_t enableAreaRangeCheck = processingConfig.enable_area_range_check ? 1 : 0;
        hid_t attr12 = H5Acreate2(infoGroupId, "processing_config_enable_area_range_check", H5T_NATIVE_UINT8, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr12 >= 0)
        {
            H5Awrite(attr12, H5T_NATIVE_UINT8, &enableAreaRangeCheck);
            H5Aclose(attr12);
        }

        double deformabilityThresholdMin = processingConfig.deformability_threshold_min;
        hid_t attrDeformMin = H5Acreate2(infoGroupId, "processing_config_deformability_threshold_min", H5T_NATIVE_DOUBLE, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attrDeformMin >= 0)
        {
            H5Awrite(attrDeformMin, H5T_NATIVE_DOUBLE, &deformabilityThresholdMin);
            H5Aclose(attrDeformMin);
        }

        double deformabilityThresholdMax = processingConfig.deformability_threshold_max;
        hid_t attrDeformMax = H5Acreate2(infoGroupId, "processing_config_deformability_threshold_max", H5T_NATIVE_DOUBLE, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attrDeformMax >= 0)
        {
            H5Awrite(attrDeformMax, H5T_NATIVE_DOUBLE, &deformabilityThresholdMax);
            H5Aclose(attrDeformMax);
        }

        uint8_t enableDeformabilityRangeCheck = processingConfig.enable_deformability_range_check ? 1 : 0;
        hid_t attrDeformCheck = H5Acreate2(infoGroupId, "processing_config_enable_deformability_range_check", H5T_NATIVE_UINT8, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attrDeformCheck >= 0)
        {
            H5Awrite(attrDeformCheck, H5T_NATIVE_UINT8, &enableDeformabilityRangeCheck);
            H5Aclose(attrDeformCheck);
        }

        uint8_t requireSingleInnerContour = processingConfig.require_single_inner_contour ? 1 : 0;
        hid_t attr13 = H5Acreate2(infoGroupId, "processing_config_require_single_inner_contour", H5T_NATIVE_UINT8, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr13 >= 0)
        {
            H5Awrite(attr13, H5T_NATIVE_UINT8, &requireSingleInnerContour);
            H5Aclose(attr13);
        }

        int32_t emptyFramePixelThreshold = static_cast<int32_t>(processingConfig.empty_frame_pixel_threshold);
        hid_t attr14a = H5Acreate2(infoGroupId, "processing_config_empty_frame_pixel_threshold", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr14a >= 0)
        {
            H5Awrite(attr14a, H5T_NATIVE_INT32, &emptyFramePixelThreshold);
            H5Aclose(attr14a);
        }

        // Write ROI attributes
        int32_t roiX = static_cast<int32_t>(roi.x);
        hid_t attr14 = H5Acreate2(infoGroupId, "roi_x", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr14 >= 0)
        {
            H5Awrite(attr14, H5T_NATIVE_INT32, &roiX);
            H5Aclose(attr14);
        }

        int32_t roiY = static_cast<int32_t>(roi.y);
        hid_t attr15 = H5Acreate2(infoGroupId, "roi_y", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr15 >= 0)
        {
            H5Awrite(attr15, H5T_NATIVE_INT32, &roiY);
            H5Aclose(attr15);
        }

        int32_t roiW = static_cast<int32_t>(roi.w);
        hid_t attr16 = H5Acreate2(infoGroupId, "roi_w", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr16 >= 0)
        {
            H5Awrite(attr16, H5T_NATIVE_INT32, &roiW);
            H5Aclose(attr16);
        }

        int32_t roiH = static_cast<int32_t>(roi.h);
        hid_t attr17 = H5Acreate2(infoGroupId, "roi_h", H5T_NATIVE_INT32, scalarSpaceId,
                                 H5P_DEFAULT, H5P_DEFAULT);
        if (attr17 >= 0)
        {
            H5Awrite(attr17, H5T_NATIVE_INT32, &roiH);
            H5Aclose(attr17);
        }

        // Multi-image recording mode attributes
        uint8_t multiImageEnabled = processingConfig.multi_image_enabled ? 1 : 0;
        hid_t attrMI1 = H5Acreate2(infoGroupId, "multi_image_enabled", H5T_NATIVE_UINT8, scalarSpaceId,
                                    H5P_DEFAULT, H5P_DEFAULT);
        if (attrMI1 >= 0) {
            H5Awrite(attrMI1, H5T_NATIVE_UINT8, &multiImageEnabled);
            H5Aclose(attrMI1);
        }

        int32_t multiImageCount = static_cast<int32_t>(processingConfig.multi_image_count);
        hid_t attrMI2 = H5Acreate2(infoGroupId, "multi_image_count", H5T_NATIVE_INT32, scalarSpaceId,
                                    H5P_DEFAULT, H5P_DEFAULT);
        if (attrMI2 >= 0) {
            H5Awrite(attrMI2, H5T_NATIVE_INT32, &multiImageCount);
            H5Aclose(attrMI2);
        }

        const auto defaultIdentity = backend::processing::bundledProcessingCoreIdentity();
        const auto& coreIdentity = processingCore ? *processingCore : defaultIdentity;
        const auto writeUint32Attribute = [&](const char* name, uint32_t value) {
            hid_t attribute = H5Acreate2(infoGroupId, name, H5T_NATIVE_UINT32, scalarSpaceId,
                                         H5P_DEFAULT, H5P_DEFAULT);
            if (attribute < 0) return false;
            const bool ok = H5Awrite(attribute, H5T_NATIVE_UINT32, &value) >= 0;
            H5Aclose(attribute);
            return ok;
        };
        const auto writeStringAttribute = [&](const char* name, const std::string& value) {
            hid_t stringType = H5Tcopy(H5T_C_S1);
            if (stringType < 0) return false;
            H5Tset_size(stringType, H5T_VARIABLE);
            H5Tset_cset(stringType, H5T_CSET_UTF8);
            hid_t attribute = H5Acreate2(infoGroupId, name, stringType, scalarSpaceId,
                                         H5P_DEFAULT, H5P_DEFAULT);
            bool ok = false;
            if (attribute >= 0) {
                const char* pointer = value.c_str();
                ok = H5Awrite(attribute, stringType, &pointer) >= 0;
                H5Aclose(attribute);
            }
            H5Tclose(stringType);
            return ok;
        };
        const bool provenanceOk =
            writeStringAttribute("processing_core_version", coreIdentity.version) &&
            writeUint32Attribute("processing_contract_version", coreIdentity.contractVersion) &&
            writeUint32Attribute("processing_engine_abi_version", coreIdentity.engineAbiVersion) &&
            writeStringAttribute("processing_core_sha256", coreIdentity.artifactSha256) &&
            writeStringAttribute("processing_release_tag", coreIdentity.releaseTag) &&
            writeStringAttribute("processing_manifest_sha256", coreIdentity.manifestSha256) &&
            writeStringAttribute("processing_core_source", coreIdentity.source) &&
            writeStringAttribute("processing_core_build_id", coreIdentity.buildId) &&
            writeStringAttribute("processing_runtime_fingerprint", coreIdentity.runtimeFingerprint);

        H5Sclose(scalarSpaceId);
        H5Gclose(infoGroupId);

        if (!provenanceOk) {
            SPDLOG_ERROR("Failed to persist processing-core provenance");
            return false;
        }

        // Write background image for reproducibility (if provided and non-empty)
        if (background != nullptr && !background->empty())
        {
            cv::Mat bgToWrite;
            if (background->channels() == 1 && background->type() == CV_8UC1)
            {
                bgToWrite = *background;
            }
            else if (background->channels() >= 3)
            {
                cv::cvtColor(*background, bgToWrite, cv::COLOR_BGR2GRAY);
            }
            else
            {
                background->convertTo(bgToWrite, CV_8UC1);
            }
            std::vector<cv::Mat> bgVec{bgToWrite};
            if (!writeImageDataset(impl_->fileId_, "/experiment_info/background", bgVec))
            {
                SPDLOG_WARN("Failed to write experiment background image to HDF5");
            }
            else
            {
                SPDLOG_DEBUG("Wrote experiment background image to HDF5");
            }
        }

        if (!flush())
        {
            SPDLOG_WARN("writeExperimentInfo: post-write flush failed");
        }

        SPDLOG_DEBUG("Wrote experiment info, processing config, and ROI to HDF5");
        return true;
    }

    bool Hdf5Service::writeConfigJson(const std::string& jsonContent)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Open the experiment_info group (must already exist from writeExperimentInfo)
        htri_t exists = H5Lexists(impl_->fileId_, "/experiment_info", H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("Cannot write config JSON: /experiment_info group does not exist");
            return false;
        }

        hid_t groupId = H5Gopen2(impl_->fileId_, "/experiment_info", H5P_DEFAULT);
        if (groupId < 0)
        {
            SPDLOG_ERROR("Failed to open /experiment_info group for config JSON");
            return false;
        }

        // Create variable-length string type
        hid_t strType = H5Tcopy(H5T_C_S1);
        H5Tset_size(strType, H5T_VARIABLE);
        H5Tset_cset(strType, H5T_CSET_UTF8);

        hid_t scalarSpace = H5Screate(H5S_SCALAR);
        hid_t attr = H5Aopen(groupId, "config_json", H5P_DEFAULT);
        if (attr < 0)
        {
            attr = H5Acreate2(groupId, "config_json", strType, scalarSpace,
                              H5P_DEFAULT, H5P_DEFAULT);
        }
        bool ok = false;
        if (attr >= 0)
        {
            const char* ptr = jsonContent.c_str();
            ok = (H5Awrite(attr, strType, &ptr) >= 0);
            H5Aclose(attr);
        }

        H5Sclose(scalarSpace);
        H5Tclose(strType);
        H5Gclose(groupId);

        if (ok)
        {
            if (!flush())
            {
                SPDLOG_WARN("writeConfigJson: post-write flush failed");
            }
            SPDLOG_DEBUG("Wrote config JSON ({} bytes) to HDF5 metadata", jsonContent.size());
        }
        else
        {
            SPDLOG_WARN("Failed to write config JSON attribute");
        }
        return ok;
    }

    bool Hdf5Service::loadFile(const std::string& filePath)
    {
        if (impl_->isOpen_)
        {
            SPDLOG_WARN("HDF5 file already open: {}", impl_->filePath_);
            return false;
        }

        // Open existing file for reading. Use the same FAPL as openFile (strong
        // close + no file locking). There is no recovery-sidecar fallback.
        const hid_t fileAccessId = createFileAccessPropertyList();
        impl_->fileId_ = H5Fopen(filePath.c_str(), H5F_ACC_RDONLY, fileAccessId);
        closePropertyList(fileAccessId);
        if (impl_->fileId_ < 0)
        {
            SPDLOG_ERROR("Failed to open HDF5 file for reading: {}", filePath);
            return false;
        }

        impl_->filePath_ = filePath;
        impl_->isOpen_ = true;
        impl_->writable_ = false;
        SPDLOG_INFO("HDF5 file opened for reading: {}", filePath);
        return true;
    }

    static bool readImageDataset(hid_t fileId, const std::string& datasetPath,
                                 std::vector<cv::Mat>& images)
    {
        // Check if dataset exists
        htri_t exists = H5Lexists(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("Dataset {} does not exist", datasetPath);
            return false;
        }

        // Open dataset
        hid_t datasetId = H5Dopen2(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("Failed to open dataset {}", datasetPath);
            return false;
        }

        // Get dataspace and dimensions
        hid_t dataspaceId = H5Dget_space(datasetId);
        int ndims = H5Sget_simple_extent_ndims(dataspaceId);
        hsize_t dims[4];
        H5Sget_simple_extent_dims(dataspaceId, dims, nullptr);
        H5Sclose(dataspaceId);

        if (ndims < 3)
        {
            SPDLOG_ERROR("Invalid dataset dimensions: {}", ndims);
            H5Dclose(datasetId);
            return false;
        }

        hsize_t numFrames = dims[0];
        hsize_t height = dims[1];
        hsize_t width = dims[2];
        hsize_t channels = (ndims == 4) ? dims[3] : 1;

        // Read data
        size_t frameSize = static_cast<size_t>(height) * width * channels;
        std::vector<uint8_t> buffer(numFrames * frameSize);
        herr_t status = H5Dread(datasetId, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());
        H5Dclose(datasetId);

        if (status < 0)
        {
            SPDLOG_ERROR("Failed to read dataset {}", datasetPath);
            return false;
        }

        // Convert to cv::Mat
        images.clear();
        images.reserve(numFrames);
        for (hsize_t i = 0; i < numFrames; ++i)
        {
            size_t offset = i * frameSize;
            cv::Mat img;
            if (channels == 1)
            {
                img = cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                             buffer.data() + offset);
            }
            else
            {
                img = cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC(channels),
                             buffer.data() + offset);
            }
            images.push_back(img.clone()); // Clone to ensure data ownership
        }

        SPDLOG_DEBUG("Read {} images from {} ({}x{}x{})", numFrames, datasetPath, height, width, channels);
        return true;
    }

    static bool readMetadataDataset(hid_t fileId, const std::string& datasetPath,
                                    std::vector<ProcessedFrame>& frames)
    {
        // Check if dataset exists
        htri_t exists = H5Lexists(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("Dataset {} does not exist", datasetPath);
            return false;
        }

        // Open dataset
        hid_t datasetId = H5Dopen2(fileId, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("Failed to open metadata dataset {}", datasetPath);
            return false;
        }

        // Get compound type
        hid_t fileTypeId = H5Dget_type(datasetId);
        if (fileTypeId < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to get compound type from dataset {}", datasetPath);
            return false;
        }

        // Get dataspace and dimensions
        hid_t dataspaceId = H5Dget_space(datasetId);
        hsize_t dims[1];
        H5Sget_simple_extent_dims(dataspaceId, dims, nullptr);
        H5Sclose(dataspaceId);

        hsize_t numFrames = dims[0];
        if (numFrames == 0)
        {
            H5Tclose(fileTypeId);
            H5Dclose(datasetId);
            frames.clear();
            return true;
        }

        // Read metadata
        std::vector<ProcessedFrameMetadataRecord> metadata(numFrames);
        for (auto& md : metadata)
        {
            md.objectId = -1;
            md.objectCount = 0;
            md.trackId = -1;
            md.trackFirstFrame = 0;
            md.trackLastFrame = 0;
            md.trackObservationCount = 0;
            md.bboxX = 0.0;
            md.bboxY = 0.0;
            md.bboxWidth = 0.0;
            md.bboxHeight = 0.0;
            md.centroidX = 0.0;
            md.centroidY = 0.0;
        }

        hid_t baseMemTypeId = createProcessedFrameMetadataType(true, false, false);
        herr_t status = H5Dread(datasetId, baseMemTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, metadata.data());
        H5Tclose(baseMemTypeId);
        if (status < 0)
        {
            H5Tclose(fileTypeId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("Failed to read metadata dataset {}", datasetPath);
            return false;
        }

        const bool hasObjectId = H5Tget_member_index(fileTypeId, "objectId") >= 0;
        const bool hasObjectCount = H5Tget_member_index(fileTypeId, "objectCount") >= 0;
        if (hasObjectId || hasObjectCount)
        {
            hid_t objectMemTypeId = createProcessedFrameMetadataType(false, true, false);
            status = H5Dread(datasetId, objectMemTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, metadata.data());
            H5Tclose(objectMemTypeId);
            if (status < 0)
            {
                H5Tclose(fileTypeId);
                H5Dclose(datasetId);
                SPDLOG_ERROR("Failed to read object metadata fields from {}", datasetPath);
                return false;
            }
        }

        const bool hasTrackId = H5Tget_member_index(fileTypeId, "trackId") >= 0;
        const bool hasTrackFirstFrame = H5Tget_member_index(fileTypeId, "trackFirstFrame") >= 0;
        const bool hasTrackLastFrame = H5Tget_member_index(fileTypeId, "trackLastFrame") >= 0;
        const bool hasTrackObservationCount = H5Tget_member_index(fileTypeId, "trackObservationCount") >= 0;
        if (hasTrackId || hasTrackFirstFrame || hasTrackLastFrame || hasTrackObservationCount)
        {
            hid_t trackingMemTypeId = createProcessedFrameMetadataType(false, false, true);
            status = H5Dread(datasetId, trackingMemTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, metadata.data());
            H5Tclose(trackingMemTypeId);
            if (status < 0)
            {
                H5Tclose(fileTypeId);
                H5Dclose(datasetId);
                SPDLOG_ERROR("Failed to read tracking metadata fields from {}", datasetPath);
                return false;
            }
        }

        H5Tclose(fileTypeId);
        H5Dclose(datasetId);

        // Convert to ProcessedFrame (images will be filled separately)
        frames.clear();
        frames.reserve(numFrames);
        for (const auto& md : metadata)
        {
            ProcessedFrame frame;
            frame.index = md.index;
            frame.timestampNs = md.timestampNs;
            frame.validation.objectId = md.objectId;
            frame.validation.objectCount = md.objectCount;
            frame.validation.trackId = md.trackId;
            frame.validation.trackFirstFrame = md.trackFirstFrame;
            frame.validation.trackLastFrame = md.trackLastFrame;
            frame.validation.trackObservationCount = md.trackObservationCount;
            frame.validation.bboxX = md.bboxX;
            frame.validation.bboxY = md.bboxY;
            frame.validation.bboxWidth = md.bboxWidth;
            frame.validation.bboxHeight = md.bboxHeight;
            frame.validation.centroidX = md.centroidX;
            frame.validation.centroidY = md.centroidY;
            frame.validation.deformability = md.deformability;
            frame.validation.area = md.area;
            frame.validation.areaRatio = md.areaRatio;
            frame.validation.ringRatio = md.ringRatio;
            frame.validation.isValid = (md.isValid != 0);
            frame.validation.touchesBorder = (md.touchesBorder != 0);
            frame.validation.hasSingleInnerContour = (md.hasSingleInnerContour != 0);
            frame.validation.inRange = (md.inRange != 0);
            frame.validation.innerContourCount = md.innerContourCount;
            frame.validation.brightness.q1 = md.brightness_q1;
            frame.validation.brightness.q2 = md.brightness_q2;
            frame.validation.brightness.q3 = md.brightness_q3;
            frame.validation.brightness.q4 = md.brightness_q4;
            frame.validation.youngsModulus = md.youngsModulus;
            frame.validation.isTargetGroup = (md.isTargetGroup != 0);
            frames.push_back(frame);
        }

        SPDLOG_DEBUG("Read {} metadata entries from {}", numFrames, datasetPath);
        return true;
    }

    bool Hdf5Service::readValidFrames(std::vector<ProcessedFrame>& frames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Read metadata first
        if (!readMetadataDataset(impl_->fileId_, "/valid_frames/metadata", frames))
        {
            return false;
        }

        // Read images
        std::vector<cv::Mat> images;
        if (!readImageDataset(impl_->fileId_, "/valid_frames/images", images))
        {
            frames.clear();
            return false;
        }

        // Read masks
        std::vector<cv::Mat> masks;
        if (!readImageDataset(impl_->fileId_, "/valid_frames/masks", masks))
        {
            frames.clear();
            return false;
        }

        // Combine images and masks with metadata
        if (frames.size() != images.size() || frames.size() != masks.size())
        {
            SPDLOG_ERROR("Mismatch in frame counts: metadata={}, images={}, masks={}",
                        frames.size(), images.size(), masks.size());
            frames.clear();
            return false;
        }

        for (size_t i = 0; i < frames.size(); ++i)
        {
            frames[i].originalImage = images[i];
            frames[i].processedImage = masks[i];
        }

        SPDLOG_INFO("Read {} valid frames from HDF5", frames.size());
        return true;
    }

    bool Hdf5Service::readInvalidFrames(std::vector<ProcessedFrame>& frames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Read metadata first
        if (!readMetadataDataset(impl_->fileId_, "/invalid_frames/metadata", frames))
        {
            return false;
        }

        // Read images
        std::vector<cv::Mat> images;
        if (!readImageDataset(impl_->fileId_, "/invalid_frames/images", images))
        {
            frames.clear();
            return false;
        }

        // Read masks
        std::vector<cv::Mat> masks;
        if (!readImageDataset(impl_->fileId_, "/invalid_frames/masks", masks))
        {
            frames.clear();
            return false;
        }

        // Combine images and masks with metadata
        if (frames.size() != images.size() || frames.size() != masks.size())
        {
            SPDLOG_ERROR("Mismatch in frame counts: metadata={}, images={}, masks={}",
                        frames.size(), images.size(), masks.size());
            frames.clear();
            return false;
        }

        for (size_t i = 0; i < frames.size(); ++i)
        {
            frames[i].originalImage = images[i];
            frames[i].processedImage = masks[i];
        }

        SPDLOG_INFO("Read {} invalid frames from HDF5", frames.size());
        return true;
    }

    bool Hdf5Service::readExperimentInfo(uint64_t& startTimeNs, uint64_t& endTimeNs,
                                         size_t& totalValidFrames, size_t& totalInvalidFrames,
                                         ProcessingService::Roi* roi)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Check if group exists
        htri_t exists = H5Lexists(impl_->fileId_, "/experiment_info", H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("Experiment info group does not exist");
            return false;
        }

        // Open group
        hid_t infoGroupId = H5Gopen2(impl_->fileId_, "/experiment_info", H5P_DEFAULT);
        if (infoGroupId < 0)
        {
            SPDLOG_ERROR("Failed to open experiment_info group");
            return false;
        }

        // Read attributes
        bool success = true;
        if (H5Aexists(infoGroupId, "start_time_ns") > 0)
        {
            hid_t attr = H5Aopen(infoGroupId, "start_time_ns", H5P_DEFAULT);
            if (attr >= 0)
            {
                H5Aread(attr, H5T_NATIVE_UINT64, &startTimeNs);
                H5Aclose(attr);
            }
            else
            {
                success = false;
            }
        }

        if (H5Aexists(infoGroupId, "end_time_ns") > 0)
        {
            hid_t attr = H5Aopen(infoGroupId, "end_time_ns", H5P_DEFAULT);
            if (attr >= 0)
            {
                H5Aread(attr, H5T_NATIVE_UINT64, &endTimeNs);
                H5Aclose(attr);
            }
            else
            {
                success = false;
            }
        }

        uint64_t validCount = 0;
        if (H5Aexists(infoGroupId, "total_valid_frames") > 0)
        {
            hid_t attr = H5Aopen(infoGroupId, "total_valid_frames", H5P_DEFAULT);
            if (attr >= 0)
            {
                H5Aread(attr, H5T_NATIVE_UINT64, &validCount);
                H5Aclose(attr);
            }
            else
            {
                success = false;
            }
        }
        totalValidFrames = static_cast<size_t>(validCount);

        uint64_t invalidCount = 0;
        if (H5Aexists(infoGroupId, "total_invalid_frames") > 0)
        {
            hid_t attr = H5Aopen(infoGroupId, "total_invalid_frames", H5P_DEFAULT);
            if (attr >= 0)
            {
                H5Aread(attr, H5T_NATIVE_UINT64, &invalidCount);
                H5Aclose(attr);
            }
            else
            {
                success = false;
            }
        }
        totalInvalidFrames = static_cast<size_t>(invalidCount);

        // Read ROI attributes if requested
        if (roi != nullptr)
        {
            int32_t roiX = 0, roiY = 0, roiW = 0, roiH = 0;
            bool roiRead = true;

            if (H5Aexists(infoGroupId, "roi_x") > 0)
            {
                hid_t attr = H5Aopen(infoGroupId, "roi_x", H5P_DEFAULT);
                if (attr >= 0)
                {
                    H5Aread(attr, H5T_NATIVE_INT32, &roiX);
                    H5Aclose(attr);
                }
                else
                {
                    roiRead = false;
                }
            }
            else
            {
                roiRead = false;
            }

            if (H5Aexists(infoGroupId, "roi_y") > 0)
            {
                hid_t attr = H5Aopen(infoGroupId, "roi_y", H5P_DEFAULT);
                if (attr >= 0)
                {
                    H5Aread(attr, H5T_NATIVE_INT32, &roiY);
                    H5Aclose(attr);
                }
                else
                {
                    roiRead = false;
                }
            }
            else
            {
                roiRead = false;
            }

            if (H5Aexists(infoGroupId, "roi_w") > 0)
            {
                hid_t attr = H5Aopen(infoGroupId, "roi_w", H5P_DEFAULT);
                if (attr >= 0)
                {
                    H5Aread(attr, H5T_NATIVE_INT32, &roiW);
                    H5Aclose(attr);
                }
                else
                {
                    roiRead = false;
                }
            }
            else
            {
                roiRead = false;
            }

            if (H5Aexists(infoGroupId, "roi_h") > 0)
            {
                hid_t attr = H5Aopen(infoGroupId, "roi_h", H5P_DEFAULT);
                if (attr >= 0)
                {
                    H5Aread(attr, H5T_NATIVE_INT32, &roiH);
                    H5Aclose(attr);
                }
                else
                {
                    roiRead = false;
                }
            }
            else
            {
                roiRead = false;
            }

            if (roiRead)
            {
                roi->x = roiX;
                roi->y = roiY;
                roi->w = roiW;
                roi->h = roiH;
            }
            else
            {
                // Default to full image if ROI not found
                roi->x = 0;
                roi->y = 0;
                roi->w = 0;
                roi->h = 0;
            }
        }

        H5Gclose(infoGroupId);

        if (success)
        {
            SPDLOG_DEBUG("Read experiment info: start={}, end={}, valid={}, invalid={}",
                        startTimeNs, endTimeNs, totalValidFrames, totalInvalidFrames);
        }
        return success;
    }

    bool Hdf5Service::readProcessingCoreIdentity(
        backend::processing::ProcessingCoreIdentity& processingCore) const
    {
        if (!isFileOpen()) return false;
        const char* groupPath = nullptr;
        if (H5Lexists(impl_->fileId_, "/experiment_info", H5P_DEFAULT) > 0)
            groupPath = "/experiment_info";
        else if (H5Lexists(impl_->fileId_, "/recording_info", H5P_DEFAULT) > 0)
            groupPath = "/recording_info";
        else
            return false;
        hid_t group = H5Gopen2(impl_->fileId_, groupPath, H5P_DEFAULT);
        if (group < 0) return false;

        const auto isScalarAttribute = [](hid_t attribute) {
            hid_t space = H5Aget_space(attribute);
            if (space < 0) return false;
            const hssize_t points = H5Sget_simple_extent_npoints(space);
            H5Sclose(space);
            return points == 1;
        };
        const auto readUint32 = [&](const char* name, uint32_t& value) {
            hid_t attribute = H5Aopen(group, name, H5P_DEFAULT);
            if (attribute < 0) return false;
            const bool ok = isScalarAttribute(attribute) &&
                            H5Aread(attribute, H5T_NATIVE_UINT32, &value) >= 0;
            H5Aclose(attribute);
            return ok;
        };
        const auto readString = [&](const char* name, std::string& value) {
            hid_t attribute = H5Aopen(group, name, H5P_DEFAULT);
            if (attribute < 0) return false;
            if (!isScalarAttribute(attribute) || H5Aget_storage_size(attribute) > 1024 * 1024) {
                H5Aclose(attribute);
                return false;
            }
            hid_t type = H5Aget_type(attribute);
            if (type < 0 || H5Tget_class(type) != H5T_STRING) {
                if (type >= 0) H5Tclose(type);
                H5Aclose(attribute);
                return false;
            }
            bool ok = false;
            if (H5Tis_variable_str(type) > 0) {
                char* pointer = nullptr;
                ok = H5Aread(attribute, type, &pointer) >= 0;
                if (ok) value = pointer ? pointer : "";
                if (pointer) H5free_memory(pointer);
            } else {
                const size_t size = H5Tget_size(type);
                if (size > 0 && size <= 1024 * 1024) {
                    std::vector<char> buffer(size + 1, '\0');
                    ok = H5Aread(attribute, type, buffer.data()) >= 0;
                    if (ok) {
                        const auto end = std::find(buffer.begin(), buffer.begin() + size, '\0');
                        value.assign(buffer.begin(), end);
                    }
                }
            }
            H5Tclose(type);
            H5Aclose(attribute);
            return ok;
        };

        backend::processing::ProcessingCoreIdentity result;
        const bool ok =
            readString("processing_core_version", result.version) &&
            readUint32("processing_contract_version", result.contractVersion) &&
            readUint32("processing_engine_abi_version", result.engineAbiVersion) &&
            readString("processing_core_sha256", result.artifactSha256) &&
            readString("processing_release_tag", result.releaseTag) &&
            readString("processing_manifest_sha256", result.manifestSha256) &&
            readString("processing_core_source", result.source) &&
            readString("processing_core_build_id", result.buildId) &&
            readString("processing_runtime_fingerprint", result.runtimeFingerprint);
        H5Gclose(group);
        if (ok) processingCore = std::move(result);
        return ok;
    }

} // namespace backend::services

// New scalable read APIs
namespace backend::services {

    bool Hdf5Service::getDatasetInfo(const std::string& datasetPath,
                                     size_t& outCount,
                                     int& outHeight,
                                     int& outWidth,
                                     int& outChannels) const
    {
        outCount = 0;
        outHeight = 0;
        outWidth = 0;
        outChannels = 0;

        if (!isFileOpen())
        {
            SPDLOG_ERROR("getDatasetInfo: HDF5 file is not open");
            return false;
        }

        // Check existence
        htri_t exists = H5Lexists(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("getDatasetInfo: dataset {} does not exist", datasetPath);
            return false;
        }

        hid_t datasetId = H5Dopen2(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("getDatasetInfo: failed to open dataset {}", datasetPath);
            return false;
        }

        hid_t dataspaceId = H5Dget_space(datasetId);
        if (dataspaceId < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("getDatasetInfo: failed to get dataspace for {}", datasetPath);
            return false;
        }

        int ndims = H5Sget_simple_extent_ndims(dataspaceId);
        hsize_t dims[4] = {0, 0, 0, 0};
        if (H5Sget_simple_extent_dims(dataspaceId, dims, nullptr) < 0)
        {
            H5Sclose(dataspaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("getDatasetInfo: failed to get extent for {}", datasetPath);
            return false;
        }

        H5Sclose(dataspaceId);
        H5Dclose(datasetId);

        if (ndims < 3 || ndims > 4)
        {
            SPDLOG_ERROR("getDatasetInfo: unsupported ndims={} for {}", ndims, datasetPath);
            return false;
        }

        outCount = static_cast<size_t>(dims[0]);
        outHeight = static_cast<int>(dims[1]);
        outWidth = static_cast<int>(dims[2]);
        outChannels = (ndims == 4) ? static_cast<int>(dims[3]) : 1;

        SPDLOG_DEBUG("HDF5: dataset info {} -> count={}, H={}, W={}, C={}, ndims={}",
                     datasetPath, outCount, outHeight, outWidth, outChannels, ndims);
        return true;
    }

    bool Hdf5Service::readImageByIndex(const std::string& datasetPath,
                                       size_t index,
                                       cv::Mat& outImage) const
    {
        outImage.release();

        if (!isFileOpen())
        {
            SPDLOG_ERROR("readImageByIndex: HDF5 file is not open");
            return false;
        }

        SPDLOG_TRACE("readImageByIndex: path='{}', index={}", datasetPath, index);
        // Check existence
        htri_t exists = H5Lexists(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("readImageByIndex: dataset {} does not exist", datasetPath);
            return false;
        }

        // Open dataset
        hid_t datasetId = H5Dopen2(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("readImageByIndex: failed to open dataset {}", datasetPath);
            return false;
        }

        // Introspect shape
        hid_t filespaceId = H5Dget_space(datasetId);
        if (filespaceId < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("readImageByIndex: failed to get dataspace for {}", datasetPath);
            return false;
        }

        int ndims = H5Sget_simple_extent_ndims(filespaceId);
        hsize_t dims[4] = {0, 0, 0, 0};
        if (H5Sget_simple_extent_dims(filespaceId, dims, nullptr) < 0)
        {
            H5Sclose(filespaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readImageByIndex: failed to get extent for {}", datasetPath);
            return false;
        }

        if (ndims < 3 || ndims > 4)
        {
            H5Sclose(filespaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readImageByIndex: unsupported ndims={} for {}", ndims, datasetPath);
            return false;
        }

        const hsize_t numFrames = dims[0];
        const int height = static_cast<int>(dims[1]);
        const int width = static_cast<int>(dims[2]);
        const int channels = (ndims == 4) ? static_cast<int>(dims[3]) : 1;

        SPDLOG_TRACE("readImageByIndex: dims: N={}, H={}, W={}, C={}, ndims={}",
                     numFrames, height, width, channels, ndims);
        if (index >= static_cast<size_t>(numFrames))
        {
            H5Sclose(filespaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readImageByIndex: index {} out of range [0, {}) for {}", index, numFrames, datasetPath);
            return false;
        }

        // Prepare selection in filespace
        hsize_t start[4] = {static_cast<hsize_t>(index), 0, 0, 0};
        hsize_t count[4];
        if (channels == 1)
        {
            // 3D dataset: (1, H, W)
            count[0] = 1;
            count[1] = static_cast<hsize_t>(height);
            count[2] = static_cast<hsize_t>(width);
            H5Sselect_hyperslab(filespaceId, H5S_SELECT_SET, start, nullptr, count, nullptr);
        }
        else
        {
            // 4D dataset: (1, H, W, C)
            count[0] = 1;
            count[1] = static_cast<hsize_t>(height);
            count[2] = static_cast<hsize_t>(width);
            count[3] = static_cast<hsize_t>(channels);
            H5Sselect_hyperslab(filespaceId, H5S_SELECT_SET, start, nullptr, count, nullptr);
        }

        // Create matching memspace
        hid_t memspaceId = H5Screate_simple((channels == 1) ? 3 : 4, count, nullptr);
        if (memspaceId < 0)
        {
            H5Sclose(filespaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readImageByIndex: failed to create memspace for {}", datasetPath);
            return false;
        }

        // Prepare OpenCV Mat
        const int type = CV_8UC(channels);
        outImage = cv::Mat(height, width, type);
        if (!outImage.isContinuous())
        {
            // Guarantee contiguous buffer
            outImage = outImage.clone();
        }

        // Read directly into Mat buffer
        herr_t status = H5Dread(datasetId, H5T_NATIVE_UINT8, memspaceId, filespaceId, H5P_DEFAULT, outImage.data);

        H5Sclose(memspaceId);
        H5Sclose(filespaceId);
        H5Dclose(datasetId);

        if (status < 0)
        {
            outImage.release();
            SPDLOG_ERROR("readImageByIndex: H5Dread failed for {}[{}]", datasetPath, index);
            return false;
        }

        SPDLOG_TRACE("readImageByIndex: success for {}[{}], bytes={}",
                     datasetPath, index, static_cast<size_t>(height) * width * channels);
        return true;
    }

    bool Hdf5Service::readBackgroundImage(cv::Mat& out) const
    {
        out.release();
        if (!isFileOpen())
            return false;
        return readImageByIndex("/experiment_info/background", 0, out);
    }

    bool Hdf5Service::readImagesRange(const std::string& datasetPath,
                                      size_t startIndex,
                                      size_t count,
                                      std::vector<cv::Mat>& outImages) const
    {
        outImages.clear();

        // Validate dataset and get count
        size_t total = 0;
        int h = 0, w = 0, c = 0;
        if (!getDatasetInfo(datasetPath, total, h, w, c))
        {
            return false;
        }
        if (startIndex >= total)
        {
            SPDLOG_WARN("readImagesRange: startIndex {} out of range for {} (total={})", startIndex, datasetPath, total);
            return false;
        }

        if (count > 4096)
        {
            SPDLOG_WARN("readImagesRange: requested count {} is large for {}, consider batching to avoid memory spikes", count, datasetPath);
        }

        SPDLOG_DEBUG("readImagesRange: path='{}', start={}, count={}, total={}, H={}, W={}, C={}",
                     datasetPath, startIndex, count, total, h, w, c);
        const size_t endIndex = std::min(total, startIndex + count);
        outImages.reserve(endIndex - startIndex);
        for (size_t i = startIndex; i < endIndex; ++i)
        {
            cv::Mat img;
            if (!readImageByIndex(datasetPath, i, img))
            {
                SPDLOG_ERROR("readImagesRange: failed to read {}[{}]", datasetPath, i);
                return false;
            }
            outImages.push_back(std::move(img));
        }
        SPDLOG_DEBUG("readImagesRange: loaded {} images from {}", outImages.size(), datasetPath);
        return true;
    }

    bool Hdf5Service::readValidMetadata(std::vector<ProcessedFrame>& frames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("readValidMetadata: HDF5 file is not open");
            return false;
        }
        frames.clear();
        if (!readMetadataDataset(impl_->fileId_, "/valid_frames/metadata", frames))
        {
            return false;
        }
        // Ensure image payloads are empty for metadata-only reads
        for (auto& f : frames)
        {
            f.originalImage.release();
            f.processedImage.release();
        }
        SPDLOG_INFO("readValidMetadata: {} entries", frames.size());
        return true;
    }

    bool Hdf5Service::readInvalidMetadata(std::vector<ProcessedFrame>& frames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("readInvalidMetadata: HDF5 file is not open");
            return false;
        }
        frames.clear();
        if (!readMetadataDataset(impl_->fileId_, "/invalid_frames/metadata", frames))
        {
            return false;
        }
        for (auto& f : frames)
        {
            f.originalImage.release();
            f.processedImage.release();
        }
        SPDLOG_INFO("readInvalidMetadata: {} entries", frames.size());
        return true;
    }

    bool Hdf5Service::saveChartSnapshot(const std::string& datasetPath, const cv::Mat& image)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        if (image.empty())
        {
            SPDLOG_WARN("Cannot save empty chart snapshot to {}", datasetPath);
            return false;
        }

        // Extract parent group path and ensure it exists
        size_t lastSlash = datasetPath.find_last_of('/');
        if (lastSlash != std::string::npos && lastSlash > 0)
        {
            std::string parentPath = datasetPath.substr(0, lastSlash);
            htri_t exists = H5Lexists(impl_->fileId_, parentPath.c_str(), H5P_DEFAULT);
            if (exists <= 0)
            {
                // Create parent group (and any intermediate groups)
                // Split path and create groups recursively
                std::string currentPath;
                size_t start = 0;
                while (start < parentPath.length())
                {
                    size_t nextSlash = parentPath.find('/', start);
                    if (nextSlash == std::string::npos)
                    {
                        currentPath = parentPath;
                        start = parentPath.length();
                    }
                    else
                    {
                        currentPath = parentPath.substr(0, nextSlash);
                        start = nextSlash + 1;
                    }
                    
                    if (!currentPath.empty() && currentPath != "/")
                    {
                        htri_t groupExists = H5Lexists(impl_->fileId_, currentPath.c_str(), H5P_DEFAULT);
                        if (groupExists <= 0)
                        {
                            hid_t groupId = H5Gcreate2(impl_->fileId_, currentPath.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                            if (groupId >= 0)
                            {
                                H5Gclose(groupId);
                            }
                        }
                    }
                }
            }
        }

        int height = image.rows;
        int width = image.cols;
        int channels = image.channels();

        // Create dataspace for single image
        hsize_t dims[4];
        int ndims;
        if (channels == 1)
        {
            ndims = 2; // 2D: height, width
            dims[0] = static_cast<hsize_t>(height);
            dims[1] = static_cast<hsize_t>(width);
        }
        else
        {
            ndims = 3; // 3D: height, width, channels
            dims[0] = static_cast<hsize_t>(height);
            dims[1] = static_cast<hsize_t>(width);
            dims[2] = static_cast<hsize_t>(channels);
        }

        hid_t dataspaceId = H5Screate_simple(ndims, dims, nullptr);
        if (dataspaceId < 0)
        {
            SPDLOG_ERROR("Failed to create dataspace for chart snapshot {}", datasetPath);
            return false;
        }

        // Create dataset
        hid_t datasetId = H5Dcreate2(impl_->fileId_, datasetPath.c_str(), H5T_NATIVE_UINT8, dataspaceId,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (datasetId < 0)
        {
            H5Sclose(dataspaceId);
            SPDLOG_ERROR("Failed to create chart snapshot dataset {}", datasetPath);
            return false;
        }

        // Prepare data buffer
        size_t imageSize = height * width * channels;
        std::vector<uint8_t> buffer(imageSize);
        
        if (image.isContinuous())
        {
            std::memcpy(buffer.data(), image.data, image.total() * image.elemSize());
        }
        else
        {
            size_t offset = 0;
            for (int r = 0; r < image.rows; r++)
            {
                std::memcpy(buffer.data() + offset, image.ptr(r), image.cols * image.elemSize());
                offset += image.cols * image.elemSize();
            }
        }

        // Write data
        herr_t status = H5Dwrite(datasetId, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());
        if (status < 0)
        {
            SPDLOG_ERROR("Failed to write chart snapshot {}", datasetPath);
            H5Dclose(datasetId);
            H5Sclose(dataspaceId);
            return false;
        }

        // Add attributes for metadata
        hid_t scalarSpaceId = H5Screate(H5S_SCALAR);
        if (scalarSpaceId >= 0)
        {
            int32_t widthAttr = static_cast<int32_t>(width);
            int32_t heightAttr = static_cast<int32_t>(height);
            int32_t channelsAttr = static_cast<int32_t>(channels);

            hid_t attrW = H5Acreate2(datasetId, "width", H5T_NATIVE_INT32, scalarSpaceId, H5P_DEFAULT, H5P_DEFAULT);
            if (attrW >= 0)
            {
                H5Awrite(attrW, H5T_NATIVE_INT32, &widthAttr);
                H5Aclose(attrW);
            }

            hid_t attrH = H5Acreate2(datasetId, "height", H5T_NATIVE_INT32, scalarSpaceId, H5P_DEFAULT, H5P_DEFAULT);
            if (attrH >= 0)
            {
                H5Awrite(attrH, H5T_NATIVE_INT32, &heightAttr);
                H5Aclose(attrH);
            }

            hid_t attrC = H5Acreate2(datasetId, "channels", H5T_NATIVE_INT32, scalarSpaceId, H5P_DEFAULT, H5P_DEFAULT);
            if (attrC >= 0)
            {
                H5Awrite(attrC, H5T_NATIVE_INT32, &channelsAttr);
                H5Aclose(attrC);
            }

            H5Sclose(scalarSpaceId);
        }

        H5Dclose(datasetId);
        H5Sclose(dataspaceId);
        SPDLOG_DEBUG("Saved chart snapshot to {} ({}x{}x{})", datasetPath, height, width, channels);
        return true;
    }

    bool Hdf5Service::readChartSnapshot(const std::string& datasetPath, cv::Mat& outImage) const
    {
        outImage.release();

        if (!isFileOpen())
        {
            SPDLOG_ERROR("readChartSnapshot: HDF5 file is not open");
            return false;
        }

        // Check existence - suppress HDF5 diagnostic errors for non-existent links
        H5E_auto_t old_func;
        void* old_client_data;
        H5Eget_auto(H5E_DEFAULT, &old_func, &old_client_data);
        H5Eset_auto(H5E_DEFAULT, nullptr, nullptr);
        
        htri_t exists = H5Lexists(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        
        // Restore error handling
        H5Eset_auto(H5E_DEFAULT, old_func, old_client_data);
        
        if (exists <= 0)
        {
            SPDLOG_DEBUG("readChartSnapshot: dataset {} does not exist", datasetPath);
            return false;
        }

        // Open dataset
        hid_t datasetId = H5Dopen2(impl_->fileId_, datasetPath.c_str(), H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("readChartSnapshot: failed to open dataset {}", datasetPath);
            return false;
        }

        // Get dataspace
        hid_t dataspaceId = H5Dget_space(datasetId);
        if (dataspaceId < 0)
        {
            H5Dclose(datasetId);
            SPDLOG_ERROR("readChartSnapshot: failed to get dataspace for {}", datasetPath);
            return false;
        }

        // Get dimensions
        int ndims = H5Sget_simple_extent_ndims(dataspaceId);
        hsize_t dims[4] = {0, 0, 0, 0};
        if (H5Sget_simple_extent_dims(dataspaceId, dims, nullptr) < 0)
        {
            H5Sclose(dataspaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readChartSnapshot: failed to get extent for {}", datasetPath);
            return false;
        }

        // Chart snapshots are stored as 2D (H, W) or 3D (H, W, C)
        int height = 0;
        int width = 0;
        int channels = 1;

        if (ndims == 2)
        {
            // 2D: (H, W) - grayscale
            height = static_cast<int>(dims[0]);
            width = static_cast<int>(dims[1]);
            channels = 1;
        }
        else if (ndims == 3)
        {
            // 3D: (H, W, C) - color
            height = static_cast<int>(dims[0]);
            width = static_cast<int>(dims[1]);
            channels = static_cast<int>(dims[2]);
        }
        else
        {
            H5Sclose(dataspaceId);
            H5Dclose(datasetId);
            SPDLOG_ERROR("readChartSnapshot: unsupported ndims={} for {} (expected 2 or 3)", ndims, datasetPath);
            return false;
        }

        SPDLOG_DEBUG("readChartSnapshot: reading {}x{}x{} from {}", height, width, channels, datasetPath);

        // Allocate output image
        int cvType = (channels == 1) ? CV_8UC1 : CV_8UC3;
        outImage = cv::Mat(height, width, cvType);

        // Read data
        herr_t status = H5Dread(datasetId, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, outImage.data);
        
        H5Sclose(dataspaceId);
        H5Dclose(datasetId);

        if (status < 0)
        {
            outImage.release();
            SPDLOG_ERROR("readChartSnapshot: failed to read data from {}", datasetPath);
            return false;
        }

        // Note: Charts are saved as BGR (OpenCV format), matToQImage will handle BGR->RGB conversion
        SPDLOG_DEBUG("readChartSnapshot: successfully read {}x{}x{} from {}", height, width, channels, datasetPath);
        return true;
    }

    // --- Multi-image series read support ---

    bool Hdf5Service::getSeriesImageInfo(size_t& outCount, size_t& outSeriesCount,
                                          int& outHeight, int& outWidth) const
    {
        if (!isFileOpen()) return false;

        hid_t dsId = H5Dopen2(impl_->fileId_, "/valid_frames/series_images", H5P_DEFAULT);
        if (dsId < 0) return false;

        hid_t spaceId = H5Dget_space(dsId);
        int ndims = H5Sget_simple_extent_ndims(spaceId);
        if (ndims != 4) {
            H5Sclose(spaceId);
            H5Dclose(dsId);
            return false;
        }
        hsize_t dims[4];
        H5Sget_simple_extent_dims(spaceId, dims, nullptr);
        H5Sclose(spaceId);
        H5Dclose(dsId);

        outCount = static_cast<size_t>(dims[0]);
        outSeriesCount = static_cast<size_t>(dims[1]);
        outHeight = static_cast<int>(dims[2]);
        outWidth = static_cast<int>(dims[3]);
        return true;
    }

    bool Hdf5Service::readSeriesImagesByIndex(size_t index, std::vector<cv::Mat>& outImages) const
    {
        if (!isFileOpen()) return false;

        hid_t dsId = H5Dopen2(impl_->fileId_, "/valid_frames/series_images", H5P_DEFAULT);
        if (dsId < 0) return false;

        hid_t spaceId = H5Dget_space(dsId);
        hsize_t dims[4];
        H5Sget_simple_extent_dims(spaceId, dims, nullptr);

        if (static_cast<hsize_t>(index) >= dims[0]) {
            H5Sclose(spaceId);
            H5Dclose(dsId);
            return false;
        }

        const size_t seriesCount = static_cast<size_t>(dims[1]);
        const int height = static_cast<int>(dims[2]);
        const int width = static_cast<int>(dims[3]);

        outImages.clear();
        outImages.reserve(seriesCount);

        for (size_t s = 0; s < seriesCount; ++s) {
            hsize_t start[4] = {static_cast<hsize_t>(index), static_cast<hsize_t>(s), 0, 0};
            hsize_t count[4] = {1, 1, static_cast<hsize_t>(height), static_cast<hsize_t>(width)};
            hid_t fileSpace = H5Dget_space(dsId);
            H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start, nullptr, count, nullptr);
            hid_t memSpace = H5Screate_simple(4, count, nullptr);

            cv::Mat img(height, width, CV_8UC1);
            herr_t status = H5Dread(dsId, H5T_NATIVE_UINT8, memSpace, fileSpace, H5P_DEFAULT, img.data);
            H5Sclose(memSpace);
            H5Sclose(fileSpace);
            if (status < 0) {
                H5Sclose(spaceId);
                H5Dclose(dsId);
                return false;
            }
            outImages.push_back(std::move(img));
        }

        H5Sclose(spaceId);
        H5Dclose(dsId);
        return true;
    }

    // --- Frame recording mode implementation ---

    bool Hdf5Service::isRecordingFile() const
    {
        if (!isFileOpen())
            return false;
        return H5Lexists(impl_->fileId_, "/recording_info", H5P_DEFAULT) > 0;
    }

    bool Hdf5Service::readRecordingMetadata(std::vector<ProcessedFrame>& frames)
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("readRecordingMetadata: HDF5 file is not open");
            return false;
        }
        frames.clear();

        const char* datasetPath = "/recorded_frames/metadata";
        htri_t exists = H5Lexists(impl_->fileId_, datasetPath, H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("Dataset {} does not exist", datasetPath);
            return false;
        }

        hid_t datasetId = H5Dopen2(impl_->fileId_, datasetPath, H5P_DEFAULT);
        if (datasetId < 0)
        {
            SPDLOG_ERROR("Failed to open recording metadata dataset {}", datasetPath);
            return false;
        }

        hid_t dataspaceId = H5Dget_space(datasetId);
        hsize_t dims[1] = {0};
        H5Sget_simple_extent_dims(dataspaceId, dims, nullptr);
        H5Sclose(dataspaceId);

        hsize_t numFrames = dims[0];
        if (numFrames == 0)
        {
            H5Dclose(datasetId);
            SPDLOG_INFO("readRecordingMetadata: 0 entries");
            return true;
        }

        // Matches the RecMeta compound written in appendRecordingFrames.
        // Build the in-memory read type explicitly (don't rely on the file's
        // compound definition) so member ordering stays fixed.
        struct RecMeta
        {
            uint64_t index;
            uint64_t timestampNs;
            uint64_t width;
            uint64_t height;
        };

        hid_t compTypeId = H5Tcreate(H5T_COMPOUND, sizeof(RecMeta));
        H5Tinsert(compTypeId, "index", HOFFSET(RecMeta, index), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "timestampNs", HOFFSET(RecMeta, timestampNs), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "width", HOFFSET(RecMeta, width), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "height", HOFFSET(RecMeta, height), H5T_NATIVE_UINT64);

        std::vector<RecMeta> entries(numFrames);
        herr_t status = H5Dread(datasetId, compTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, entries.data());
        H5Tclose(compTypeId);
        H5Dclose(datasetId);

        if (status < 0)
        {
            SPDLOG_ERROR("Failed to read recording metadata dataset {}", datasetPath);
            return false;
        }

        frames.reserve(numFrames);
        for (const auto& e : entries)
        {
            ProcessedFrame frame;
            frame.index = e.index;
            frame.timestampNs = e.timestampNs;
            // width/height are implicit in the images dataset; other
            // ProcessedFrame fields (validation, masks, images) remain default.
            frames.push_back(std::move(frame));
        }

        SPDLOG_INFO("readRecordingMetadata: {} entries", frames.size());
        return true;
    }

    bool Hdf5Service::readRecordingInfo(uint64_t& startTimeNs, uint64_t& endTimeNs,
                                        uint64_t& totalFrames, uint64_t& filteredFrames,
                                        bool* multiImageEnabled,
                                        uint64_t* multiImageCount)
    {
        startTimeNs = 0;
        endTimeNs = 0;
        totalFrames = 0;
        filteredFrames = 0;
        if (multiImageEnabled) {
            *multiImageEnabled = false;
        }
        if (multiImageCount) {
            *multiImageCount = 1;
        }

        if (!isFileOpen())
        {
            SPDLOG_ERROR("readRecordingInfo: HDF5 file is not open");
            return false;
        }

        htri_t exists = H5Lexists(impl_->fileId_, "/recording_info", H5P_DEFAULT);
        if (exists <= 0)
        {
            SPDLOG_WARN("readRecordingInfo: /recording_info group not found");
            return false;
        }

        hid_t groupId = H5Gopen2(impl_->fileId_, "/recording_info", H5P_DEFAULT);
        if (groupId < 0)
        {
            SPDLOG_ERROR("readRecordingInfo: failed to open /recording_info");
            return false;
        }

        auto readAttr = [&](const char* name, uint64_t& out) -> bool {
            if (H5Aexists(groupId, name) <= 0)
                return false;
            hid_t attr = H5Aopen(groupId, name, H5P_DEFAULT);
            if (attr < 0)
                return false;
            herr_t s = H5Aread(attr, H5T_NATIVE_UINT64, &out);
            H5Aclose(attr);
            return s >= 0;
        };

        readAttr("start_time_ns", startTimeNs);
        readAttr("end_time_ns", endTimeNs);
        readAttr("total_recorded_frames", totalFrames);
        readAttr("total_filtered_empty_frames", filteredFrames);
        if (multiImageEnabled && H5Aexists(groupId, "multi_image_enabled") > 0) {
            hid_t attr = H5Aopen(groupId, "multi_image_enabled", H5P_DEFAULT);
            if (attr >= 0) {
                uint8_t enabled = 0;
                if (H5Aread(attr, H5T_NATIVE_UINT8, &enabled) >= 0) {
                    *multiImageEnabled = (enabled != 0);
                }
                H5Aclose(attr);
            }
        }
        if (multiImageCount && H5Aexists(groupId, "multi_image_count") > 0) {
            hid_t attr = H5Aopen(groupId, "multi_image_count", H5P_DEFAULT);
            if (attr >= 0) {
                uint64_t count = 1;
                if (H5Aread(attr, H5T_NATIVE_UINT64, &count) >= 0) {
                    *multiImageCount = std::max<uint64_t>(1, count);
                }
                H5Aclose(attr);
            }
        }

        H5Gclose(groupId);
        SPDLOG_INFO("readRecordingInfo: recorded={}, filtered={}, multi_image_enabled={}, multi_image_count={}",
                    totalFrames,
                    filteredFrames,
                    multiImageEnabled ? (*multiImageEnabled ? 1 : 0) : -1,
                    multiImageCount ? *multiImageCount : 0);
        return true;
    }

    bool Hdf5Service::initializeRecordingDatasets()
    {
        if (!isFileOpen())
        {
            SPDLOG_ERROR("HDF5 file is not open");
            return false;
        }

        // Create/open /recorded_frames group
        hid_t groupId = H5Gopen2(impl_->fileId_, "/recorded_frames", H5P_DEFAULT);
        if (groupId < 0)
        {
            groupId = H5Gcreate2(impl_->fileId_, "/recorded_frames", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        }
        if (groupId >= 0)
        {
            H5Gclose(groupId);
        }
        else
        {
            SPDLOG_ERROR("Failed to create/open /recorded_frames group");
            return false;
        }

        SPDLOG_DEBUG("HDF5 recording datasets initialized");
        return true;
    }

    bool Hdf5Service::appendRecordingFrames(const std::vector<cv::Mat>& images,
                                            const std::vector<RecordingFrameMeta>& metadata)
    {
        if (!isFileOpen() || images.empty())
            return images.empty(); // empty is success

        if (images.size() != metadata.size())
        {
            SPDLOG_ERROR("appendRecordingFrames: images ({}) and metadata ({}) size mismatch",
                         images.size(), metadata.size());
            return false;
        }

        // Track how many recording frames have been written using a simple counter.
        // We reuse validFramesWritten_ for the recording images dataset since recording
        // and experiment modes are mutually exclusive in practice.
        hsize_t alreadyWritten = impl_->validFramesWritten_;

        // Write/append images
        if (alreadyWritten == 0)
        {
            if (!writeImageDataset(impl_->fileId_, "/recorded_frames/images", images))
                return false;
        }
        else
        {
            if (!appendImageDataset(impl_->fileId_, "/recorded_frames/images", images, impl_->validFramesWritten_))
                return false;
        }

        // Write/append metadata (simple compound: index + timestamp + width + height)
        struct RecMeta
        {
            uint64_t index;
            uint64_t timestampNs;
            uint64_t width;
            uint64_t height;
        };

        hid_t compTypeId = H5Tcreate(H5T_COMPOUND, sizeof(RecMeta));
        H5Tinsert(compTypeId, "index", HOFFSET(RecMeta, index), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "timestampNs", HOFFSET(RecMeta, timestampNs), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "width", HOFFSET(RecMeta, width), H5T_NATIVE_UINT64);
        H5Tinsert(compTypeId, "height", HOFFSET(RecMeta, height), H5T_NATIVE_UINT64);

        std::vector<RecMeta> metaEntries(metadata.size());
        for (size_t i = 0; i < metadata.size(); ++i)
        {
            metaEntries[i].index = metadata[i].index;
            metaEntries[i].timestampNs = metadata[i].timestampNs;
            metaEntries[i].width = metadata[i].width;
            metaEntries[i].height = metadata[i].height;
        }

        const std::string metaPath = "/recorded_frames/metadata";

        if (alreadyWritten == 0)
        {
            // Create metadata dataset
            hsize_t dims[1] = {metaEntries.size()};
            hsize_t maxDims[1] = {H5S_UNLIMITED};
            hid_t dataspaceId = H5Screate_simple(1, dims, maxDims);

            hid_t propId = H5Pcreate(H5P_DATASET_CREATE);
            hsize_t chunkDims[1] = {std::min(static_cast<hsize_t>(1000), dims[0])};
            H5Pset_chunk(propId, 1, chunkDims);

            hid_t datasetId = H5Dcreate2(impl_->fileId_, metaPath.c_str(), compTypeId, dataspaceId,
                                         H5P_DEFAULT, propId, H5P_DEFAULT);
            H5Pclose(propId);

            if (datasetId < 0)
            {
                H5Sclose(dataspaceId);
                H5Tclose(compTypeId);
                SPDLOG_ERROR("Failed to create recording metadata dataset");
                return false;
            }

            herr_t status = H5Dwrite(datasetId, compTypeId, H5S_ALL, H5S_ALL, H5P_DEFAULT, metaEntries.data());
            H5Dclose(datasetId);
            H5Sclose(dataspaceId);

            if (status < 0)
            {
                H5Tclose(compTypeId);
                SPDLOG_ERROR("Failed to write recording metadata");
                return false;
            }
        }
        else
        {
            // Append to existing metadata dataset
            hid_t datasetId = H5Dopen2(impl_->fileId_, metaPath.c_str(), H5P_DEFAULT);
            if (datasetId < 0)
            {
                H5Tclose(compTypeId);
                SPDLOG_ERROR("Failed to open recording metadata dataset for append");
                return false;
            }

            hsize_t newSize[1] = {alreadyWritten + metaEntries.size()};
            H5Dset_extent(datasetId, newSize);

            hid_t filespace = H5Dget_space(datasetId);
            hsize_t offset[1] = {alreadyWritten};
            hsize_t count[1] = {metaEntries.size()};
            H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr, count, nullptr);

            hid_t memspace = H5Screate_simple(1, count, nullptr);
            herr_t status = H5Dwrite(datasetId, compTypeId, memspace, filespace, H5P_DEFAULT, metaEntries.data());

            H5Sclose(memspace);
            H5Sclose(filespace);
            H5Dclose(datasetId);

            if (status < 0)
            {
                H5Tclose(compTypeId);
                SPDLOG_ERROR("Failed to append recording metadata");
                return false;
            }
        }

        H5Tclose(compTypeId);

        if (alreadyWritten == 0)
            impl_->validFramesWritten_ = images.size();
        if (!maybeIntervalFlush())
        {
            SPDLOG_WARN("appendRecordingFrames: post-write flush failed");
        }
        SPDLOG_DEBUG("Recording: appended {} frames (total: {})", images.size(), impl_->validFramesWritten_);
        return true;
    }

    bool Hdf5Service::writeRecordingInfo(uint64_t startTimeNs, uint64_t endTimeNs,
                                         uint64_t totalFrames, uint64_t filteredFrames,
                                         bool multiImageEnabled,
                                         uint64_t multiImageCount,
                                         const backend::processing::ProcessingCoreIdentity* processingCore)
    {
        if (!isFileOpen())
            return false;

        hid_t infoGroupId = H5Gopen2(impl_->fileId_, "/recording_info", H5P_DEFAULT);
        if (infoGroupId < 0)
        {
            infoGroupId = H5Gcreate2(impl_->fileId_, "/recording_info", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        }
        if (infoGroupId < 0)
        {
            SPDLOG_ERROR("Failed to create recording_info group");
            return false;
        }

        hid_t scalarSpaceId = H5Screate(H5S_SCALAR);

        auto writeAttr = [&](const char* name, uint64_t value) {
            hid_t attr = H5Aopen(infoGroupId, name, H5P_DEFAULT);
            if (attr < 0)
            {
                attr = H5Acreate2(infoGroupId, name, H5T_NATIVE_UINT64, scalarSpaceId,
                                  H5P_DEFAULT, H5P_DEFAULT);
            }
            if (attr >= 0)
            {
                H5Awrite(attr, H5T_NATIVE_UINT64, &value);
                H5Aclose(attr);
            }
        };

        writeAttr("start_time_ns", startTimeNs);
        writeAttr("end_time_ns", endTimeNs);
        writeAttr("total_recorded_frames", totalFrames);
        writeAttr("total_filtered_empty_frames", filteredFrames);

        const uint8_t multiImageEnabledValue = multiImageEnabled ? 1 : 0;
        hid_t multiEnabledAttr = H5Aopen(infoGroupId, "multi_image_enabled", H5P_DEFAULT);
        if (multiEnabledAttr < 0)
        {
            multiEnabledAttr = H5Acreate2(infoGroupId, "multi_image_enabled", H5T_NATIVE_UINT8, scalarSpaceId,
                                          H5P_DEFAULT, H5P_DEFAULT);
        }
        if (multiEnabledAttr >= 0)
        {
            H5Awrite(multiEnabledAttr, H5T_NATIVE_UINT8, &multiImageEnabledValue);
            H5Aclose(multiEnabledAttr);
        }

        const uint64_t multiImageCountValue = std::max<uint64_t>(1, multiImageCount);
        hid_t multiCountAttr = H5Aopen(infoGroupId, "multi_image_count", H5P_DEFAULT);
        if (multiCountAttr < 0)
        {
            multiCountAttr = H5Acreate2(infoGroupId, "multi_image_count", H5T_NATIVE_UINT64, scalarSpaceId,
                                        H5P_DEFAULT, H5P_DEFAULT);
        }
        if (multiCountAttr >= 0)
        {
            H5Awrite(multiCountAttr, H5T_NATIVE_UINT64, &multiImageCountValue);
            H5Aclose(multiCountAttr);
        }

        const auto defaultIdentity = backend::processing::bundledProcessingCoreIdentity();
        const auto& coreIdentity = processingCore ? *processingCore : defaultIdentity;
        const auto writeUint32Attribute = [&](const char* name, uint32_t value) {
            hid_t attribute = H5Aopen(infoGroupId, name, H5P_DEFAULT);
            if (attribute < 0) {
                attribute = H5Acreate2(infoGroupId, name, H5T_NATIVE_UINT32, scalarSpaceId,
                                       H5P_DEFAULT, H5P_DEFAULT);
            }
            if (attribute < 0) return false;
            const bool ok = H5Awrite(attribute, H5T_NATIVE_UINT32, &value) >= 0;
            H5Aclose(attribute);
            return ok;
        };
        const auto writeStringAttribute = [&](const char* name, const std::string& value) {
            hid_t stringType = H5Tcopy(H5T_C_S1);
            if (stringType < 0) return false;
            H5Tset_size(stringType, H5T_VARIABLE);
            H5Tset_cset(stringType, H5T_CSET_UTF8);
            hid_t attribute = H5Aopen(infoGroupId, name, H5P_DEFAULT);
            if (attribute < 0) {
                attribute = H5Acreate2(infoGroupId, name, stringType, scalarSpaceId,
                                       H5P_DEFAULT, H5P_DEFAULT);
            }
            bool ok = false;
            if (attribute >= 0) {
                const char* pointer = value.c_str();
                ok = H5Awrite(attribute, stringType, &pointer) >= 0;
                H5Aclose(attribute);
            }
            H5Tclose(stringType);
            return ok;
        };
        const bool provenanceOk =
            writeStringAttribute("processing_core_version", coreIdentity.version) &&
            writeUint32Attribute("processing_contract_version", coreIdentity.contractVersion) &&
            writeUint32Attribute("processing_engine_abi_version", coreIdentity.engineAbiVersion) &&
            writeStringAttribute("processing_core_sha256", coreIdentity.artifactSha256) &&
            writeStringAttribute("processing_release_tag", coreIdentity.releaseTag) &&
            writeStringAttribute("processing_manifest_sha256", coreIdentity.manifestSha256) &&
            writeStringAttribute("processing_core_source", coreIdentity.source) &&
            writeStringAttribute("processing_core_build_id", coreIdentity.buildId) &&
            writeStringAttribute("processing_runtime_fingerprint", coreIdentity.runtimeFingerprint);

        // Mark recording mode
        const char* mode = "frame_recording";
        hid_t strType = H5Tcopy(H5T_C_S1);
        H5Tset_size(strType, std::strlen(mode) + 1);
        hid_t modeAttr = H5Aopen(infoGroupId, "mode", H5P_DEFAULT);
        if (modeAttr < 0)
        {
            modeAttr = H5Acreate2(infoGroupId, "mode", strType, scalarSpaceId,
                                  H5P_DEFAULT, H5P_DEFAULT);
        }
        if (modeAttr >= 0)
        {
            H5Awrite(modeAttr, strType, mode);
            H5Aclose(modeAttr);
        }
        H5Tclose(strType);

        H5Sclose(scalarSpaceId);
        H5Gclose(infoGroupId);

        if (!provenanceOk) {
            SPDLOG_ERROR("Failed to persist recording processing-core provenance");
            return false;
        }

        if (!flush())
        {
            SPDLOG_WARN("writeRecordingInfo: post-write flush failed");
        }
        SPDLOG_INFO("Recording info written: recorded={}, filtered={}, multi_image_enabled={}, multi_image_count={}",
                    totalFrames,
                    filteredFrames,
                    multiImageEnabledValue,
                    multiImageCountValue);
        return true;
    }

    // ---- Run accounting provenance (issue #367) ------------------------------

    namespace {
    const char* runInfoGroupPath(hid_t fileId)
    {
        if (H5Lexists(fileId, "/experiment_info", H5P_DEFAULT) > 0) return "/experiment_info";
        if (H5Lexists(fileId, "/recording_info", H5P_DEFAULT) > 0) return "/recording_info";
        return nullptr;
    }
    } // namespace

    bool Hdf5Service::writeRunAccounting(const backend::recording::RecordingAccountingSnapshot& in)
    {
        if (!isFileOpen()) return false;
        const char* groupPath = runInfoGroupPath(impl_->fileId_);
        if (!groupPath) {
            SPDLOG_ERROR("writeRunAccounting: neither /experiment_info nor /recording_info exists");
            return false;
        }
        // Always persist the reconciled form so the stored completion state
        // can never disagree with the stored counters.
        const auto a = backend::recording::reconcile(in);
        hid_t group = H5Gopen2(impl_->fileId_, groupPath, H5P_DEFAULT);
        if (group < 0) return false;
        hid_t scalar = H5Screate(H5S_SCALAR);
        bool ok = true;
        auto writeU64 = [&](const char* name, uint64_t value) {
            hid_t attr = H5Aexists(group, name) > 0
                             ? H5Aopen(group, name, H5P_DEFAULT)
                             : H5Acreate2(group, name, H5T_NATIVE_UINT64, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr < 0) { ok = false; return; }
            if (H5Awrite(attr, H5T_NATIVE_UINT64, &value) < 0) ok = false;
            H5Aclose(attr);
        };
        auto writeU8 = [&](const char* name, bool value) {
            const uint8_t v = value ? 1 : 0;
            hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
            if (attr < 0) attr = H5Acreate2(group, name, H5T_NATIVE_UINT8, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr < 0) { ok = false; return; }
            if (H5Awrite(attr, H5T_NATIVE_UINT8, &v) < 0) ok = false;
            H5Aclose(attr);
        };
        auto writeStr = [&](const char* name, const std::string& value) {
            if (H5Aexists(group, name) > 0) H5Adelete(group, name);
            hid_t type = H5Tcopy(H5T_C_S1);
            H5Tset_size(type, H5T_VARIABLE);
            H5Tset_cset(type, H5T_CSET_UTF8);
            hid_t attr = H5Acreate2(group, name, type, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr >= 0) {
                const char* ptr = value.c_str();
                if (H5Awrite(attr, type, &ptr) < 0) ok = false;
                H5Aclose(attr);
            } else {
                ok = false;
            }
            H5Tclose(type);
        };

        writeU64("accounting_schema_version", backend::recording::RecordingAccountingSnapshot::kSchemaVersion);
        writeU64("accounting_admitted_frames", a.admitted);
        writeU64("accounting_empty_frames", a.empty);
        writeU64("accounting_processed_frames", a.processed);
        writeU64("accounting_scientifically_rejected_frames", a.scientificallyRejected);
        writeU64("accounting_processing_failed_frames", a.processingFailed);
        writeU64("accounting_store_overwritten_frames", a.storeOverwritten);
        writeU64("accounting_store_not_committed_frames", a.storeNotCommitted);
        writeU64("accounting_store_malformed_frames", a.storeMalformed);
        writeU64("accounting_cancelled_by_policy_frames", a.cancelledByPolicy);
        writeU64("accounting_pending_at_stop_frames", a.pendingAtStop);
        writeU64("accounting_persistence_admitted_frames", a.persistenceAdmitted);
        writeU64("accounting_persistence_committed_frames", a.persistenceCommitted);
        writeU64("accounting_persistence_failed_frames", a.persistenceFailed);
        writeU64("accounting_persistence_pending_at_stop_frames", a.persistencePendingAtStop);
        writeU64("accounting_persistence_cancelled_by_policy_frames", a.persistenceCancelledByPolicy);
        writeU64("accounting_objects_detected", a.objectsDetected);
        writeU8("accounting_has_index_range", a.hasIndexRange);
        writeU64("accounting_first_frame_index", a.firstFrameIndex);
        writeU64("accounting_last_frame_index", a.lastFrameIndex);
        writeU64("accounting_sequence_gaps", a.sequenceGaps);
        writeU64("accounting_sequence_gap_frames", a.sequenceGapFrames);
        writeU64("accounting_session_generation", a.sessionGeneration);
        writeU8("accounting_policy_allows_drops", a.policyAllowsDrops);
        writeU8("accounting_fatal_error", a.fatalError);
        writeStr("accounting_fatal_message", a.fatalMessage);
        writeStr("accounting_completion_state", backend::recording::toString(a.completion));
        writeStr("accounting_completion_reason", a.completionReason);
        writeU8("accounting_reconciled", a.reconciled);

        H5Sclose(scalar);
        H5Gclose(group);
        if (!ok) {
            SPDLOG_ERROR("writeRunAccounting: failed to persist one or more accounting attributes");
            return false;
        }
        SPDLOG_INFO("Run accounting persisted on {}: completion={} reconciled={} admitted={} "
                    "empty={} processed={} rejected={} processingFailed={} storeLoss={} "
                    "persisted={}/{} failed={}",
                    groupPath, backend::recording::toString(a.completion), a.reconciled, a.admitted,
                    a.empty, a.processed, a.scientificallyRejected, a.processingFailed,
                    a.storeOverwritten + a.storeNotCommitted + a.storeMalformed,
                    a.persistenceCommitted, a.persistenceAdmitted, a.persistenceFailed);
        return true;
    }

    bool Hdf5Service::readRunAccounting(backend::recording::RecordingAccountingSnapshot& out) const
    {
        out = backend::recording::RecordingAccountingSnapshot{};
        out.completion = backend::recording::RunCompletionState::Unknown;
        if (!isFileOpen()) return false;
        const char* groupPath = runInfoGroupPath(impl_->fileId_);
        if (!groupPath) return false;
        hid_t group = H5Gopen2(impl_->fileId_, groupPath, H5P_DEFAULT);
        if (group < 0) return false;
        if (H5Aexists(group, "accounting_schema_version") <= 0) {
            // Legacy file: no accounting was recorded; explicitly Unknown.
            H5Gclose(group);
            return false;
        }
        auto readU64 = [&](const char* name, uint64_t& v) {
            if (H5Aexists(group, name) <= 0) return false;
            hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
            if (attr < 0) return false;
            const bool ok = H5Aread(attr, H5T_NATIVE_UINT64, &v) >= 0;
            H5Aclose(attr);
            return ok;
        };
        auto readU8 = [&](const char* name, bool& v) {
            if (H5Aexists(group, name) <= 0) return false;
            hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
            if (attr < 0) return false;
            uint8_t raw = 0;
            const bool ok = H5Aread(attr, H5T_NATIVE_UINT8, &raw) >= 0;
            H5Aclose(attr);
            if (ok) v = raw != 0;
            return ok;
        };
        auto readStr = [&](const char* name, std::string& v) {
            if (H5Aexists(group, name) <= 0) return false;
            hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
            if (attr < 0) return false;
            hid_t type = H5Aget_type(attr);
            bool ok = false;
            if (type >= 0 && H5Tget_class(type) == H5T_STRING && H5Tis_variable_str(type) > 0) {
                char* ptr = nullptr;
                ok = H5Aread(attr, type, &ptr) >= 0;
                if (ok) v = ptr ? ptr : "";
                if (ptr) H5free_memory(ptr);
            }
            if (type >= 0) H5Tclose(type);
            H5Aclose(attr);
            return ok;
        };
        readU64("accounting_schema_version", out.schemaVersion);
        readU64("accounting_admitted_frames", out.admitted);
        readU64("accounting_empty_frames", out.empty);
        readU64("accounting_processed_frames", out.processed);
        readU64("accounting_scientifically_rejected_frames", out.scientificallyRejected);
        readU64("accounting_processing_failed_frames", out.processingFailed);
        readU64("accounting_store_overwritten_frames", out.storeOverwritten);
        readU64("accounting_store_not_committed_frames", out.storeNotCommitted);
        readU64("accounting_store_malformed_frames", out.storeMalformed);
        readU64("accounting_cancelled_by_policy_frames", out.cancelledByPolicy);
        readU64("accounting_pending_at_stop_frames", out.pendingAtStop);
        readU64("accounting_persistence_admitted_frames", out.persistenceAdmitted);
        readU64("accounting_persistence_committed_frames", out.persistenceCommitted);
        readU64("accounting_persistence_failed_frames", out.persistenceFailed);
        readU64("accounting_persistence_pending_at_stop_frames", out.persistencePendingAtStop);
        readU64("accounting_persistence_cancelled_by_policy_frames", out.persistenceCancelledByPolicy);
        readU64("accounting_objects_detected", out.objectsDetected);
        readU8("accounting_has_index_range", out.hasIndexRange);
        readU64("accounting_first_frame_index", out.firstFrameIndex);
        readU64("accounting_last_frame_index", out.lastFrameIndex);
        readU64("accounting_sequence_gaps", out.sequenceGaps);
        readU64("accounting_sequence_gap_frames", out.sequenceGapFrames);
        readU64("accounting_session_generation", out.sessionGeneration);
        readU8("accounting_policy_allows_drops", out.policyAllowsDrops);
        readU8("accounting_fatal_error", out.fatalError);
        readStr("accounting_fatal_message", out.fatalMessage);
        std::string completion;
        readStr("accounting_completion_state", completion);
        out.completion = backend::recording::runCompletionStateFromString(completion);
        readStr("accounting_completion_reason", out.completionReason);
        readU8("accounting_reconciled", out.reconciled);
        H5Gclose(group);
        return true;
    }

    // ---- Acquisition time/telemetry provenance (issue #368) --------------------

    bool Hdf5Service::writeAcquisitionProvenance(const ::camera::common::TimestampDescriptor& d,
                                                 const AcquisitionTelemetrySnapshot& t)
    {
        if (!isFileOpen()) return false;
        const char* groupPath = runInfoGroupPath(impl_->fileId_);
        if (!groupPath) return false;
        hid_t group = H5Gopen2(impl_->fileId_, groupPath, H5P_DEFAULT);
        if (group < 0) return false;
        hid_t scalar = H5Screate(H5S_SCALAR);
        bool ok = true;
        auto writeU64 = [&](const std::string& name, uint64_t value) {
            hid_t attr = H5Aopen(group, name.c_str(), H5P_DEFAULT);
            if (attr < 0) attr = H5Acreate2(group, name.c_str(), H5T_NATIVE_UINT64, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr < 0) { ok = false; return; }
            if (H5Awrite(attr, H5T_NATIVE_UINT64, &value) < 0) ok = false;
            H5Aclose(attr);
        };
        auto writeStr = [&](const std::string& name, const std::string& value) {
            if (H5Aexists(group, name.c_str()) > 0) H5Adelete(group, name.c_str());
            hid_t type = H5Tcopy(H5T_C_S1);
            H5Tset_size(type, H5T_VARIABLE);
            H5Tset_cset(type, H5T_CSET_UTF8);
            hid_t attr = H5Acreate2(group, name.c_str(), type, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr >= 0) {
                const char* ptr = value.c_str();
                if (H5Awrite(attr, type, &ptr) < 0) ok = false;
                H5Aclose(attr);
            } else {
                ok = false;
            }
            H5Tclose(type);
        };
        writeU64("timestamp_schema_version", ::camera::common::TimestampDescriptor::kSchemaVersion);
        writeStr("timestamp_clock_domain", ::camera::common::toString(d.domain));
        writeU64("timestamp_ticks_per_second", d.ticksPerSecond);
        writeU64("timestamp_native_ticks_per_second", d.nativeTicksPerSecond);
        writeStr("timestamp_semantic", ::camera::common::toString(d.semantic));
        writeStr("timestamp_validity", ::camera::common::toString(d.validity));
        writeU64("timestamp_counter_bits", d.counterBits);
        writeU64("timestamp_session_generation", d.sessionGeneration);
        writeStr("timestamp_host_receipt_domain", "hostMonotonicUs"); // hostTimestampUs column semantics
        writeU64("telemetry_session_generation", t.sessionGeneration);
        auto writeMetric = [&](const char* name, const MetricSample& m) {
            const std::string base = std::string("telemetry_") + name;
            writeU64(base + "_value", m.value);
            writeStr(base + "_validity", toString(m.validity));
            writeU64(base + "_sample_host_time_us", m.sampleHostTimeUs);
        };
        writeMetric("frames_delivered", t.framesDelivered);
        writeMetric("capture_frame_rate", t.captureFrameRate);
        writeMetric("capture_data_rate_mbps", t.captureDataRateMBps);
        writeMetric("sdk_completed_queue_depth", t.sdkCompletedQueueDepth);
        writeMetric("sdk_input_buffer_count", t.sdkInputBufferCount);
        writeMetric("buffer_underruns", t.bufferUnderruns);
        writeMetric("transport_lost_frames", t.transportLostFrames);
        writeMetric("intentionally_discarded_frames", t.intentionallyDiscardedFrames);
        writeMetric("frame_age_us", t.frameAgeUs);
        writeMetric("publish_latency_us", t.publishLatencyUs);
        H5Sclose(scalar);
        H5Gclose(group);
        if (!ok) SPDLOG_ERROR("writeAcquisitionProvenance: failed to persist one or more attributes");
        return ok;
    }

    bool Hdf5Service::readAcquisitionProvenance(::camera::common::TimestampDescriptor& d,
                                                AcquisitionTelemetrySnapshot& t) const
    {
        d = {};
        d.validity = ::camera::common::TimestampValidity::Unsupported; // legacy default
        t = {};
        if (!isFileOpen()) return false;
        const char* groupPath = runInfoGroupPath(impl_->fileId_);
        if (!groupPath) return false;
        hid_t group = H5Gopen2(impl_->fileId_, groupPath, H5P_DEFAULT);
        if (group < 0) return false;
        if (H5Aexists(group, "timestamp_schema_version") <= 0) {
            H5Gclose(group);
            return false;
        }
        auto readU64 = [&](const std::string& name, uint64_t& v) {
            if (H5Aexists(group, name.c_str()) <= 0) return false;
            hid_t attr = H5Aopen(group, name.c_str(), H5P_DEFAULT);
            if (attr < 0) return false;
            const bool ok = H5Aread(attr, H5T_NATIVE_UINT64, &v) >= 0;
            H5Aclose(attr);
            return ok;
        };
        auto readStr = [&](const std::string& name, std::string& v) {
            if (H5Aexists(group, name.c_str()) <= 0) return false;
            hid_t attr = H5Aopen(group, name.c_str(), H5P_DEFAULT);
            if (attr < 0) return false;
            hid_t type = H5Aget_type(attr);
            bool ok = false;
            if (type >= 0 && H5Tget_class(type) == H5T_STRING && H5Tis_variable_str(type) > 0) {
                char* ptr = nullptr;
                ok = H5Aread(attr, type, &ptr) >= 0;
                if (ok) v = ptr ? ptr : "";
                if (ptr) H5free_memory(ptr);
            }
            if (type >= 0) H5Tclose(type);
            H5Aclose(attr);
            return ok;
        };
        std::string str;
        uint64_t u = 0;
        if (readStr("timestamp_clock_domain", str)) d.domain = ::camera::common::clockDomainFromString(str);
        if (readU64("timestamp_ticks_per_second", u)) d.ticksPerSecond = u;
        if (readU64("timestamp_native_ticks_per_second", u)) d.nativeTicksPerSecond = u;
        if (readStr("timestamp_semantic", str)) d.semantic = ::camera::common::timestampSemanticFromString(str);
        if (readStr("timestamp_validity", str)) d.validity = ::camera::common::timestampValidityFromString(str);
        if (readU64("timestamp_counter_bits", u)) d.counterBits = static_cast<uint32_t>(u);
        if (readU64("timestamp_session_generation", u)) d.sessionGeneration = u;
        if (readU64("telemetry_session_generation", u)) t.sessionGeneration = u;
        auto readMetric = [&](const char* name, MetricSample& m) {
            const std::string base = std::string("telemetry_") + name;
            readU64(base + "_value", m.value);
            if (readStr(base + "_validity", str)) m.validity = metricValidityFromString(str);
            readU64(base + "_sample_host_time_us", m.sampleHostTimeUs);
            m.sessionGeneration = t.sessionGeneration;
        };
        readMetric("frames_delivered", t.framesDelivered);
        readMetric("capture_frame_rate", t.captureFrameRate);
        readMetric("capture_data_rate_mbps", t.captureDataRateMBps);
        readMetric("sdk_completed_queue_depth", t.sdkCompletedQueueDepth);
        readMetric("sdk_input_buffer_count", t.sdkInputBufferCount);
        readMetric("buffer_underruns", t.bufferUnderruns);
        readMetric("transport_lost_frames", t.transportLostFrames);
        readMetric("intentionally_discarded_frames", t.intentionallyDiscardedFrames);
        readMetric("frame_age_us", t.frameAgeUs);
        readMetric("publish_latency_us", t.publishLatencyUs);
        t.timestampDescriptor = d;
        H5Gclose(group);
        return true;
    }

    // ---- Open-object diagnostics (issue #344) ------------------------------------

    long long Hdf5Service::globalOpenObjectCountForDiagnostics()
    {
        const ssize_t n = H5Fget_obj_count(static_cast<hid_t>(H5F_OBJ_ALL), H5F_OBJ_ALL);
        return n < 0 ? -1 : static_cast<long long>(n);
    }

    long long Hdf5Service::openObjectCountForDiagnostics() const
    {
        if (!isFileOpen()) return 0;
        const ssize_t n = H5Fget_obj_count(impl_->fileId_, H5F_OBJ_ALL);
        return n < 0 ? -1 : static_cast<long long>(n);
    }

    // ---- Run configuration snapshot (issue #369) --------------------------------

    bool Hdf5Service::writeRunSnapshotJson(const std::string& runSnapshotJson,
                                           const std::string& readinessJson)
    {
        if (!isFileOpen()) return false;
        hid_t group = H5Lexists(impl_->fileId_, "/run_provenance", H5P_DEFAULT) > 0
                          ? H5Gopen2(impl_->fileId_, "/run_provenance", H5P_DEFAULT)
                          : H5Gcreate2(impl_->fileId_, "/run_provenance", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (group < 0) return false;
        hid_t scalar = H5Screate(H5S_SCALAR);
        bool ok = true;
        auto writeStr = [&](const char* name, const std::string& value) {
            if (H5Aexists(group, name) > 0) H5Adelete(group, name);
            hid_t type = H5Tcopy(H5T_C_S1);
            H5Tset_size(type, H5T_VARIABLE);
            H5Tset_cset(type, H5T_CSET_UTF8);
            hid_t attr = H5Acreate2(group, name, type, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr >= 0) {
                const char* ptr = value.c_str();
                if (H5Awrite(attr, type, &ptr) < 0) ok = false;
                H5Aclose(attr);
            } else {
                ok = false;
            }
            H5Tclose(type);
        };
        auto writeU64 = [&](const char* name, uint64_t value) {
            hid_t attr = H5Aexists(group, name) > 0
                             ? H5Aopen(group, name, H5P_DEFAULT)
                             : H5Acreate2(group, name, H5T_NATIVE_UINT64, scalar, H5P_DEFAULT, H5P_DEFAULT);
            if (attr < 0) { ok = false; return; }
            if (H5Awrite(attr, H5T_NATIVE_UINT64, &value) < 0) ok = false;
            H5Aclose(attr);
        };
        writeU64("run_snapshot_schema_version", 1);
        writeStr("run_snapshot_json", runSnapshotJson);
        writeStr("readiness_json", readinessJson);
        H5Sclose(scalar);
        H5Gclose(group);
        if (ok && !flush()) SPDLOG_WARN("writeRunSnapshotJson: flush after provenance write failed");
        return ok;
    }

    bool Hdf5Service::readRunSnapshotJson(std::string& runSnapshotJson, std::string* readinessJson) const
    {
        runSnapshotJson.clear();
        if (readinessJson) readinessJson->clear();
        if (!isFileOpen()) return false;
        if (H5Lexists(impl_->fileId_, "/run_provenance", H5P_DEFAULT) <= 0) return false;
        hid_t group = H5Gopen2(impl_->fileId_, "/run_provenance", H5P_DEFAULT);
        if (group < 0) return false;
        auto readStr = [&](const char* name, std::string& v) {
            if (H5Aexists(group, name) <= 0) return false;
            hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
            if (attr < 0) return false;
            hid_t type = H5Aget_type(attr);
            bool ok = false;
            if (type >= 0 && H5Tget_class(type) == H5T_STRING && H5Tis_variable_str(type) > 0) {
                char* ptr = nullptr;
                ok = H5Aread(attr, type, &ptr) >= 0;
                if (ok) v = ptr ? ptr : "";
                if (ptr) H5free_memory(ptr);
            }
            if (type >= 0) H5Tclose(type);
            H5Aclose(attr);
            return ok;
        };
        const bool ok = readStr("run_snapshot_json", runSnapshotJson);
        if (readinessJson) readStr("readiness_json", *readinessJson);
        H5Gclose(group);
        return ok && !runSnapshotJson.empty();
    }

} // namespace backend::services
