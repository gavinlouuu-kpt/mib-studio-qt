#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"

#include <cstdlib>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <string>

namespace {
void setMode(const char* value) {
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE", value ? value : "");
#else
    if (value)
        setenv("MIB_CAMERA_MODE", value, 1);
    else
        unsetenv("MIB_CAMERA_MODE");
#endif
}
} // namespace

int main() {
    const char* previous = std::getenv("MIB_CAMERA_MODE");
    const bool hadMode = previous != nullptr;
    const std::string previousMode = previous ? previous : "";
    setMode(nullptr);
    backend::AppBackend app;
    if (!app.initialize("data/test_mindvision_selection_state")) {
        SPDLOG_ERROR("AppBackend initialization failed");
        return 1;
    }

#if MIB_HAS_MINDVISION && !MIB_HAS_EGRABBER
    if (app.isCameraConfigured()) {
        SPDLOG_ERROR("Implicit SDK-free fallback must leave single-camera discovery enabled");
        return 6;
    }
#endif
    setMode("mock");
    {
        backend::AppBackend explicitMock;
        if (!explicitMock.initialize("data/test_mindvision_selection_explicit_mock") ||
            !explicitMock.isCameraConfigured()) {
            SPDLOG_ERROR("Explicit mock mode must remain configured and bypass discovery");
            return 7;
        }
    }
    setMode(hadMode ? previousMode.c_str() : nullptr);

    app.setMindVisionCameraSelection(2, "MindVision camera 2");
    if (!app.isMindVisionCameraSelected()) {
        SPDLOG_ERROR("MindVision selection state was not recorded");
        return 2;
    }

    if (!app.isCameraConfigured()) {
        SPDLOG_ERROR("MindVision selection did not mark the backend configured");
        return 3;
    }

    app.setHardwareCameraSelection(1, 4, "EGrabber camera 1/4");
    if (app.isMindVisionCameraSelected()) {
        SPDLOG_ERROR("Hardware selection did not clear the MindVision selection state");
        return 4;
    }

    app.configureMockCamera(camera::mock::MockCameraOptions{});
    if (app.isMindVisionCameraSelected()) {
        SPDLOG_ERROR("Mock selection did not clear the MindVision selection state");
        return 5;
    }

    SPDLOG_INFO("MindVision selection state test passed");
    return 0;
}
