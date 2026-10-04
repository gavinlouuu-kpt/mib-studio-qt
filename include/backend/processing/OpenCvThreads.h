#pragma once

// MIB_OPENCV_THREADS resolution, shared by the host (AppBackend) and the
// processing-core plugins. On Windows OpenCV parallelises through the
// Concurrency Runtime, whose idle workers spin; a per-frame parallel_for on the
// realtime path keeps the whole pool busy and starves the processing thread.
// Released cores link OpenCV statically, so each core applies the setting to
// its own OpenCV copy; dev builds share one OpenCV DLL and apply it twice
// (idempotent).

#include <opencv2/core.hpp>

#include <cstdlib>
#include <string>

namespace backend::processing {

struct OpenCvThreadsSetting {
    bool keepOpenCvDefault = false; // MIB_OPENCV_THREADS=opencv
    int threads = 0;                // passed to cv::setNumThreads unless keepOpenCvDefault
    bool invalid = false;           // set but not "opencv" or an integer 0..256
    std::string raw;                // the environment value, empty when unset
};

// Default (unset or empty): 0, OpenCV runs inline on the calling thread.
inline OpenCvThreadsSetting resolveOpenCvThreads(const char* env) {
    OpenCvThreadsSetting setting;
    if (!env || !*env) {
        return setting;
    }
    setting.raw = env;
    if (setting.raw == "opencv") {
        setting.keepOpenCvDefault = true;
        return setting;
    }
    char* end = nullptr;
    const long parsed = std::strtol(setting.raw.c_str(), &end, 10);
    if (end != setting.raw.c_str() && *end == '\0' && parsed >= 0 && parsed <= 256) {
        setting.threads = static_cast<int>(parsed);
    } else {
        setting.invalid = true;
    }
    return setting;
}

// Applies MIB_OPENCV_THREADS to the OpenCV instance this binary links.
inline OpenCvThreadsSetting applyOpenCvThreadsFromEnvironment() {
    const OpenCvThreadsSetting setting = resolveOpenCvThreads(std::getenv("MIB_OPENCV_THREADS"));
    if (!setting.keepOpenCvDefault) {
        cv::setNumThreads(setting.threads);
    }
    return setting;
}

} // namespace backend::processing
