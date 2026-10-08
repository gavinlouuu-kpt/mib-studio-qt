// stage_bridge_facade_test
//
// Z stage commands through BackendFacade (#464, ADR 0013 Amendment 1), over a
// real AppBackend with a fake ZC300 on the shared serial bus. The stage is never
// homed. Pins the safety rules the backend enforces whatever the shell does:
//  1. MoveTo/MoveBy are refused until the operator set zero this power-up, and
//     outside the travel envelope around it (+/-1000 um, +/-2900 um once the
//     operator declares mid-travel);
//  2. SetZero is only ever the explicit command and moves nothing: Connect,
//     ApplyProfile and discovery (identity-only FC04) never move or write the
//     position, and no facade path can write the limits-verified record (it
//     only clears a badge and gates nothing);
//  3. Stop is always accepted, also during an experiment;
//  4. every other stage command, SetZero included, is refused while an
//     experiment is active.
// Plus: moves are tracked operations whose facade cancel stops the axis, and a
// pump cannot claim the stage's bus address.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "backend/services/SerialBus.h"
#include "backend/services/StageService.h"
#include "backend/stage/LimitVerification.h"

#include "support/assert.h"
#include "support/fake_zc300.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace bridge = backend::bridge;
using bridge::BackendOperationState;
using bridge::StageCommand;
using bridge::StageCommandAction;
using mib::test::FakeZc300;
using mib::test::FakeZc300Port;

namespace {

// Terminal operation states from the facade event stream.
struct OperationLog {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::uint64_t, BackendOperationState> terminal;

    void onEvent(const bridge::BackendEvent& event)
    {
        const auto* op = std::get_if<bridge::OperationStatusEvent>(&event);
        if (!op || op->state == BackendOperationState::Started || op->state == BackendOperationState::Progress) return;
        std::lock_guard<std::mutex> lock(mutex);
        terminal[op->operationId] = op->state;
        cv.notify_all();
    }
    std::optional<BackendOperationState> wait(std::uint64_t id, int ms = 10000)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return terminal.count(id) != 0; });
        const auto it = terminal.find(id);
        if (it == terminal.end()) return std::nullopt;
        return it->second;
    }
};

bridge::BackendCommandResult stage(bridge::BackendFacade& facade, StageCommandAction action, double um = 0.0,
                                   const std::string& port = {}, bool midTravel = false)
{
    StageCommand cmd;
    cmd.action = action;
    cmd.targetUm = um;
    cmd.portName = port;
    cmd.midTravel = midTravel;
    return facade.dispatch(cmd);
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

int motionOpcodes(FakeZc300& d) { return d.opcodeCount(0x64) + d.opcodeCount(0x65) + d.opcodeCount(0x66); }

} // namespace

