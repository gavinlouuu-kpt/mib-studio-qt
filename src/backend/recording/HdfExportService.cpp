#include "backend/recording/HdfExportService.h"

#include "backend/recording/Hdf5Service.h"
#include "backend/processing/ProcessingService.h"

#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace backend::recording {

const char* toString(HdfExportPhase phase)
{
    switch (phase) {
    case HdfExportPhase::Validating: return "validating";
    case HdfExportPhase::Metadata: return "metadata";
    case HdfExportPhase::Metrics: return "metrics";
    case HdfExportPhase::ValidImages: return "valid_images";
    case HdfExportPhase::SeriesImages: return "series_images";
    case HdfExportPhase::InvalidImages: return "invalid_images";
    case HdfExportPhase::Charts: return "charts";
    case HdfExportPhase::Committing: return "committing";
    case HdfExportPhase::Cleanup: return "cleanup";
    }
    return "unknown";
}

const char* toString(HdfExportStatus status)
{
    switch (status) {
    case HdfExportStatus::Completed: return "completed";
    case HdfExportStatus::Cancelled: return "cancelled";
    case HdfExportStatus::Failed: return "failed";
    }
    return "unknown";
}

namespace {

struct Cancelled {};
struct Failed { std::string message; };

std::string fixed(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string padded(uint64_t v, int width)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*llu", width, static_cast<unsigned long long>(v));
    return buf;
}

// Same columns/format as the historical HdfReviewTab CSV writer.
bool writeMetricsCsv(const fs::path& path, const std::vector<services::ProcessedFrame>& valid,
                     const std::vector<services::ProcessedFrame>& invalid, double conversionFactor,
                     std::string& error)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open metrics file for writing: " + path.string();
        return false;
    }
    const double areaFactor = conversionFactor * conversionFactor;
    out << "Frame Type,Index,Timestamp,Object Id,Object Count,Track Id,Track First,Track Last,Track Observations,"
        << "Deformability,Area,Area (um\xC2\xB2),Area Ratio,Ring Ratio,"
        << "Valid,Touches Border,Single Inner,In Range,Inner Count,"
        << "Bright Q1,Bright Q2,Bright Q3,Bright Q4\n";
    auto writeFrame = [&](const char* type, const services::ProcessedFrame& f) {
        const auto& v = f.validation;
        out << type << "," << f.index << "," << f.timestampNs << "," << v.objectId << "," << v.objectCount << ","
            << v.trackId << "," << v.trackFirstFrame << "," << v.trackLastFrame << "," << v.trackObservationCount << ","
            << fixed(v.deformability, 3) << "," << fixed(v.area, 2) << "," << fixed(v.area * areaFactor, 2) << ","
            << fixed(v.areaRatio, 3) << "," << fixed(v.ringRatio, 3) << ","
            << (v.isValid ? "Yes" : "No") << "," << (v.touchesBorder ? "Yes" : "No") << ","
            << (v.hasSingleInnerContour ? "Yes" : "No") << "," << (v.inRange ? "Yes" : "No") << ","
            << v.innerContourCount << "," << fixed(v.brightness.q1, 2) << "," << fixed(v.brightness.q2, 2) << ","
            << fixed(v.brightness.q3, 2) << "," << fixed(v.brightness.q4, 2) << "\n";
    };
    for (const auto& f : valid) writeFrame("Valid", f);
    for (const auto& f : invalid) writeFrame("Invalid", f);
    out.flush();
    if (!out) {
        error = "failed while writing metrics: " + path.string();
        return false;
    }
    return true;
}

void writeFailureManifest(const fs::path& partial, const std::string& jobId, HdfExportStatus status,
                          const std::string& error, const HdfExportResult& r)
{
    std::error_code ec;
    const fs::path target = fs::is_directory(partial, ec) ? partial / "export-failure.json"
                                                          : fs::path(partial.string() + ".failure.json");
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << "{\"job_id\":\"" << jobId << "\",\"state\":\"" << toString(status) << "\",\"error\":\"";
    for (char c : error) out << (c == '"' ? '\'' : c);
    out << "\",\"images_exported\":" << r.imagesExported << ",\"series_exported\":" << r.seriesExported
        << ",\"note\":\"This export did not complete; contents are partial.\"}\n";
}

} // namespace

