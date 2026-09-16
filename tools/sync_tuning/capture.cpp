// One bounded raw-frame measurement. Selection/reporting/profile writes live in
// the Python package; hardware ownership and cleanup stay in AppBackend.
#include "intensity.h"
#include "backend/app/AppBackend.h"
#include "backend/app/Tools.h"
#include "backend/camera/mindvision/MindVisionConfig.h"
#include "backend/services/CaptureService.h"
#include "backend/services/PulseGeneratorService.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using backend::services::CaptureLifecycleState;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) {
    interrupted = 1;
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name, value.c_str()) != 0)
        throw std::runtime_error("Cannot set capture environment");
#else
    if (setenv(name, value.c_str(), 1) != 0)
        throw std::runtime_error("Cannot set capture environment");
#endif
}

std::ofstream outputFile(const fs::path& path) {
    std::ofstream out;
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.open(path, std::ios::binary);
    return out;
}

void saveFrame(const fs::path& path, const backend::playback::Frame& frame) {
    auto out = outputFile(path);
    out << "P5\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (size_t y = 0; y < frame.height; ++y)
        out.write(reinterpret_cast<const char*>(frame.data.data() + y * frame.linePitch),
                  static_cast<std::streamsize>(frame.width));
    out.close();
}

struct Sample {
    uint64_t index, hostUs, cameraTicks;
    mib::sync::Intensity intensity;
};

double number(const std::string& text, double min, double max) {
    size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value) || value < min || value > max)
        throw std::runtime_error("Numeric option outside supported range: " + text);
    return value;
}
} // namespace

