// zc300_stage_test
//
// Zc300Stage driver over the real SerialBusManager / ModbusBusSession, with a
// fake ZC300 behind the port factory (#464, ADR 0013). Each wire-level rule of
// the execution plan has a check here:
//  - connect is observe-only: zero writes, even for a misconfigured controller;
//  - motion is refused (nothing on the wire) when misconfigured or off-grid;
//  - motion opcodes go out exactly once; a lost reply is reconciled from status;
//  - reads survive the dropped-request-after-move quirk; stop is retried;
//  - the save acknowledges after ~1.1 s and applyProfile still succeeds;
//  - vendor exception codes map to stage errors;
//  - disconnect stops a moving axis and writes nothing when idle.

#include "backend/services/SerialBus.h"
#include "backend/stage/zc300/Zc300Stage.h"

#include "support/assert.h"
#include "support/fake_zc300.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace backend::stage;
using backend::services::serialbus::SerialBusManager;
using mib::test::FakeZc300;
using mib::test::FakeZc300Port;

namespace {

constexpr double kUmPerPulse = 0.4375;

void useFake(SerialBusManager& manager, FakeZc300& device)
{
    manager.setSerialPortFactory([&device] { return std::make_unique<FakeZc300Port>(device); });
}

StageEndpoint endpointFor(const FakeZc300& device)
{
    StageEndpoint e;
    e.systemPort = device.portName;
    return e;
}

bool waitIdle(IMotionStage& stage, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        StageStatus s;
        if (stage.readStatus(s) == StageError::None && s.state == MoveState::Idle) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

StageError connectTo(zc300::Zc300Stage& stage, FakeZc300& device, StageIdentity& id)
{
    std::string detail;
    const StageError err = stage.connect(endpointFor(device), tbzf6_60Profile(), id, detail);
    if (err != StageError::None) std::printf("connect: %s (%s)\n", toString(err), detail.c_str());
    return err;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(60);

    // --- connect is observe-only ---------------------------------------------
    watchdog.mark("connect read-only");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connects to the fake ZC300");
        MIB_EXPECT(id.model == "ZC300-1A" && id.serial == "26017" && id.firmware == "1.2",
                   "identity read over FC04");
        MIB_EXPECT(stage.isConfigured(), "TBZF6-60 configuration recognised");
        MIB_EXPECT(std::abs(stage.calibration().umPerPulse - kUmPerPulse) < 1e-6, "calibration from controller");
        MIB_EXPECT(device.writes() == 0, "connect wrote nothing");
        stage.disconnect();
        MIB_EXPECT(device.writes() == 0, "idle disconnect wrote nothing");
    }

    watchdog.mark("misconfigured and wrong device");
    {
        FakeZc300 device(FakeZc300::Config{0, 0, 4.0f, 1600}); // factory: pulses, lead 4
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "misconfigured controller still connects");
        MIB_EXPECT(!stage.isConfigured(), "factory configuration is flagged");
        MIB_EXPECT(stage.moveAbsolute(100) == StageError::Misconfigured, "move refused");
        MIB_EXPECT(stage.jog(Direction::Positive) == StageError::Misconfigured, "jog refused");
        MIB_EXPECT(stage.setPosition(0) == StageError::Misconfigured, "setPosition refused");
        MIB_EXPECT(device.writes() == 0, "nothing reached the wire");
        MIB_EXPECT(stage.stop() == StageError::None, "stop is always allowed");

        FakeZc300 other;
        other.model = "DLSP501";
        SerialBusManager manager2;
        useFake(manager2, other);
        zc300::Zc300Stage wrong(manager2);
        MIB_EXPECT(connectTo(wrong, other, id) == StageError::WrongDevice, "non-ZC300 model refused");
        MIB_EXPECT(!wrong.isConnected() && other.writes() == 0, "wrong device: disconnected, no writes");
    }

    // --- motion: whole micrometres, exact absolute moves ---------------------
    watchdog.mark("absolute and relative moves");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");

        MIB_EXPECT(stage.moveAbsolute(12.5) == StageError::OffGrid, "12.5 um refused");
        MIB_EXPECT(device.writes() == 0, "off-grid target wrote nothing");

