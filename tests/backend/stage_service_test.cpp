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
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

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

// A zero store that records every change in one ordered log shared with the
// controller's register writes, and can be told to ignore clear() or save().
// Used to ask: after each step of a Set zero, what would a crash leave behind?
struct StoreEvent {
    enum Kind { Token, Position, Save, Clear } kind{Save};
    std::uint16_t token{0};                       // Token: the value written to register 30054
    std::optional<StageReferenceRecord> record;   // Save: the record written
};
struct EventLog {
    std::mutex mutex;
    std::vector<StoreEvent> events;
    void add(StoreEvent e) { std::lock_guard<std::mutex> lock(mutex); events.push_back(std::move(e)); }
    std::vector<StoreEvent> copy() { std::lock_guard<std::mutex> lock(mutex); return events; }
};
class RecordingStore final : public IStageReferenceStore {
public:
    RecordingStore(std::shared_ptr<MemoryStageReferenceStore> inner, std::shared_ptr<EventLog> log, bool failClear,
                   bool failSave)
        : inner_(std::move(inner)), log_(std::move(log)), failClear_(failClear), failSave_(failSave) {}
    std::optional<StageReferenceRecord> load() override { return inner_->load(); }
    bool save(const StageReferenceRecord& r) override
    {
        if (failSave_.load()) return false; // a failed write: nothing is stored
        inner_->save(r);
        StoreEvent e; e.kind = StoreEvent::Save; e.record = r;
        log_->add(std::move(e));
        return true;
    }
    bool clear() override
    {
        struct Mark { std::atomic<bool>& f; explicit Mark(std::atomic<bool>& x) : f(x) { f = true; } ~Mark() { f = false; } } mark(inClear_);
        if (clearDelayMs_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(clearDelayMs_.load()));
        if (failClear_.load()) return false; // a failed deletion: the old record stays on disk
        inner_->clear();
        log_->add(StoreEvent{StoreEvent::Clear, 0, std::nullopt});
        return true;
    }
    void setFailSave(bool on) { failSave_.store(on); }
    void setFailClear(bool on) { failClear_.store(on); }
    void setClearDelayMs(int ms) { clearDelayMs_.store(ms); }
    bool inClear() const { return inClear_.load(); }

private:
    std::shared_ptr<MemoryStageReferenceStore> inner_;
    std::shared_ptr<EventLog> log_;
    std::atomic<bool> failClear_;
    std::atomic<bool> failSave_;
    std::atomic<int> clearDelayMs_{0};
    std::atomic<bool> inClear_{false};
};

// A reconnect at any point of the log must recognise the same power-up: a record
// exists and the controller holds its token (or the one it is about to switch
// to). Otherwise the window of this power-up would be lost. Returns the first
// prefix where it is not so, -1 when there is none.
int firstLostWindow(const std::vector<StoreEvent>& log, std::optional<StageReferenceRecord> record,
                    std::uint16_t controllerToken)
{
    const auto sameLifetime = [&] {
        return record && record->token != 0 &&
               (record->token == controllerToken || (record->nextToken != 0 && record->nextToken == controllerToken));
    };
    if (!sameLifetime()) return 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto& e = log[i];
        if (e.kind == StoreEvent::Token) controllerToken = e.token;
        else if (e.kind == StoreEvent::Save) record = e.record;
        else if (e.kind == StoreEvent::Clear) record.reset();
        if (!sameLifetime()) return static_cast<int>(i) + 1;
    }
    return -1;
}