int main()
{
    mib::test::Watchdog watchdog(120);
#if defined(_WIN32)
    _putenv_s("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");
#else
    setenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json", 1);
#endif
    mib::test::TempDir td("stage_bridge_facade");
    const auto frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 16, 96, 96), "mock frames");

    FakeZc300 device;
    device.setPositionPulses(1234); // wherever the operator left it
    backend::AppBackend app;
    bridge::BackendFacade facade(app);
    OperationLog ops;
    facade.setEventSink([&ops](const bridge::BackendEvent& e) { ops.onEvent(e); });
    MIB_REQUIRE(facade.initialize((td.path() / "data").string()), "facade initializes");
    app.serialBus().setSerialPortFactory([&device] { return std::make_unique<FakeZc300Port>(device); });

    // --- 3. Stop is accepted even with no stage -----------------------------
    watchdog.mark("stop before connect");
    MIB_EXPECT(stage(facade, StageCommandAction::Stop).ok, "Stop accepted with no stage connected");
    MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 0).ok, "MoveTo refused with no stage");

    // --- 2. Discovery is identity-only --------------------------------------
    watchdog.mark("discovery");
    {
        backend::discovery::DiscoveryRequest request;
        request.kinds = {backend::discovery::DeviceKind::MotionStage};
        backend::discovery::SerialScanScope scope;
        scope.portName = device.portName;
        scope.addressFrom = 1;
        scope.addressTo = 2;
        scope.perAddressTimeoutMs = 150;
        request.serialScope = scope;
        request.origin = "stage-test";
        const auto started = app.deviceDiscovery().startDiscovery(request);
        MIB_REQUIRE(started.accepted, "discovery job accepted");
        MIB_REQUIRE(app.deviceDiscovery().waitForTerminal(started.jobId, std::chrono::seconds(10)), "job ends");
        const auto snap = app.deviceDiscovery().discoverySnapshot(started.jobId);
        MIB_REQUIRE(snap.candidates.size() == 1, "one ZC300 found (address 2 is silent)");
        const auto& d = snap.candidates.front();
        MIB_EXPECT(d.identification == backend::discovery::IdentificationStatus::Identified &&
                       d.stableIdentity == "zc300:26017" &&
                       d.identityStrength == backend::discovery::IdentityStrength::Persistent,
                   "identified by the controller's serial, not the adapter");
        MIB_EXPECT(device.writes() == 0 && motionOpcodes(device) == 0 && device.positionPulses() == 1234,
                   "discovery wrote nothing and moved nothing");
    }

    // --- 2. Connect is observe-only; 1. no motion before zero is set -----------
    watchdog.mark("connect");
    {
        const auto r = stage(facade, StageCommandAction::Connect, 0, device.portName);
        MIB_REQUIRE(r.ok, "Connect: " + r.message);
        bridge::BackendStageStatus st;
        MIB_REQUIRE(facade.fetchStageStatus(st), "status");
        MIB_EXPECT(st.connected && st.configured && !st.zeroSet && st.serial == "26017", "connected, zero not set");
        MIB_EXPECT(device.writes() == 0 && device.positionPulses() == 1234, "Connect wrote nothing, moved nothing");
        MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 100).ok, "MoveTo refused before zero is set");
        MIB_EXPECT(!stage(facade, StageCommandAction::MoveBy, 5).ok, "MoveBy refused before zero is set");
        MIB_EXPECT(device.writes() == 0, "refusals wrote nothing");
        MIB_EXPECT(!stage(facade, StageCommandAction::Connect, 0, device.portName).ok, "second Connect refused");
    }

    watchdog.mark("apply profile never moves");
    {
        const auto r = stage(facade, StageCommandAction::ApplyProfile);
        MIB_EXPECT(r.ok, "ApplyProfile: " + r.message);
        MIB_EXPECT(motionOpcodes(device) == 0 && device.positionPulses() == 1234, "ApplyProfile moved nothing");
        MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 0).ok, "still no motion before zero is set");
    }

    // --- Set zero (explicit), the envelope, tracked operations ----------------
    watchdog.mark("limit record gates nothing");
    const auto limitsRecord = td.path() / "data" / "stage_limits_verified.json";
    {
        MIB_EXPECT(stage(facade, StageCommandAction::Stop).ok, "Stop accepted");
        MIB_EXPECT(!std::filesystem::exists(limitsRecord),
                   "no facade path (discovery, connect, apply profile, Set zero, stop) wrote the record");
        bridge::BackendStageStatus st;
        facade.fetchStageStatus(st);
        MIB_EXPECT(!st.limitsVerified, "status reports unverified limits (a badge only)");
    }

    watchdog.mark("set zero");
    {
        const int writesBeforeZero = device.writes();
        const int savesBefore = device.opcodeCount(0x6D);
        const auto motionsBefore = motionOpcodes(device);
        const auto atZero = device.positionPulses();
        const auto zero = stage(facade, StageCommandAction::SetZero);
        MIB_REQUIRE(zero.ok && zero.operationId == 0, "Set zero accepted with unverified limits: " + zero.message);
        MIB_EXPECT(device.writes() > writesBeforeZero && device.opcodeCount(0x6D) == savesBefore, "it wrote the position counter");
        MIB_EXPECT(motionOpcodes(device) == motionsBefore && device.positionPulses() == atZero,
                   "Set zero moved nothing");
        bridge::BackendStageStatus st;
        facade.fetchStageStatus(st);
        MIB_EXPECT(st.zeroSet && !st.midTravelDeclared && st.envelopeMinUm == -1000.0 && st.envelopeMaxUm == 1000.0,
                   "zero set with the default +/-1000 um envelope");
        MIB_EXPECT(device.counterPulses() == 0, "the counter reads 0 here");
        // What `zc300ctl verify-limits --supervised` writes on the bench: a
        // badge, it widens nothing.
        backend::stage::LimitsVerificationStore(limitsRecord.string())
            .save({"26017", "2026-10-06T12:00:00Z", -3000.0, 3000.0, 6000.0, "zc300ctl verify-limits"});
        MIB_EXPECT(waitFor([&] { facade.fetchStageStatus(st); return st.limitsVerified; }, std::chrono::seconds(2)),
                   "the verified record clears the badge");
        MIB_EXPECT(st.envelopeMaxUm == 1000.0, "and does not widen the envelope");

        MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 1001).ok, "beyond the envelope refused");
        MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 3000).ok, "far beyond refused");
        MIB_EXPECT(!stage(facade, StageCommandAction::MoveTo, 12.5).ok, "off-grid refused");
        const auto move = stage(facade, StageCommandAction::MoveTo, 500);
        MIB_REQUIRE(move.ok && move.operationId != 0, "MoveTo 500 um");
        MIB_EXPECT(ops.wait(move.operationId) == BackendOperationState::Completed, "move completed");
        facade.fetchStageStatus(st);
        MIB_EXPECT(std::abs(st.positionUm - 500) < 0.5, "at +500 um");

        device.setPulsesPerSecond(2000);
        const auto slow = stage(facade, StageCommandAction::MoveTo, -900);
        MIB_REQUIRE(slow.ok, "slow move");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bridge::OperationCommand cancel;
        cancel.operationId = slow.operationId;
        MIB_EXPECT(facade.dispatch(cancel).ok, "facade cancel accepted");
        MIB_EXPECT(ops.wait(slow.operationId) == BackendOperationState::Cancelled, "move cancelled");
        MIB_EXPECT(waitFor([&] { return !device.moving(); }, std::chrono::seconds(2)), "cancel stopped the axis");
        device.setPulsesPerSecond(200000);

        // The bridge runs one command at a time and Stop needs the same lock:
        // Disconnect and ApplyProfile must not wait for a running move.
        device.setPulsesPerSecond(2000);
        const auto running = stage(facade, StageCommandAction::MoveTo, 900);
        MIB_REQUIRE(running.ok, "slow move for the lock check");
        // Until the axis really moves: each leg reads status and the token first, so a fixed
        // sleep is not enough on a slow runner (~28 ms per transaction on Windows).
        MIB_REQUIRE(waitFor([&] { return device.moving(); }, std::chrono::seconds(5)), "the move has started");
        const auto since = [](std::chrono::steady_clock::time_point t0) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        };
        auto t0 = std::chrono::steady_clock::now();
        const auto applyDuring = stage(facade, StageCommandAction::ApplyProfile);
        MIB_EXPECT(!applyDuring.ok && since(t0) < 500,
                   "ApplyProfile during a move is refused at once (" + std::to_string(since(t0)) + " ms)");
        MIB_EXPECT(device.moving(), "and the move carries on");
        t0 = std::chrono::steady_clock::now();
        const auto disconnectDuring = stage(facade, StageCommandAction::Disconnect);
        MIB_EXPECT(disconnectDuring.ok && since(t0) < 1500,
                   "Disconnect during a move stops it and returns (" + std::to_string(since(t0)) + " ms)");
        MIB_EXPECT(ops.wait(running.operationId) == BackendOperationState::Cancelled, "the move ended Cancelled");
        MIB_EXPECT(waitFor([&] { return !device.moving(); }, std::chrono::seconds(2)), "axis stopped");
        device.setPulsesPerSecond(200000);

        // Rule 2 with a zero set: a reconnect must still be observe-only (it
        // neither moves nor rewrites the position).
        MIB_REQUIRE(stage(facade, StageCommandAction::Disconnect).ok, "Disconnect");
        const int writes = device.writes();
        const auto motions = motionOpcodes(device);
        const auto at = device.positionPulses();
        MIB_REQUIRE(stage(facade, StageCommandAction::Connect, 0, device.portName).ok, "reconnect");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        MIB_EXPECT(device.writes() == writes && motionOpcodes(device) == motions && device.positionPulses() == at,
                   "reconnect wrote nothing and moved nothing");
        facade.fetchStageStatus(st);
        MIB_EXPECT(st.zeroSet && st.limitsVerified && st.envelopeMaxUm == 1000.0,
                   "zero still set (power-up token), and verified");

        // A mid-travel declaration widens the envelope; the next zero without
        // it narrows it again.
        MIB_REQUIRE(stage(facade, StageCommandAction::SetZero, 0, {}, true).ok, "Set zero with mid-travel declared");
        facade.fetchStageStatus(st);
        MIB_EXPECT(st.midTravelDeclared && st.envelopeMaxUm == 2900.0 && st.envelopeMinUm == -2900.0,
                   "declared: +/-2900 um");
        MIB_REQUIRE(stage(facade, StageCommandAction::SetZero).ok, "Set zero again without it");
        facade.fetchStageStatus(st);
        MIB_EXPECT(!st.midTravelDeclared && st.envelopeMaxUm <= 1000.0, "narrowed again");
    }

    // --- pump cannot claim the stage's bus address ----------------------------
    watchdog.mark("bus conflict");
    {
        bridge::PumpCommand pump;
        pump.action = bridge::PumpCommandAction::Connect;
        pump.pumpId = 0;
        pump.portName = device.portName;
        pump.baudRate = 115200;
        pump.modbusAddress = 1;
        const auto r = facade.dispatch(pump);
        MIB_EXPECT(!r.ok && r.message.find("Z stage") != std::string::npos, "pump refused: " + r.message);
    }

    // --- 4. experiment lock; 3. Stop still accepted ---------------------------
    watchdog.mark("experiment lock");
    {
        auto& coord = app.experiment();
        coord.setApplicationIdentity("test-version", "test-build", "test-os");
        auto& proc = app.processing();
        auto cfg = proc.getProcessingConfig();
        cfg.empty_frame_pixel_threshold = 1;
        cfg.bg_subtract_threshold = 100;
        cfg.enable_border_check = false;
        cfg.enable_area_range_check = false;
        cfg.enable_deformability_range_check = false;
        cfg.enable_ring_ratio_check = false;
        cfg.enable_area_ratio_check = false;
        cfg.require_single_inner_contour = false;
        cfg.auto_background_enabled = false;
        cfg.enable_target_group = false;
        proc.setProcessingConfig(cfg);
        proc.setRealtimeRoi(backend::services::ProcessingService::Roi{0, 0, 96, 96});
        proc.setRealtimeProcessingMode(backend::services::ProcessingService::RealtimeProcessingMode::Inline);
        proc.startRealtime(app.getFrameStore());
        ::camera::mock::MockCameraOptions opts;
        opts.folder = frames;
        opts.frameInterval = std::chrono::microseconds(2000);
        opts.loopFiles = true;
        app.configureMockCamera(opts);
        MIB_REQUIRE(app.capture().requestStart() == backend::services::CaptureStartOutcome::Accepted, "capture");
        MIB_REQUIRE(waitFor([&] { return app.capture().stats().framesProcessed.load() > 2; },
                            std::chrono::seconds(5)),
                    "frames flowing");
        const std::string out = (td.path() / "run.h5").string();
        const auto readiness = coord.evaluateReadiness(out);
        MIB_REQUIRE(readiness.ready, "experiment ready");
        backend::app::ExperimentStartRequest req;
        req.outputPath = out;
        req.readinessGeneration = readiness.generation;
        // #533: an experiment never starts under a moving stage. A slow move is running; Start
        // is refused, the move carries on, and once it is stopped Start goes through.
        {
            bridge::BackendStageStatus st;
            facade.fetchStageStatus(st);
            MIB_REQUIRE(st.zeroSet, "the zero is set");
            const double target = (st.positionUm - st.envelopeMinUm > st.envelopeMaxUm - st.positionUm) ? st.envelopeMinUm
                                                                                                         : st.envelopeMaxUm;
            MIB_REQUIRE(std::abs(target - st.positionUm) > 50, "room to move");
            device.setPulsesPerSecond(2000);
            const auto slow = stage(facade, StageCommandAction::MoveTo, std::round(target));
            MIB_REQUIRE(slow.ok && slow.operationId != 0, "a slow move is running");
            // Until the axis really moves (each leg reads status and the token before its opcode:
            // ~28 ms per transaction on a Windows runner).
            MIB_REQUIRE(waitFor([&] { return device.moving(); }, std::chrono::seconds(5)), "the move has started");
            const auto refused = coord.start(req);
            MIB_EXPECT(refused.outcome == backend::app::ExperimentStartOutcome::Busy &&
                           refused.message.find("stage operation") != std::string::npos,
                       "Start is refused while the stage moves: " + refused.message);
            MIB_EXPECT(coord.state() == backend::app::ExperimentRunState::Idle, "and no experiment was started");
            MIB_EXPECT(device.moving(), "the move carries on");
            MIB_EXPECT(stage(facade, StageCommandAction::Stop).ok, "Stop");
            MIB_EXPECT(ops.wait(slow.operationId) == BackendOperationState::Cancelled, "the move ended Cancelled");
            MIB_EXPECT(waitFor([&] { return !device.moving(); }, std::chrono::seconds(2)), "axis stopped");
            device.setPulsesPerSecond(200000);
        }
        // Fail closed: a cleanup Stop that fails while the axis keeps moving must not let Start through,
        // although the operation is over.
        {
            bridge::BackendStageStatus st;
            facade.fetchStageStatus(st);
            // Halfway to the roomier edge, so a move against the approach direction still has its
            // overshoot inside the envelope.
            const double up = st.envelopeMaxUm - st.positionUm;
            const double down = st.positionUm - st.envelopeMinUm;
            const double target = std::round(up > down ? st.positionUm + up / 2 : st.positionUm - down / 2);
            MIB_REQUIRE(std::abs(target - st.positionUm) > 50, "room to move");
            device.setPulsesPerSecond(2000);
            const auto slow = stage(facade, StageCommandAction::MoveTo, std::round(target));
            MIB_REQUIRE(slow.ok && slow.operationId != 0, "a slow move is running: " + slow.message);
            MIB_REQUIRE(waitFor([&] { return device.moving(); }, std::chrono::seconds(5)), "the move has started");
            device.setStopFails(true);
            stage(facade, StageCommandAction::Stop); // reports a failure: the axis keeps going
            MIB_EXPECT(ops.wait(slow.operationId) == BackendOperationState::Cancelled, "the operation ended Cancelled");
            MIB_EXPECT(device.moving(), "the axis is still moving");
            const auto refused = coord.start(req);
            MIB_EXPECT(refused.outcome == backend::app::ExperimentStartOutcome::Busy,
                       "Start is still refused: " + refused.message);
            MIB_EXPECT(coord.state() == backend::app::ExperimentRunState::Idle, "no experiment was started");
            device.setStopFails(false);
            MIB_EXPECT(stage(facade, StageCommandAction::Stop).ok, "a Stop that works");
            MIB_EXPECT(waitFor([&] { return !device.moving(); }, std::chrono::seconds(2)), "axis stopped");
            device.setPulsesPerSecond(200000);
            MIB_EXPECT(waitFor([&] { return !app.stage().motionPossible(); }, std::chrono::seconds(3)),
                       "a poll confirms idle, so motion is no longer possible");
        }
        MIB_REQUIRE(coord.start(req).outcome == backend::app::ExperimentStartOutcome::Started, "experiment started");

        const int opcodesBefore = motionOpcodes(device);
        const int writesBefore = device.writes();
        for (const auto action : {StageCommandAction::MoveTo, StageCommandAction::MoveBy, StageCommandAction::SetZero,
                                  StageCommandAction::ApplyProfile, StageCommandAction::Disconnect}) {
            const auto r = stage(facade, action, 10);
            MIB_EXPECT(!r.ok && r.operationId == 0, "refused during an experiment: " + r.message);
        }
        MIB_EXPECT(motionOpcodes(device) == opcodesBefore && device.writes() == writesBefore,
                   "nothing moved or was written during the experiment");
        MIB_EXPECT(stage(facade, StageCommandAction::Stop).ok, "Stop accepted during the experiment");

        coord.requestStop(false);
        MIB_REQUIRE(waitFor([&] { return coord.state() == backend::app::ExperimentRunState::Idle; },
                            std::chrono::seconds(10)),
                    "experiment back to idle");
        app.capture().stop();
        const auto again = stage(facade, StageCommandAction::MoveTo, 0);
        MIB_EXPECT(again.ok, "moves allowed again once idle");
        MIB_EXPECT(ops.wait(again.operationId) == BackendOperationState::Completed, "move completed");
    }

    watchdog.mark("shutdown");
    MIB_EXPECT(stage(facade, StageCommandAction::Disconnect).ok, "Disconnect");
    facade.shutdown();
    if (mib::test::exitCode() == 0) std::printf("stage bridge facade verified\n");
    return mib::test::exitCode();
}
