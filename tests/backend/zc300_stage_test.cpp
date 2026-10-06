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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
        MIB_EXPECT(!s.zeroSet, "drivers never claim a zero");

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
    // Status pollers that re-lock the driver back to back must not starve a
    // command: PR #511's TSan lane stalled 60 s here with one tight poller.
    // Every command must get through within a bound, and so must Stop.
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
        std::atomic<long long> worstPollMs{0};
        std::vector<std::thread> pollers;
        for (int p = 0; p < 3; ++p) {
            pollers.emplace_back([&] {
                while (!done.load()) { // deliberately no pause between polls
                    StageStatus s;
                    const auto t0 = std::chrono::steady_clock::now();
                    if (stage.readStatus(s) != StageError::None) pollErrors.fetch_add(1);
                    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                    long long seen = worstPollMs.load();
                    while (ms > seen && !worstPollMs.compare_exchange_weak(seen, ms)) {}
                }
            });
        }
        using Ms = std::chrono::milliseconds;
        const auto elapsed = [](std::chrono::steady_clock::time_point t0) {
            return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - t0).count();
        };
        const auto stepStart = std::chrono::steady_clock::now();
        int moveErrors = 0;
        long long slowestCommandMs = 0;
        for (int i = 0; i < 20; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            if (stage.moveAbsolute(i % 2 ? 50 : 0) != StageError::None) ++moveErrors;
            slowestCommandMs = std::max<long long>(slowestCommandMs, elapsed(t0));
            if (!waitIdle(stage)) ++moveErrors;
        }
        device.setPulsesPerSecond(2000);
        MIB_EXPECT(stage.moveAbsolute(2000) == StageError::None, "slow move under pollers");
        const auto t0 = std::chrono::steady_clock::now();
        MIB_EXPECT(stage.stop() == StageError::None, "stop under pollers");
        const long long stopMs = elapsed(t0);
        done.store(true);
        for (auto& t : pollers) t.join();
        std::printf("concurrency: slowest command %lld ms, stop %lld ms, step %lld ms\n", slowestCommandMs, stopMs,
                    static_cast<long long>(elapsed(stepStart)));
        MIB_EXPECT(moveErrors == 0 && pollErrors.load() == 0, "20 moves with three tight pollers, no errors");
        MIB_EXPECT(worstPollMs.load() < 2000, "no status poll waited 2 s or more behind the other pollers (worst " +
                                                   std::to_string(worstPollMs.load()) + " ms)");
        MIB_EXPECT(slowestCommandMs < 2000, "no command waited more than 2 s behind the pollers");
        MIB_EXPECT(stopMs < 1000, "Stop got through the pollers within 1 s");
        MIB_EXPECT(!device.moving(), "stopped");
    }

    // Tight pollers must share the driver fairly. PR #516's plain Linux lane saw
    // a poll refused after 15 s while commands were fine (slowest command 40 ms):
    // the lock was a try_lock + sleep loop, so a thread that re-locked within
    // nanoseconds kept the driver while sleepers lost every race.
    watchdog.mark("fairness");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        constexpr int kPollers = 6;
        std::atomic<bool> stopFlag{false};
        std::atomic<long long> calls{0}; // finished polls, for the sample target below
        std::vector<long long> served(kPollers, 0), failed(kPollers, 0), worstMs(kPollers, 0); // one slot per thread
        std::vector<std::thread> threads;
        for (int p = 0; p < kPollers; ++p) {
            threads.emplace_back([&, p] {
                while (!stopFlag.load()) {
                    StageStatus s;
                    const auto t0 = std::chrono::steady_clock::now();
                    if (stage.readStatus(s) == StageError::None) ++served[p]; else ++failed[p];
                    worstMs[p] = std::max<long long>(worstMs[p], std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                    calls.fetch_add(1);
                }
            });
        }
        // Run for at least 1.5 s and until enough polls were served for the
        // share check to mean something, bounded by a deadline. A fixed window
        // is a Linux-speed assumption: on the Windows runner a poll takes
        // ~28 ms (timer granularity in the fake serial path), so 1.5 s served
        // only 53 polls although fairness was perfect (fewest 8, mean 8, worst
        // wait 187 ms).
        constexpr long long kSampleTarget = 120;
        const auto windowStart = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        while (calls.load() < kSampleTarget &&
               std::chrono::steady_clock::now() - windowStart < std::chrono::seconds(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stopFlag.store(true);
        for (auto& t : threads) t.join();
        long long total = 0, fewest = served[0], worst = 0, errors = 0;
        for (int p = 0; p < kPollers; ++p) { total += served[p]; fewest = std::min(fewest, served[p]); worst = std::max(worst, worstMs[p]); errors += failed[p]; }
        const long long mean = total / kPollers;
        std::printf("fairness: %d pollers, %lld calls, fewest %lld, mean %lld, worst wait %lld ms\n", kPollers, total, fewest, mean, worst);
        MIB_EXPECT(errors == 0, "no poll was refused");
        MIB_EXPECT(total >= kSampleTarget, "the pollers served the sample target within the deadline (" +
                                                std::to_string(total) + " polls)");
        MIB_EXPECT(fewest * 4 >= mean, "every poller got at least a quarter of the average share (fewest " +
                                           std::to_string(fewest) + ", mean " + std::to_string(mean) + ")");
        MIB_EXPECT(worst < 2000, "no poll waited 2 s or more (worst " + std::to_string(worst) + " ms)");
    }

    // A move that was decided before a Stop must not start motion after it (review
    // of #531): the caller reads the stop generation first, the driver checks it
    // under its lock right before the opcode.
    watchdog.mark("stop generation");
    {
        FakeZc300 device;
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        const auto generation = stage.stopGeneration();
        MIB_EXPECT(stage.stop() == StageError::None, "Stop");
        MIB_EXPECT(stage.stopGeneration() == generation + 1, "every Stop bumps the generation");
        const int motions = device.opcodeCount(0x64) + device.opcodeCount(0x65);
        MIB_EXPECT(stage.moveAbsolute(100, generation) == StageError::Stopped, "a move decided before the Stop is refused");
        MIB_EXPECT(device.opcodeCount(0x64) + device.opcodeCount(0x65) == motions, "no opcode was sent");
        MIB_EXPECT(stage.moveAbsolute(100, stage.stopGeneration()) == StageError::None, "a move decided after it goes out");
        MIB_EXPECT(device.opcodeCount(0x64) == 1, "that opcode was sent");
    }

    // Stop goes to the front of the line. A reviewer found that Stop shared the
    // command FIFO: queued moves and teardown went first, and after the
    // timeout it returned Busy without ever sending. With every reply delayed,
    // each driver call holds the driver ~200 ms, so a queue builds up
    // behind the in-flight call; Stop must be sent next, ahead of it.
    watchdog.mark("stop jumps the queue");
    {
        FakeZc300 device;
        device.setPulsesPerSecond(2000); // the first move keeps running
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        device.setReplyDelayMs(200);
        std::thread inFlight([&] { stage.moveRelative(100); });
        while (stage.waitingCalls() == 0 && device.opcodeCount(0x65) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::thread queuedMove([&] { stage.moveRelative(50); });
        std::thread queuedSpeed([&] { stage.setSpeed(1000, 2000); });
        const auto queued = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (stage.waitingCalls() < 2 && std::chrono::steady_clock::now() < queued) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        MIB_REQUIRE(stage.waitingCalls() >= 2, "two commands are queued behind the in-flight call");

        const auto t0 = std::chrono::steady_clock::now();
        MIB_EXPECT(stage.stop() == StageError::None, "Stop is sent, not refused");
        const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        inFlight.join(); queuedMove.join(); queuedSpeed.join();
        const auto ops = device.opcodeSequence();
        // in-flight move (0x65), then Stop (0x68), only then the queued move (0x65)
        MIB_REQUIRE(ops.size() >= 3, "all three opcodes reached the wire");
        MIB_EXPECT(ops[0] == 0x65 && ops[1] == 0x68 && ops[2] == 0x65,
                   "Stop was sent next, ahead of the queued move (sequence " + std::to_string(ops[0]) + "," +
                       std::to_string(ops[1]) + "," + std::to_string(ops[2]) + ")");
        MIB_EXPECT(stopMs < 1200, "Stop took about one in-flight call, not the whole queue (" + std::to_string(stopMs) + " ms)");
        device.setReplyDelayMs(0);
    }

    // ...but a Stop storm must not starve teardown: at most four Stops are
    // granted in a row while a Disconnect waits.
    watchdog.mark("stop storm does not starve disconnect");
    {
        FakeZc300 device;
        device.setPulsesPerSecond(2000);
        SerialBusManager manager;
        useFake(manager, device);
        zc300::Zc300Stage stage(manager);
        StageIdentity id;
        MIB_REQUIRE(connectTo(stage, device, id) == StageError::None, "connect");
        device.setReplyDelayMs(120);
        std::thread inFlight([&] { stage.moveRelative(100); });
        while (device.opcodeCount(0x65) == 0 && stage.waitingCalls() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::atomic<int> completed{0};
        std::atomic<int> disconnectIndex{-1};
        std::thread disconnecting([&] { stage.disconnect(); disconnectIndex = completed.fetch_add(1); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (stage.waitingCalls() < 1 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::vector<std::thread> storm;
        for (int i = 0; i < 6; ++i) storm.emplace_back([&] { stage.stop(); completed.fetch_add(1); });
        while (stage.waitingCalls() < 7 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        MIB_REQUIRE(stage.waitingCalls() >= 7, "a Disconnect and six Stops are queued behind the in-flight call");
        inFlight.join();
        disconnecting.join();
        for (auto& t : storm) t.join();
        MIB_EXPECT(disconnectIndex.load() >= 0 && disconnectIndex.load() <= 4,
                   "Disconnect finished after at most four Stops (index " + std::to_string(disconnectIndex.load()) + " of 7)");
        MIB_EXPECT(!stage.isConnected(), "disconnected");
        device.setReplyDelayMs(0);
    }

    if (mib::test::exitCode() == 0) std::printf("ZC300 stage driver verified\n");
    return mib::test::exitCode();
}
