#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "backend/processing/ProcessingService.h"
#include "backend/camera/mock/MockCamera.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

using backend::services::ProcessedFrame;
using backend::services::ProcessingConfig;
using backend::services::ProcessingService;

struct TimingSummary {
    double totalMs{0.0};
    double avgUs{0.0};
    double maxUs{0.0};
};

struct ProofResult {
    size_t expectedFrames{0};
    std::atomic<uint64_t> captureCallbacks{0};
    std::atomic<uint64_t> enqueueAccepted{0};
    std::atomic<uint64_t> enqueueDropped{0};
    std::atomic<bool> allFramesEnqueued{false};
    bool firstBatchCallbackAfterAllFramesEnqueued{false};
    std::vector<size_t> callbackBatchSizes;
    std::vector<double> enqueueDurationsUs;
    size_t processedFrames{0};
    size_t nonEmptyMasks{0};
    size_t framesWithContours{0};
    bool captureStopped{false};
    ProcessedFrame sample;
};

std::string jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    return out.str();
}

std::filesystem::path makeTempDir(const std::string& prefix)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto path = std::filesystem::temp_directory_path() /
                    (prefix + "_" + std::to_string(dist(gen)));
        std::error_code ec;
        if (std::filesystem::create_directories(path, ec)) {
            return path;
        }
    }
    throw std::runtime_error("failed to create temporary directory");
}

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

ProcessingConfig makeProofConfig()
{
    ProcessingConfig config;
    config.gaussian_blur_size = 3;
    config.bg_subtract_threshold = 180;
    config.morph_kernel_size = 3;
    config.morph_iterations = 1;
    config.enable_border_check = false;
    config.enable_area_range_check = false;
    config.enable_deformability_range_check = false;
    config.enable_ring_ratio_check = false;
    config.enable_area_ratio_check = false;
    config.require_single_inner_contour = false;
    config.empty_frame_pixel_threshold = 1;
    return config;
}

cv::Mat makeSyntheticRingFrame(size_t index)
{
    cv::Mat image(96, 96, CV_8UC1, cv::Scalar(0));
    const cv::Point center(48 + static_cast<int>(index % 3) - 1,
                           48 + static_cast<int>((index / 3) % 3) - 1);
    cv::circle(image, center, 24, cv::Scalar(255), -1);
    cv::circle(image, center, 11, cv::Scalar(0), -1);
    return image;
}

bool generateSyntheticFrames(const std::filesystem::path& frameDir, size_t frameCount)
{
    std::filesystem::create_directories(frameDir);
    for (size_t i = 0; i < frameCount; ++i) {
        const auto path = frameDir / ("mock_frame_" + std::to_string(i) + ".png");
        if (!cv::imwrite(path.string(), makeSyntheticRingFrame(i))) {
            std::cerr << "failed to write synthetic frame: " << path << '\n';
            return false;
        }
    }
    return true;
}

size_t countSupportedImages(const std::filesystem::path& frameDir)
{
    if (!std::filesystem::exists(frameDir)) {
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(frameDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
            ext == ".bmp" || ext == ".tif" || ext == ".tiff") {
            ++count;
        }
    }
    return count;
}

TimingSummary summarizeDurations(const std::vector<double>& durationsUs)
{
    TimingSummary summary;
    if (durationsUs.empty()) {
        return summary;
    }
    double totalUs = 0.0;
    for (const double value : durationsUs) {
        totalUs += value;
        summary.maxUs = std::max(summary.maxUs, value);
    }
    summary.totalMs = totalUs / 1000.0;
    summary.avgUs = totalUs / static_cast<double>(durationsUs.size());
    return summary;
}

