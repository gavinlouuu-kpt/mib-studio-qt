// AppBackend::initialize turns OpenCV's own parallel_for off unless
// MIB_OPENCV_THREADS says otherwise. On Windows the Conan OpenCV parallelises
// through the Concurrency Runtime, whose idle workers spin: a per-frame
// parallel call in the realtime path kept ~31 workers busy and halved
// experiment throughput at 5000 fps on the rig (2026-10-01).
#include "backend/app/AppBackend.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// Resets OpenCV to its own default, boots a mock-camera backend with the given
// MIB_OPENCV_THREADS (nullptr = unset) and returns cv::getNumThreads() after
// initialize.
int threadsAfterInitialize(const char* setting) {
    cv::setNumThreads(-1);
    if (setting) {
        setEnv("MIB_OPENCV_THREADS", setting);
    } else {
        unsetEnv("MIB_OPENCV_THREADS");
    }
    mib::test::TempDir td("mib_opencv_threads");
    std::filesystem::create_directories(td / "mock_frames");
    setEnv("MIB_MOCK_CAMERA_DIR", (td / "mock_frames").string().c_str());
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "AppBackend initialize");
    const int threads = cv::getNumThreads();
    backend.shutdown();
    return threads;
}

} // namespace

int main() {
    setEnv("MIB_CAMERA_MODE", "mock");
    // What this platform's OpenCV reports for "inline" (setNumThreads(0)) and
    // for its own default; the reported numbers differ between parallel
    // backends (ConcRT on Windows, pthreads/TBB/OpenMP elsewhere).
    cv::setNumThreads(0);
    const int inlineThreads = cv::getNumThreads();
    cv::setNumThreads(-1);
    const int opencvDefault = cv::getNumThreads();
    std::printf("OpenCV reports inline=%d default=%d\n", inlineThreads, opencvDefault);

    const int byDefault = threadsAfterInitialize(nullptr);
    std::printf("unset -> %d\n", byDefault);
    MIB_EXPECT(byDefault == inlineThreads, "default: OpenCV runs inline on the calling thread");

    const int three = threadsAfterInitialize("3");
    std::printf("3 -> %d\n", three);
    MIB_EXPECT(three == 3, "MIB_OPENCV_THREADS=3 is passed through");

    const int invalid = threadsAfterInitialize("not-a-number");
    std::printf("not-a-number -> %d\n", invalid);
    MIB_EXPECT(invalid == inlineThreads,
               "an invalid MIB_OPENCV_THREADS falls back to the inline default");

    const int kept = threadsAfterInitialize("opencv");
    std::printf("opencv -> %d\n", kept);
    MIB_EXPECT(kept == opencvDefault, "MIB_OPENCV_THREADS=opencv keeps OpenCV's own default");

    return mib::test::exitCode();
}
