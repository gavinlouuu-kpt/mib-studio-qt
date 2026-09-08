// mock_pipeline_timing_run
//
// Headless dry-run of the FULL production pipeline —
// MockCamera -> CaptureService -> FrameStore -> ProcessingService (inline
// realtime) -> TriggerService — with PipelineTimingRecorder enabled, for
// diagnosing pipeline/trigger latency without hardware.
//
// Feed it a folder of stream frames (e.g. the Hugging Face dataset
// gavinlouuu/512x96stream, fetched with scripts/fetch_hf_512x96stream.py).
// By default the ROI is the RIGHT THIRD of the field of view, the background
// is the per-pixel median of sampled frames, and the target-group gates are
// wide open so EVERY valid detection fires TriggerService (MockCamera
// simulates the digital output line). Timing CSVs are dumped on stop; analyse
// with scripts/analyze_pipeline_timing.py.
//
// Usage:
//   mock_pipeline_timing_run --frames <dir> [--fps 200] [--duration 20]
//       [--out <dump dir>] [--data-dir <dir>] [--roi x,y,w,h]
//       [--background <image>] [--drop-frames]
//
// This is a manual diagnosis tool, not a CTest (it needs a frames folder).

#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/diagnostics/PipelineTimingRecorder.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/TriggerService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using backend::diagnostics::PipelineSkipReason;
using backend::diagnostics::PipelineTimingRecorder;

namespace {

struct Options {
    std::string framesDir;
    std::string outDir;
    std::string dataDir;
    std::string backgroundPath;
    double fps{200.0};
    double durationSec{20.0};
    bool dropFrames{false};
    int roi[4]{-1, -1, -1, -1}; // x,y,w,h; -1 = derive right third
};

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--frames") {
            const char* v = next("--frames");
            if (!v) return false;
            opt.framesDir = v;
        } else if (arg == "--out") {
            const char* v = next("--out");
            if (!v) return false;
            opt.outDir = v;
        } else if (arg == "--data-dir") {
            const char* v = next("--data-dir");
            if (!v) return false;
            opt.dataDir = v;
        } else if (arg == "--background") {
            const char* v = next("--background");
            if (!v) return false;
            opt.backgroundPath = v;
        } else if (arg == "--fps") {
            const char* v = next("--fps");
            if (!v) return false;
            opt.fps = std::atof(v);
        } else if (arg == "--duration") {
            const char* v = next("--duration");
            if (!v) return false;
            opt.durationSec = std::atof(v);
        } else if (arg == "--drop-frames") {
            opt.dropFrames = true;
        } else if (arg == "--roi") {
            const char* v = next("--roi");
            if (!v) return false;
            if (std::sscanf(v, "%d,%d,%d,%d", &opt.roi[0], &opt.roi[1], &opt.roi[2], &opt.roi[3]) !=
                4) {
                std::cerr << "--roi expects x,y,w,h\n";
                return false;
            }
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    if (opt.framesDir.empty()) {
        std::cerr << "usage: mock_pipeline_timing_run --frames <dir> [--fps N] "
                     "[--duration Sec] [--out dir] [--roi x,y,w,h] [--background img] "
                     "[--drop-frames]\n";
        return false;
    }
    return true;
}

