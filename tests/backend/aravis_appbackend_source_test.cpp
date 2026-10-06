#include "backend/app/AppBackend.h"
#include "backend/services/CaptureService.h"
#include "backend/playback/FrameStore.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <cstdlib>
#include <chrono>
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
}

int main()
{
    mib::test::TempDir temp("aravis_appbackend");
    setEnv("MIB_CAMERA_MODE", "aravis");
    setEnv("MIB_ARAVIS_FAKE", "1");
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,yolo,autofocus,trigger,playback");

    backend::AppBackend app;
    MIB_REQUIRE(app.initialize(temp.path().string()), "AppBackend initializes in explicit Aravis mode");
    const auto info = app.cameraSourceInfo();
    MIB_EXPECT(info.requested == "aravis", "requested source remains Aravis");
#if MIB_HAS_ARAVIS
    MIB_EXPECT(info.effective == "aravis", "enabled build selects Aravis");
    MIB_EXPECT(info.simulated && !info.fallback, "enabled Fake source is truthful");
    MIB_EXPECT(app.isCameraConfigured(), "enabled Aravis source is configured");
    MIB_REQUIRE(app.capture().start(), "enabled Aravis capture is accepted");
    MIB_REQUIRE(app.capture().waitForState({backend::services::CaptureLifecycleState::Running,
                                            backend::services::CaptureLifecycleState::Faulted},
                                           std::chrono::seconds(5)) ==
                    backend::services::CaptureLifecycleState::Running,
                "enabled Aravis capture reaches Running");
    bool committed = false;
    const auto frameDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < frameDeadline) {
        if (app.getFrameStore()->committedCount() > 0) {
            committed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MIB_REQUIRE(committed, "enabled Aravis capture commits frames to the application FrameStore");
    app.capture().stop();
#else
    MIB_EXPECT(info.effective == "unavailable", "disabled build does not substitute another source");
    MIB_EXPECT(info.fallback && !info.fallbackReason.empty(), "disabled Aravis request is actionable");
    MIB_EXPECT(app.isCameraConfigured(), "explicit disabled source remains configured for readiness");
    MIB_REQUIRE(app.capture().start(), "disabled Aravis capture request is accepted for fault reporting");
    MIB_EXPECT(app.capture().waitForState({backend::services::CaptureLifecycleState::Faulted},
                                          std::chrono::seconds(5)) ==
                    backend::services::CaptureLifecycleState::Faulted,
                "disabled Aravis capture faults instead of substituting Mock");
    MIB_EXPECT(app.getFrameStore()->committedCount() == 0,
               "disabled Aravis capture publishes no substitute frames");
#endif
    app.shutdown();
    return mib::test::exitCode();
}