std::string HdfExportService::newJobId()
{
    static std::atomic<uint64_t> counter{0};
    std::random_device rd;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llx-%04x%02llx", static_cast<unsigned long long>(ms),
                  static_cast<unsigned>(rd() & 0xFFFF), static_cast<unsigned long long>(counter.fetch_add(1) & 0xFF));
    return buf;
}

std::string HdfExportService::sourceBaseName(const std::string& hdfPath)
{
    std::string base = fs::path(hdfPath).stem().string();
    // Trim whitespace like the Qt/Python policies.
    const auto first = base.find_first_not_of(" \t");
    const auto last = base.find_last_not_of(" \t");
    base = first == std::string::npos ? std::string() : base.substr(first, last - first + 1);
    return base.empty() ? "hdf_export" : base;
}

std::string HdfExportService::nextAvailableName(const std::string& parentDir, const std::string& firstName,
                                                const std::string& prefix, const std::string& suffix)
{
    std::error_code ec;
    std::vector<std::string> names;
    if (fs::is_directory(parentDir, ec)) {
        for (const auto& entry : fs::directory_iterator(parentDir, ec)) names.push_back(entry.path().filename().string());
    }
    const fs::path parent(parentDir);
    if (std::find(names.begin(), names.end(), firstName) == names.end()) return (parent / firstName).string();
    unsigned long long maxSuffix = 1;
    for (const auto& name : names) {
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        try { maxSuffix = std::max(maxSuffix, std::stoull(digits)); } catch (...) {}
    }
    unsigned long long candidate = maxSuffix + 1;
    for (int i = 0; i < 1000; ++i, ++candidate) {
        const std::string name = prefix + std::to_string(candidate) + suffix;
        if (std::find(names.begin(), names.end(), name) == names.end()) return (parent / name).string();
    }
    return (parent / (prefix + std::to_string(candidate) + suffix)).string();
}

