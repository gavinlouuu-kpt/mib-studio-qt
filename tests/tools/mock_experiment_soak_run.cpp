// mock_experiment_soak_run
//
// Headless recording soak through the production save path, used for the
// ADR 0006 (lossless compression) headroom measurements:
//
//   MockCamera -> CaptureService -> FrameStore -> ProcessingService (inline
//   realtime) -> ExperimentCoordinator (experiment mode) or
//   AppBackend::startFrameRecording (recording mode) -> HdfWriteQueue -> HDF5
//
// It runs one run for --duration seconds at --fps, stops it through the same
// backend path the app uses, and prints one JSON object: capture/processing
// counters, the run accounting as stored in the file (#367), stop latency,
// file size and process CPU time. scripts/run_compression_headroom_e2e.py
// runs it with and without a concurrent compression load and compares runs.
//
// Feed it the 512x96 stream frames:
//   python3 scripts/provision-assets.py --asset 512x96stream-mock-frames --count 1000
// Usage:
//   mib_backend_tests mock_experiment_soak_run --frames <dir> --out <file.h5>
//       [--mode experiment|recording] [--fps 1000] [--duration 300]
//       [--gates default|open] [--roi x,y,w,h] [--data-dir <dir>] [--json <report.json>]
//
// --gates default keeps the app's default ProcessingConfig (only the
// background comes from the frames); --gates open is mock_pipeline_timing_run's
// wide-open contour config, which detects far more objects per frame.
//
// This is a manual measurement tool, not a CTest (it needs a frames folder).

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/services/CaptureService.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string framesDir;
    std::string outPath;
    std::string dataDir;
    std::string jsonPath;
    std::string mode{"experiment"};
    std::string gates{"default"};
    double fps{1000.0};
    double durationSec{300.0};
    int roi[4]{-1, -1, -1, -1}; // x,y,w,h; -1 = full frame
};

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (arg == "--frames" && (v = next())) opt.framesDir = v;
        else if (arg == "--out" && (v = next())) opt.outPath = v;
        else if (arg == "--data-dir" && (v = next())) opt.dataDir = v;
        else if (arg == "--json" && (v = next())) opt.jsonPath = v;
        else if (arg == "--mode" && (v = next())) opt.mode = v;
        else if (arg == "--gates" && (v = next())) opt.gates = v;
        else if (arg == "--fps" && (v = next())) opt.fps = std::atof(v);
        else if (arg == "--duration" && (v = next())) opt.durationSec = std::atof(v);
        else if (arg == "--roi" && (v = next())) {
            if (std::sscanf(v, "%d,%d,%d,%d", &opt.roi[0], &opt.roi[1], &opt.roi[2], &opt.roi[3]) != 4) {
                std::cerr << "--roi expects x,y,w,h\n";
                return false;
            }
        } else {
            std::cerr << "unknown or incomplete argument: " << arg << "\n";
            return false;
        }
    }
    if (opt.framesDir.empty() || opt.outPath.empty() ||
        (opt.mode != "experiment" && opt.mode != "recording") ||
        (opt.gates != "default" && opt.gates != "open")) {
        std::cerr << "usage: mock_experiment_soak_run --frames <dir> --out <file.h5> "
                     "[--mode experiment|recording] [--gates default|open] [--fps N] [--duration Sec] "
                     "[--roi x,y,w,h] "
                     "[--data-dir dir] [--json report.json]\n";
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
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Per-pixel median of evenly sampled frames: a clean static background.
cv::Mat medianBackground(const std::vector<fs::path>& files, size_t maxSamples) {
    std::vector<cv::Mat> samples;
    const size_t stride = std::max<size_t>(1, files.size() / std::max<size_t>(1, maxSamples));
    for (size_t i = 0; i < files.size() && samples.size() < maxSamples; i += stride) {
        cv::Mat g = cv::imread(files[i].string(), cv::IMREAD_GRAYSCALE);
        if (!g.empty() && (samples.empty() || g.size() == samples.front().size())) samples.push_back(g);
    }
    if (samples.empty()) return {};
    cv::Mat median(samples.front().size(), CV_8UC1);
    std::vector<uint8_t> values(samples.size());
    for (int y = 0; y < median.rows; ++y)
        for (int x = 0; x < median.cols; ++x) {
            for (size_t k = 0; k < samples.size(); ++k) values[k] = samples[k].at<uint8_t>(y, x);
            std::nth_element(values.begin(), values.begin() + static_cast<long>(values.size() / 2), values.end());
            median.at<uint8_t>(y, x) = values[values.size() / 2];
        }
    return median;
}

double processCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    auto toSec = [](const FILETIME& f) {
        ULARGE_INTEGER v;
        v.LowPart = f.dwLowDateTime;
        v.HighPart = f.dwHighDateTime;
        return static_cast<double>(v.QuadPart) / 1e7;
    };
    return toSec(k) + toSec(u);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
           static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#endif
}

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

