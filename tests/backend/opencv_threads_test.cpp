// AppBackend::initialize turns OpenCV's own parallel_for off unless
// MIB_OPENCV_THREADS says otherwise. On Windows the Conan OpenCV parallelises
// through the Concurrency Runtime, whose idle workers spin: a per-frame
// parallel call in the realtime path kept ~31 workers busy and halved
// experiment throughput at 5000 fps on the rig (2026-10-01).
#include "backend/app/AppBackend.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <opencv2/core.hpp>

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

// Boots a mock-camera backend with the given MIB_OPENCV_THREADS (nullptr =
// unset) and returns cv::getNumThreads() after initialize.
int threadsAfterInitialize(const char* setting) {
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
    cv::setNumThreads(-1); // OpenCV's own default
    const int opencvDefault = cv::getNumThreads();

    MIB_EXPECT(threadsAfterInitialize(nullptr) == 1,
               "default: OpenCV runs inline on the calling thread");

    MIB_EXPECT(threadsAfterInitialize("3") == 3, "MIB_OPENCV_THREADS=3 is passed through");

    MIB_EXPECT(threadsAfterInitialize("not-a-number") == 1,
               "an invalid MIB_OPENCV_THREADS falls back to the inline default");

    cv::setNumThreads(-1);
    MIB_EXPECT(threadsAfterInitialize("opencv") == opencvDefault,
               "MIB_OPENCV_THREADS=opencv keeps OpenCV's own default");

    return mib::test::exitCode();
}
