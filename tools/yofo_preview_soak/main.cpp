// yofo_preview_soak: on-target smoke and soak of the YOFO Studio preview path
// (impl spec S6): AravisCamera against the PZ7035 GenTL producer, without the
// UI. Prints the settings the device applied and its delivered-rate model,
// then grabs for the requested time and reports, once per interval and at the
// end, delivered images/s against the model, queue telemetry, RSS and CPU.
//
//   yofo_preview_soak [--region X,Y,W,H] [--fps HZ] [--exposure US]
//                     [--mode latest|every] [--seconds N] [--interval S]
//
// Output is one JSON object per line: {"event":"session",...},
// {"event":"interval",...} and a final {"event":"summary",...}. Exit status 0
// when every interval delivered frames and the device applied the request
// without a geometry error; 1 otherwise.
#include "backend/camera/aravis/AravisCamera.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

struct ProcessSample {
    double cpuSeconds = 0.0; // user + system
    long rssKiB = 0;
};

ProcessSample sampleProcess()
{
    ProcessSample s;
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.rfind("VmRSS:", 0) == 0)
            s.rssKiB = std::strtol(line.c_str() + 6, nullptr, 10);
    }
    std::ifstream stat("/proc/self/stat");
    std::string all((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    // Fields after the parenthesised command name; utime and stime are 14 and 15.
    const auto close = all.rfind(')');
    if (close != std::string::npos) {
        std::istringstream rest(all.substr(close + 2));
        std::string field;
        unsigned long long utime = 0, stime = 0;
        for (int i = 3; i <= 15 && rest >> field; ++i) {
            if (i == 14)
                utime = std::strtoull(field.c_str(), nullptr, 10);
            if (i == 15)
                stime = std::strtoull(field.c_str(), nullptr, 10);
        }
        s.cpuSeconds = static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
    }
    return s;
}

bool parseRegion(const char* text, camera::aravis::AravisRegion& r)
{
    return std::sscanf(text, "%d,%d,%d,%d", &r.x, &r.y, &r.width, &r.height) == 4;
}

[[noreturn]] void usage()
{
    std::cerr << "usage: yofo_preview_soak [--region X,Y,W,H] [--fps HZ] [--exposure US]\n"
                 "                         [--mode latest|every] [--seconds N] [--interval S]\n";
    std::exit(2);
}

} // namespace

