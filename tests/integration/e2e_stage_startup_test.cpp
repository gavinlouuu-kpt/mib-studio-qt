// e2e_stage_startup_test
//
// The Z stage through a real AppBackend (#464, ADR 0013): a fake ZC300 behind
// the backend's shared serial bus. Start-up is read-only (zero writes, no
// motion); an operator Home is persisted under the data directory, so a new
// AppBackend on the same data directory starts referenced while the
// controller stays powered; AppBackend::shutdown stops a moving axis.

#include "backend/app/AppBackend.h"
#include "backend/services/SerialBus.h"
#include "backend/services/StageService.h"

#include "support/assert.h"
#include "support/fake_zc300.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

using backend::services::StageConfig;
using backend::stage::StageError;
using mib::test::FakeZc300;
using mib::test::FakeZc300Port;

namespace {

std::filesystem::path makeTempDir()
{
    std::random_device rd;
    const auto path =
        std::filesystem::temp_directory_path() / ("mib_stage_e2e_" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path);
    return path;
}

StageConfig stageConfig(const FakeZc300& device)
{
    StageConfig c;
    c.enabled = true;
    c.endpoint.systemPort = device.portName;
    c.pollMovingMs = 5;
    c.pollIdleMs = 20;
    return c;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(90);
#if defined(_WIN32)
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json", 1);
#endif
    const auto dataDir = makeTempDir();
    const auto record = dataDir / "stage_reference.json";
    FakeZc300 device;

    watchdog.mark("read-only start-up");
    {
        backend::AppBackend app;
        MIB_REQUIRE(app.initialize(dataDir.string()), "backend initializes");
        MIB_EXPECT(device.frames() == 0, "initialize does not touch the stage");
        app.serialBus().setSerialPortFactory([&device] { return std::make_unique<FakeZc300Port>(device); });
        MIB_REQUIRE(app.stage().setConfig(stageConfig(device)), "config accepted");
        MIB_REQUIRE(app.stage().startup() == StageError::None, "start-up connects");
        const auto snap = app.stage().snapshot();
        MIB_EXPECT(snap.connected && snap.configured && !snap.referenced, "connected, unreferenced");
        MIB_EXPECT(device.writes() == 0 && !device.moving(), "start-up: zero writes, no motion");

        const auto home = app.stage().reference();
        MIB_REQUIRE(home.accepted() && app.stage().waitForOperation(home.id, std::chrono::seconds(10)),
                    "operator Home");
        MIB_EXPECT(app.stage().snapshot().referenced, "referenced");
        app.shutdown();
        MIB_EXPECT(std::filesystem::exists(record), "reference record persisted in the data directory");
    }

    watchdog.mark("restart keeps the reference");
    {
        backend::AppBackend app;
        MIB_REQUIRE(app.initialize(dataDir.string()), "second backend initializes");
        app.serialBus().setSerialPortFactory([&device] { return std::make_unique<FakeZc300Port>(device); });
        app.stage().setConfig(stageConfig(device));
        const int writes = device.writes();
        MIB_REQUIRE(app.stage().startup() == StageError::None, "start-up");
        MIB_EXPECT(app.stage().snapshot().referenced, "same power-up: still referenced");
        MIB_EXPECT(device.writes() == writes, "restart start-up wrote nothing");

        device.setPulsesPerSecond(2000);
        const auto move = app.stage().moveTo(2000);
        MIB_REQUIRE(move.accepted(), "slow move");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        app.shutdown(); // must stop the axis before releasing the bus
        MIB_EXPECT(!device.moving(), "AppBackend::shutdown stopped the axis");
    }

    std::error_code ec;
    std::filesystem::remove_all(dataDir, ec);
    if (mib::test::exitCode() == 0) std::printf("stage start-up e2e verified\n");
    return mib::test::exitCode();
}
