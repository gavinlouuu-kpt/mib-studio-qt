// stage_limit_verification_test
//
// The supervised limit-switch check (#464, ADR 0013 §6) that Home requires,
// against the fake ZC300. It is operator-paced and must stay safe on a stage
// whose switches are unwired or swapped:
//  - nothing moves until the operator confirms a direction; declining at
//    either prompt aborts with the axis stopped;
//  - motion is controller-bounded steps of <= 500 um at the slow speed, total
//    travel toward each switch <= the cap, even when the host stalls;
//  - Enter / Ctrl-C (the cancel callback) stops the axis mid-step;
//  - a missing switch, swapped switches or a wrong span fail;
//  - success returns to the start position; the record store round-trips.

#include "backend/services/SerialBus.h"
#include "backend/stage/LimitVerification.h"
#include "backend/stage/zc300/Zc300Stage.h"

#include "support/assert.h"
#include "support/fake_zc300.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>

using namespace backend::stage;
using backend::services::serialbus::SerialBusManager;
using mib::test::FakeZc300;
using mib::test::FakeZc300Port;

namespace {

constexpr double kUmPerPulse = 0.4375;

struct Bench {
    FakeZc300 device;
    SerialBusManager bus;
    std::unique_ptr<zc300::Zc300Stage> stage;

    Bench()
    {
        bus.setSerialPortFactory([this] { return std::make_unique<FakeZc300Port>(device); });
        zc300::Zc300Stage::Timing timing;
        timing.transactionMs = 200;
        stage = std::make_unique<zc300::Zc300Stage>(bus, timing);
        StageEndpoint e;
        e.systemPort = device.portName;
        StageIdentity id;
        std::string detail;
        MIB_REQUIRE(stage->connect(e, tbzf6_60Profile(), id, detail) == StageError::None, "connect");
    }
};

LimitVerificationOptions options(int* prompts = nullptr, int declineAt = 0)
{
    LimitVerificationOptions o;
    o.pollMs = 5;
    o.confirm = [prompts, declineAt](const std::string&) {
        if (!prompts) return true;
        ++*prompts;
        return *prompts != declineAt;
    };
    return o;
}

bool stepsBounded(FakeZc300& d)
{
    const auto ops = d.motionLog();
    const auto dist = d.motionDistances();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] == 0x66) return false;                        // never a jog
        if (ops[i] == 0x65 && dist[i] > 0.50001f) return false;  // <= 500 um per step
    }
    return true;
}

bool allAtSpeed(FakeZc300& d, float mmPerS)
{
    for (const float s : d.motionSpeeds())
        if (std::abs(s - mmPerS) > 1e-4f) return false;
    return true;
}

} // namespace