bool writeVisualEvidence(const std::filesystem::path& outputDir, const ProcessedFrame& sample)
{
    if (sample.originalImage.empty() || sample.processedImage.empty()) {
        std::cerr << "cannot write app visual evidence without a processed sample\n";
        return false;
    }

    const auto inputPath = outputDir / "mib_app_input_sample.png";
    const auto maskPath = outputDir / "mib_app_processed_mask_sample.png";
    const auto overlayPath = outputDir / "mib_app_contour_overlay_sample.png";

    if (!cv::imwrite(inputPath.string(), sample.originalImage)) {
        std::cerr << "failed to write " << inputPath << '\n';
        return false;
    }
    if (!cv::imwrite(maskPath.string(), sample.processedImage)) {
        std::cerr << "failed to write " << maskPath << '\n';
        return false;
    }

    cv::Mat overlay;
    cv::cvtColor(sample.originalImage, overlay, cv::COLOR_GRAY2BGR);
    if (sample.validation.allContours) {
        cv::drawContours(overlay, *sample.validation.allContours, -1, cv::Scalar(0, 255, 0), 1);
    }
    cv::putText(overlay,
                "mib app capture -> async batch",
                cv::Point(8, 18),
                cv::FONT_HERSHEY_SIMPLEX,
                0.45,
                cv::Scalar(0, 255, 255),
                1);
    if (!cv::imwrite(overlayPath.string(), overlay)) {
        std::cerr << "failed to write " << overlayPath << '\n';
        return false;
    }

    return true;
}

bool writeProofJson(const std::filesystem::path& outputPath,
                    const std::filesystem::path& frameDir,
                    const std::filesystem::path& dataDir,
                    const ProofResult& result,
                    const ProcessingService::BatchPipelineStats& batchStats,
                    const backend::services::CaptureStats& captureStats,
                    const TimingSummary& enqueueTiming,
                    bool success)
{
    std::ofstream out(outputPath);
    if (!out) {
        std::cerr << "failed to open proof JSON output: " << outputPath << '\n';
        return false;
    }

    const size_t maxCallbackBatch = result.callbackBatchSizes.empty()
                                        ? 0
                                        : *std::max_element(result.callbackBatchSizes.begin(),
                                                           result.callbackBatchSizes.end());

    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"runtime\": {\n";
    out << "    \"component\": \"backend::AppBackend + CaptureService + MockCamera + ProcessingService\",\n";
    out << "    \"proof\": \"mib studio software capture callback enqueues frames into the async batch pipeline\",\n";
    out << "    \"frame_dir\": \"" << jsonEscape(frameDir.string()) << "\",\n";
    out << "    \"data_dir\": \"" << jsonEscape(dataDir.string()) << "\"\n";
    out << "  },\n";
    out << "  \"capture\": {\n";
    out << "    \"expected_frames\": " << result.expectedFrames << ",\n";
    out << "    \"capture_callbacks\": " << result.captureCallbacks.load() << ",\n";
    out << "    \"capture_service_frames_processed\": " << captureStats.framesProcessed.load() << ",\n";
    out << "    \"capture_stopped\": " << (result.captureStopped ? "true" : "false") << ",\n";
    out << "    \"enqueue_accepted_from_capture_callback\": " << result.enqueueAccepted.load() << ",\n";
    out << "    \"enqueue_dropped_from_capture_callback\": " << result.enqueueDropped.load() << ",\n";
    out << "    \"enqueue_total_ms\": " << enqueueTiming.totalMs << ",\n";
    out << "    \"enqueue_avg_us\": " << enqueueTiming.avgUs << ",\n";
    out << "    \"enqueue_max_us\": " << enqueueTiming.maxUs << "\n";
    out << "  },\n";
    out << "  \"batch\": {\n";
    out << "    \"batch_size\": " << batchStats.batchSize << ",\n";
    out << "    \"worker_count\": " << batchStats.workerCount << ",\n";
    out << "    \"frames_accepted\": " << batchStats.framesAccepted << ",\n";
    out << "    \"frames_processed\": " << batchStats.framesProcessed << ",\n";
    out << "    \"frames_dropped\": " << batchStats.framesDropped << ",\n";
    out << "    \"batches_processed\": " << batchStats.batchesProcessed << ",\n";
    out << "    \"max_queue_depth\": " << batchStats.maxQueueDepth << ",\n";
    out << "    \"callback_batch_sizes\": [";
    for (size_t i = 0; i < result.callbackBatchSizes.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << result.callbackBatchSizes[i];
    }
    out << "],\n";
    out << "    \"max_callback_batch_size\": " << maxCallbackBatch << ",\n";
    out << "    \"first_batch_callback_after_all_frames_enqueued\": "
        << (result.firstBatchCallbackAfterAllFramesEnqueued ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"outputs\": {\n";
    out << "    \"processed_frames\": " << result.processedFrames << ",\n";
    out << "    \"non_empty_masks\": " << result.nonEmptyMasks << ",\n";
    out << "    \"frames_with_contours\": " << result.framesWithContours << ",\n";
    out << "    \"sample_index\": " << result.sample.index << ",\n";
    out << "    \"sample_area\": " << result.sample.validation.area << ",\n";
    out << "    \"sample_deformability\": " << result.sample.validation.deformability << ",\n";
    out << "    \"sample_ring_width\": " << result.sample.validation.ringRatio << "\n";
    out << "  },\n";
    out << "  \"success\": " << (success ? "true" : "false") << "\n";
    out << "}\n";
    return true;
}

} // namespace

