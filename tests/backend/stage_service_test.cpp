// stage_service_test
//
// StageService (#464, ADR 0013 §5–6) over the ZC300 driver, a real
// ModbusBusSession and the fake controller. Covers the policy decided by
// Gavin on 2026-10-05 and the referencing rules:
//  - start-up is read-only by default (zero writes, zero motion); before Home
//    only Home and Stop are accepted; a disabled stage is not even opened;
//  - Home probes both limits, checks the span, zeroes at mid-travel with the
//    one-sided approach, and sets symmetric soft limits;
//  - the reference survives an application restart through the power-up
//    token, and is lost on a controller power cycle;
//  - every failure path (span, missing limit, e-stop, deadline, cancel)
//    leaves the axis stopped, and losing the frame drops the reference;
//  - Stop always wins; one operation at a time; shutdown stops a moving axis.

#include "backend/services/StageService.h"

#include "support/assert.h"
#include "support/stage_rig.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace backend::services;
using backend::stage::StageError;
using mib::test::StageRig;
using OpState = StageService::OperationState;

namespace {

constexpr double kUmPerPulse = 0.4375;

OpState finish(StageService& s, StageService::OperationId id, int ms = 10000)
{
    MIB_EXPECT(s.waitForOperation(id, std::chrono::milliseconds(ms)), "operation reached a terminal state");
    const auto op = s.operation(id);
    return op ? op->state : OpState::Queued;
}

bool home(StageService& s)
{
    const auto r = s.reference();
    if (!r.accepted()) {
        std::printf("Home refused: %s\n", r.detail.c_str());
        return false;
    }
    return finish(s, r.id) == OpState::Completed;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

int main()
{
    mib::test::Watchdog watchdog(90);

    // --- start-up is read-only ------------------------------------------------
    watchdog.mark("read-only start-up");
    {
        StageRig rig;
        rig.device.setPositionPulses(-1000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up connects");
        const auto snap = svc->snapshot();
        MIB_EXPECT(snap.connected && snap.configured && !snap.referenced, "connected, configured, unreferenced");
        MIB_EXPECT(snap.identity.serial == "26017", "identity in the snapshot");
        MIB_EXPECT(rig.device.writes() == 0 && rig.device.opcodes() == 0, "start-up wrote nothing");
        MIB_EXPECT(svc->moveTo(0).error == StageError::NotReferenced, "absolute move refused before Home");
        MIB_EXPECT(svc->moveBy(1).error == StageError::NotReferenced, "1 um jog refused before Home");
        sleepMs(60); // idle polling only
        MIB_EXPECT(rig.device.writes() == 0, "idle polling wrote nothing");
        MIB_EXPECT(rig.device.positionPulses() == -1000, "the stage never moved");
        svc->shutdown();
        MIB_EXPECT(rig.device.writes() == 0, "idle shutdown wrote nothing");
    }

    watchdog.mark("disabled stage");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.enabled = false;
        auto svc = rig.service(cfg);
        MIB_EXPECT(svc->startup() == StageError::None && !svc->snapshot().connected, "disabled: not connected");
        MIB_EXPECT(rig.device.frames() == 0, "disabled: the port was never used");
    }

    watchdog.mark("misconfigured controller");
    {
        StageRig rig(mib::test::FakeZc300Config{0, 0, 4.0f, 1600}); // factory settings
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "connects read-only");
        MIB_EXPECT(!svc->snapshot().configured, "flagged as misconfigured");
        MIB_EXPECT(svc->reference().error == StageError::Misconfigured, "Home refused");
        MIB_EXPECT(rig.device.writes() == 0, "nothing written");
        MIB_EXPECT(svc->applyProfile() == StageError::None && svc->snapshot().configured,
                   "explicit applyProfile configures it");
    }

    // --- Home -----------------------------------------------------------------
    watchdog.mark("Home at mid-travel");
    {
        StageRig rig;
        rig.device.setLimits(-2000, 11716); // span 13716 pulses = 6000.75 um, mid at 4858
        rig.device.setPositionPulses(500);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        MIB_REQUIRE(home(*svc), "Home completes");
        const auto snap = svc->snapshot();
        MIB_EXPECT(snap.referenced && snap.status.referenced, "referenced");
        MIB_EXPECT(std::abs(snap.spanUm - 13716 * kUmPerPulse) < 1.0, "span measured limit to limit");
        MIB_EXPECT(std::abs(snap.softMaxUm - (snap.spanUm / 2 - 100)) < 1e-6 && snap.softMinUm == -snap.softMaxUm,
                   "symmetric soft limits, 100 um inside the switches");
        MIB_EXPECT(std::abs(rig.device.positionPulses() - 4858) <= 2, "physically at mid-travel");
        MIB_EXPECT(rig.device.counterPulses() == 0 && std::abs(snap.status.positionUm) < 0.5, "zeroed there");
        MIB_EXPECT(rig.device.lastMoveDirection() == 1, "final approach was positive (backlash taken up)");
        MIB_EXPECT(rig.device.scratch() != 0 && rig.store->load() &&
                       rig.store->load()->token == rig.device.scratch(),
                   "power-up token written and stored");

        // Moves in the referenced frame.
        auto r = svc->moveTo(1000);
        MIB_REQUIRE(r.accepted() && finish(*svc, r.id) == OpState::Completed, "move to +1000 um");
        MIB_EXPECT(rig.device.counterPulses() == 2286, "1000 um -> 2286 pulses");
        const int writes = rig.device.writes();
        MIB_EXPECT(svc->moveTo(2950).error == StageError::OutOfSoftLimits, "beyond the soft limit refused");
        MIB_EXPECT(svc->moveTo(12.5).error == StageError::OffGrid, "off-grid refused");
        MIB_EXPECT(rig.device.writes() == writes, "refusals wrote nothing");
        r = svc->moveTo(-500);
        MIB_REQUIRE(r.accepted() && finish(*svc, r.id) == OpState::Completed, "move down to -500 um");
        MIB_EXPECT(rig.device.counterPulses() == -1143 && rig.device.lastMoveDirection() == 1,
                   "downward move overshot, then approached from below");
        r = svc->moveBy(25);
        MIB_REQUIRE(r.accepted() && finish(*svc, r.id) == OpState::Completed, "relative +25 um");
        MIB_EXPECT(std::abs(svc->snapshot().status.positionUm - (-475)) < 0.5, "relative move on the um grid");
    }

    watchdog.mark("reference across restart and power cycle");
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && home(*svc), "Home with the first instance");
            auto r = svc->moveTo(300);
            MIB_REQUIRE(r.accepted() && finish(*svc, r.id) == OpState::Completed, "move");
        }
        {
            const int writesBefore = rig.device.writes();
            auto svc = rig.service(); // application restart, controller still powered
            MIB_REQUIRE(svc->startup() == StageError::None, "restart connects");
            const auto snap = svc->snapshot();
            MIB_EXPECT(snap.referenced && snap.softMaxUm > 2800, "reference kept across the restart");
            MIB_EXPECT(std::abs(snap.status.positionUm - 300) < 0.5, "frame intact");
            MIB_EXPECT(rig.device.writes() == writesBefore, "restart connect wrote nothing");
            auto r = svc->moveTo(0);
            MIB_EXPECT(r.accepted() && finish(*svc, r.id) == OpState::Completed, "moves without a new Home");
        }
        rig.device.powerCycle();
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None, "connect after power cycle");
            MIB_EXPECT(!svc->snapshot().referenced, "power cycle: Home required again");
            MIB_EXPECT(!rig.store->load(), "stale record discarded");
        }
    }

    watchdog.mark("session-only reference");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.reference.powerUpTokenRegister = 0;
        {
            auto svc = rig.service(cfg);
            MIB_REQUIRE(svc->startup() == StageError::None && home(*svc), "Home");
            MIB_EXPECT(rig.device.scratch() == 0 && !rig.store->load(), "no token, no record");
        }
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "restart");
        MIB_EXPECT(!svc->snapshot().referenced, "reference held for one session only");
    }

    watchdog.mark("on_startup opt-in");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.reference.onStartup = true;
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        const auto active = svc->snapshot().activeOperation;
        MIB_REQUIRE(active != 0, "start-up queued a Home");
        MIB_EXPECT(finish(*svc, active) == OpState::Completed && svc->snapshot().referenced,
                   "opted-in rig is referenced after start-up");
    }

    // --- Home failures leave the axis stopped and unreferenced -----------------
    watchdog.mark("Home failures");
    {
        StageRig rig;
        rig.device.setLimits(-4000, 4000); // 3500 um: wrong stage or wiring
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        const auto r = svc->reference();
        MIB_REQUIRE(r.accepted(), "Home starts");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed, "span mismatch fails");
        MIB_EXPECT(svc->operation(r.id)->error == StageError::ReferenceFailed, "ReferenceFailed");
        MIB_EXPECT(!svc->snapshot().referenced && !rig.device.moving(), "stopped and unreferenced");
    }
    {
        StageRig rig;
        rig.device.setLimits(-40000, 40000); // no switch within the 6500 um search
        rig.device.setPulsesPerSecond(20000);   // ~44 um per 5 ms poll
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        const auto r = svc->reference();
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed, "missing limit fails");
        MIB_EXPECT(!rig.device.moving(), "jog stopped");
        MIB_EXPECT(std::abs(rig.device.positionPulses()) * kUmPerPulse < 6500 + 300,
                   "search stopped near its 6500 um bound");
    }
    {
        StageRig rig;
        rig.device.setPulsesPerSecond(2000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        const auto r = svc->reference();
        sleepMs(100);
        MIB_EXPECT(svc->stop() == StageError::None, "Stop during Home");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Cancelled, "Home cancelled");
        MIB_EXPECT(!rig.device.moving() && !svc->snapshot().referenced, "stopped, unreferenced");
    }

    // --- move failures ---------------------------------------------------------
    watchdog.mark("move failures");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && home(*svc), "Home");
        rig.device.setPulsesPerSecond(2000);

        auto r = svc->moveTo(2000);
        MIB_REQUIRE(r.accepted(), "slow move");
        MIB_EXPECT(svc->moveTo(-2000).error == StageError::Busy, "one operation at a time");
        sleepMs(100);
        MIB_EXPECT(svc->stop() == StageError::None && finish(*svc, r.id) == OpState::Cancelled, "Stop wins");
        MIB_EXPECT(!rig.device.moving() && svc->snapshot().referenced, "operator Stop keeps the reference");

        r = svc->moveTo(-2000);
        sleepMs(100);
        rig.device.setEmergencyStop(true);
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed, "e-stop fails the move");
        MIB_EXPECT(svc->operation(r.id)->error == StageError::EmergencyStop, "EmergencyStop");
        MIB_EXPECT(!svc->snapshot().referenced, "e-stop drops the reference");
        MIB_EXPECT(svc->reference().error == StageError::EmergencyStop, "Home refused while e-stop is active");
        rig.device.setEmergencyStop(false);
    }
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && home(*svc), "Home");
        rig.device.setPulsesPerSecond(100); // 44 um/s against a 1000 um/s budget
        const auto r = svc->moveTo(500);
        MIB_EXPECT(finish(*svc, r.id) == OpState::TimedOut, "deadline -> TimedOut");
        MIB_EXPECT(!rig.device.moving() && !svc->snapshot().referenced, "stopped; reference dropped");
    }

    watchdog.mark("shutdown while moving");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && home(*svc), "Home");
        rig.device.setPulsesPerSecond(2000);
        const auto r = svc->moveTo(2500);
        sleepMs(50);
        svc->shutdown();
        MIB_EXPECT(!rig.device.moving(), "shutdown stopped the axis");
        MIB_EXPECT(svc->operation(r.id)->state == OpState::Cancelled, "operation cancelled");
        MIB_EXPECT(!svc->moveTo(0).accepted(), "no operations after shutdown");
    }

    if (mib::test::exitCode() == 0) std::printf("StageService verified\n");
    return mib::test::exitCode();
}
