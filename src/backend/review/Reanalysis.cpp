// Reanalysis engine (#512 slice 2): moved verbatim from
// src/backend/app/ReviewReanalysis.cpp (BackendFacade); see Reanalysis.h.
#include "backend/review/Reanalysis.h"

#include "backend/processing/BatchMaskSources.h"
#include "backend/processing/ProcessingConfigJson.h"
#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/HdfExportService.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace backend::review
{

    bool parseReanalysisRequest(const std::string &text, const services::ProcessingConfig &defaults,
                                ReanalysisRequest &out, std::string &error)
    {
        using nlohmann::json;
        ReanalysisRequest r;
        r.config = defaults;
        try
        {
            const auto request = json::parse(text);
            r.sourceKind = request.value("source_kind", std::string("hdf"));
            if (r.sourceKind != "hdf" && r.sourceKind != "folder" && r.sourceKind != "avi")
                throw std::runtime_error("Unknown source kind");
            r.sourcePath = request.at("source_path").get<std::string>();
            r.syntheticBackground = request.value("synthetic_background", false);
            if (request.contains("image_processing"))
            {
                std::string configError;
                if (!processing::config_json::fromJson(request.at("image_processing"), r.config, &configError))
                    throw std::runtime_error(configError);
            }
            if (request.contains("roi"))
            {
                const auto &roi = request.at("roi");
                for (const auto *field : {"x", "y", "w", "h"})
                {
                    if (!roi.at(field).is_number_integer() || roi.at(field) < 0 ||
                        roi.at(field) > std::numeric_limits<int>::max())
                        throw std::runtime_error("ROI fields must be nonnegative 32-bit integers");
                }
                r.roi = {roi.at("x").get<int>(), roi.at("y").get<int>(), roi.at("w").get<int>(), roi.at("h").get<int>()};
                if (r.roi.x < 0 || r.roi.y < 0 || r.roi.w <= 0 || r.roi.h <= 0)
                    throw std::runtime_error("ROI must have nonnegative origin and positive size");
                r.overrideRoi = true;
            }
            r.outputPath = request.at("output_path").get<std::string>();
            r.dataset = request.value("dataset", std::string("/valid_frames/images"));
            const auto rangeValue = [&](const char *name) {
                if (!request.contains(name)) return std::uint64_t{0};
                const auto &value = request.at(name);
                if (!value.is_number_integer() ||
                    (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0))
                    throw std::runtime_error(std::string(name) + " must be a nonnegative integer");
                return value.get<std::uint64_t>();
            };
            r.start = rangeValue("start");
            r.count = rangeValue("count");
            if (request.contains("max_frames")) r.maxFrames = rangeValue("max_frames");
            const auto mib = request.contains("max_input_mib") ? rangeValue("max_input_mib") : 256;
            if (r.maxFrames == 0 || r.maxFrames > 1000000 || mib == 0 || mib > 16384 ||
                mib > std::numeric_limits<size_t>::max() / 1048576)
                throw std::runtime_error("Input budget must be 1–1000000 frames and 1–16384 MiB");
            r.maxInputBytes = mib * 1048576;
            r.clearBackground = request.value("clear_background", false);
            if (request.contains("background_index")) r.backgroundIndex = rangeValue("background_index");
            r.backgroundDataset = request.value("background_dataset", std::string("/valid_frames/images"));
            if (r.sourcePath.empty() || r.outputPath.empty())
                throw std::runtime_error("Source and output paths are required");
            if (r.dataset != "all" && r.dataset != "/valid_frames/images" && r.dataset != "/invalid_frames/images" &&
                r.dataset != "/recorded_frames/images")
                throw std::runtime_error("Unsupported HDF dataset");
            if (std::filesystem::exists(r.outputPath)) throw std::runtime_error("Output already exists; choose a new file");
        }
        catch (const std::exception &failure)
        {
            error = failure.what();
            return false;
        }
        out = std::move(r);
        return true;
    }

    ReanalysisOutcome runReanalysis(const ReanalysisRequest &request, services::ProcessingService &kernel,
                                    const ReanalysisCancelFlag &cancel, const ReanalysisProgressFn &onProgress)
    {
        const std::string &kind = request.sourceKind;
        const std::string &source = request.sourcePath;
        const std::string &output = request.outputPath;
        const std::string &dataset = request.dataset;
        const std::uint64_t start = request.start;
        const std::uint64_t count = request.count;
        const std::uint64_t maxFrames = request.maxFrames;
        const std::uint64_t maxInputBytes = request.maxInputBytes;
        const auto &config = request.config;

        ReanalysisOutcome outcome;
        std::filesystem::path partial;
        bool published = false;
        std::string error, warning, retained;
        size_t produced = 0;
        try
        {
            const auto checkCancel = [&] {
                if (cancel->load(std::memory_order_acquire)) throw std::runtime_error("Reanalysis cancelled");
            };
            checkCancel();
            std::vector<services::ProcessedFrame> metadata;
            std::vector<cv::Mat> images;
            services::ProcessingService::Roi roi{0, 0, 0, 0};
            cv::Mat background;
            processing::ProcessingCoreIdentity originalCore;
            bool hasOriginalCore = false;
            if (kind == "hdf")
            {
                services::Hdf5Service reader;
                if (!reader.loadFile(source)) throw std::runtime_error("Cannot open HDF source");
                struct Ref
                {
                    std::string dataset;
                    size_t row;
                    services::ProcessedFrame metadata;
                    int height, width, channels;
                };
                std::vector<Ref> refs;
                const auto collect = [&](const std::string &path, bool optional) {
                    size_t available = 0;
                    int height = 0, width = 0, channels = 0;
                    if (!reader.getDatasetInfo(path, available, height, width, channels))
                    {
                        if (optional &&
                            reader.metadataDatasetPresent(path == "/valid_frames/images") == std::optional<bool>(false))
                            return;
                        throw std::runtime_error("Selected source dataset is missing");
                    }
                    std::vector<services::ProcessedFrame> rows;
                    const bool ok = path == "/valid_frames/images"     ? reader.readValidMetadata(rows)
                                    : path == "/invalid_frames/images" ? reader.readInvalidMetadata(rows)
                                                                       : reader.readRecordingMetadata(rows);
                    if (!ok || rows.size() < available)
                        throw std::runtime_error("Source frame identities/timestamps are missing");
                    for (size_t i = 0; i < available; ++i)
                        refs.push_back({path, i, std::move(rows[i]), height, width, channels});
                };
                if (dataset == "all")
                {
                    if (reader.isRecordingFile()) collect("/recorded_frames/images", false);
                    else
                    {
                        collect("/valid_frames/images", true);
                        collect("/invalid_frames/images", true);
                    }
                    const bool hasTime = std::any_of(refs.begin(), refs.end(),
                                                     [](const auto &ref) { return ref.metadata.timestampNs != 0; });
                    std::sort(refs.begin(), refs.end(), [hasTime](const auto &a, const auto &b) {
                        if (hasTime && a.metadata.timestampNs != b.metadata.timestampNs)
                            return a.metadata.timestampNs < b.metadata.timestampNs;
                        if (a.metadata.index != b.metadata.index) return a.metadata.index < b.metadata.index;
                        if (a.dataset != b.dataset) return a.dataset < b.dataset;
                        return a.row < b.row;
                    });
                }
                else
                    collect(dataset, false);
                if (start >= refs.size()) throw std::runtime_error("Selected source range is empty");
                const auto selected = count == 0 ? refs.size() - start : count;
                if (selected > refs.size() - start || selected > maxFrames)
                    throw std::runtime_error("Selected range exceeds configured frame budget");
                uint64_t inputBytes = 0;
                for (size_t i = start; i < start + selected; ++i)
                {
                    const auto &ref = refs[i];
                    if (ref.height <= 0 || ref.width <= 0 || ref.channels != 1)
                        throw std::runtime_error("Source frames must be grayscale");
                    const uint64_t bytes = static_cast<uint64_t>(ref.height) * static_cast<uint64_t>(ref.width);
                    if (bytes > maxInputBytes - inputBytes)
                        throw std::runtime_error("Selected range exceeds configured input memory budget");
                    inputBytes += bytes;
                }
                uint64_t first = 0, last = 0;
                size_t valid = 0, invalid = 0;
                if (!reader.isRecordingFile() && !reader.readExperimentInfo(first, last, valid, invalid, &roi))
                    throw std::runtime_error("Source ROI metadata is missing");
                reader.readBackgroundImage(background);
                hasOriginalCore = reader.readProcessingCoreIdentity(originalCore);
                images.reserve(selected);
                metadata.reserve(selected);
                for (size_t i = start; i < start + selected; ++i)
                {
                    checkCancel();
                    cv::Mat frame;
                    if (!reader.readImageByIndex(refs[i].dataset, refs[i].row, frame))
                        throw std::runtime_error("Failed to read source frame");
                    images.push_back(std::move(frame));
                    metadata.push_back(std::move(refs[i].metadata));
                }
            }
            else
            {
                services::batch_masks::LoadOptions limits;
                limits.start = start;
                limits.count = count;
                limits.maxFrames = maxFrames;
                limits.maxBytes = maxInputBytes;
                limits.cancelled = [cancel] { return cancel->load(std::memory_order_acquire); };
                std::vector<std::string> names, errors;
                const bool loaded = kind == "folder"
                                        ? services::batch_masks::loadFromFolder(source, images, names, errors, limits)
                                        : services::batch_masks::loadFromAvi(source, images, names, errors, limits);
                if (!loaded || !errors.empty() || images.empty())
                    throw std::runtime_error(errors.empty() ? "No source images loaded" : errors.front());
                metadata.resize(images.size());
                for (size_t i = 0; i < metadata.size(); ++i) metadata[i].index = start + i;
            }
            if (request.overrideRoi) roi = request.roi;
            if (roi.w > 0 && (roi.x > images.front().cols - roi.w || roi.y > images.front().rows - roi.h))
                throw std::runtime_error("ROI exceeds source image dimensions");
            if (request.clearBackground) background.release();
            if (request.backgroundIndex &&
                !services::batch_masks::loadPreview(kind, source, request.backgroundDataset, *request.backgroundIndex,
                                                    background))
                throw std::runtime_error("Selected background frame could not be loaded");
            if (!background.empty() && background.size() != images.front().size())
                throw std::runtime_error("Background dimensions do not match the selected source range");
            if (request.syntheticBackground && background.empty())
                background = services::batch_masks::buildSyntheticBackground(
                    images, [cancel] { return cancel->load(std::memory_order_acquire); });
            const auto selected = images.size();
            processing::ProcessingCoreIdentity usedCore;
            auto frames = kernel.processBatch(
                images, config, background, roi,
                [&](const services::ProcessingService::BatchProgress &progress) {
                    checkCancel();
                    if (onProgress) onProgress(progress.done, progress.total);
                },
                &usedCore);
            checkCancel();
            if (hasOriginalCore && originalCore != usedCore)
                warning = "Source processing core differs; output records the active processing core.";
            for (auto &frame : frames)
            {
                const auto local = frame.index;
                if (local >= selected) throw std::runtime_error("Processing returned an out-of-range source index");
                const auto &origin = metadata[local];
                frame.index = origin.index;
                frame.timestampNs = origin.timestampNs;
                if (frame.validation.trackId >= 0)
                {
                    if (frame.validation.trackFirstFrame < selected)
                        frame.validation.trackFirstFrame = metadata[frame.validation.trackFirstFrame].index;
                    if (frame.validation.trackLastFrame < selected)
                        frame.validation.trackLastFrame = metadata[frame.validation.trackLastFrame].index;
                }
            }
            produced = frames.size();
            if (frames.empty()) throw std::runtime_error("Reanalysis produced no output frames");
            partial = std::filesystem::path(output).parent_path() /
                      ("." + std::filesystem::path(output).filename().string() + ".partial-" +
                       recording::HdfExportService::newJobId());
            if (!services::batch_masks::saveMasksToHdf5(frames, partial.string(), config, roi.x, roi.y, roi.w, roi.h,
                                                        background, kind == "hdf", &usedCore))
                throw std::runtime_error("Failed to save regenerated HDF");
            checkCancel();
            // Atomic no-clobber publication; supports NTFS/FAT on Windows and
            // Linux renameat2 filesystems without requiring hard-link support.
#ifdef _WIN32
            if (!MoveFileExW(partial.wstring().c_str(), std::filesystem::path(output).wstring().c_str(),
                             MOVEFILE_WRITE_THROUGH))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Publish regenerated output");
#elif defined(__linux__)
            if (syscall(SYS_renameat2, AT_FDCWD, partial.c_str(), AT_FDCWD, output.c_str(), RENAME_NOREPLACE) != 0)
            {
                if (errno != ENOSYS && errno != EINVAL)
                    throw std::system_error(errno, std::generic_category(), "Publish regenerated output");
                std::filesystem::create_hard_link(partial, output);
            }
#else
            std::filesystem::create_hard_link(partial, output);
#endif
            published = true;
        }
        catch (const std::exception &failure)
        {
            error = failure.what();
        }
        if (!partial.empty())
        {
            std::error_code cleanup;
            std::filesystem::remove(partial, cleanup);
            if (cleanup)
            {
                retained = partial.string();
                warning += " Temporary file cleanup failed: " + cleanup.message();
            }
        }
        outcome.published = published;
        outcome.cancelled = !published && cancel->load(std::memory_order_acquire);
        outcome.error = std::move(error);
        outcome.warning = std::move(warning);
        outcome.retainedPartialPath = std::move(retained);
        outcome.produced = produced;
        return outcome;
    }

    bool loadReanalysisPreview(const std::string &text, cv::Mat &image, std::uint64_t &index)
    {
        try
        {
            const auto request = nlohmann::json::parse(text);
            const auto &value = request.at("index");
            if (!value.is_number_integer() || value < 0) return false;
            index = value.get<uint64_t>();
            return services::batch_masks::loadPreview(request.value("source_kind", std::string("hdf")),
                                                      request.at("source_path").get<std::string>(),
                                                      request.value("dataset", std::string("/valid_frames/images")),
                                                      index, image);
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    std::string reanalysisLoadingStatusJson(std::uint64_t operationId)
    {
        return nlohmann::json{{"state", "running"}, {"operation_id", std::to_string(operationId)}, {"phase", "loading"}}
            .dump();
    }

    std::string reanalysisProgressStatusJson(std::uint64_t operationId, std::uint64_t done, std::uint64_t total)
    {
        return nlohmann::json{{"state", "running"},
                              {"operation_id", std::to_string(operationId)},
                              {"phase", "processing"},
                              {"completed", std::to_string(done)},
                              {"total", std::to_string(total)}}
            .dump();
    }

    std::string reanalysisTerminalStatusJson(std::uint64_t operationId, const ReanalysisRequest &request,
                                             const ReanalysisOutcome &outcome)
    {
        using nlohmann::json;
        return json{{"state", outcome.published ? "completed" : outcome.cancelled ? "cancelled" : "failed"},
                    {"operation_id", std::to_string(operationId)},
                    {"final_path", outcome.published ? request.outputPath : ""},
                    {"retained_partial_path", outcome.retainedPartialPath},
                    {"error", outcome.error},
                    {"warnings", outcome.warning.empty() ? json::array() : json::array({outcome.warning})},
                    {"objects", std::to_string(outcome.produced)}}
            .dump();
    }

    std::string reanalysisStartFailedStatusJson(std::uint64_t operationId, const std::string &error)
    {
        return nlohmann::json{{"state", "failed"}, {"error", error}, {"operation_id", std::to_string(operationId)}}
            .dump();
    }

} // namespace backend::review