int main(int argc, char* argv[])
{

    try {
        std::filesystem::path frameDir;
        std::filesystem::path outputDir;
        size_t expectedFrames = 64;
        size_t batchSize = 16;
        bool generatedFrames = false;

        if (argc >= 3) {
            frameDir = argv[1];
            outputDir = argv[2];
            if (argc >= 4) {
                expectedFrames = static_cast<size_t>(std::stoull(argv[3]));
            } else {
                expectedFrames = countSupportedImages(frameDir);
            }
            if (argc >= 5) {
                batchSize = static_cast<size_t>(std::stoull(argv[4]));
            } else {
                batchSize = expectedFrames;
            }
        } else {
            outputDir = makeTempDir("kin6_mib_app_capture_output");
            frameDir = makeTempDir("kin6_mib_app_capture_frames");
            generatedFrames = true;
            if (!generateSyntheticFrames(frameDir, expectedFrames)) {
                return 2;
            }
        }

        if (expectedFrames == 0 || batchSize == 0) {
            std::cerr << "expected frame count and batch size must be nonzero\n";
            return 3;
        }
        if (!std::filesystem::exists(frameDir)) {
            std::cerr << "frame directory does not exist: " << frameDir << '\n';
            return 4;
        }
        std::filesystem::create_directories(outputDir);

        const auto dataDir = makeTempDir("kin6_mib_app_data");
        backend::AppBackend backend;
        if (!backend.initialize((dataDir / "data").string())) {
            std::cerr << "AppBackend failed to initialize\n";
            return 5;
        }

        camera::mock::MockCameraOptions cameraOptions;
        cameraOptions.folder = frameDir;
        cameraOptions.frameInterval = std::chrono::microseconds(0);
        cameraOptions.loopFiles = false;
        backend.configureMockCamera(cameraOptions);

        ProcessingService::BatchPipelineConfig batchConfig;
        batchConfig.batchSize = batchSize;
        batchConfig.maxQueuedFrames = std::max(expectedFrames, batchSize);
        batchConfig.workerCount = 2;
        batchConfig.processing = makeProofConfig();

        ProofResult result;
        result.expectedFrames = expectedFrames;
        result.enqueueDurationsUs.reserve(expectedFrames);

        std::mutex mutex;
        std::condition_variable condition;
        bool capturedFirstBatchCallback = false;

        const bool batchStarted = backend.processing().startBatchPipeline(
            batchConfig,
            [&](std::vector<ProcessedFrame> batch) {
                std::scoped_lock lock(mutex);
                if (!capturedFirstBatchCallback) {
                    result.firstBatchCallbackAfterAllFramesEnqueued =
                        result.allFramesEnqueued.load(std::memory_order_acquire);
                    capturedFirstBatchCallback = true;
                }
                result.callbackBatchSizes.push_back(batch.size());
                for (auto& frame : batch) {
                    if (!frame.processedImage.empty() && cv::countNonZero(frame.processedImage) > 0) {
                        ++result.nonEmptyMasks;
                    }
                    if (frame.validation.allContours && !frame.validation.allContours->empty()) {
                        ++result.framesWithContours;
                    }
                    if (result.sample.originalImage.empty() &&
                        !frame.originalImage.empty() &&
                        !frame.processedImage.empty()) {
                        result.sample = frame;
                    }
                }
                result.processedFrames += batch.size();
                condition.notify_all();
            });
        if (!batchStarted) {
            std::cerr << "failed to start async batch pipeline\n";
            return 6;
        }

        backend.capture().setFrameCallback(
            [&](const uint8_t* data,
                size_t size,
                uint64_t width,
                uint64_t height,
                uint64_t timestampNs) {
                const uint64_t callbackIndex =
                    result.captureCallbacks.fetch_add(1, std::memory_order_relaxed);

                bool accepted = false;
                const auto start = std::chrono::steady_clock::now();
                if (data != nullptr && width > 0 && height > 0 && size > 0) {
                    const size_t h = static_cast<size_t>(height);
                    const size_t w = static_cast<size_t>(width);
                    const size_t step = h > 0 ? size / h : w;
                    if (step >= w && step * h <= size) {
                        cv::Mat view(static_cast<int>(height),
                                     static_cast<int>(width),
                                     CV_8UC1,
                                     const_cast<uint8_t*>(data),
                                     step);
                        accepted = backend.processing().enqueueBatchFrame(view, callbackIndex, timestampNs);
                    }
                }
                const auto end = std::chrono::steady_clock::now();

                {
                    std::scoped_lock lock(mutex);
                    result.enqueueDurationsUs.push_back(
                        std::chrono::duration<double, std::micro>(end - start).count());
                }

                if (accepted) {
                    const uint64_t acceptedCount =
                        result.enqueueAccepted.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (acceptedCount >= result.expectedFrames) {
                        result.allFramesEnqueued.store(true, std::memory_order_release);
                    }
                } else {
                    result.enqueueDropped.fetch_add(1, std::memory_order_relaxed);
                }
            });

        if (!backend.capture().start()) {
            std::cerr << "CaptureService failed to start\n";
            backend.processing().stopBatchPipeline();
            return 7;
        }

        const auto timeout = expectedFrames >= 5000
                                 ? std::chrono::minutes(4)
                                 : std::chrono::seconds(20);
        result.captureStopped = waitFor([&] { return !backend.capture().isRunning(); }, timeout);
        if (!result.captureStopped) {
            std::cerr << "timed out waiting for CaptureService to stop\n";
            backend.capture().stop();
        }

        const bool processedExpected = waitFor(
            [&] {
                std::scoped_lock lock(mutex);
                return result.processedFrames >= expectedFrames;
            },
            timeout);

        ProcessingService::BatchPipelineStats batchStats = backend.processing().getBatchPipelineStats();
        backend.processing().stopBatchPipeline();

        const TimingSummary enqueueTiming = summarizeDurations(result.enqueueDurationsUs);
        const bool visualWritten = writeVisualEvidence(outputDir, result.sample);
        const bool success =
            result.captureStopped &&
            processedExpected &&
            result.captureCallbacks.load() == expectedFrames &&
            backend.capture().stats().framesProcessed.load() == expectedFrames &&
            result.enqueueAccepted.load() == expectedFrames &&
            result.enqueueDropped.load() == 0 &&
            batchStats.framesAccepted == expectedFrames &&
            batchStats.framesProcessed == expectedFrames &&
            batchStats.framesDropped == 0 &&
            batchStats.batchSize == batchSize &&
            !result.callbackBatchSizes.empty() &&
            visualWritten;

        const auto proofPath = outputDir / "mib_app_capture_proof.json";
        if (!writeProofJson(proofPath,
                            frameDir,
                            dataDir,
                            result,
                            batchStats,
                            backend.capture().stats(),
                            enqueueTiming,
                            success)) {
            return 8;
        }

        std::cout << "MIB app capture proof: expected=" << expectedFrames
                  << " capture_callbacks=" << result.captureCallbacks.load()
                  << " accepted=" << result.enqueueAccepted.load()
                  << " processed=" << batchStats.framesProcessed
                  << " dropped=" << batchStats.framesDropped
                  << " batch_size=" << batchStats.batchSize
                  << " output=" << proofPath << '\n';

        if (generatedFrames) {
            std::error_code ec;
            std::filesystem::remove_all(frameDir, ec);
        }
        return success ? 0 : 9;
    } catch (const std::exception& ex) {
        std::cerr << "kin6_mib_app_capture_proof exception: " << ex.what() << '\n';
        return 10;
    }
}