        MIB_EXPECT(stage.moveAbsolute(100) == StageError::None, "move to 100 um accepted");
        MIB_EXPECT(waitIdle(stage), "move completes");
        MIB_EXPECT(device.positionPulses() == 229, "100 um -> 229 pulses (0.1 mm, rounded)");
        StageStatus s;
        MIB_EXPECT(stage.readStatus(s) == StageError::None && std::abs(s.positionUm - 229 * kUmPerPulse) < 0.01,
                   "readback reports 100.19 um, finer than the command grid");
        MIB_EXPECT(!s.referenced, "drivers never claim a reference");

        MIB_EXPECT(stage.moveRelative(4) == StageError::None, "relative 4 um");
        MIB_EXPECT(waitIdle(stage), "relative move completes");
        MIB_EXPECT(device.positionPulses() == 229 + 9, "4 um -> 9 pulses (bench observation)");

        MIB_EXPECT(stage.moveAbsolute(-20) == StageError::None, "negative absolute target");
        MIB_EXPECT(waitIdle(stage), "completes");
        MIB_EXPECT(device.positionPulses() == -46, "-20 um -> -46 pulses (N direction)");
        MIB_EXPECT(stage.moveAbsolute(0) == StageError::None && waitIdle(stage) && device.positionPulses() == 0,
                   "absolute return to 0 is pulse-exact");

        const int opcodesBefore = device.opcodes();
        MIB_EXPECT(stage.moveRelative(0) == StageError::None && device.opcodes() == opcodesBefore,
                   "zero relative move sends nothing");