int main(int argc, char** argv) {
    fs::path output;
    Json report{{"schema_version", 1}, {"ok", false}, {"shutdown_confirmed", false}};
    try {
        std::map<std::string, std::string> args;
        bool validateOnly = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                SPDLOG_INFO("mib_sync_capture --profile FILE [--validate] --output NEW_DIR "
                            "--mode overview|experiment [--seconds 5] [--settle 1.5] "
                            "[--camera-index 0]. Close other camera owners first.");
                return 0;
            }
            if (key == "--validate") {
                validateOnly = true;
                continue;
            }
            if (key != "--profile" && key != "--output" && key != "--mode" && key != "--seconds" &&
                key != "--settle" && key != "--camera-index")
                throw std::runtime_error("Unknown option: " + key);
            if (++i >= argc || !args.emplace(key, argv[i]).second)
                throw std::runtime_error("Missing value or duplicate option: " + key);
        }
        if (!args.count("--profile")) throw std::runtime_error("--profile is required");
        std::ifstream input(fs::u8path(args.at("--profile")), std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read profile");
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        const auto parsed = backend::camera::mindvision::parseConfig(text);
        if (!parsed.ok || !parsed.config.illuminatedLive || !parsed.warnings.empty())
            throw std::runtime_error("Invalid illuminated profile: " + parsed.error);
        const auto config = parsed.config;
        if (config.acqTriggerDelayUs + config.exposureUs >= config.liveView.periodUs())
            throw std::runtime_error("Acquisition delay plus exposure must fit the trigger period");
        if (config.width < 4 || config.height < 12)
            throw std::runtime_error("ROI too small for three-band intensity measurement");
        if (validateOnly) {
            SPDLOG_INFO("Profile valid (no hardware opened)");
            return 0;
        }
        if (!args.count("--output") || !args.count("--mode"))
            throw std::runtime_error("--output and --mode are required");
        const bool overview = args.at("--mode") == "overview";
        if (!overview && args.at("--mode") != "experiment")
            throw std::runtime_error("--mode must be overview or experiment");
        const double seconds = number(args.count("--seconds") ? args.at("--seconds") : "5", 1, 120);
        const double settle = number(args.count("--settle") ? args.at("--settle") : "1.5", 0, 10);
        const double index =
            number(args.count("--camera-index") ? args.at("--camera-index") : "0", 0, 1024);
        if (index != std::floor(index)) throw std::runtime_error("Camera index must be an integer");
        const auto destination = fs::absolute(fs::u8path(args.at("--output")));
        if (!fs::create_directory(destination))
            throw std::runtime_error("Output directory already exists; use a fresh directory");
        output = destination; // Never overwrite an earlier run's result on failure.
        {
            auto preflight = outputFile(output / "profile.json");
            preflight << text;
            preflight.close();
        }

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        setEnv("MIB_CAMERA_MODE", "mindvision");
        setEnv("MIB_MINDVISION_CAMERA_INDEX", std::to_string(static_cast<int>(index)));
        setEnv("MIB_MINDVISION_CONFIG", fs::absolute(fs::u8path(args.at("--profile"))).u8string());
        // No focus movement, sorting, processing, recording or desktop startup.
        setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,processing,yolo,autofocus,trigger,playback");
        backend::AppBackend backend;
        if (!backend.initialize((output / "runtime").u8string()) ||
            !backend.isMindVisionCameraSelected())
            throw std::runtime_error(
                "MindVision backend unavailable (mock fallback is not calibration)");
        auto& capture = backend.capture();
        auto& generator = backend.pulseGenerator();
        auto check = [&] {
            if (interrupted || fs::exists(output / "cancel.request"))
                throw std::runtime_error("Capture cancelled");
            const auto state = capture.lifecycleSnapshot();
            if (state.state != CaptureLifecycleState::Running)
                throw std::runtime_error("Capture stopped: " + state.lastFailureMessage);
        };
        auto stop = [&] {
            capture.stop();
            const auto stopped = capture.lifecycleSnapshot();
            report["shutdown_confirmed"] =
                stopped.state == CaptureLifecycleState::Idle &&
                stopped.lastFailure != backend::services::CaptureFailureKind::ShutdownFailed &&
                !generator.liveViewOwned();
            if (!report["shutdown_confirmed"].get<bool>())
                throw std::runtime_error("Generator/LED shutdown unconfirmed: " +
                                         stopped.lastFailureMessage);
        };
        std::vector<Sample> samples;
        backend::playback::Frame first, dimmest, brightest;
        uint64_t missing = 0, excluded = 0, startIndex = 0, next = 0;
        double minMean = 256, maxMean = -1;
        try {
            std::string error;
            if (!backend.setMindVisionOverview(overview, &error)) throw std::runtime_error(error);
            if (interrupted || fs::exists(output / "cancel.request"))
                throw std::runtime_error("Capture cancelled");
            if (!capture.start()) throw std::runtime_error("Capture start rejected");
            capture.waitForState({CaptureLifecycleState::Running, CaptureLifecycleState::Faulted,
                                  CaptureLifecycleState::Idle},
                                 std::chrono::seconds(30));
            check();
            const auto warmUntil =
                backend::Tools::getTimestamp() + static_cast<uint64_t>(settle * 1e6);
            while (backend::Tools::getTimestamp() < warmUntil) {
                check();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto store = backend.getFrameStore();
            if (!store) throw std::runtime_error("No frame store");
            startIndex = next = store->committedCount();
            const auto startUs = backend::Tools::getTimestamp();
            const auto endUs = startUs + static_cast<uint64_t>(seconds * 1e6);
            const auto initialFrames = capture.stats().framesProcessed.load();
            const mib::sync::Region region{overview ? config.offsetX : 0,
                                           (overview ? config.offsetY : 0) + config.height / 4,
                                           config.width, config.height / 2};
            while (backend::Tools::getTimestamp() < endUs) {
                check();
                const auto end = store->committedCount();
                const auto earliest = store->earliestAvailableIndex();
                if (next < earliest) {
                    missing += earliest - next;
                    next = earliest;
                }
                for (; next < end; ++next) {
                    backend::playback::Frame frame;
                    const auto outcome = store->readByWriteIndex(next, frame);
                    if (outcome == backend::playback::FrameReadOutcome::Overwritten) {
                        ++missing;
                        continue;
                    }
                    if (outcome != backend::playback::FrameReadOutcome::Available)
                        throw std::runtime_error("Unreadable committed frame: " +
                                                 std::string(backend::playback::toString(outcome)));
                    if (!frame.hostTimestampUs)
                        throw std::runtime_error("Missing acquisition timestamp");
                    if (frame.hostTimestampUs < startUs || frame.hostTimestampUs >= endUs) {
                        ++excluded;
                        continue;
                    }
                    if (samples.size() >= 2000000)
                        throw std::runtime_error(
                            "Measurement exceeds 2 million frame limit; shorten duration");
                    const auto oriented =
                        mib::sync::orientRegion(region, frame.width, frame.height,
                                                config.flipHorizontal, config.flipVertical);
                    auto intensity = mib::sync::measure(frame, oriented);
                    if (samples.empty()) first = frame;
                    if (intensity.mean < minMean) {
                        minMean = intensity.mean;
                        dimmest = frame;
                    }
                    if (intensity.mean > maxMean) {
                        maxMean = intensity.mean;
                        brightest = frame;
                    }
                    samples.push_back({next, frame.hostTimestampUs, frame.timestamp, intensity});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check();
            const auto& stats = capture.stats();
            auto metric = [](uint64_t value, int validity) -> Json {
                return {
                    {"value", value},
                    {"validity", backend::services::toString(
                                     static_cast<backend::services::MetricValidity>(validity))}};
            };
            report.update({{"mode", args.at("--mode")},
                           {"seconds", seconds},
                           {"settle_seconds", settle},
                           {"profile", Json::parse(text)},
                           {"camera_index", index},
                           {"sensor_region",
                            {config.offsetX, config.offsetY + config.height / 4, config.width,
                             config.height / 2}},
                           {"sampling_stride", {4, 2}},
                           {"frames", samples.size()},
                           {"reader_missing", missing},
                           {"reader_excluded", excluded},
                           {"reader_considered", next - startIndex},
                           {"capture_fps", (stats.framesProcessed.load() - initialFrames) /
                                               ((backend::Tools::getTimestamp() - startUs) / 1e6)},
                           {"transport_lost", metric(stats.transportLostFrames.load(),
                                                     stats.transportLossValidity.load())},
                           {"discarded", metric(stats.intentionallyDiscardedFrames.load(),
                                                stats.discardsValidity.load())}});
            if (samples.empty()) throw std::runtime_error("No measured frames");
            if (next - startIndex != samples.size() + missing + excluded)
                throw std::runtime_error("Frame accounting mismatch");
            stop();
        } catch (...) {
            const auto failure = std::current_exception();
            stop();
            std::rethrow_exception(failure);
        }
        backend.shutdown();
        // Files are written only after the coordinated hardware shutdown.
        auto csv = outputFile(output / "frames.csv");
        csv << "index,host_us,camera_ticks,mean,spatial_sd,clipped_fraction,dark_fraction,top,"
               "middle,bottom\n";
        csv << std::setprecision(12);
        for (const auto& s : samples) {
            const auto& v = s.intensity;
            csv << s.index << ',' << s.hostUs << ',' << s.cameraTicks << ',' << v.mean << ','
                << v.spatialSd << ',' << v.clippedFraction << ',' << v.darkFraction << ','
                << v.bands[0] << ',' << v.bands[1] << ',' << v.bands[2] << '\n';
        }
        csv.close();
        saveFrame(output / "first.pgm", first);
        saveFrame(output / "dimmest.pgm", dimmest);
        saveFrame(output / "brightest.pgm", brightest);
        report["ok"] = true;
        auto result = outputFile(output / "capture.json");
        result << report.dump(2) << '\n';
        result.close();
        SPDLOG_INFO("Measured {} frames; {} reader misses; shutdown confirmed", samples.size(),
                    missing);
        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Calibration capture failed: {}", e.what());
        report["ok"] = false;
        report["error"] = e.what();
        if (!output.empty()) {
            try {
                auto out = outputFile(output / "capture.json");
                out << report.dump(2);
                out.close();
            } catch (const std::exception& io) {
                SPDLOG_ERROR("Cannot save failure report: {}", io.what());
            }
        }
        return 1;
    }
}