// Replays the log from `initial` and returns the first prefix at which a
// reconnect would restore a zero although the counter was already rewritten
// and no record for the new frame exists yet; -1 when there is none.
int firstUnsafeRestore(const std::vector<StoreEvent>& log, std::optional<StageReferenceRecord> record,
                       std::uint16_t controllerToken)
{
    bool counterRewritten = false;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto& e = log[i];
        switch (e.kind) {
        case StoreEvent::Token: controllerToken = e.token; break;
        case StoreEvent::Position: counterRewritten = true; break;
        case StoreEvent::Save:
            record = e.record;
            if (e.record && e.record->zeroValid && !e.record->frameUncertain && e.record->token == controllerToken) {
                counterRewritten = false; // a record for the new frame is on disk
            }
            break;
        case StoreEvent::Clear: record.reset(); break;
        }
        const bool restorable = record && record->zeroValid && !record->frameUncertain && record->token != 0 &&
                                record->token == controllerToken;
        if (restorable && counterRewritten) return static_cast<int>(i);
    }
    return -1;
}

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
        cfg.reference.allowSessionOnlyZero = true; // hardware-acceptance mode, refused otherwise
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
        MIB_EXPECT(!svc->snapshot().zeroSet, "rewriting the configuration drops the zero");
        MIB_EXPECT(waitFor([&] { const auto k = rig.store->load(); return k && !k->zeroValid; }),
                   "the stored record keeps its window but is no longer a valid zero");
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
        MIB_EXPECT(waitFor([&] { const auto k = rig.store->load(); return k && !k->zeroValid; }),
                   "the stored record keeps its window but is no longer a valid zero");
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
        MIB_EXPECT(!svc->snapshot().zeroSet, "a stored zero is not restored under an e-stop");
        MIB_EXPECT(waitFor([&] { const auto k = rig.store->load(); return k && !k->zeroValid; }),
                   "and the record is marked not valid, keeping its window");
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


    // --- Codex review of #531: four P1 findings ------------------------------------------
    // 1. A power cycle while the app stays connected resets the counter and the
    //    token; the old zero and envelope must not stay trusted.
    watchdog.mark("power cycle while connected");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000; // the idle poll must not be what saves us
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.powerCycle(); // counter and token reset, the application never disconnected
        const auto motions = rig.device.motionLog().size();
        const auto r = svc->moveTo(100); // admitted from the cached state ...
        if (r.accepted()) {
            MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::ZeroNotSet,
                       "... but the token check before the opcode fails it");
        }
        MIB_EXPECT(rig.device.motionLog().size() == motions, "no motion opcode reached the controller");
        MIB_EXPECT(!svc->snapshot().zeroSet && !rig.store->load(), "the zero and its record are dropped");
    }
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.powerCycle();
        MIB_EXPECT(waitFor([&] { return !svc->snapshot().zeroSet; }), "the idle status poll notices the new power-up");
        MIB_EXPECT(svc->moveTo(0).error == StageError::ZeroNotSet, "moves are refused until the operator sets zero");
    }

    // 2. Dropping the zero (alarm, e-stop, applied profile...) must not hand out a
    //    fresh +/-1000 um window within the same power-up.
    watchdog.mark("the first window survives dropping the zero");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "first zero (window +/-1000 um)");
            MIB_REQUIRE(moveTo(*svc, 900), "to +900 um");
            rig.device.setDriverAlarm(true);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "an alarm drops the zero");
            rig.device.setDriverAlarm(false);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.driverAlarm; }), "alarm cleared");
            // Moved outside the first window while the zero is dropped (a hand move): an
            // undeclared zero there is refused just as if the zero had never been dropped.
            rig.device.setPositionPulses(rig.device.positionPulses() + 5000);
            std::string refusal;
            MIB_EXPECT(svc->setZero(false, &refusal) == StageError::OutOfSoftLimits &&
                           refusal.find("first zero of this power-up") != std::string::npos,
                       "outside the first window with the zero dropped: refused: " + refusal);
            rig.device.setPositionPulses(rig.device.positionPulses() - 5000);
            MIB_REQUIRE(zero(*svc), "the operator sets zero again, here at +900 um of the first frame");
            const auto snap = svc->snapshot();
            MIB_EXPECT(snap.envelopeMinUm == -1000.0 && snap.envelopeMaxUm == 100.0,
                       "only the 100 um left of the first window: [-1000, +100], not another +1000");
            MIB_EXPECT(svc->moveTo(101).error == StageError::OutOfSoftLimits, "+101 um is refused");
            // Same again after an e-stop, and after an applied profile.
            rig.device.setEmergencyStop(true);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "an e-stop drops the zero");
            rig.device.setEmergencyStop(false);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.emergencyStop; }), "e-stop released");
            MIB_REQUIRE(zero(*svc), "zero again");
            MIB_EXPECT(svc->snapshot().envelopeMaxUm <= 100.0, "the window is still the first one");
            MIB_EXPECT(svc->applyProfile() == StageError::None && !svc->snapshot().zeroSet, "profile applied");
            MIB_REQUIRE(zero(*svc), "zero once more");
            MIB_EXPECT(svc->snapshot().envelopeMaxUm <= 100.0, "an applied profile does not reset it either");
        }
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None, "connect");
            MIB_REQUIRE(svc->snapshot().zeroSet, "the zero set last is restored");
            rig.device.setDriverAlarm(true); // drop it once more before the next restart
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "dropped");
            rig.device.setDriverAlarm(false);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.driverAlarm; }), "alarm cleared");
        }
        {
            auto svc = rig.service(); // application restart, same controller power-up
            MIB_REQUIRE(svc->startup() == StageError::None, "restart");
            MIB_EXPECT(!svc->snapshot().zeroSet, "the dropped zero is not restored");
            MIB_REQUIRE(zero(*svc), "zero again after the restart");
            MIB_EXPECT(svc->snapshot().envelopeMaxUm <= 100.0, "a restart does not reset the window either");
        }
        rig.device.powerCycle();
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero after a real power-up");
            MIB_EXPECT(svc->snapshot().envelopeMinUm == -1000.0 && svc->snapshot().envelopeMaxUm == 1000.0,
                       "only a real power-up starts a fresh window");
        }
    }

    // 3. Whatever a crash leaves behind, a reconnect must not restore a stored zero
    //    against a counter that was already rewritten, even if the stale record
    //    could not be deleted (or the interim record could not be written).
    watchdog.mark("crash ordering of Set zero");
    for (const bool failClear : {false, true}) {
        for (const bool failSave : {false, true}) {
            StageRig rig;
            rig.device.setLimits(-20000, 20000);
            auto log = std::make_shared<EventLog>();
            {
                auto svc = rig.service(); // plain store: the first zero, before logging starts
                MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc) && moveTo(*svc, 300), "first zero");
            }
            const auto before = rig.store->load();
            MIB_REQUIRE(before.has_value(), "a record to go stale");
            const std::uint16_t tokenBefore = rig.device.scratch();
            rig.device.setWriteObserver([log](int start, const std::vector<std::uint16_t>& words) {
                if (start == 30054 && words.size() == 1) log->add(StoreEvent{StoreEvent::Token, words[0], std::nullopt});
                if (start == 30059) log->add(StoreEvent{StoreEvent::Position, 0, std::nullopt});
            });
            auto svc = rig.serviceWithStore(rig.config(), std::make_unique<RecordingStore>(rig.store, log, failClear, failSave));
            MIB_REQUIRE(svc->startup() == StageError::None, "reconnect");
            MIB_REQUIRE(svc->snapshot().zeroSet, "the zero was restored");
            const std::string label = std::string(" (clear fails: ") + (failClear ? "yes" : "no") +
                                      ", save fails: " + (failSave ? "yes" : "no") + ")";
            std::string detail;
            const bool done = svc->setZero(false, &detail) == StageError::None;
            const auto events = log->copy();
            bool sawToken = false, sawPosition = false;
            for (const auto& e : events) {
                sawToken = sawToken || e.kind == StoreEvent::Token;
                sawPosition = sawPosition || e.kind == StoreEvent::Position;
            }
            if (failSave) {
                // The interim record cannot be stored: refused before anything changed.
                MIB_EXPECT(!done && !sawToken && !sawPosition, "Set zero is refused with nothing written" + label + ": " + detail);
                MIB_EXPECT(svc->snapshot().zeroSet && rig.device.scratch() == tokenBefore, "the old zero and token stand" + label);
                continue;
            }
            MIB_EXPECT(done && sawToken && sawPosition, "the token and then the counter were written" + label);
            const int unsafe = firstUnsafeRestore(events, before, tokenBefore);
            MIB_EXPECT(unsafe < 0, "no crash point leaves a record that restores against the new counter; first unsafe step " +
                                       std::to_string(unsafe) + label);
            const int lost = firstLostWindow(events, before, tokenBefore);
            MIB_EXPECT(lost < 0, "a reconnect at every crash point still recognises this power-up (window kept); first loss at step " +
                                     std::to_string(lost) + label);
        }
    }

    // A reconnect after a crash that left the interim record (before or after the
    // token write) keeps this power-up's window, restores no zero, and refuses an
    // undeclared re-zero until the frame is re-established.
    watchdog.mark("reconnect over an interim record");
    for (const bool controllerHoldsNew : {false, true}) {
      for (const bool tokenReadFails : {false, true}) {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        }
        auto interim = rig.store->load();
        MIB_REQUIRE(interim.has_value(), "stored");
        const std::uint16_t onController = rig.device.scratch();
        interim->zeroValid = false;
        interim->frameUncertain = true;
        if (controllerHoldsNew) { interim->token = static_cast<std::uint16_t>(onController ^ 0x55); interim->nextToken = onController; }
        else { interim->nextToken = static_cast<std::uint16_t>(onController ^ 0x55); }
        rig.store->save(*interim);
        if (tokenReadFails) rig.device.failTokenReads(1); // then the poll has to decide
        auto svc = rig.service();
        const std::string label = std::string(controllerHoldsNew ? " (controller already holds the new token" : " (controller still holds the old token") +
                                  (tokenReadFails ? ", first token read fails)" : ")");
        MIB_REQUIRE(svc->startup() == StageError::None, "restart" + label);
        if (tokenReadFails) sleepMs(150); // several idle polls: the same power-up is recognised, not reset
        MIB_EXPECT(!svc->snapshot().zeroSet, "no zero is restored from an interim record" + label);
        MIB_EXPECT(rig.store->load().has_value(), "but the record, and so the window, is kept" + label);
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::OutOfSoftLimits && detail.find("interrupted") != std::string::npos,
                   "an undeclared zero is refused: the frame is uncertain" + label + ": " + detail);
        MIB_EXPECT(svc->setZero(true, &detail) == StageError::None, "a declaration re-establishes it" + label);
      }
    }

    // 4. A fault that appears after the cached admission must stop the opcode.
    watchdog.mark("fresh status before the opcode");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000; // the cached status stays "all clear"
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setEmergencyStop(true);
        const auto motions = rig.device.motionLog().size();
        const auto r = svc->moveTo(100);
        MIB_REQUIRE(r.accepted(), "admitted from the stale cache");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::EmergencyStop,
                   "the fresh status fails it with EmergencyStop");
        MIB_EXPECT(rig.device.motionLog().size() == motions, "no motion opcode was sent");
        MIB_EXPECT(!svc->snapshot().zeroSet, "and the fresh e-stop dropped the zero");
    }
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000;
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.setDriverAlarm(true);
        const auto motions = rig.device.motionLog().size();
        const auto r = svc->moveTo(100);
        MIB_REQUIRE(r.accepted(), "admitted from the stale cache");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::DriverAlarm,
                   "DriverAlarm");
        MIB_EXPECT(rig.device.motionLog().size() == motions && !svc->snapshot().zeroSet, "nothing sent, zero dropped");
    }
    {
        StageRig rig;
        rig.device.setPulsesPerSecond(2000);
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000;
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        rig.device.startExternalMove(2000); // someone else started the axis after the last poll
        const auto motions = rig.device.motionLog().size();
        const auto r = svc->moveTo(100);
        MIB_REQUIRE(r.accepted(), "admitted from the stale cache");
        MIB_EXPECT(finish(*svc, r.id) == OpState::Failed && svc->operation(r.id)->error == StageError::Busy,
                   "an axis that is already moving is not given another opcode");
        MIB_EXPECT(rig.device.motionLog().size() == motions, "no motion opcode was sent");
    }

    // --- Codex re-review of #531 ---------------------------------------------------------
    // A Stop that completes while the operation is still reading status and token
    // must be seen before the opcode.
    watchdog.mark("stop during the pre-opcode reads");
    {
        StageRig rig;
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
        MIB_REQUIRE(moveTo(*svc, 10), "a first move, so the speed is applied and the next one starts straight at its status read");
        rig.device.setReplyDelayMs(100); // every driver call now takes ~100 ms (the rig times out at 150)
        const auto motions = rig.device.motionLog().size();
        const auto r = svc->moveTo(100);
        MIB_REQUIRE(r.accepted(), "move accepted");
        sleepMs(160);             // past the first status read, inside moveAndWait's fresh status and token reads
        MIB_EXPECT(svc->stop() == StageError::None, "Stop (it is served ahead of the operation's next command)");
        MIB_EXPECT(finish(*svc, r.id, 20000) == OpState::Cancelled, "the move ended Cancelled");
        MIB_EXPECT(rig.device.motionLog().size() == motions, "and no motion opcode ever reached the controller");
        rig.device.setReplyDelayMs(0);
    }

    // A failed save of "the zero was dropped" must not let a restart restore it.
    watchdog.mark("failed invalidation save");
    {
        StageRig rig;
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, false);
        std::uint16_t tokenAtZero = 0;
        {
            auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "Set zero");
            tokenAtZero = rig.device.scratch();
            recording->setFailSave(true); // the disk stops taking writes
            rig.device.setDriverAlarm(true);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "an alarm drops the zero");
            MIB_EXPECT(waitFor([&] { return rig.device.scratch() != tokenAtZero; }),
                       "the failed save is made up for by rotating the controller token");
            rig.device.setDriverAlarm(false);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.driverAlarm; }), "alarm cleared");
        }
        auto svc = rig.service(); // restart over the (stale, still 'valid') stored record
        MIB_REQUIRE(svc->startup() == StageError::None, "restart");
        MIB_EXPECT(!svc->snapshot().zeroSet, "the faulted zero is not resurrected");
    }

    // The 'fresh' token must differ from the one it replaces, always.
    watchdog.mark("token never repeats");
    {
        StageRig rig;
        auto svc = rig.service();
        int calls = 0;
        svc->setTokenSourceForTest([&] { return static_cast<std::uint16_t>(++calls < 6 ? 777 : 888); });
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "first zero");
        MIB_EXPECT(rig.device.scratch() == 777, "the first token");
        calls = 0; // the source repeats 777 five times before offering something else
        MIB_REQUIRE(zero(*svc), "second zero");
        MIB_EXPECT(rig.device.scratch() == 888, "a repeated token is redrawn");
    }
    {
        StageRig rig;
        auto svc = rig.service();
        svc->setTokenSourceForTest([] { return static_cast<std::uint16_t>(777); }); // a source that never varies
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "first zero");
        MIB_REQUIRE(zero(*svc), "second zero");
        MIB_EXPECT(rig.device.scratch() != 777 && rig.device.scratch() != 0, "even then the token changes, and is never 0");
    }

    // power_up_token_register 0 is acceptance-only and visible.
    watchdog.mark("session-only zero is refused and visible");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.reference.powerUpTokenRegister = 0;
        auto svc = rig.service(cfg); // setConfig refuses it without the explicit flag
        MIB_EXPECT(svc->config().reference.powerUpTokenRegister != 0, "the unsafe config was not applied");
        cfg.reference.allowSessionOnlyZero = true;
        MIB_EXPECT(svc->setConfig(cfg), "with the explicit acceptance flag it is accepted");
        MIB_REQUIRE(svc->startup() == StageError::None, "connect");
        MIB_EXPECT(svc->snapshot().sessionOnlyZero, "the snapshot says power cycles are not detected");
        auto normal = rig.service();
        MIB_REQUIRE(normal->startup() == StageError::None, "connect");
        MIB_EXPECT(!normal->snapshot().sessionOnlyZero, "and only then");
    }

    // An unreadable token at reconnect is unknown, not a power cycle.
    watchdog.mark("transient token read failure at reconnect");
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc, true), "zero with mid-travel declared");
        }
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000; // keep the zero unverified for the checks below
        rig.device.failTokenReads(1);
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "connect with one failing token read");
        MIB_EXPECT(rig.store->load().has_value(), "the stored record is kept");
        MIB_EXPECT(!svc->snapshot().zeroSet, "but not trusted yet");
        MIB_EXPECT(svc->moveTo(0).error == StageError::ZeroNotSet, "moves are refused meanwhile");
        // The next successful check (here: Set zero resolves it first) decides.
        std::string detail;
        MIB_EXPECT(zero(*svc, true, &detail), "Set zero resolves the unverified token first: " + detail);
    }
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc, true), "zero");
        }
        rig.device.failTokenReads(1);
        auto svc = rig.service(); // fast idle poll
        MIB_REQUIRE(svc->startup() == StageError::None, "connect with one failing token read");
        MIB_EXPECT(waitFor([&] { return svc->snapshot().zeroSet; }), "the next poll confirms the same power-up and restores the zero");
        MIB_EXPECT(svc->snapshot().midTravelDeclared, "with its declaration");
    }
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        }
        rig.device.powerCycle();
        rig.device.failTokenReads(1);
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "connect after a power cycle, one failing read");
        MIB_EXPECT(waitFor([&] { return !rig.store->load().has_value(); }), "the next poll sees the mismatch and drops the record");
        MIB_EXPECT(!svc->snapshot().zeroSet, "no zero");
    }

    // --- third Codex review of #531: persistence, fail closed ----------------------------
    // 3. The first Set zero with no record must not change anything when the record
    //    cannot be stored.
    watchdog.mark("first zero with an unwritable store");
    {
        StageRig rig;
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, true); // every save fails
        auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
        MIB_REQUIRE(svc->startup() == StageError::None, "connect");
        const auto counter = rig.device.counterPulses();
        const int writes = rig.device.writes();
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) != StageError::None, "refused: " + detail);
        MIB_EXPECT(rig.device.writes() == writes && rig.device.scratch() == 0 && rig.device.counterPulses() == counter,
                   "no token, no counter write: nothing changed");
        MIB_EXPECT(!svc->snapshot().zeroSet && svc->moveTo(0).error == StageError::ZeroNotSet, "and no motion is enabled");
    }

    // 3b. A final record that cannot be stored leaves nothing trusted.
    watchdog.mark("final record unwritable");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, false);
        auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
        MIB_REQUIRE(svc->startup() == StageError::None, "connect");
        // The interim record is stored, then the disk fails before the final one:
        rig.device.setWriteObserver([&](int start, const std::vector<std::uint16_t>&) {
            if (start == 30059) recording->setFailSave(true);
        });
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) != StageError::None && detail.find("could not be stored") != std::string::npos,
                   "the result is a failure, not a success: " + detail);
        recording->setFailSave(false);
        rig.device.setWriteObserver(nullptr); // it points into the store, which the next line's reset destroys
        MIB_EXPECT(!svc->snapshot().zeroSet && svc->moveTo(0).error == StageError::ZeroNotSet, "nothing is trusted");
        // After a reconnect the stored interim record (first zero: token 0, next token = the
        // controller's) is recognised: the declaration is still required.
        svc.reset();
        auto again = rig.service();
        MIB_REQUIRE(again->startup() == StageError::None, "reconnect");
        MIB_EXPECT(!again->snapshot().zeroSet, "no zero is restored");
        MIB_EXPECT(rig.store->load().has_value(), "the interim record is kept");
        std::string refusal;
        MIB_EXPECT(again->setZero(false, &refusal) == StageError::OutOfSoftLimits && refusal.find("interrupted") != std::string::npos,
                   "an undeclared zero is still refused after the reconnect: " + refusal);
        MIB_EXPECT(again->setZero(true, &refusal) == StageError::None, "a declaration re-establishes it");
    }

    // 4. After a token rotation the replacement record keeps being retried, so the
    //    window survives a store that comes back.
    watchdog.mark("rotation record retried");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, false);
        std::uint16_t tokenAtZero = 0;
        {
            auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc) && moveTo(*svc, 900), "zero, then +900 um");
            tokenAtZero = rig.device.scratch();
            recording->setFailSave(true);
            rig.device.setDriverAlarm(true);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "an alarm drops the zero");
            MIB_REQUIRE(waitFor([&] { return rig.device.scratch() != tokenAtZero; }), "the token is rotated");
            sleepMs(120); // several polls with the store still down: it must keep trying
            rig.device.setDriverAlarm(false);
            recording->setFailSave(false); // the disk comes back
            MIB_EXPECT(waitFor([&] {
                           const auto k = rig.store->load();
                           return k && !k->zeroValid && k->token == rig.device.scratch();
                       }),
                       "the replacement record (new token, window kept, not valid) is stored once the disk is back");
        }
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "restart");
        MIB_EXPECT(!svc->snapshot().zeroSet, "no zero is restored");
        MIB_REQUIRE(zero(*svc), "an undeclared zero inside the old window");
        MIB_EXPECT(svc->snapshot().envelopeMaxUm <= 101.0, "the window of this power-up survived the outage");
    }

    // 5. A token write that timed out may have been applied: keep accepting the new
    //    token, trust nothing, and do not treat the next reconnect as a power cycle.
    watchdog.mark("unacknowledged token write");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        std::uint16_t before = 0;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
            before = rig.device.scratch();
            rig.device.dropWriteAcks(30054, 8); // applied, never acknowledged, on every attempt
            std::string detail;
            MIB_EXPECT(svc->setZero(false, &detail) != StageError::None, "Set zero reports the failure: " + detail);
            MIB_EXPECT(rig.device.scratch() != before, "although the controller did take the new token");
            MIB_EXPECT(!svc->snapshot().zeroSet, "nothing is trusted meanwhile");
            rig.device.dropWriteAcks(30054, 0);
        }
        auto svc = rig.service();
        MIB_REQUIRE(svc->startup() == StageError::None, "reconnect");
        MIB_EXPECT(rig.store->load().has_value(), "the record survives: the new token is not a power cycle");
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::OutOfSoftLimits && detail.find("interrupted") != std::string::npos,
                   "an undeclared zero is refused (window kept, frame uncertain): " + detail);
        MIB_EXPECT(svc->setZero(true, &detail) == StageError::None, "a declaration re-establishes it");
    }

    // 6. A fault seen while the stored zero is not trusted yet must still cost it.
    watchdog.mark("fault while the token is unknown");
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        }
        auto cfg = rig.config();
        cfg.pollIdleMs = 40;
        rig.device.failTokenReads(10); // the token stays unknown for ~10 polls
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "connect with the token unreadable");
        rig.device.setDriverAlarm(true);
        MIB_REQUIRE(waitFor([&] { return svc->snapshot().status.driverAlarm; }), "the alarm is seen");
        rig.device.setDriverAlarm(false); // and clears before the token can be checked
        MIB_REQUIRE(waitFor([&] { return !svc->snapshot().status.driverAlarm; }), "alarm cleared");
        sleepMs(700); // the token reads recover
        MIB_EXPECT(!svc->snapshot().zeroSet, "the zero is not restored once the token matches again");
        const auto k = rig.store->load();
        MIB_EXPECT(k.has_value() && !k->zeroValid, "its record says so, keeping the window");
    }

    // Final review: a token rotation whose replacement record never reached the disk must
    // not read as a power cycle after a restart.
    watchdog.mark("rotation without a stored replacement, then restart");
    {
        StageRig rig;
        rig.device.setLimits(-20000, 20000);
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, false);
        std::uint16_t tokenAtZero = 0;
        {
            auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc) && moveTo(*svc, 900), "zero, then +900 um");
            tokenAtZero = rig.device.scratch();
            recording->setFailSave(true); // and the disk never comes back
            rig.device.setDriverAlarm(true);
            MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "an alarm drops the zero");
            MIB_REQUIRE(waitFor([&] { return rig.device.scratch() != tokenAtZero; }), "the token is rotated");
        }
        rig.device.setDriverAlarm(false);
        auto svc = rig.service(); // restart over the stale record; the controller token is non-zero and new
        MIB_REQUIRE(svc->startup() == StageError::None, "restart");
        MIB_EXPECT(!svc->snapshot().zeroSet, "no zero is restored");
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::OutOfSoftLimits, "an undeclared zero is refused: this is not a power cycle: " + detail);
        MIB_EXPECT(svc->setZero(true, &detail) == StageError::None, "a declaration is the way out");
        // A real power cycle (the register reads 0) is a new power-up with a fresh window.
        svc.reset();
        rig.device.powerCycle();
        auto fresh = rig.service();
        MIB_REQUIRE(fresh->startup() == StageError::None && zero(*fresh), "zero after a power cycle needs no declaration");
        MIB_EXPECT(fresh->snapshot().envelopeMaxUm == 1000.0, "and gets a fresh window");
    }

    // A deletion that failed earlier and is still pending must not erase a record
    // that Set zero saved afterwards.
    watchdog.mark("pending deletion does not erase a newer record");
    {
        StageRig rig;
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, true, false); // every deletion fails
        auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        rig.device.powerCycle(); // the next poll drops the record: the deletion fails and stays pending
        MIB_REQUIRE(waitFor([&] { return !svc->snapshot().zeroSet; }), "the poll sees the new power-up");
        sleepMs(120); // several retries of the deletion, all failing
        MIB_REQUIRE(zero(*svc), "the operator sets zero in the new power-up (saves a newer record)");
        recording->setFailClear(false); // the disk recovers: a retry of the old deletion would now succeed
        sleepMs(250);                    // many polls
        const auto kept = rig.store->load();
        MIB_EXPECT(kept.has_value() && kept->zeroValid && kept->token == rig.device.scratch(),
                   "the newer record survives the retry");
        svc.reset();
        auto again = rig.service();
        MIB_REQUIRE(again->startup() == StageError::None, "reconnect");
        MIB_EXPECT(again->snapshot().zeroSet, "and the zero is restored without a declaration");
    }

    // Final review: Set zero publishes the status it reads, so a fault that is gone by
    // the next poll still costs the existing zero.
    watchdog.mark("set zero sees a fault");
    {
        StageRig rig;
        auto cfg = rig.config();
        cfg.pollIdleMs = 60000; // no poll will notice
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        rig.device.setEmergencyStop(true);
        std::string detail;
        MIB_EXPECT(svc->setZero(false, &detail) == StageError::EmergencyStop, "refused during the e-stop");
        rig.device.setEmergencyStop(false); // gone before any poll
        MIB_EXPECT(!svc->snapshot().zeroSet, "but the old zero is already dropped");
        MIB_EXPECT(!svc->moveTo(0).accepted(), "and no motion can use it");
    }

    // Final review: file I/O never runs under the service lock, so a slow store cannot
    // hold stop() or snapshot().
    watchdog.mark("slow store does not block stop");
    {
        StageRig rig;
        auto log = std::make_shared<EventLog>();
        auto* recording = new RecordingStore(rig.store, log, false, false);
        auto svc = rig.serviceWithStore(rig.config(), std::unique_ptr<IStageReferenceStore>(recording));
        MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        recording->setClearDelayMs(1500);
        rig.device.powerCycle(); // the next poll drops the record: a deletion that takes 1.5 s
        MIB_REQUIRE(waitFor([&] { return recording->inClear(); }, 4000), "the slow deletion started");
        const auto t0 = std::chrono::steady_clock::now();
        svc->stop();
        const auto snap = svc->snapshot();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        MIB_EXPECT(ms < 400, "stop() and snapshot() did not wait for the deletion (" + std::to_string(ms) + " ms)");
        MIB_EXPECT(!snap.zeroSet, "the zero is already dropped in memory");
        recording->setClearDelayMs(0);
    }

    // 6b. The same when the fault is already there at connect (the first poll comes later).
    watchdog.mark("fault at connect while the token is unknown");
    {
        StageRig rig;
        {
            auto svc = rig.service();
            MIB_REQUIRE(svc->startup() == StageError::None && zero(*svc), "zero");
        }
        auto cfg = rig.config();
        cfg.pollIdleMs = 300; // the first poll comes after the alarm is gone
        rig.device.setDriverAlarm(true);
        rig.device.failTokenReads(1);
        auto svc = rig.service(cfg);
        MIB_REQUIRE(svc->startup() == StageError::None, "connect under an alarm with the token unreadable");
        rig.device.setDriverAlarm(false);
        sleepMs(900); // polls verify the token; the alarm is long gone
        MIB_EXPECT(!svc->snapshot().zeroSet, "the zero is not restored after a fault seen at connect");
        const auto k = rig.store->load();
        MIB_EXPECT(k.has_value() && !k->zeroValid, "and its record says so");
    }

    // 7. A record file that fails at flush or close time is not renamed into place.