int main()
{
    mib::test::Watchdog watchdog(120);

    watchdog.mark("success");
    {
        Bench b;
        b.device.setPositionPulses(1000); // the operator's focus position
        int prompts = 0;
        const auto r = verifyLimits(*b.stage, options(&prompts));
        MIB_EXPECT(r.passed(), "check passes: " + r.detail);
        MIB_EXPECT(prompts == 2, "one confirmation per direction");
        MIB_EXPECT(std::abs(r.spanUm - 13716 * kUmPerPulse) < 1.0, "span limit to limit");
        MIB_EXPECT(r.returnedToStart && std::abs(b.device.positionPulses() - 1000) <= 2, "back at the start");
        MIB_EXPECT(stepsBounded(b.device), "only bounded steps of <= 500 um, never a jog");
        MIB_EXPECT(allAtSpeed(b.device, 0.2f), "everything at the 200 um/s check speed");
        MIB_EXPECT(b.device.motionLog().size() >= 13, "moved in many small steps");
    }

    watchdog.mark("declined");
    {
        Bench b;
        int prompts = 0;
        const auto r = verifyLimits(*b.stage, options(&prompts, 1));
        MIB_EXPECT(r.cancelled && !r.passed(), "declining the first prompt aborts");
        MIB_EXPECT(b.device.motionLog().empty() && b.device.positionPulses() == 0, "nothing moved");
    }
    {
        Bench b;
        int prompts = 0;
        const auto r = verifyLimits(*b.stage, options(&prompts, 2));
        MIB_EXPECT(r.cancelled && !r.passed(), "declining the second prompt aborts");
        MIB_EXPECT(!b.device.moving() && b.device.positionPulses() == -6858, "stopped at the negative switch");
    }
    {
        Bench b;
        b.device.setPulsesPerSecond(457); // ~200 um/s, as on the bench
        std::atomic<int> polls{0};
        auto o = options();
        o.cancelled = [&] { return ++polls > 30; }; // Enter pressed mid-step
        const auto r = verifyLimits(*b.stage, o);
        MIB_EXPECT(r.cancelled && !r.passed(), "Enter / Ctrl-C cancels");
        MIB_EXPECT(!b.device.moving(), "the axis is stopped");
        MIB_EXPECT(std::abs(b.device.positionPulses()) < 1143, "stopped within the first step");
    }

    watchdog.mark("missing switch, stalled host");
    {
        Bench b;
        b.device.setLimits(-40000, 40000); // unwired switches
        b.device.setPulsesPerSecond(20000);
        b.device.setReplyDelayMs(80);
        const auto r = verifyLimits(*b.stage, options());
        MIB_EXPECT(r.error == StageError::LimitCheckFailed && r.detail.find("wiring") != std::string::npos,
                   "missing switch fails: " + r.detail);
        MIB_EXPECT(!b.device.moving() && std::abs(b.device.positionPulses()) <= 14858,
                   "total travel stayed within the 6500 um cap");
        MIB_EXPECT(stepsBounded(b.device), "bounded steps only");
    }

    watchdog.mark("swapped switches");
    {
        Bench b;
        b.device.setSwapLimitBits(true); // the controller runs past the physical switch
        const auto r = verifyLimits(*b.stage, options());
        MIB_EXPECT(r.error == StageError::LimitCheckFailed && r.detail.find("swapped") != std::string::npos,
                   "swapped switches fail: " + r.detail);
        MIB_EXPECT(!b.device.moving() && b.device.positionPulses() >= -6858 - 1143 - 1,
                   "overran the physical switch by at most one step");
    }

    watchdog.mark("span mismatch");
    {
        Bench b;
        b.device.setLimits(-4000, 4000);
        const auto r = verifyLimits(*b.stage, options());
        MIB_EXPECT(r.error == StageError::LimitCheckFailed && r.detail.find("span") != std::string::npos,
                   "wrong span fails: " + r.detail);
        MIB_EXPECT(!b.device.moving(), "stopped");
    }

    watchdog.mark("record store");
    {
        mib::test::TempDir td("limits_store");
        LimitsVerificationStore store((td.path() / "stage_limits_verified.json").string());
        MIB_EXPECT(!store.find("26017"), "empty store");
        MIB_EXPECT(store.save({"26017", "2026-10-06T10:00:00Z", -3000, 3000, 6000, "zc300ctl verify-limits"}), "save");
        MIB_EXPECT(store.save({"30001", "2026-10-06T11:00:00Z", -2990, 3010, 6000, "zc300ctl verify-limits"}),
                   "save a second controller");
        const auto a = store.find("26017");
        MIB_EXPECT(a && a->spanUm == 6000 && a->procedure == "zc300ctl verify-limits", "first record kept");
        MIB_EXPECT(store.find("30001").has_value() && !store.find("99999"), "per-controller lookup");
        std::ofstream(store.path()) << "{ not json";
        MIB_EXPECT(!store.find("26017"), "a corrupt file verifies nothing");
    }

    if (mib::test::exitCode() == 0) std::printf("limit verification verified\n");
    return mib::test::exitCode();
}
