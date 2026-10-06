// stage_service_test
//
// StageService (#464, ADR 0013 §5 and Amendment 1) over the ZC300 driver, a real
// ModbusBusSession and the fake controller. The stage is never homed: the
// operator sets zero, and travel is bounded around it. Covers:
//  - start-up is read-only (zero writes, zero motion); before zero is set only
//    Set zero and Stop are accepted; a disabled stage is not even opened;
//  - Set zero is one register write and no motion; the envelope is +/-1000 um
//    by default and +/-2900 um only after a mid-travel declaration; targets
//    (and the approach overshoot) outside it are refused, never clamped;
//  - re-zeroing without a declaration cannot walk the envelope along the stage;
//  - the zero survives an application restart through the power-up token, is
//    lost on a power cycle, an e-stop or a driver alarm, and not on Stop;
//  - limit bits only stop a move heading toward an active switch; they gate
//    nothing else;
//  - Stop always wins; one operation at a time; shutdown stops a moving axis.

#include "backend/services/StageService.h"

#include "support/assert.h"
#include "support/stage_rig.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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

// Set zero here; true when accepted.
bool zero(StageService& s, bool midTravel = false, std::string* detail = nullptr)
{
    std::string text;
    const StageError err = s.setZero(midTravel, &text);
    if (detail) *detail = text;
    if (err != StageError::None) std::printf("Set zero refused: %s\n", text.c_str());
    return err == StageError::None;
}

