// Camera & Alignment for an Aravis camera (YOFO Studio, ABI 20), against the Aravis Fake device:
// the overview shows the whole sensor with processing off and a small frame store, the window
// saved on it persists in the Aravis profile and is what Experiment mode acquires, guards and
// the readiness gate hold.
#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/playback/FrameStore.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// Start capture and return the geometry of the first frame it commits.
bool frameGeometry(backend::AppBackend& app, uint64_t& width, uint64_t& height)
{
    if (!app.capture().isRunning() && !app.capture().start()) return false;
    const auto store = app.getFrameStore();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        backend::playback::Frame frame;
        if (store->committedCount() > 0 && store->getLatest(frame)) {
            width = frame.width;
            height = frame.height;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

int main()
{
#if MIB_HAS_ARAVIS
    mib::test::TempDir temp("aravis_overview");
    setEnv("MIB_CAMERA_MODE", "aravis");
    setEnv("MIB_ARAVIS_FAKE", "1");
    setEnv("MIB_ARAVIS_REGION", nullptr);
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");

    backend::AppBackend app;
    MIB_REQUIRE(app.initialize(temp.path().string()), "AppBackend initializes with the Aravis Fake camera");
    auto geometry = app.cameraGeometry();
    MIB_EXPECT(geometry.supported && geometry.camera == "aravis", "an Aravis camera has an overview");
    MIB_EXPECT(!geometry.overview && !app.isCameraOverview(), "experiment mode by default");
    MIB_EXPECT(geometry.sensorWidth == 0, "the sensor size is unknown before the first start");
    std::string error;
    MIB_EXPECT(!app.saveCameraRoi(0, 0, 64, 64, &error) && error.find("sensor size") != std::string::npos,
               "a window cannot be saved before the sensor size is known");

    uint64_t width = 0, height = 0;
    MIB_REQUIRE(frameGeometry(app, width, height), "experiment-mode capture delivers");
    geometry = app.cameraGeometry();
    MIB_REQUIRE(geometry.sensorWidth > 0 && geometry.sensorHeight > 0, "the read-back reports the sensor size");
    const auto experimentCapacity = app.getFrameStore()->capacity();

    // Overview: whole sensor, processing off, 8-frame store; the caller restarts capture.
    MIB_REQUIRE(app.setCameraOverview(true, &error), "overview is accepted while idle");
    MIB_EXPECT(app.isCameraOverview() && !app.capture().isRunning(), "overview stops capture");
    MIB_EXPECT(app.getFrameStore()->capacity() == 8, "overview uses a small frame store");
    MIB_REQUIRE(frameGeometry(app, width, height), "overview capture delivers");
    MIB_EXPECT(static_cast<int>(width) == geometry.sensorWidth && static_cast<int>(height) == geometry.sensorHeight,
               "overview frames are the whole sensor");
    const auto readiness = app.experiment().evaluateReadiness(temp.path().string());
    bool modeGate = false;
    for (const auto& gate : readiness.gates)
        if (gate.id == "camera.mode") modeGate = true;
    MIB_EXPECT(modeGate && !readiness.ready, "an experiment cannot start from the overview");

    // The window placed on the overview is bounds- and step-checked and persisted.
    MIB_EXPECT(!app.saveCameraRoi(geometry.sensorWidth - 8, 0, 64, 64, &error), "a window off the sensor is refused");
    MIB_REQUIRE(app.saveCameraRoi(16, 8, 256, 128, &error), "a window on the sensor is saved");
    const auto profile = temp.path() / "config" / "aravis-camera.json";
    MIB_EXPECT(std::filesystem::exists(profile), "the Aravis profile is written");
    geometry = app.cameraGeometry();
    MIB_EXPECT(geometry.roiX == 16 && geometry.roiY == 8 && geometry.roiWidth == 256 && geometry.roiHeight == 128,
               "geometry reports the saved window");
    MIB_REQUIRE(frameGeometry(app, width, height) && static_cast<int>(width) == geometry.sensorWidth,
                "saving does not change the live overview");

    // Experiment mode acquires the saved window and restores the frame store.
    app.capture().stop();
    MIB_REQUIRE(app.setCameraOverview(false, &error), "experiment mode is accepted");
    MIB_EXPECT(app.getFrameStore()->capacity() == experimentCapacity, "experiment frame store restored");
    MIB_REQUIRE(frameGeometry(app, width, height), "experiment capture delivers");
    MIB_EXPECT(width == 256 && height == 128, "experiment frames are the saved window");
    app.capture().stop();
    app.shutdown();

    // A new session reads the window back from the profile.
    backend::AppBackend again;
    MIB_REQUIRE(again.initialize(temp.path().string()), "AppBackend re-initializes");
    geometry = again.cameraGeometry();
    MIB_EXPECT(geometry.roiWidth == 256 && geometry.roiHeight == 128, "the saved window survives a restart");
    MIB_REQUIRE(frameGeometry(again, width, height) && width == 256 && height == 128,
                "the saved window is acquired after a restart");
    again.capture().stop();
    again.shutdown();
#endif
    return mib::test::exitCode();
}