std::vector<fs::path> listFrameFiles(const fs::path& dir) {
    static const std::vector<std::string> exts = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"};
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Per-pixel median over up to `maxSamples` frames evenly sampled from the
// folder — a clean static background even if some frames contain objects.
cv::Mat computeMedianBackground(const std::vector<fs::path>& files, size_t maxSamples) {
    const size_t sampleCount = std::min(maxSamples, files.size());
    std::vector<cv::Mat> samples;
    samples.reserve(sampleCount);
    const size_t stride = std::max<size_t>(1, files.size() / sampleCount);
    for (size_t i = 0; i < files.size() && samples.size() < sampleCount; i += stride) {
        cv::Mat gray = cv::imread(files[i].string(), cv::IMREAD_GRAYSCALE);
        if (!gray.empty() && (samples.empty() || gray.size() == samples.front().size())) {
            samples.push_back(std::move(gray));
        }
    }
    if (samples.empty()) return {};

    cv::Mat median(samples.front().rows, samples.front().cols, CV_8UC1);
    std::vector<uint8_t> values(samples.size());
    for (int y = 0; y < median.rows; ++y) {
        uint8_t* dst = median.ptr<uint8_t>(y);
        for (int x = 0; x < median.cols; ++x) {
            for (size_t k = 0; k < samples.size(); ++k) {
                values[k] = samples[k].ptr<uint8_t>(y)[x];
            }
            auto mid = values.begin() + static_cast<long>(values.size() / 2);
            std::nth_element(values.begin(), mid, values.end());
            dst[x] = *mid;
        }
    }
    return median;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    const auto files = listFrameFiles(opt.framesDir);
    if (files.empty()) {
        std::cerr << "no frame images found in " << opt.framesDir << "\n";
        return 2;
    }
    cv::Mat firstFrame = cv::imread(files.front().string(), cv::IMREAD_GRAYSCALE);
    if (firstFrame.empty()) {
        std::cerr << "could not read " << files.front() << "\n";
        return 2;
    }
    const int frameW = firstFrame.cols;
    const int frameH = firstFrame.rows;

    // Default ROI: right third of the field of view.
    if (opt.roi[2] <= 0 || opt.roi[3] <= 0) {
        opt.roi[0] = (frameW * 2) / 3;
        opt.roi[1] = 0;
        opt.roi[2] = frameW - opt.roi[0];
        opt.roi[3] = frameH;
    }

    if (opt.dataDir.empty()) opt.dataDir = "data/mock_timing_run";
    if (opt.outDir.empty()) opt.outDir = opt.dataDir + "/pipeline_timing";

    // Background: explicit file, or per-pixel median of sampled frames.
    cv::Mat background;
    if (!opt.backgroundPath.empty()) {
        background = cv::imread(opt.backgroundPath, cv::IMREAD_GRAYSCALE);
        if (background.empty()) {
            std::cerr << "could not read background " << opt.backgroundPath << "\n";
            return 2;
        }
    } else {
        std::cout << "computing per-pixel median background from sampled frames...\n";
        background = computeMedianBackground(files, 128);
        if (background.empty()) {
            std::cerr << "failed to compute median background\n";
            return 2;
        }
        const auto bgOut = fs::path(opt.dataDir) / "median_background.png";
        fs::create_directories(bgOut.parent_path());
        cv::imwrite(bgOut.string(), background);
        std::cout << "background saved to " << bgOut << "\n";
    }

    // Enable the latency recorder before backend init so AppBackend picks up
    // the dump directory and auto-dumps on capture stop.
#ifdef _WIN32
    _putenv_s("MIB_PIPELINE_TIMING", "1");
    _putenv_s("MIB_PIPELINE_TIMING_DIR", opt.outDir.c_str());
#else
    setenv("MIB_PIPELINE_TIMING", "1", 1);
    setenv("MIB_PIPELINE_TIMING_DIR", opt.outDir.c_str(), 1);
#endif

    backend::AppBackend backendApp;
    if (!backendApp.initialize(opt.dataDir)) {
        std::cerr << "AppBackend::initialize failed\n";
        return 1;
    }

    camera::mock::MockCameraOptions mockOptions;
    mockOptions.folder = opt.framesDir;
    mockOptions.frameInterval =
        std::chrono::microseconds(static_cast<int64_t>(1'000'000.0 / std::max(1.0, opt.fps)));
    mockOptions.loopFiles = true;
    backendApp.configureMockCamera(mockOptions);

    // Every valid detection is a target: wide-open target-group gates.
    backend::services::ProcessingConfig cfg;
    cfg.gaussian_blur_size = 3;
    cfg.bg_subtract_threshold = 8;
    cfg.morph_kernel_size = 3;
    cfg.morph_iterations = 1;
    cfg.enable_border_check = false;
    cfg.enable_area_range_check = false;
    cfg.enable_deformability_range_check = false;
    cfg.enable_area_ratio_check = false;
    cfg.enable_ring_ratio_check = false;
    cfg.require_single_inner_contour = false;
    cfg.empty_frame_pixel_threshold = 100;
    cfg.auto_background_enabled = false;
    cfg.enable_target_group = true;
    cfg.target_group_area_min = 0;
    cfg.target_group_area_max = 1'000'000'000;
    cfg.target_group_deformability_min = 0.0;
    cfg.target_group_deformability_max = 1.0;
    cfg.enable_target_group_emodulus = false;

    auto& proc = backendApp.processing();
    proc.setProcessingConfig(cfg);
    proc.setRealtimeProcessingMode(
        backend::services::ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeDropFrames(opt.dropFrames);
    proc.setRealtimeRoi({opt.roi[0], opt.roi[1], opt.roi[2], opt.roi[3]});
    proc.setRealtimeBackgroundGray(background);
    proc.setMonitoringActive(false);

    std::cout << "frames=" << files.size() << " (" << frameW << "x" << frameH << ")"
              << " fps=" << opt.fps << " duration=" << opt.durationSec << "s"
              << " roi=" << opt.roi[0] << "," << opt.roi[1] << "," << opt.roi[2] << ","
              << opt.roi[3] << " drop_frames=" << (opt.dropFrames ? "on" : "off")
              << "\ndump dir: " << opt.outDir << "\n";

    proc.startRealtime(backendApp.getFrameStore());
    proc.setRealtimeEnabled(true);
    if (!backendApp.capture().start()) {
        std::cerr << "capture start failed\n";
        return 1;
    }

    auto& recorder = PipelineTimingRecorder::instance();
    const auto start = std::chrono::steady_clock::now();
    uint64_t lastTriggerCount = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
           opt.durationSec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint64_t triggers = backendApp.trigger().getTriggerCount();
        std::cout
            << "  t="
            << static_cast<int>(
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count())
            << "s capture_fps=" << backendApp.capture().stats().lastFrameRate.load()
            << " algo_fps=" << proc.getAlgoFps1s() << " valid_fps=" << proc.getValidFps1s()
            << " algo_avg_us=" << proc.getAlgoAvgUs1s() << " triggers=" << triggers << " (+"
            << (triggers - lastTriggerCount) << "/s)"
            << " frame_records=" << recorder.frameRecordCount() << "\n";
        lastTriggerCount = triggers;
    }

    backendApp.capture().stop(); // joins trigger thread, auto-dumps timing CSVs
    proc.setRealtimeEnabled(false);
    proc.stopRealtime();

    const uint64_t frameRecords = recorder.frameRecordCount();
    const uint64_t triggerRecords = recorder.triggerRecordCount();
    std::cout << "\n=== session summary ===\n"
              << "frames captured:  " << backendApp.capture().stats().framesProcessed.load()
              << "\nframe records:    " << frameRecords
              << "\nempty frames:     " << recorder.skippedCount(PipelineSkipReason::EmptyFrame)
              << "\ndropped_to_latest:"
              << recorder.skippedCount(PipelineSkipReason::DroppedToLatest)
              << "\nring_behind:      " << recorder.skippedCount(PipelineSkipReason::RingBehind)
              << "\ntrigger pulses:   " << backendApp.trigger().getTriggerCount()
              << "\ntrigger records:  " << triggerRecords << "\n";

    backendApp.shutdown();
    std::cout << "\nanalyse with:\n  python3 scripts/analyze_pipeline_timing.py " << opt.outDir
              << "\n";
    return 0;
}