void appendAccounting(std::ostringstream& j, const char* key,
                      const backend::recording::RecordingAccountingSnapshot& a, bool present) {
    j << ",\"" << key << "\":";
    if (!present) {
        j << "null";
        return;
    }
    j << "{\"completion\":\"" << backend::recording::toString(a.completion) << "\""
      << ",\"completion_reason\":\"" << jsonEscape(a.completionReason) << "\""
      << ",\"reconciled\":" << (a.reconciled ? "true" : "false")
      << ",\"admitted\":" << a.admitted << ",\"empty\":" << a.empty << ",\"processed\":" << a.processed
      << ",\"scientifically_rejected\":" << a.scientificallyRejected
      << ",\"processing_failed\":" << a.processingFailed << ",\"store_overwritten\":" << a.storeOverwritten
      << ",\"store_not_committed\":" << a.storeNotCommitted << ",\"store_malformed\":" << a.storeMalformed
      << ",\"cancelled_by_policy\":" << a.cancelledByPolicy << ",\"pending_at_stop\":" << a.pendingAtStop
      << ",\"persistence_admitted\":" << a.persistenceAdmitted
      << ",\"persistence_committed\":" << a.persistenceCommitted
      << ",\"persistence_failed\":" << a.persistenceFailed
      << ",\"persistence_pending_at_stop\":" << a.persistencePendingAtStop
      << ",\"persistence_cancelled_by_policy\":" << a.persistenceCancelledByPolicy
      << ",\"sequence_gaps\":" << a.sequenceGaps << ",\"sequence_gap_frames\":" << a.sequenceGapFrames
      << ",\"fatal\":" << (a.fatalError ? "true" : "false") << ",\"fatal_message\":\""
      << jsonEscape(a.fatalMessage) << "\"}";
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
    const cv::Mat first = cv::imread(files.front().string(), cv::IMREAD_GRAYSCALE);
    if (first.empty()) {
        std::cerr << "could not read " << files.front() << "\n";
        return 2;
    }
    if (opt.roi[2] <= 0 || opt.roi[3] <= 0) {
        opt.roi[0] = 0;
        opt.roi[1] = 0;
        opt.roi[2] = first.cols;
        opt.roi[3] = first.rows;
    }
    if (opt.dataDir.empty()) opt.dataDir = (fs::path(opt.outPath).parent_path() / "soak_data").string();
    std::error_code ec;
    fs::remove(opt.outPath, ec);

    const cv::Mat background = medianBackground(files, 128);
    if (background.empty()) {
        std::cerr << "failed to compute median background\n";
        return 2;
    }

    backend::AppBackend app;
    if (!app.initialize(opt.dataDir)) {
        std::cerr << "AppBackend::initialize failed\n";
        return 1;
    }
    app.experiment().setApplicationIdentity("mock_experiment_soak_run", "local", "headless");

    camera::mock::MockCameraOptions mock;
    mock.folder = opt.framesDir;
    mock.frameInterval = std::chrono::microseconds(static_cast<int64_t>(1'000'000.0 / std::max(1.0, opt.fps)));
    mock.loopFiles = true;
    app.configureMockCamera(mock);

    auto& proc = app.processing();
    backend::services::ProcessingConfig cfg = proc.getProcessingConfig();
    cfg.auto_background_enabled = false; // the median background below is authoritative
    if (opt.gates == "open") {
        // mock_pipeline_timing_run's gates: a real contour pipeline with every
        // range check open, so detections (not the gates) set the load.
        cfg = backend::services::ProcessingConfig{};
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
        cfg.enable_target_group = false;
    }
    proc.setProcessingConfig(cfg);
    proc.setRealtimeProcessingMode(backend::services::ProcessingService::RealtimeProcessingMode::Inline);
    proc.setRealtimeRoi({opt.roi[0], opt.roi[1], opt.roi[2], opt.roi[3]});
    proc.setRealtimeBackgroundGray(background);
    proc.setMonitoringActive(false);
    proc.startRealtime(app.getFrameStore());
    proc.setRealtimeEnabled(true);

    if (app.capture().requestStart() != backend::services::CaptureStartOutcome::Accepted ||
        !waitFor([&] { return app.capture().stats().framesProcessed.load() > 10; }, std::chrono::seconds(10))) {
        std::cerr << "mock capture did not start\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // let rates settle

    std::cerr << "mode=" << opt.mode << " frames=" << files.size() << " (" << first.cols << "x" << first.rows
              << ") fps=" << opt.fps << " duration=" << opt.durationSec << "s out=" << opt.outPath << "\n";

    const uint64_t capture0 = app.capture().stats().framesProcessed.load();
    const uint64_t dropV0 = proc.getDroppedValidFrames();
    const uint64_t dropI0 = proc.getDroppedInvalidFrames();
    const double cpu0 = processCpuSeconds();
    const auto t0 = Clock::now();

    std::string startError;
    if (opt.mode == "experiment") {
        auto& coord = app.experiment();
        const auto readiness = coord.evaluateReadiness(opt.outPath);
        backend::app::ExperimentStartRequest req;
        req.outputPath = opt.outPath;
        req.readinessGeneration = readiness.generation;
        const auto started = coord.start(req);
        if (!started.started()) startError = std::string(backend::app::toString(started.outcome)) + ": " + started.message;
    } else if (!app.startFrameRecording(opt.outPath)) {
        startError = "startFrameRecording failed";
    }
    if (!startError.empty()) {
        std::cerr << "run did not start: " << startError << "\n";
        app.shutdown();
        return 1;
    }

    std::vector<double> algoFps;
    while (std::chrono::duration<double>(Clock::now() - t0).count() < opt.durationSec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        algoFps.push_back(proc.getAlgoFps1s());
        const bool stillRunning = opt.mode == "experiment"
                                      ? app.experiment().state() == backend::app::ExperimentRunState::Active
                                      : app.isFrameRecording();
        if (!stillRunning) {
            std::cerr << "run ended early (fatal save error?)\n";
            break;
        }
    }
    const double runSec = std::chrono::duration<double>(Clock::now() - t0).count();
    const uint64_t captured = app.capture().stats().framesProcessed.load() - capture0;

    const auto tStop = Clock::now();
    backend::app::ExperimentStatus status;
    backend::recording::RecordingAccountingSnapshot live;
    if (opt.mode == "experiment") {
        app.experiment().requestStop(false);
        waitFor([&] { return app.experiment().status().terminal; }, std::chrono::minutes(5));
        status = app.experiment().status();
    } else {
        app.stopFrameRecording();
        live = app.recordingAccounting();
    }
    const double stopMs = std::chrono::duration<double, std::milli>(Clock::now() - tStop).count();
    const double cpuSec = processCpuSeconds() - cpu0;

    app.capture().stop();
    proc.setRealtimeEnabled(false);
    proc.stopRealtime();

    // The stored accounting is the authoritative record of the run.
    backend::recording::RecordingAccountingSnapshot stored;
    bool storedOk = false;
    size_t imageFrames = 0;
    {
        backend::services::Hdf5Service reader;
        if (reader.loadFile(opt.outPath)) {
            storedOk = reader.readRunAccounting(stored);
            int h = 0, w = 0, c = 0;
            const char* ds = opt.mode == "experiment" ? "/valid_frames/images" : "/recorded_frames/images";
            if (!reader.getDatasetInfo(ds, imageFrames, h, w, c)) imageFrames = 0;
            reader.closeFile();
        }
    }
    const auto fileBytes = fs::exists(opt.outPath) ? fs::file_size(opt.outPath) : 0;

    double algoMean = 0.0, algoMin = algoFps.empty() ? 0.0 : algoFps.front();
    for (double f : algoFps) {
        algoMean += f;
        algoMin = std::min(algoMin, f);
    }
    if (!algoFps.empty()) algoMean /= static_cast<double>(algoFps.size());

    std::ostringstream j;
    j << "{\"tool\":\"mock_experiment_soak_run\",\"mode\":\"" << opt.mode << "\",\"gates\":\"" << opt.gates
      << "\",\"fps_requested\":" << opt.fps
      << ",\"duration_s\":" << runSec << ",\"frame_width\":" << first.cols << ",\"frame_height\":" << first.rows
      << ",\"roi\":[" << opt.roi[0] << "," << opt.roi[1] << "," << opt.roi[2] << "," << opt.roi[3] << "]"
      << ",\"hardware_concurrency\":" << std::thread::hardware_concurrency()
      << ",\"capture\":{\"frames\":" << captured << ",\"fps_measured\":" << (runSec > 0 ? captured / runSec : 0.0)
      << ",\"intentionally_discarded\":" << app.capture().stats().intentionallyDiscardedFrames.load()
      << ",\"transport_lost\":" << app.capture().stats().transportLostFrames.load() << "}"
      << ",\"processing\":{\"algo_fps_mean\":" << algoMean << ",\"algo_fps_min\":" << algoMin
      << ",\"dropped_valid\":" << (proc.getDroppedValidFrames() - dropV0)
      << ",\"dropped_invalid\":" << (proc.getDroppedInvalidFrames() - dropI0) << "}"
      << ",\"stop_ms\":" << stopMs << ",\"process_cpu_s\":" << cpuSec
      << ",\"process_cpu_cores\":" << (runSec > 0 ? cpuSec / (runSec + stopMs / 1000.0) : 0.0)
      << ",\"file_bytes\":" << fileBytes << ",\"image_frames_in_file\":" << imageFrames;
    if (opt.mode == "experiment")
        j << ",\"experiment\":{\"state\":\"" << backend::app::toString(status.state) << "\",\"finalization_ok\":"
          << (status.finalizationOk ? "true" : "false") << ",\"fault_code\":\"" << jsonEscape(status.faultCode)
          << "\",\"fault_message\":\"" << jsonEscape(status.faultMessage) << "\"}";
    else
        appendAccounting(j, "live_accounting", live, true);
    appendAccounting(j, "stored_accounting", stored, storedOk);
    j << "}";

    app.shutdown();

    std::cout << j.str() << std::endl;
    if (!opt.jsonPath.empty()) std::ofstream(opt.jsonPath) << j.str() << "\n";
    const bool complete = storedOk && stored.completion == backend::recording::RunCompletionState::Complete;
    return complete ? 0 : 3;
}
