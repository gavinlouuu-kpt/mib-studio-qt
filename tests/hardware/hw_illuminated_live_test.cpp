// hw_illuminated_live_test  (LABEL: hardware) — runs only against the real
// XGC/R5D rig (issue #413). It drives the complete one-click path through
// AppBackend exactly as the app does: MindVision selection at boot, saved
// profile with `live_view` enabled, automatic generator discovery, coordinated
// start, frames, coordinated stop, and a fresh read-only generator readback
// proving the trigger train is gated off. Then it restarts to prove the cycle
// repeats. Bounded: MIB_TEST_RUN_SECONDS per run (default 5), MIB_TEST_RESTARTS
// extra cycles (default 1), watchdog on top.
//
// Env:
//   MIB_TEST_ILLUMINATED_LIVE=1              enable (skips otherwise)
//   MIB_CAMERA_MODE=mindvision               select the MindVision camera at boot
//   MIB_MINDVISION_CONFIG=<profile.json>     profile with live_view.enabled=true
//   MIB_MINDVISION_CAMERA_INDEX=<n>          optional, default 0
//
// This is lifecycle/discovery evidence with SDK and Modbus readback. It is not
// a timing measurement: only the oscilloscope establishes physical LED timing.

#include "backend/app/AppBackend.h"
#include "backend/camera/mindvision/MindVisionConfig.h"
#include "backend/playback/FrameStore.h"
#include "backend/services/CaptureService.h"
#include "backend/services/PulseGeneratorService.h"

#include "support/assert.h"
#include "support/hardware.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {
using backend::services::CaptureFailureKind;
using backend::services::CaptureLifecycleState;

