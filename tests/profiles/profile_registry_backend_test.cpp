// Central registry inside AppBackend (#398 M1): the registry is configured by
// environment + a shell-injected transport, a hung/unreachable registry never
// touches a running mock capture, and AppBackend::shutdown() aborts the
// in-flight registry request instead of waiting it out.
#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace std::chrono_literals;

namespace {
void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

template <class Pred> bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}
} // namespace

int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir scratch("mib_registry_backend");
    const auto frames = scratch / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 64, 64), "synthesize mock frames");

    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");

    watchdog.mark("unconfigured");
    {
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((scratch / "data-off").string()), "initialize");
        MIB_EXPECT(!backend.profileRegistry().snapshot().configured,
                   "no registry env: worker inert");
        MIB_EXPECT(backend.profileRegistry().requestRefresh() == 0, "inert worker refuses");
        backend.shutdown();
    }

    setEnv("MIB_PROFILE_REGISTRY_URL", "https://registry.example");
    setEnv("MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY", "sb_publishable_test");

    watchdog.mark("configured without transport");
    {
        backend::AppBackend backend;
        MIB_REQUIRE(backend.initialize((scratch / "data-notransport").string()), "initialize");
        MIB_EXPECT(!backend.profileRegistry().snapshot().configured,
                   "no shell transport: worker disabled, not crashing");
        backend.shutdown();
    }

    watchdog.mark("hung registry during capture");
    std::atomic<int> hung{0};
    {
        backend::AppBackend backend;
        backend.setProfileRegistryTransport([&](const backend::profiles::RegistryHttpRequest& r) {
            ++hung;
            while (!(r.cancelled && r.cancelled()))
                std::this_thread::sleep_for(5ms);
            return backend::profiles::RegistryHttpResponse{0, {}};
        });
        MIB_REQUIRE(backend.initialize((scratch / "data").string()), "initialize");
        MIB_REQUIRE(backend.profileRegistry().snapshot().configured, "registry enabled");

        camera::mock::MockCameraOptions options;
        options.folder = frames;
        options.frameInterval = std::chrono::microseconds(1000);
        options.loopFiles = true;
        backend.configureMockCamera(options);
        MIB_REQUIRE(backend.capture().start(), "mock capture start");
        MIB_REQUIRE(
            waitFor([&] { return backend.capture().stats().framesProcessed.load() > 0; }, 5s),
            "frames flowing");

        const auto signIn = backend.profileRegistry().requestSignIn("bob@lab", "pw");
        MIB_REQUIRE(signIn != 0, "sign-in queued");
        MIB_REQUIRE(waitFor([&] { return hung.load() > 0; }, 5s), "registry request in flight");
        const auto before = backend.capture().stats().framesProcessed.load();
        std::this_thread::sleep_for(200ms);
        MIB_EXPECT(backend.capture().isRunning() &&
                       backend.capture().stats().framesProcessed.load() > before,
                   "capture keeps running while the registry hangs");
        MIB_EXPECT(backend.profileRegistry().snapshot().busy,
                   "registry snapshot answers while its request hangs");

        watchdog.mark("shutdown aborts registry request");
        const auto started = std::chrono::steady_clock::now();
        backend.shutdown();
        const auto took = std::chrono::steady_clock::now() - started;
        MIB_EXPECT(took < 8s, "shutdown did not wait out the registry request timeout");
        MIB_EXPECT(backend.profileRegistry().job(signIn).state ==
                       backend::profiles::RegistryJobState::Cancelled,
                   "in-flight sign-in cancelled by shutdown");
    }
    return mib::test::exitCode();
}