        MIB_EXPECT(stage.setPosition(0) == StageError::None, "setPosition writes the coordinate");
        MIB_EXPECT(stage.setSpeed(1000, 2000) == StageError::None, "speed in um/s accepted");
        MIB_EXPECT(stage.setSpeed(0, 2000) == StageError::InvalidArgument, "zero speed refused");
    }

    // --- controller refusals map to stage errors ------------------------------
    watchdog.mark("exceptions");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");

        device.setEmergencyStop(true);
        MIB_EXPECT(stage.moveRelative(10) == StageError::EmergencyStop, "e-stop -> EmergencyStop");
        StageStatus s;
        MIB_EXPECT(stage.readStatus(s) == StageError::None && s.emergencyStop, "e-stop visible in status");
        device.setEmergencyStop(false);

        device.setEnabled(false);
        MIB_EXPECT(stage.moveRelative(10) == StageError::NotEnabled, "disabled axis -> NotEnabled");
        device.setEnabled(true);

        device.setPositionPulses(-6858); // on the negative limit
        MIB_EXPECT(stage.jog(Direction::Negative) == StageError::LimitSwitch, "jog into active limit refused");
        MIB_EXPECT(stage.readStatus(s) == StageError::None && s.limitNegative, "negative limit visible");
        device.setPositionPulses(0);

        device.setPulsesPerSecond(2000); // slow, so the axis is still moving
        MIB_EXPECT(stage.moveRelative(100) == StageError::None, "slow move starts");
        MIB_EXPECT(stage.moveRelative(100) == StageError::Busy, "second move while moving -> Busy");
        MIB_EXPECT(stage.stop() == StageError::None && !device.moving(), "stop halts the axis");

        device.setDriverAlarm(true);
        MIB_EXPECT(stage.readStatus(s) == StageError::None && s.driverAlarm && s.state == MoveState::Faulted,
                   "driver alarm -> Faulted");
        device.setDriverAlarm(false);
    }

    // --- quirks: dropped request after a move, lost acks, slow save ----------
    watchdog.mark("dropped request after move");
    {
        FakeZc300 device;
        device.setDropAfterMove(true);
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage::Timing fast;
        fast.transactionMs = 150;
        zc300::Zc300Stage stage(manager, fast);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        MIB_EXPECT(stage.moveRelative(50) == StageError::None, "move");
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // move finishes, drop armed
        StageStatus s;
        MIB_EXPECT(stage.readStatus(s) == StageError::None, "status read survives the dropped request");
        MIB_EXPECT(device.droppedRequests() == 1, "exactly one request was dropped");
    }

    watchdog.mark("lost motion ack");
    {
        FakeZc300 device;
        device.setPulsesPerSecond(2000);
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage::Timing fast;
        fast.transactionMs = 150;
        zc300::Zc300Stage stage(manager, fast);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");

        device.dropNextMotionAck(); // executes, never replies
        MIB_EXPECT(stage.moveRelative(200) == StageError::None, "lost ack, axis moving -> accepted");
        MIB_EXPECT(device.opcodeCount(0x65) == 1, "the move went out exactly once");
        MIB_EXPECT(stage.stop() == StageError::None, "stop");

        device.swallowNextMotion(); // never executed, never replied
        MIB_EXPECT(stage.moveRelative(200) == StageError::LostAck, "lost ack, axis idle -> LostAck");
        MIB_EXPECT(device.opcodeCount(0x65) == 2 && !device.moving(), "not re-sent, nothing moving");

        MIB_EXPECT(stage.moveRelative(200) == StageError::None, "next move works");
        device.dropNextStopAck();
        MIB_EXPECT(stage.stop() == StageError::None, "stop succeeds after a lost reply");
        MIB_EXPECT(device.opcodeCount(0x68) == 3, "stop is idempotent and was retried once");
    }

    watchdog.mark("apply profile with slow save");
    {
        FakeZc300 device(FakeZc300::Config{0, 0, 4.0f, 1600});
        device.setSaveDelayMs(1100); // bench: ~1.06 s
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        MIB_EXPECT(!stage.isConfigured(), "starts misconfigured");
        const auto t0 = std::chrono::steady_clock::now();
        MIB_EXPECT(stage.applyProfile(tbzf6_60Profile()) == StageError::None, "profile applied and saved");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(ms >= 1000, "waited for the slow save acknowledgement");
        MIB_EXPECT(stage.isConfigured(), "configured after apply");
        MIB_EXPECT(device.saves() == 1, "saved exactly once (no retry needed)");
        const auto flash = device.flash();
        MIB_EXPECT(flash.unit == 1 && flash.pulsesPerRev == 1600 && std::abs(flash.leadMm - 0.7f) < 1e-6,
                   "profile persisted to flash");

        device.setPulsesPerSecond(2000);
        MIB_EXPECT(stage.moveRelative(200) == StageError::None, "move");
        MIB_EXPECT(stage.applyProfile(tbzf6_60Profile()) == StageError::Busy, "apply refused while moving");
        stage.stop();
    }

    // --- power-up token and disconnect ---------------------------------------
    watchdog.mark("power-up token");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        MIB_EXPECT(stage.writePowerUpToken(0xBEEF) == StageError::None, "token written");
        stage.disconnect();
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "reconnect");
        std::uint16_t token = 0;
        MIB_EXPECT(stage.readPowerUpToken(token) == StageError::None && token == 0xBEEF,
                   "token survives an application reconnect");
        stage.disconnect();
        device.powerCycle();
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "reconnect after power cycle");
        MIB_EXPECT(stage.readPowerUpToken(token) == StageError::None && token == 0,
                   "power cycle clears the token");
    }

    watchdog.mark("disconnect while moving");
    {
        FakeZc300 device;
        device.setPulsesPerSecond(1000);
        SerialBusManager manager;
        useFake(manager, device);
        auto stage = createStage(StageKind::Zc300, manager);
        StageIdentity id;
        std::string detail;
        MIB_REQUIRE(stage->connect(endpointFor(device), tbzf6_60Profile(), id, detail) == StageError::None,
                    "connect via factory");
        MIB_EXPECT(stage->jog(Direction::Positive) == StageError::None, "jog");
        stage->disconnect();
        MIB_EXPECT(!device.moving() && device.opcodeCount(0x68) == 1, "disconnect stopped the moving axis");
    }

    // --- concurrent pollers and commands (TSan lane) ------------------------
    watchdog.mark("concurrency");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        std::atomic<bool> done{false};
        std::atomic<int> pollErrors{0};
        std::thread poller([&] {
            while (!done.load()) {
                StageStatus s;
                if (stage.readStatus(s) != StageError::None) pollErrors.fetch_add(1);
            }
        });
        int moveErrors = 0;
        for (int i = 0; i < 20; ++i) {
            if (stage.moveAbsolute(i % 2 ? 50 : 0) != StageError::None) ++moveErrors;
            if (!waitIdle(stage)) ++moveErrors;
        }
        done.store(true);
        poller.join();
        MIB_EXPECT(moveErrors == 0 && pollErrors.load() == 0, "20 moves with a concurrent poller, no errors");
        MIB_EXPECT(device.positionPulses() == 114, "ends at 50 um (114 pulses)");
    }

    if (mib::test::exitCode() == 0) std::printf("ZC300 stage driver verified\n");
    return mib::test::exitCode();
}