bool moveTo(StageService& s, double um)
{
    const auto r = s.moveTo(um);
    if (!r.accepted()) {
        std::printf("move to %.0f refused: %s\n", um, r.detail.c_str());
        return false;
    }
    return finish(s, r.id) == OpState::Completed;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool waitFor(const std::function<bool()>& pred, int ms = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        sleepMs(10);
    }
    return pred();
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(120);

    // --- start-up is read-only ------------------------------------------------
    watchdog.mark("read-only start-up");
    {
        StageRig rig;
        rig.device.setPositionPulses(-1000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up connects");
        const auto snap = svc->snapshot();
        MIB_EXPECT(snap.connected && snap.configured && !snap.zeroSet, "connected, configured, zero not set");
        MIB_EXPECT(snap.identity.serial == "26017", "identity in the snapshot");
        MIB_EXPECT(rig.device.writes() == 0 && rig.device.opcodes() == 0, "start-up wrote nothing");
        MIB_EXPECT(svc->moveTo(0).error == StageError::ZeroNotSet, "absolute move refused before zero is set");
        MIB_EXPECT(svc->moveBy(1).error == StageError::ZeroNotSet, "1 um jog refused before zero is set");
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
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::Misconfigured, "Set zero refused");
        MIB_EXPECT(rig.device.writes() == 0, "nothing written");
        MIB_EXPECT(svc->applyProfile() == StageError::None && svc->snapshot().configured,
                   "explicit applyProfile configures it");
    }

    watchdog.mark("not connected");
    {
        StageRig rig;
        auto svc = rig.service();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::NotConnected, "Set zero needs a connection");
        MIB_EXPECT(rig.device.writes() == 0, "nothing written");
    }

    // --- the limit-switch record is a badge, never a gate -----------------------
    watchdog.mark("limit record gates nothing");
    {
        StageRig rig;
        rig.unverifyLimits();
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up connects");
        MIB_EXPECT(!svc->snapshot().limitsVerified, "wiring unverified");
        MIB_EXPECT(zero(*svc) && moveTo(*svc, 100), "zero and moves work with unverified limits");
        // The bench tool records a passing check while the app is connected.
        rig.limits->save({"26017", "2026-10-06T12:00:00Z", -3000.0, 3000.0, 6000.0, "zc300ctl verify-limits"});
        MIB_EXPECT(waitFor([&] { return svc->snapshot().limitsVerified; }), "the badge clears without reconnecting");
        MIB_EXPECT(svc->snapshot().envelopeMaxUm == 1000.0, "a verified check does not widen the envelope");
    }

    // --- Set zero ---------------------------------------------------------------
    watchdog.mark("Set zero here");
    {
        StageRig rig;
        rig.device.setPositionPulses(-1000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        std::string detail;
        MIB_REQUIRE(zero(*svc, false, &detail), "Set zero accepted");
        const auto snap = svc->snapshot();
        MIB_EXPECT(snap.zeroSet && snap.status.zeroSet && !snap.midTravelDeclared, "zero set, no declaration");
        MIB_EXPECT(snap.envelopeMinUm == -1000.0 && snap.envelopeMaxUm == 1000.0, "default envelope +/-1000 um");
        MIB_EXPECT(rig.device.counterPulses() == 0, "the counter now reads 0 here");
        MIB_EXPECT(rig.device.positionPulses() == -1000, "the stage did not move");
        MIB_EXPECT(rig.device.opcodes() == 0 && rig.device.motionLog().empty(), "Set zero issued no opcode at all");
        const auto record = rig.store->load();
        MIB_REQUIRE(record.has_value(), "the zero is stored for a restart");
        MIB_EXPECT(record->token != 0 && record->token == rig.device.scratch(), "power-up token written and stored");
        MIB_EXPECT(!record->midTravelDeclared && record->windowMinUm == -1000.0 && record->windowMaxUm == 1000.0,
                   "window of the first zero");

        // Moves inside the envelope.
        MIB_EXPECT(moveTo(*svc, 1000), "move to +1000 um");
        MIB_EXPECT(rig.device.counterPulses() == 2286, "1000 um -> 2286 pulses");
        const int writes = rig.device.writes();
        auto r = svc->moveTo(1001);
        MIB_EXPECT(r.error == StageError::OutOfSoftLimits && r.detail.find("envelope") != std::string::npos,
                   "1 um beyond the envelope is refused, not clamped: " + r.detail);
        MIB_EXPECT(svc->moveBy(1).error == StageError::OutOfSoftLimits, "a relative move beyond it is refused too");
        MIB_EXPECT(svc->moveTo(-1001).error == StageError::OutOfSoftLimits, "negative side too");
        MIB_EXPECT(svc->moveTo(2950).error == StageError::OutOfSoftLimits, "far beyond refused");
        MIB_EXPECT(svc->moveTo(12.5).error == StageError::OffGrid, "off-grid refused");
        MIB_EXPECT(rig.device.writes() == writes, "refusals wrote nothing");
        MIB_EXPECT(moveTo(*svc, -500), "move down to -500 um");
        MIB_EXPECT(rig.device.counterPulses() == -1143 && rig.device.lastMoveDirection() == 1,
                   "downward move overshot, then approached from below");
        const auto by = svc->moveBy(25);
        MIB_REQUIRE(by.accepted() && finish(*svc, by.id) == OpState::Completed, "relative +25 um");
        MIB_EXPECT(std::abs(svc->snapshot().status.positionUm - (-475)) < 0.5, "relative move on the um grid");
    }

    // The overshoot leg of the one-sided approach is part of the move: a target
    // whose overshoot would leave the envelope is refused (never clamped).
    watchdog.mark("approach overshoot stays inside the envelope");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero set");
        const int motions = static_cast<int>(rig.device.motionLog().size());
        const auto r = svc->moveTo(-990); // overshoot waypoint -1010 um
        MIB_EXPECT(r.error == StageError::OutOfSoftLimits && r.detail.find("overshoot") != std::string::npos,
                   "overshoot beyond the envelope refused: " + r.detail);
        MIB_EXPECT(static_cast<int>(rig.device.motionLog().size()) == motions, "no motion was sent");
        MIB_EXPECT(moveTo(*svc, -980), "-980 um: its overshoot waypoint is exactly on the edge");
        MIB_EXPECT(rig.device.counterPulses() >= -2286, "the axis never went below the envelope");
    }

    watchdog.mark("mid-travel declaration widens the envelope");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000); // away from the switches
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc, true), "zero with mid-travel declared");
        auto snap = svc->snapshot();
        MIB_EXPECT(snap.midTravelDeclared && snap.envelopeMinUm == -2900.0 && snap.envelopeMaxUm == 2900.0,
                   "declared: +/-2900 um (the stage's half span less 100 um)");
        MIB_EXPECT(moveTo(*svc, 2900), "the full envelope is reachable");
        MIB_EXPECT(svc->moveTo(2901).error == StageError::OutOfSoftLimits, "the cap is not exceeded");
        MIB_EXPECT(svc->moveTo(-2900).error == StageError::OutOfSoftLimits, "-2900 would overshoot to -2920");
        MIB_EXPECT(moveTo(*svc, -2880), "-2880 um");
        // The declaration belongs to one zero: setting it again without it narrows.
        MIB_REQUIRE(zero(*svc, false), "set zero again, no declaration");
        snap = svc->snapshot();
        MIB_EXPECT(!snap.midTravelDeclared && snap.envelopeMaxUm <= 1000.0 && snap.envelopeMinUm >= -1000.0,
                   "the declaration is cleared by the next zero");
    }

    // --- re-zeroing cannot walk the envelope -------------------------------------
    watchdog.mark("re-zero cannot walk the envelope");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "first zero (window +/-1000 um)");
        MIB_REQUIRE(moveTo(*svc, 800), "to +800 um");
        MIB_REQUIRE(zero(*svc), "re-zero here");
        auto snap = svc->snapshot();
        // 800 um is 1829 pulses = 800.19 um, so the first window ends 199.8 um
        // ahead; the edge rounds inward to a whole micrometre.
        MIB_EXPECT(snap.envelopeMinUm == -1000.0 && snap.envelopeMaxUm == 199.0,
                   "the envelope keeps the first window: [-1000, +199] um");
        MIB_EXPECT(svc->moveTo(200).error == StageError::OutOfSoftLimits, "+200 um is beyond the first window");
        MIB_REQUIRE(moveTo(*svc, 199), "to the edge of the first window");
        MIB_REQUIRE(zero(*svc), "re-zero at the edge");
        snap = svc->snapshot();
        MIB_EXPECT(snap.envelopeMinUm == -1000.0 && snap.envelopeMaxUm == 0.0,
                   "the envelope is now [-1000, 0] um, not +/-1000");
        MIB_EXPECT(svc->moveBy(1).error == StageError::OutOfSoftLimits, "no way further positive");
        MIB_REQUIRE(zero(*svc), "re-zero at the edge again");
        MIB_EXPECT(svc->snapshot().envelopeMaxUm == 0.0, "repeating it does not walk further");
        MIB_EXPECT(rig.device.positionPulses() <= 2286 + 2, "physically never beyond +1000 um of the first zero");
        const auto record = rig.store->load();
        MIB_REQUIRE(record.has_value(), "stored");
        MIB_EXPECT(std::abs(record->windowMinUm + 1999.25) < 0.01 && std::abs(record->windowMaxUm - 0.75) < 0.01,
                   "the stored window follows the zero (unrounded)");

        // A hand move outside the window: only a declaration can re-zero there.
        rig.device.setPositionPulses(rig.device.positionPulses() + 5000);
        const int writes = rig.device.writes();
        const auto counter = rig.device.counterPulses();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::OutOfSoftLimits, "Set zero refused outside the window");
        MIB_EXPECT(detail.find("declare mid-travel") != std::string::npos, "the refusal says what to do: " + detail);
        MIB_EXPECT(rig.device.writes() == writes && rig.device.counterPulses() == counter, "nothing was written");
        MIB_EXPECT(svc->snapshot().envelopeMaxUm == 0.0, "the envelope is unchanged");
        MIB_EXPECT(zero(*svc, true) && svc->snapshot().envelopeMaxUm == 2900.0,
                   "an explicit mid-travel declaration is accepted there");
    }

    // --- the zero across restarts and power cycles --------------------------------
    watchdog.mark("zero across restart and power cycle");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc, true), "zero with the first instance");
            MIB_REQUIRE(moveTo(*svc, 300), "move");
        }
        {
            const int writesBefore = rig.device.writes();
            auto svc = rig.service(); // application restart, controller still powered
            MIB_REQUIRE(svc->startup() == StageError::None, "restart connects");
            const auto snap = svc->snapshot();
            MIB_EXPECT(snap.zeroSet && snap.midTravelDeclared && snap.envelopeMaxUm == 2900.0,
                       "zero and declaration kept across the restart");
            MIB_EXPECT(std::abs(snap.status.positionUm - 300) < 0.5, "frame intact");
            MIB_EXPECT(rig.device.writes() == writesBefore, "restart connect wrote nothing");
            MIB_EXPECT(moveTo(*svc, 0), "moves without setting zero again");
        }
        rig.device.powerCycle();
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None, "connect after power cycle");
            MIB_EXPECT(!svc->snapshot().zeroSet, "power cycle: Set zero required again");
            MIB_EXPECT(!rig.store->load(), "stale record discarded");
            MIB_EXPECT(svc->moveTo(0).error == StageError::ZeroNotSet, "and moves are refused until then");
        }
    }

    watchdog.mark("session-only zero");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.reference.powerUpTokenRegister = 0;
        {
            auto svc = rig.service(cfg);
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
            MIB_EXPECT(rig.device.scratch() == 0 && !rig.store->load(), "no token, no record");
            MIB_EXPECT(svc->snapshot().zeroSet, "still set for this session");
        }
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "restart");
        MIB_EXPECT(!svc->snapshot().zeroSet, "zero held for one session only");
    }

    watchdog.mark("a lower default envelope is honoured");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.envelope.defaultUm = 250.0;
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        MIB_EXPECT(svc->snapshot().envelopeMaxUm == 250.0, "+/-250 um");
        MIB_EXPECT(svc->moveTo(251).error == StageError::OutOfSoftLimits, "251 um refused");
    }

    watchdog.mark("applyProfile drops the zero");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        MIB_EXPECT(svc->applyProfile() == StageError::None, "profile applied");
        MIB_EXPECT(!svc->snapshot().zeroSet && !rig.store->load(), "rewriting the configuration drops the zero");
    }

    // --- Set zero refusals --------------------------------------------------------
    watchdog.mark("Set zero refusals");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setPulsesPerSecond(2000);
        const auto move = svc->moveTo(800); // ~0.9 s
        MIB_REQUIRE(move.accepted(), "slow move");
        sleepMs(50);
        const int writes = rig.device.writes();
        const auto counter = rig.device.counterPulses();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::Busy, "Set zero refused while a move runs");
        MIB_EXPECT(rig.device.writes() == writes, "and wrote nothing");
        MIB_EXPECT(rig.device.counterPulses() >= counter, "the counter was not rewritten under the move");
        MIB_EXPECT(svc->stop() == StageError::None && finish(*svc, move.id) == OpState::Cancelled, "stopped");
    }
    {
        // The axis moves although this service started nothing (front panel, or
        // another master): the counter is changing, so it cannot be rewritten.
        StageRig rig;
        rig.device.setPulsesPerSecond(2000);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        rig.device.startExternalMove(2000);
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.state == backend::stage::MoveState::Moving; }),
                    "the external motion is seen");
        const int writes = rig.device.writes();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::Busy, "Set zero refused while the axis moves");
        MIB_EXPECT(rig.device.writes() == writes, "nothing written under a moving counter");
        rig.device.startExternalMove(rig.device.positionPulses()); // let it stop where it is
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.state != backend::stage::MoveState::Moving; }),
                    "settled");
        MIB_EXPECT(zero(*svc), "once it is idle, Set zero is accepted");
    }
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        rig.device.setEmergencyStop(true);
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.emergencyStop; }), "e-stop seen");
        const int writes = rig.device.writes();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::EmergencyStop, "Set zero refused during an e-stop");
        MIB_EXPECT(rig.device.writes() == writes, "nothing written");
        rig.device.setEmergencyStop(false);
        MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.emergencyStop; }), "e-stop released");
        rig.device.setDriverAlarm(true);
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.driverAlarm; }), "alarm seen");
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::DriverAlarm, "Set zero refused during a driver alarm");
        MIB_EXPECT(rig.device.writes() == writes, "nothing written");
        rig.device.setDriverAlarm(false);
        MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.driverAlarm; }), "alarm cleared");
        MIB_EXPECT(zero(*svc), "after both clear, the operator can set zero");
    }

    // --- e-stop and alarm drop the zero; Stop does not ---------------------------------
    watchdog.mark("e-stop and driver alarm drop the zero");
    for (const bool alarm : {false, true}) {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        MIB_REQUIRE(rig.store->load().has_value(), "stored");
        if (alarm) rig.device.setDriverAlarm(true);
        else rig.device.setEmergencyStop(true);
        MIB_EXPECT(waitFor([&] { return !svc->snapshot().zeroSet; }),
                   std::string(alarm ? "a driver alarm" : "an e-stop") + " with the stage idle drops the zero");
        MIB_EXPECT(!rig.store->load(), "and the stored record");
        MIB_EXPECT(svc->snapshot().envelopeMinUm == 0.0 && svc->snapshot().envelopeMaxUm == 0.0, "no envelope");
        if (alarm) rig.device.setDriverAlarm(false);
        else rig.device.setEmergencyStop(false);
        MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.emergencyStop && !svc->snapshot().status.driverAlarm; }),
                    "cleared");
        sleepMs(60);
        MIB_EXPECT(!svc->snapshot().zeroSet, "clearing the fault does not bring the zero back");
        MIB_EXPECT(svc->moveTo(0).error == StageError::ZeroNotSet, "moves refused until the operator sets zero");
        MIB_EXPECT(zero(*svc), "Set zero again");
    }
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        }
        rig.device.setEmergencyStop(true); // the e-stop is already active at the next connect
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "connect during an e-stop");
        MIB_EXPECT(!svc->snapshot().zeroSet && !rig.store->load(), "a stored zero is not restored under an e-stop");
    }

    // --- limit bits: a backstop that only stops -------------------------------------------
    watchdog.mark("limit bits");
    {
        // The controller halts at the switch: the move ends short and fails.
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setLimits(-6858, 458); // + switch at ~200 um
        const auto r = svc->moveTo(500);
        MIB_REQUIRE(r.accepted(), "the envelope allows it");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::LimitSwitch,
                   "a limit that stops the move short fails it");
        MIB_EXPECT(!rig.device.moving() && !svc->snapshot().zeroSet, "stopped; the counter is in doubt, zero dropped");
    }
    {
        // Sitting on the + switch: moving toward it is refused before anything is sent;
        // moving away is allowed (the bit gates nothing else).
        StageRig rig;
        rig.device.setLimits(-6858, 0); // the + switch is active here
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "start-up");
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.limitPositive; }), "the + bit is read");
        MIB_EXPECT(zero(*svc), "Set zero works with a limit bit active");
        const int motions = static_cast<int>(rig.device.motionLog().size());
        const auto toward = svc->moveBy(300);
        MIB_EXPECT(toward.error == StageError::LimitSwitch, "toward the active switch: refused");
        MIB_EXPECT(static_cast<int>(rig.device.motionLog().size()) == motions, "and nothing was sent");
        MIB_EXPECT(svc->snapshot().zeroSet, "the refusal does not cost the zero");
        MIB_EXPECT(moveTo(*svc, -300), "away from it: allowed");
    }
    {
        // The controller does not honour the switch: the host stops the axis.
        StageRig rig;
        rig.device.setPulsesPerSecond(2000); // ~100 pulses between host polls
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setLimits(-6858, 458);
        rig.device.setLimitsHalt(false);
        const auto r = svc->moveTo(1000);
        MIB_REQUIRE(r.accepted(), "accepted");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::LimitSwitch,
                   "the move fails on the limit bit");
        MIB_EXPECT(!rig.device.moving(), "the axis is stopped");
        MIB_EXPECT(rig.device.positionPulses() < 458 + 300, "stopped by the host near the switch, not at the target");
    }

    // --- move failures ---------------------------------------------------------
    watchdog.mark("move failures");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setPulsesPerSecond(2000);

        auto r = svc->moveTo(900);
        MIB_REQUIRE(r.accepted(), "slow move");
        MIB_EXPECT(svc->moveTo(-900).error == StageError::Busy, "one operation at a time");
        sleepMs(100);
        MIB_EXPECT(svc->stop() == StageError::None && finish(*svc, r.id) == OpState::Cancelled, "Stop wins");
        MIB_EXPECT(!rig.device.moving() && svc->snapshot().zeroSet, "operator Stop keeps the zero");

        r = svc->moveTo(-900);
        sleepMs(100);
        rig.device.setEmergencyStop(true);
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed, "e-stop fails the move");
        MIB_EXPECT(svc->operation(r.id)->error == StageError::EmergencyStop, "EmergencyStop");
        MIB_EXPECT(!svc->snapshot().zeroSet, "e-stop drops the zero");
        rig.device.setEmergencyStop(false);
    }
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setPulsesPerSecond(100); // 44 um/s against a 1000 um/s budget
        const auto r = svc->moveTo(500);
        MIB_EXPECT(finish(*svc, r.id) == OpState::TimedOut, "deadline -> TimedOut");
        MIB_EXPECT(!rig.device.moving() && !svc->snapshot().zeroSet, "stopped; zero dropped");
    }

    // The bridge runs one command at a time, and Stop needs the same lock: a
    // Disconnect or ApplyProfile that queues behind a running move would hold
    // Stop for the whole move (found while designing the Tauri panel, #464).
    watchdog.mark("disconnect and apply profile never wait behind a move");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setPulsesPerSecond(2000);
        const auto move = svc->moveTo(950);
        MIB_REQUIRE(move.accepted(), "slow move (~1 s)");
        sleepMs(100);

        auto t0 = std::chrono::steady_clock::now();
        MIB_EXPECT(svc->applyProfile() == StageError::Busy, "ApplyProfile is refused while a move runs");
        const auto applyMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(applyMs < 500, "ApplyProfile answered at once, not after the move (" + std::to_string(applyMs) + " ms)");
        MIB_EXPECT(rig.device.moving() && rig.device.saves() == 0, "the move was not disturbed and nothing was saved");

        std::string detail;
        MIB_EXPECT(svc->connect(&detail) == StageError::Busy, "Connect is refused while a move runs");
        MIB_EXPECT(rig.device.moving(), "still moving");

        t0 = std::chrono::steady_clock::now();
        svc->disconnect();
        const auto discMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(discMs < 800, "Disconnect stopped the move and returned (" + std::to_string(discMs) + " ms), not after ~1 s");
        MIB_EXPECT(!rig.device.moving(), "the axis is stopped");
        MIB_EXPECT(svc->operation(move.id)->state == OpState::Cancelled, "the move was cancelled");
        MIB_EXPECT(!svc->snapshot().connected, "disconnected");
        MIB_EXPECT(!svc->moveTo(0).accepted(), "no motion after Disconnect");
    }

    watchdog.mark("shutdown while moving");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setPulsesPerSecond(2000);
        const auto r = svc->moveTo(950);
        sleepMs(50);
        svc->shutdown();
        MIB_EXPECT(!rig.device.moving(), "shutdown stopped the axis");
        MIB_EXPECT(svc->operation(r.id)->state == OpState::Cancelled, "operation cancelled");
        MIB_EXPECT(!svc->moveTo(0).accepted(), "no operations after shutdown");
    }

    if (mib::test::exitCode() == 0) std::printf("StageService verified\n");
    return mib::test::exitCode();
}