int main(int argc, char** argv)
{
    camera::aravis::AravisCameraOptions options;
    options.popTimeoutMs = 1000;
    camera::common::CameraConfig config;
    config.numBuffers = 8;
    config.deliveryMode = camera::common::FrameDeliveryMode::LatestFrame;
    double seconds = 10.0, interval = 1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (value == nullptr)
            usage();
        if (arg == "--region") {
            camera::aravis::AravisRegion r;
            if (!parseRegion(value, r))
                usage();
            options.region = r;
        } else if (arg == "--fps") {
            options.frameRateHz = std::atof(value);
        } else if (arg == "--exposure") {
            options.exposureUs = std::atof(value);
        } else if (arg == "--mode") {
            if (std::strcmp(value, "every") == 0)
                config.deliveryMode = camera::common::FrameDeliveryMode::EveryFrame;
            else if (std::strcmp(value, "latest") != 0)
                usage();
        } else if (arg == "--seconds") {
            seconds = std::atof(value);
        } else if (arg == "--interval") {
            interval = std::atof(value);
        } else {
            usage();
        }
        ++i;
    }

    const ProcessSample before = sampleProcess();
    const auto openStart = Clock::now();
    camera::aravis::AravisCamera camera(options);
    camera.applyConfig(config);
    if (!camera.start()) {
        const auto failure = camera.lastFailure();
        std::cout << nlohmann::json{{"event", "error"}, {"code", failure.code}, {"message", failure.message}}.dump()
                  << std::endl;
        return 1;
    }
    const double openSeconds = std::chrono::duration<double>(Clock::now() - openStart).count();
    const auto info = camera.sessionInfo();
    const auto ts = camera.timestampDescriptor();
    std::cout << nlohmann::json{
                     {"event", "session"},
                     {"device", info.deviceId},
                     {"vendor", info.vendor},
                     {"model", info.model},
                     {"open_s", openSeconds},
                     {"region", {info.region.x, info.region.y, info.region.width, info.region.height}},
                     {"frame_rate_hz", info.frameRateHz},
                     {"frame_rate_max_hz", info.frameRateMaxHz},
                     {"frame_rate_limit", info.frameRateLimitReason},
                     {"frame_rate_clamped", info.frameRateClamped},
                     {"exposure_us", info.exposureUs},
                     {"exposure_max_us", info.exposureMaxUs},
                     {"exposure_clamped", info.exposureClamped},
                     {"band_count", info.bandCount},
                     {"delivered_model_hz", info.deliveredFrameRateHz},
                     {"delivered_limit", info.deliveredFrameRateLimit},
                     {"timestamp_domain", camera::common::toString(ts.domain)},
                     {"timestamp_valid", ts.isValid()},
                 }
                     .dump()
              << std::endl;

    const auto start = Clock::now();
    auto mark = start;
    ProcessSample markSample = sampleProcess();
    const long rssAtStart = markSample.rssKiB;
    uint64_t total = 0, inInterval = 0, timeouts = 0, emptyIntervals = 0, intervals = 0;
    uint64_t lastTimestamp = 0, maxGapNs = 0, maxAgeNs = 0;
    long rssMax = rssAtStart;
    camera::common::Frame frame;
    while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
        if (camera.grabFrame(frame)) {
            ++total;
            ++inInterval;
            if (ts.isValid()) {
                const auto now = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
                if (now > frame.timestamp && now - frame.timestamp > maxAgeNs)
                    maxAgeNs = now - frame.timestamp;
                if (lastTimestamp != 0 && frame.timestamp > lastTimestamp &&
                    frame.timestamp - lastTimestamp > maxGapNs)
                    maxGapNs = frame.timestamp - lastTimestamp;
                lastTimestamp = frame.timestamp;
            }
        } else {
            ++timeouts;
        }
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - mark).count();
        if (elapsed >= interval) {
            const ProcessSample sample = sampleProcess();
            camera::common::AcquisitionQueueStats queue;
            camera.pollAcquisitionQueueStats(queue);
            ++intervals;
            if (inInterval == 0)
                ++emptyIntervals;
            if (sample.rssKiB > rssMax)
                rssMax = sample.rssKiB;
            std::cout << nlohmann::json{
                             {"event", "interval"},
                             {"t_s", std::chrono::duration<double>(now - start).count()},
                             {"images_per_s", inInterval / elapsed},
                             {"model_hz", info.deliveredFrameRateHz},
                             {"cpu_percent", 100.0 * (sample.cpuSeconds - markSample.cpuSeconds) / elapsed},
                             {"rss_kib", sample.rssKiB},
                             {"discarded", queue.intentionallyDiscardedFrames},
                             {"transport_lost", queue.transportLostFrames},
                             {"underruns", queue.bufferUnderruns},
                             {"max_gap_ms", maxGapNs / 1e6},
                             {"max_age_ms", maxAgeNs / 1e6},
                         }
                             .dump()
                      << std::endl;
            mark = now;
            markSample = sample;
            inInterval = 0;
            maxGapNs = 0;
            maxAgeNs = 0;
        }
    }
    const double run = std::chrono::duration<double>(Clock::now() - start).count();
    const ProcessSample after = sampleProcess();
    camera::common::AcquisitionQueueStats queue;
    camera.pollAcquisitionQueueStats(queue);
    camera.stop();
    const double rate = total / run;
    const bool ok = total > 0 && emptyIntervals == 0;
    std::cout << nlohmann::json{
                     {"event", "summary"},
                     {"ok", ok},
                     {"seconds", run},
                     {"images", total},
                     {"images_per_s", rate},
                     {"model_hz", info.deliveredFrameRateHz},
                     {"ratio_to_model", info.deliveredFrameRateHz > 0 ? rate / info.deliveredFrameRateHz : 0.0},
                     {"timeouts", timeouts},
                     {"empty_intervals", emptyIntervals},
                     {"discarded", queue.intentionallyDiscardedFrames},
                     {"transport_lost", queue.transportLostFrames},
                     {"underruns", queue.bufferUnderruns},
                     {"cpu_percent", 100.0 * (after.cpuSeconds - before.cpuSeconds) / (run + openSeconds)},
                     {"rss_start_kib", rssAtStart},
                     {"rss_end_kib", after.rssKiB},
                     {"rss_max_kib", rssMax},
                 }
                     .dump()
              << std::endl;
    return ok ? 0 : 1;
}