void printGenerator(const backend::services::PulseGeneratorService& gen, const char* when)
{
    const auto cfg = gen.getConfig();
    const auto st = gen.getStatus();
    std::printf("[%s] generator port=%s addr=%u connected=%d owned=%d\n", when,
                cfg.portName.c_str(), static_cast<unsigned>(cfg.modbusAddress),
                st.connected ? 1 : 0, gen.liveViewOwned() ? 1 : 0);
    for (int ch = 0; ch < backend::services::PulseGeneratorService::CHANNEL_COUNT; ++ch) {
        const auto& c = st.channels[static_cast<size_t>(ch)];
        std::printf("[%s]   ch%d: %.2f Hz  duty %.2f %%  output %s\n", when, ch + 1, c.frequencyHz,
                    c.dutyPercent, c.outputEnabled ? "ON" : "off");
    }
    std::fflush(stdout);
}
} // namespace

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    mib::test::requireDeviceEnv("MIB_TEST_ILLUMINATED_LIVE");
    const std::string profilePath = mib::test::requireDeviceEnv("MIB_MINDVISION_CONFIG");
    const bool testModes = mib::test::envInt("MIB_TEST_OVERVIEW_MODES", 0) != 0;
    const int runSeconds = mib::test::envInt("MIB_TEST_RUN_SECONDS", 5);
    const int restarts = mib::test::envInt("MIB_TEST_RESTARTS", 1);
    mib::test::Watchdog wd(60 + (runSeconds + 30) * (restarts + 1));

    std::ifstream profileFile(profilePath, std::ios::binary);
    const auto profile = backend::camera::mindvision::parseConfig(
        std::string(std::istreambuf_iterator<char>(profileFile), {}));
    MIB_REQUIRE(profile.ok, ("profile parses: " + profile.error).c_str());
    MIB_REQUIRE(profile.config.illuminatedLive, "profile enables illuminated Live View");
    const int channel = profile.config.liveView.channel - 1;
    std::printf("profile %s: port=%s channel=%d %.1f Hz duty %.2f %% exposure %.1f us strobe %d us\n",
                profilePath.c_str(), profile.config.liveView.port.c_str(), channel + 1,
                profile.config.liveView.frequencyHz, profile.config.liveView.dutyPercent,
                profile.config.exposureUs, profile.config.strobePulseUs);

    mib::test::TempDir td("mib_hw_illuminated_live");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td / "data").string()), "AppBackend initialize");
    MIB_REQUIRE(backend.isCameraConfigured(),
                "MIB_CAMERA_MODE=mindvision selects the MindVision camera at boot");
    auto& cap = backend.capture();
    auto& gen = backend.pulseGenerator();
    MIB_REQUIRE(!gen.liveViewOwned(), "generator not owned before the first start");

    for (int run = 0; run <= restarts; ++run) {
        const bool overview = testModes && run % 2 == 0;
        if (testModes) {
            std::string error;
            MIB_REQUIRE(backend.setMindVisionOverview(overview, &error), error.c_str());
            MIB_EXPECT(!cap.isRunning(), "mode staging stays idle until explicit start");
        }
        wd.mark(run == 0 ? "first start" : "restart");
        std::printf("=== run %d: start ===\n", run + 1);
        std::fflush(stdout);
        MIB_REQUIRE(cap.start(), "capture start accepted");
        const auto state = cap.waitForState(
            {CaptureLifecycleState::Running, CaptureLifecycleState::Faulted,
             CaptureLifecycleState::Idle},
            std::chrono::seconds(30));
        const auto started = cap.lifecycleSnapshot();
        std::printf("[run %d] state=%s failure=%s \"%s\"\n", run + 1,
                    backend::services::toString(state),
                    backend::services::toString(started.lastFailure),
                    started.lastFailureMessage.c_str());
        printGenerator(gen, "started");
        MIB_REQUIRE(state == CaptureLifecycleState::Running,
                    "coordinated start reaches Running (see failure above)");
        MIB_EXPECT(gen.liveViewOwned(), "generator owned by the capture session while running");
        MIB_EXPECT(gen.getStatus().channels[static_cast<size_t>(channel)].outputEnabled,
                   "requested channel enabled after camera armed");

        if (testModes) {
            const auto actual = gen.getStatus().channels[static_cast<size_t>(channel)];
            const auto expected = overview
                                      ? backend::camera::mindvision::overviewConfig(profile.config)
                                      : profile.config;
            MIB_EXPECT(std::abs(actual.frequencyHz - expected.liveView.frequencyHz) < 0.01,
                       "mode trigger frequency verified");
            MIB_EXPECT(std::abs(actual.dutyPercent - expected.liveView.dutyPercent) < 0.01,
                       "mode pulse width preserved");
        }
        const uint64_t framesAtStart = cap.stats().framesProcessed.load();
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(runSeconds)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t frames = cap.stats().framesProcessed.load() - framesAtStart;
        const auto snapshot = cap.lifecycleSnapshot();
        // Supplementary evidence only: a live frame's mean grey level says
        // whether the LED is lighting the sensor (bench: ~150/255 lit, ~5 dark).
        double meanGrey = -1.0;
        if (auto store = backend.getFrameStore()) {
            backend::playback::Frame frame;
            if (store->getLatest(frame) && !frame.data.empty()) {
                if (testModes) {
                    const auto sensor = backend.mindVisionSensor();
                    const int expectedW = overview ? sensor.sensorWidth : profile.config.width;
                    const int expectedH = overview ? sensor.sensorHeight : profile.config.height;
                    std::printf("[mode] %s actual=%llux%llu expected=%dx%d ROI=%d,%d buffer=%zu\n",
                                overview ? "Overview" : "Experiment", frame.width, frame.height,
                                expectedW, expectedH, overview ? 0 : profile.config.offsetX,
                                overview ? 0 : profile.config.offsetY, store->capacity());
                    MIB_EXPECT(frame.width == expectedW && frame.height == expectedH,
                               "mode frame geometry");
                }
                unsigned long long sum = 0;
                for (const auto v : frame.data) sum += v;
                meanGrey = static_cast<double>(sum) / static_cast<double>(frame.data.size());
            }
        }
        std::printf("[run %d] %.2f s: %llu frames (%.1f frames/s host count, backend rate %llu), "
                    "transport lost %llu, discarded %llu, latest frame mean grey %.1f/255, "
                    "state=%s failure=%s \"%s\"\n",
                    run + 1, elapsed, static_cast<unsigned long long>(frames), frames / elapsed,
                    static_cast<unsigned long long>(cap.stats().lastFrameRate.load()),
                    static_cast<unsigned long long>(cap.stats().transportLostFrames.load()),
                    static_cast<unsigned long long>(
                        cap.stats().intentionallyDiscardedFrames.load()),
                    meanGrey, backend::services::toString(snapshot.state),
                    backend::services::toString(snapshot.lastFailure),
                    snapshot.lastFailureMessage.c_str());
        std::fflush(stdout);
        MIB_EXPECT(snapshot.state == CaptureLifecycleState::Running, "session stayed Running");
        MIB_EXPECT(frames > 0, "camera delivered frames from the generator trigger train");

        wd.mark("stop");
        cap.stop();
        const auto stopped = cap.lifecycleSnapshot();
        std::printf("[run %d] after stop: state=%s failure=%s \"%s\"\n", run + 1,
                    backend::services::toString(stopped.state),
                    backend::services::toString(stopped.lastFailure),
                    stopped.lastFailureMessage.c_str());
        MIB_EXPECT(stopped.state == CaptureLifecycleState::Idle, "explicit stop ends Idle");
        MIB_EXPECT(stopped.lastFailure != CaptureFailureKind::ShutdownFailed,
                   "generator and LED OFF confirmed by the session");
        MIB_EXPECT(!gen.liveViewOwned(), "generator released after stop");
        printGenerator(gen, "stopped");

        // Independent readback: a fresh connect re-reads every channel register
        // from the module (never writes). The requested channel must read duty 0.
        const auto cfg = gen.getConfig();
        MIB_REQUIRE(!cfg.portName.empty() && cfg.portName != "auto",
                    "discovery resolved a concrete port");
        MIB_REQUIRE(gen.connect(cfg.portName, cfg.serial, cfg.modbusAddress),
                    "read-only reconnect to the discovered generator");
        printGenerator(gen, "readback");
        const auto readback = gen.getStatus().channels[static_cast<size_t>(channel)];
        MIB_EXPECT(!readback.outputEnabled && readback.dutyPercent == 0.0,
                   "hardware readback: requested channel duty is 0 (trigger train off)");
        gen.disconnect();
        MIB_EXPECT(!gen.isConnected(), "manual connection released for the next run");
    }

    backend.shutdown();
    if (mib::test::exitCode() == 0) std::printf("illuminated live hardware cycle OK\n");
    return mib::test::exitCode();
}
