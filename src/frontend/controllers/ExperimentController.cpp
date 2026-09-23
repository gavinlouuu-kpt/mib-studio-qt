#include "frontend/controllers/ExperimentController.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <chrono>
#include <spdlog/spdlog.h>

#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"

namespace frontend
{

    ExperimentController::ExperimentController(backend::AppBackend &backend, QObject *parent)
        : QObject(parent), backend_(backend)
    {
    }

    bool ExperimentController::startExperiment(const QString &hdf5FilePath, QString *errorMsg)
    {
        if (state_ != State::Idle)
        {
            if (errorMsg)
                *errorMsg = "Experiment is already running or starting";
            return false;
        }

        if (!backend_.processing().isProcessingCorePinSatisfied())
        {
            if (errorMsg)
                *errorMsg = QString("Administrator-pinned processing core %1 is not active")
                                .arg(QString::fromStdString(
                                    backend_.processing().requiredProcessingCoreVersion()));
            return false;
        }

        // Guard: Experiment cannot start without first starting camera
        if (!backend_.capture().isRunning())
        {
            if (errorMsg)
                *errorMsg = "Camera must be running before starting an experiment";
            return false;
        }

        // Issue #451: claim the shared HDF5 writer before opening anything, so
        // this path can never replace a coordinator run or a manual recording.
        auto claim = backend_.persistenceOwnership().tryAcquire(
            backend::app::PersistenceOwner::Experiment, "legacy experiment start");
        if (!claim.granted)
        {
            if (errorMsg)
                *errorMsg = QString::fromStdString(claim.message);
            return false;
        }
        persistenceLease_ = backend::app::PersistenceLease(
            backend_.persistenceOwnership(), backend::app::PersistenceOwner::Experiment, claim.runId);

        state_ = State::Starting;
        emit stateChanged(state_);

        // Convert to std::string
        std::string hdf5Path = hdf5FilePath.toStdString();

        // Ensure .h5 extension
        if (hdf5Path.size() < 3 ||
            (hdf5Path.substr(hdf5Path.size() - 3) != ".h5" &&
             hdf5Path.substr(hdf5Path.size() - 5) != ".hdf5"))
        {
            hdf5Path += ".h5";
        }

        // Open HDF5 file
        auto &hdf5 = backend_.hdf5();
        if (!hdf5.openFile(hdf5Path))
        {
            persistenceLease_.release();
            state_ = State::Idle;
            emit stateChanged(state_);
            if (errorMsg)
                *errorMsg = QString("Failed to open HDF5 file: %1").arg(hdf5FilePath);
            return false;
        }

        // Initialize datasets for incremental writing
        if (!hdf5.initializeDatasets())
        {
            SPDLOG_WARN("Failed to initialize HDF5 datasets");
        }

        // Start experiment (clear frame buffers)
        auto &processing = backend_.processing();
        processing.startExperiment();

        // Record experiment start time
        startTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
        hdf5FilePath_ = hdf5Path;

        state_ = State::Active;
        emit stateChanged(state_);
        emit experimentStarted(startTimeNs_);

        return true;
    }

    bool ExperimentController::stopExperiment(QString *errorMsg)
    {
        if (state_ != State::Active)
        {
            if (errorMsg)
                *errorMsg = "No experiment is currently running";
            return false;
        }

        state_ = State::Stopping;
        emit stateChanged(state_);

        // End experiment and flush any remaining frames
        auto &processing = backend_.processing();

        // Flush any remaining buffered frames (synchronous for final flush)
        auto &hdf5 = backend_.hdf5();
        size_t validFrames = 0;
        size_t invalidFrames = 0;
        bool metadataOk = true;

        if (hdf5.isFileOpen())
        {
            size_t flushed = processing.flushBufferedFrames(hdf5);
            if (flushed > 0)
            {
                SPDLOG_INFO("Final flush: {} frames submitted to HDF5 write queue", flushed);
            }

            // Drain the async write queue so every submitted batch is on disk and
            // the writer thread has stopped before any direct HDF5 writes below
            // (avoids two threads writing the shared file concurrently).
            if (!processing.finishFlush())
            {
                SPDLOG_WARN("Experiment flush queue reported a save error during final drain");
            }

            // Get final frame counts (should be empty after flush, but check anyway)
            auto validFramesVec = processing.getValidFrames();
            auto invalidFramesVec = processing.getInvalidFrames();

            // Record experiment end time
            endTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

            // Save any remaining frames and write experiment info
            if (!validFramesVec.empty() || !invalidFramesVec.empty())
            {
                // Save any remaining frames that weren't flushed
                if (!hdf5.appendFrames(validFramesVec, invalidFramesVec))
                {
                    SPDLOG_WARN("Failed to save remaining frames to HDF5");
                }
            }

            validFrames = validFramesVec.size();
            invalidFrames = invalidFramesVec.size();

            // Flush all frame data to disk before writing metadata. This ensures that
            // even if a crash occurs during writeExperimentInfo, the frame datasets are
            // already safely committed and the root object header is consistent on disk.
            if (!hdf5.flush())
                SPDLOG_WARN("H5Fflush before writeExperimentInfo failed — file may be corrupt on crash");

            // Write experiment metadata (including background image for reproducibility if set)
            auto processingConfig = processing.getProcessingConfig();
            auto roi = processing.getRealtimeRoi();
            cv::Mat bg = processing.getRealtimeBackgroundGray();
            const auto processingCore = processing.activeProcessingCoreIdentity();
            metadataOk = hdf5.writeExperimentInfo(
                startTimeNs_, endTimeNs_, validFrames, invalidFrames, processingConfig, roi,
                bg.empty() ? nullptr : &bg, &processingCore);
            if (!metadataOk) {
                SPDLOG_ERROR("Experiment metadata/provenance write failed");
                if (errorMsg)
                    *errorMsg = "Mandatory experiment metadata/provenance write failed";
            }

            // Save full config.json content for backtracking
            std::string configJson = backend_.getLastConfigJson();
            if (metadataOk && !configJson.empty()) {
                hdf5.writeConfigJson(configJson);
            }

            hdf5.closeFile();
        }
        else
        {
            endTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        }

        processing.endExperiment();
        backend_.processing().resetRealtimeMetrics();
        persistenceLease_.release();

        state_ = State::Idle;
        emit stateChanged(state_);
        emit experimentStopped(endTimeNs_, validFrames, invalidFrames);

        return metadataOk;
    }

} // namespace frontend