HdfExportResult HdfExportService::run(const HdfExportRequest& request, const HdfExportCancelToken& cancel,
                                      const HdfExportProgressFn& onProgress) const
{
    HdfExportResult result;
    result.jobId = newJobId();
    const auto started = std::chrono::steady_clock::now();
    fs::path finalPath;
    fs::path partial;
    uint64_t completedUnits = 0, totalUnits = 0;

    auto progress = [&](HdfExportPhase phase, const std::string& current = {}) {
        if (onProgress) onProgress(HdfExportProgress{result.jobId, phase, completedUnits, totalUnits, current});
    };
    auto checkCancel = [&] { if (cancel.cancelled()) throw Cancelled{}; };
    auto writeImage = [&](const fs::path& path, const cv::Mat& image) -> bool {
        if (imageWriter_) return imageWriter_(path.string(), image);
        return cv::imwrite(path.string(), image);
    };

    SPDLOG_INFO("HdfExport {}: start {} -> {} (format={}, frames={})", result.jobId, request.sourcePath,
                request.outputRoot, static_cast<int>(request.format), static_cast<int>(request.frames));
    try {
        progress(HdfExportPhase::Validating);
        std::error_code ec;
        if (!fs::is_regular_file(request.sourcePath, ec)) throw Failed{"source file does not exist: " + request.sourcePath};
        if (request.outputRoot.empty()) throw Failed{"no output directory selected"};
        if (fs::exists(request.outputRoot, ec) && !fs::is_directory(request.outputRoot, ec))
            throw Failed{"output path exists and is not a directory: " + request.outputRoot};
        fs::create_directories(request.outputRoot, ec);
        if (ec || !fs::is_directory(request.outputRoot, ec))
            throw Failed{"failed to create output directory " + request.outputRoot + ": " + ec.message()};
        checkCancel();

        // Destination + same-parent partial location.
        const std::string base = sourceBaseName(request.sourcePath);
        const bool folderJob = request.format != HdfExportFormat::MetricsCsv;
        if (!request.explicitDestination.empty()) {
            finalPath = request.explicitDestination;
        } else if (folderJob) {
            finalPath = nextAvailableName(request.outputRoot, base, base + "_", "");
        } else {
            finalPath = nextAvailableName(request.outputRoot, base + "_metrics.csv", base + "_metrics_", ".csv");
        }
        // An explicit *file* destination (chosen through a save dialog that
        // already confirmed overwrite) is replaced atomically at commit; an
        // existing folder is never merged into.
        const bool overwriteFile = !request.explicitDestination.empty() && !folderJob &&
                                   fs::is_regular_file(finalPath, ec);
        if (fs::exists(finalPath, ec) && !overwriteFile) throw Failed{"destination already exists: " + finalPath.string()};
        partial = finalPath.parent_path() / ("." + finalPath.filename().string() + ".partial-" + result.jobId);
        if (folderJob) {
            fs::create_directories(partial, ec);
            if (ec) throw Failed{"failed to create export directory " + partial.string() + ": " + ec.message()};
        }

        // Own read-only reader for the job: never the caller's live state.
        progress(HdfExportPhase::Metadata);
        services::Hdf5Service reader;
        if (!reader.loadFile(request.sourcePath)) throw Failed{"file could not be opened as HDF5: " + request.sourcePath};
        std::vector<services::ProcessedFrame> valid, invalid;
        result.recordingMode = reader.isRecordingFile();
        if (result.recordingMode) {
            if (!reader.readRecordingMetadata(valid)) throw Failed{"failed to read recording metadata"};
        } else {
            if (request.frames != HdfExportFrames::Invalid && !reader.readValidMetadata(valid))
                throw Failed{"failed to read valid-frame metadata"};
            if (request.frames != HdfExportFrames::Valid && !reader.readInvalidMetadata(invalid))
                throw Failed{"failed to read invalid-frame metadata"};
        }
        if (request.frames == HdfExportFrames::Invalid) valid.clear();
        if (request.frames == HdfExportFrames::Valid) invalid.clear();
        checkCancel();
        if (valid.empty() && invalid.empty()) throw Failed{"no exportable frame data found"};

        // Series geometry (experiment files only).
        size_t seriesRecords = 0, seriesCount = 0;
        int seriesH = 0, seriesW = 0;
        const bool hasSeries = !result.recordingMode && folderJob && request.series.exportSeries &&
                               reader.getSeriesImageInfo(seriesRecords, seriesCount, seriesH, seriesW) && seriesCount > 0;
        size_t seriesStart = 0, seriesEnd = 0;
        if (hasSeries) {
            seriesStart = std::min(request.series.startInclusive, seriesCount - 1);
            seriesEnd = request.series.endInclusive < request.series.startInclusive
                            ? seriesCount - 1
                            : std::min(request.series.endInclusive, seriesCount - 1);
        }
        const int seriesDigits = std::max<int>(2, static_cast<int>(std::to_string(std::max<size_t>(seriesCount, 1)).size()));

        const bool writeMetrics = !result.recordingMode && request.format != HdfExportFormat::Images;
        totalUnits = (writeMetrics ? 1 : 0) + (folderJob ? valid.size() + invalid.size() : 0) +
                     (hasSeries ? std::min(seriesRecords, valid.size()) * (seriesEnd - seriesStart + 1) : 0) +
                     (folderJob && request.format == HdfExportFormat::All ? request.supplementalImages.size() : 0);

        // Metrics.
        if (writeMetrics) {
            checkCancel();
            const fs::path csv = folderJob ? partial / "metrics.csv" : partial;
            progress(HdfExportPhase::Metrics, csv.string());
            std::string err;
            if (!writeMetricsCsv(csv, valid, invalid, request.conversionFactor, err)) throw Failed{err};
            result.metricsWritten = true;
            result.validCount = valid.size();
            result.invalidCount = invalid.size();
            ++completedUnits;
        }

        if (folderJob) {
            const std::string validImages = result.recordingMode ? "/recorded_frames/images" : "/valid_frames/images";
            const std::string validPrefix = result.recordingMode ? "frame_" : "valid_frame_";
            progress(HdfExportPhase::ValidImages);
            for (size_t i = 0; i < valid.size(); ++i) {
                checkCancel();
                cv::Mat image; // one frame resident at a time
                const fs::path path = partial / (validPrefix + padded(valid[i].index, 6) + ".tiff");
                if (reader.readImageByIndex(validImages, i, image)) {
                    if (!writeImage(path, image)) throw Failed{"failed to write image " + path.string()};
                    ++result.imagesExported;
                } else {
                    ++result.imagesFailed;
                    result.warnings.push_back("could not read image " + std::to_string(i) + " from " + validImages);
                }
                ++completedUnits;
                if (hasSeries && i < seriesRecords) {
                    std::vector<cv::Mat> series;
                    if (reader.readSeriesImagesByIndex(i, series) && !series.empty()) {
                        const size_t end = std::min(seriesEnd, series.size() - 1);
                        for (size_t s = std::min(seriesStart, series.size() - 1); s <= end; ++s) {
                            checkCancel();
                            const fs::path sp = partial / ("valid_frame_" + padded(valid[i].index, 6) + "_series_" +
                                                           padded(s + 1, seriesDigits) + ".tiff");
                            if (!writeImage(sp, series[s])) throw Failed{"failed to write series image " + sp.string()};
                            ++result.seriesExported;
                            ++completedUnits;
                        }
                    } else {
                        result.warnings.push_back("could not read series record " + std::to_string(i));
                    }
                    if ((i + 1) % 10 == 0) progress(HdfExportPhase::SeriesImages, partial.string());
                }
                if ((i + 1) % 25 == 0 || i + 1 == valid.size()) progress(HdfExportPhase::ValidImages, path.string());
            }
            progress(HdfExportPhase::InvalidImages);
            for (size_t i = 0; i < invalid.size(); ++i) {
                checkCancel();
                cv::Mat image;
                const fs::path path = partial / ("invalid_frame_" + padded(invalid[i].index, 6) + ".tiff");
                if (reader.readImageByIndex("/invalid_frames/images", i, image)) {
                    if (!writeImage(path, image)) throw Failed{"failed to write image " + path.string()};
                    ++result.imagesExported;
                } else {
                    ++result.imagesFailed;
                    result.warnings.push_back("could not read image " + std::to_string(i) + " from /invalid_frames/images");
                }
                ++completedUnits;
                if ((i + 1) % 25 == 0 || i + 1 == invalid.size()) progress(HdfExportPhase::InvalidImages, path.string());
            }
            if (request.format == HdfExportFormat::All && !result.recordingMode) {
                progress(HdfExportPhase::Charts);
                for (const auto& [name, image] : request.supplementalImages) {
                    checkCancel();
                    if (image.empty()) { result.warnings.push_back("chart " + name + " is empty"); continue; }
                    const fs::path path = partial / name;
                    if (!writeImage(path, image)) throw Failed{"failed to write chart " + path.string()};
                    ++result.chartsExported;
                    ++completedUnits;
                }
            }
        }
        reader.closeFile();

        // Publish.
        checkCancel();
        progress(HdfExportPhase::Committing, finalPath.string());
        fs::rename(partial, finalPath, ec);
        if (ec) {
            // Name taken meanwhile: pick the next one once.
            if (folderJob) finalPath = nextAvailableName(request.outputRoot, base, base + "_", "");
            else finalPath = nextAvailableName(request.outputRoot, base + "_metrics.csv", base + "_metrics_", ".csv");
            fs::rename(partial, finalPath, ec);
            if (ec) throw Failed{"could not publish export as " + finalPath.string() + ": " + ec.message()};
        }
        result.status = HdfExportStatus::Completed;
        result.finalPath = finalPath.string();
        partial.clear();
    } catch (const Cancelled&) {
        result.status = HdfExportStatus::Cancelled;
        result.error = "export cancelled";
    } catch (const Failed& f) {
        result.status = HdfExportStatus::Failed;
        result.error = f.message;
    } catch (const std::exception& e) {
        result.status = HdfExportStatus::Failed;
        result.error = std::string("unexpected error: ") + e.what();
    }

    if (result.status != HdfExportStatus::Completed && !partial.empty()) {
        progress(HdfExportPhase::Cleanup, partial.string());
        std::error_code ec;
        if (fs::exists(partial, ec)) {
            bool keep = request.keepPartialOnFailure;
            if (!keep) {
                fs::remove_all(partial, ec);
                keep = ec || fs::exists(partial, ec);
            }
            if (keep) {
                writeFailureManifest(partial, result.jobId, result.status, result.error, result);
                result.retainedPartialPath = partial.string();
            }
        }
    }
    result.durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (result.completed()) {
        SPDLOG_INFO("HdfExport {}: completed in {:.1f} ms (images={} series={} charts={} metrics={}) -> {}",
                    result.jobId, result.durationMs, result.imagesExported, result.seriesExported,
                    result.chartsExported, result.metricsWritten, result.finalPath);
    } else {
        SPDLOG_WARN("HdfExport {}: {} after {:.1f} ms: {}{}", result.jobId, toString(result.status), result.durationMs,
                    result.error, result.retainedPartialPath.empty() ? "" : " (partial retained: " + result.retainedPartialPath + ")");
    }
    return result;
}

} // namespace backend::recording
