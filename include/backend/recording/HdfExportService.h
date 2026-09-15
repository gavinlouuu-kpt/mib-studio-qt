// Qt-free HDF5 export service (issue #344).
//
// One bounded, cancellable export job: opens its own read-only Hdf5Service,
// streams images one at a time (readImageByIndex / readSeriesImagesByIndex),
// writes everything into a same-parent temporary destination
// (".<name>.partial-<job>") and publishes the final name only after success.
// A cancelled or failed job removes its partial output (or leaves it under
// the ".partial-" name with an export-failure.json manifest when removal
// fails) — a normal-looking export directory never contains a partial run.
//
// The service never touches the caller's live reader/model state, so a Qt
// shell can run it off the GUI thread with a shared cancel token and a
// progress callback; the same API is reusable by the backend bridge (#276).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace backend::recording {

enum class HdfExportFormat { MetricsCsv, Images, All };
enum class HdfExportFrames { Valid, Invalid, Both };
enum class HdfExportPhase {
    Validating, Metadata, Metrics, ValidImages, SeriesImages, InvalidImages, Charts, Committing, Cleanup
};
enum class HdfExportStatus { Completed, Cancelled, Failed };

const char* toString(HdfExportPhase phase);
const char* toString(HdfExportStatus status);

struct HdfExportSeriesRange {
    bool exportSeries{true};
    // Inclusive zero-based series indices; end < start means "all".
    size_t startInclusive{0};
    size_t endInclusive{static_cast<size_t>(-1)};
};

struct HdfExportRequest {
    std::string sourcePath;   // HDF5 file (opened read-only by the job)
    std::string outputRoot;   // parent directory for the generated name
    HdfExportFormat format{HdfExportFormat::All};
    HdfExportFrames frames{HdfExportFrames::Both};
    double conversionFactor{0.4886}; // pixel -> micron
    HdfExportSeriesRange series;
    // Chart TIFFs rendered by the caller on its own thread (name -> BGR image),
    // written into the export folder for All jobs (e.g. "scatter_plot.tiff").
    std::map<std::string, cv::Mat> supplementalImages;
    // Retain ".partial-<job>" output (with a failure manifest) instead of
    // deleting it when the job does not complete.
    bool keepPartialOnFailure{false};
    // Optional explicit final destination (metrics CSV file for MetricsCsv,
    // folder for Images/All). Empty -> derived from the source base name with
    // a bounded "_N" suffix lookup.
    std::string explicitDestination;
};

struct HdfExportProgress {
    std::string jobId;
    HdfExportPhase phase{HdfExportPhase::Validating};
    uint64_t completed{0};
    uint64_t total{0};
    std::string currentOutput;
};

struct HdfExportResult {
    std::string jobId;
    HdfExportStatus status{HdfExportStatus::Failed};
    std::string finalPath;          // published destination (Completed only)
    std::string retainedPartialPath; // visible .partial-* output, if kept
    uint64_t validCount{0};
    uint64_t invalidCount{0};
    uint64_t imagesExported{0};
    uint64_t imagesFailed{0};
    uint64_t seriesExported{0};
    uint64_t chartsExported{0};
    bool metricsWritten{false};
    bool recordingMode{false};
    double durationMs{0.0};
    std::vector<std::string> warnings;
    std::string error;
    bool completed() const { return status == HdfExportStatus::Completed; }
};

// Shared cancellation token: set() from any thread; the job polls it before
// every artifact/frame.
class HdfExportCancelToken {
public:
    HdfExportCancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() { flag_->store(true, std::memory_order_release); }
    bool cancelled() const { return flag_->load(std::memory_order_acquire); }
private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

using HdfExportProgressFn = std::function<void(const HdfExportProgress&)>;
// Test seam: replaces cv::imwrite for TIFF outputs (return false = write failure).
using HdfExportImageWriter = std::function<bool(const std::string& path, const cv::Mat& image)>;

class HdfExportService {
public:
    HdfExportService() = default;

    // Runs one job synchronously on the calling thread. Never throws for job
    // failures; the result carries the terminal status.
    HdfExportResult run(const HdfExportRequest& request,
                        const HdfExportCancelToken& cancel,
                        const HdfExportProgressFn& onProgress = {}) const;

    void setImageWriterForTests(HdfExportImageWriter writer) { imageWriter_ = std::move(writer); }

    // Bounded generated-name policy shared with the Qt shell: one directory
    // listing, "<first>" or "<pattern with max suffix + 1>".
    static std::string nextAvailableName(const std::string& parentDir, const std::string& firstName,
                                         const std::string& prefix, const std::string& suffix);
    static std::string sourceBaseName(const std::string& hdfPath);
    static std::string newJobId();

private:
    HdfExportImageWriter imageWriter_;
};

} // namespace backend::recording