#if defined(__linux__)
    watchdog.mark("file store close failure");
    {
        mib::test::TempDir dir("stage_store");
        const auto path = dir.path() / "stage_reference.json";
        FileStageReferenceStore store(path.string());
        StageReferenceRecord r;
        r.controllerSerial = "26017"; r.token = 5; r.windowMinUm = -1000; r.windowMaxUm = 1000;
        MIB_EXPECT(store.save(r), "a normal save succeeds");
        const auto loaded = store.load();
        MIB_EXPECT(loaded && loaded->token == 5 && loaded->zeroValid && !loaded->frameUncertain, "and reads back");
        std::filesystem::remove(path);
        std::error_code ec;
        std::filesystem::create_symlink("/dev/full", path.string() + ".tmp", ec); // writes fail with ENOSPC at flush
        if (!ec) {
            MIB_EXPECT(!store.save(r), "a flush-time failure is reported");
            MIB_EXPECT(!std::filesystem::exists(path) && !store.load().has_value(), "and nothing was renamed into place");
        }
    }
#endif

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
        // Wait for the axis to really move: since each leg re-reads status and the token before its
        // opcode, a fixed sleep is not enough on a slow runner (~28 ms per transaction on Windows).
        MIB_REQUIRE(waitFor([&] { return rig.device.moving(); }, 5000), "the move has started");

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
