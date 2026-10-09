// PZ7035 camera modes (#501 P1): the single register writer (LED, cell path, cell capture) and
// AppBackend's Align/Run sequence on a PL-science backend with the mock camera standing in for
// the GenTL producer. The rules under test come from pz7035-imx426 docs/YOFO_HOST_INTERFACE.md:
// - no PL register is touched while the PL is unconfigured (PCFG_DONE);
// - the LED is written off-first (S[0] = 0 ... S[0] = 1);
// - the cell path is never on while the camera stream (the producer) runs: its AcquisitionStart
//   pulses clear the U-Net enable;
// - in Run the live camera stays stopped and the readiness gates ask for Run, not a live camera;
// - raw LED values only in Service mode and within the mode's limits, enforced by the backend.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/RecordingTarget.h"
#include "backend/app/SciencePlacement.h"
#include "backend/discovery/DeviceDiscoveryService.h"
#include "backend/pz/AlignLock.h"
#include "backend/processing/IExecutionProvider.h"
#include "backend/pz/PzInstrumentControl.h"
#include "backend/services/CaptureService.h"

#include <nlohmann/json.hpp>

#include "support/assert.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pz = backend::pz;

// An execution provider that records how the live session drives it (#651 G5).
class FakeProvider final : public backend::processing::IExecutionProvider {
public:
    std::string name() const override { return "fake-live"; }
    void setSink(Sink) override {}
    bool configure(const backend::processing::pz::CompiledProfile& profile, std::string*) override {
        ++configures;
        lastPage = profile.page;
        return true;
    }
    bool start(uint64_t, std::string* error) override {
        if (failStart) {
            if (error) *error = "frame ring: the device is still draining a stopped run: restore the PL";
            return false;
        }
        ++starts;
        running = true;
        return true;
    }
    void stop() override {
        if (running) ++stops;
        running = false;
    }
    backend::processing::ProviderStatus status() const override { return {}; }
    // The every-frame ring (#649): what the provider reports, nothing read from a device.
    uint32_t ringFramesWanted() const override { return ringFrames; }
    std::string ringPlacementProblem() override { return placementProblem; }
    backend::pz::RingStatus ringStatus() override { return ring; }
    backend::pz::RingRead ringRead(uint64_t seq, backend::pz::RingFrame& out, std::string* why) override {
        if (seq < ring.lo || seq >= ring.final) {
            if (why) *why = "out of range";
            return backend::pz::RingRead::OutOfRange;
        }
        out = backend::pz::RingFrame{};
        out.seq = seq;
        out.frameId = 500 + seq;
        out.width = 512;
        out.height = 96;
        out.gray.assign(512u * 96u, 7);
        out.mask.assign(512u * 96u / 8u, 0);
        out.maskPresent = true;
        return backend::pz::RingRead::Ok;
    }
    uint32_t ringTickHz() override { return 100000000u; }
    std::atomic<uint32_t> ringFrames{0};
    std::atomic<bool> failStart{false};
    std::string placementProblem;
    backend::pz::RingStatus ring;
    std::atomic<int> configures{0}, starts{0}, stops{0};
    std::atomic<bool> running{false};
    std::array<uint32_t, 32> lastPage{};
};

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int processId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

constexpr unsigned S0 = 128; // S[i] = P[128 + i]

struct Write {
    unsigned index;
    uint32_t value;
};

// A U-Net cell image: 'CEL2', 100 MHz strobe clock, a finished capture with two listed cells.
struct FakeState {
    std::map<unsigned, uint32_t> live;
    std::vector<Write> writes;
    unsigned reads{0};
    bool configured{true};
    std::function<void(unsigned, uint32_t)> onWrite;
};

class FakeControl final : public pz::IPzControlRegisters {
public:
    explicit FakeControl(FakeState& s) : s_(s) {}
    uint32_t live(unsigned i) override {
        ++s_.reads;
        auto it = s_.live.find(i);
        return it == s_.live.end() ? 0 : it->second;
    }
    void setLive(unsigned i, uint32_t v) override {
        s_.writes.push_back({i, v});
        s_.live[i] = v;
        if (i == S0 + 36) s_.live[S0 + 42] = 1; // capture done at once
        if (s_.onWrite) s_.onWrite(i, v);
    }
    uint32_t window(unsigned w) override {
        ++s_.reads;
        if (w < 12288) return 0x04030201u + w;                       // gray pattern
        if (w < 13824) return 0xFF00FF00u;                            // mask pattern
        const unsigned cell = (w - 13824) / 32, k = (w - 13824) % 32;
        if (k == 15) return (10u + cell) << 16 | (100u + 40 * cell); // y << 16 | x
        if (k == 16) return 20u << 16 | 30u;                         // h << 16 | w
        if (k == 17) return 2u << 24 | cell << 16 | 1u;               // count, rank, valid
        return k;
    }
    bool plConfigured(std::string* why) override {
        if (!s_.configured && why) *why = "PL not configured (DEVCFG PCFG_DONE = 0): load the PL image";
        return s_.configured;
    }
    void sleepUs(unsigned) override {}

private:
    FakeState& s_;
};

void seedCellImage(FakeState& s) {
    s.live[S0 + 41] = 0x43454C32u; // 'CEL2'
    s.live[S0 + 10] = 100000;      // kHz
    s.live[8] = 0x01;              // some other command bit that must survive
    s.live[S0 + 43] = 777;         // capture tag
    s.live[S0 + 44] = 2u | (0x5u << 8) | (3u << 16);
    s.live[S0 + 45] = 4u | (1u << 16);
}

std::vector<Write> strobeWrites(const FakeState& s, size_t from = 0) {
    std::vector<Write> w;
    for (size_t i = from; i < s.writes.size(); ++i)
        if (s.writes[i].index >= S0 && s.writes[i].index <= S0 + 5) w.push_back(s.writes[i]);
    return w;
}

void testControl() {
    FakeState s;
    seedCellImage(s);
    pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
    std::string err;

    MIB_REQUIRE(control.setLed(pz::kRunLed, &err), "Run LED: " + err);
    const auto w = strobeWrites(s);
    MIB_EXPECT(w.size() == 6 && w.front().index == S0 && w.front().value == 0 && w.back().index == S0 &&
                   w.back().value == 1,
               "LED written off first and enabled last");
    MIB_EXPECT(s.live[S0 + 1] == 700 && s.live[S0 + 2] == 6000, "7/60 µs = 700/6000 cycles at 100 MHz");

    MIB_REQUIRE(control.setCellPath(true, &err), "cell path on");
    MIB_EXPECT(s.live[S0 + 46] == 1 && s.live[8] == 0x11, "S[46] and P[8] bit 4 set, other bits kept");
    MIB_EXPECT(control.cellPathOn(&err), "cell path reads on");

    pz::PzCellCapture capture;
    MIB_REQUIRE(control.captureCell(capture, std::chrono::milliseconds(50), &err), "capture: " + err);
    MIB_EXPECT(capture.frameId == 777 && capture.listed == 2 && capture.flags == 5 && capture.dropped == 3 &&
                   capture.cells == 4 && capture.blemishes == 1,
               "capture listing words");
    MIB_EXPECT(capture.gray.size() == 512 * 96 && capture.gray[0] == 1 && capture.gray[3] == 4 && capture.gray[4] == 2,
               "gray little-endian, pixel 0 in bits 7:0");
    MIB_EXPECT(capture.mask.size() == 6144 && capture.mask[0] == 0x00 && capture.mask[1] == 0xFF, "mask bytes");
    MIB_EXPECT(capture.list.size() == 2 && capture.list[1].x() == 140 && capture.list[1].y() == 11 &&
                   capture.list[1].width() == 30 && capture.list[1].height() == 20 && capture.list[1].valid(),
               "cell bbox {y,x}, {h,w}, validity");

    const auto packet = pz::encodeRunPreview(capture);
    MIB_EXPECT(packet.size() == 32 + 49152 + 6144 + 2 * 72 && std::memcmp(packet.data(), "MIBC", 4) == 0 &&
                   packet[4] == 1 && packet[6] == 32 && packet[8] == 0x09 && packet[9] == 0x03 && packet[16] == 2,
               "run preview packet: header, frame id 777, 2 cells");

    MIB_REQUIRE(control.setCellPath(false, &err), "cell path off");
    MIB_EXPECT(s.live[S0 + 46] == 0 && s.live[8] == 0x01, "cell path off clears only bit 4");
    MIB_EXPECT(!control.captureCell(capture, std::chrono::milliseconds(50), &err) &&
                   err.find("cell path is off") != std::string::npos,
               "no capture with the cell path off");

    // A blank PL: nothing is read or written at all.
    s.configured = false;
    const auto reads = s.reads;
    const auto writes = s.writes.size();
    MIB_EXPECT(!control.ledOff(&err) && err.find("PCFG_DONE") != std::string::npos, "refused while the PL is blank");
    MIB_EXPECT(!control.setCellPath(true, &err) && !control.setLed(pz::kAlignLed, &err), "every write is refused");
    MIB_EXPECT(s.reads == reads && s.writes.size() == writes, "no PL access while the PL is blank");

    // Another image: refused before any write.
    s.configured = true;
    s.live[S0 + 41] = 0;
    MIB_EXPECT(!control.ledOff(&err) && err.find("CEL2") != std::string::npos, "not the U-Net cell image");
    MIB_EXPECT(s.writes.size() == writes, "nothing written on another image");
}

// The Align ingress recovery (#629): sticky lane overflow flags (P[13] bits 15:8) stop every
// preview and clear only at a receiver reset; each reset (P[8] bit 6 held ~100 ms) is a fresh try
// at the lane deskew (about 40-45 % each), so up to eight are made before the operator error.
void testAlignIngressRecovery() {
    {
        FakeState s;
        seedCellImage(s);
        s.live[8] = 0x11; // U-Net enabled: the receiver reset must keep it
        s.live[13] = 0x0001FF22u;
        s.live[12] = 11;
        s.live[14] = 15;
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        std::string err;
        pz::IngressStatus st;
        MIB_REQUIRE(control.ingressStatus(st, &err), "ingress status: " + err);
        MIB_EXPECT(st.status == 0x0001FF22u && st.laneOverflow() == 0xFF && st.errors == 11 && st.resyncs == 15,
                   "P[13], P[12], P[14] and the lane overflow byte");
        s.writes.clear();
        MIB_REQUIRE(control.resetReceiver(std::chrono::milliseconds(100), &err), "receiver reset: " + err);
        MIB_EXPECT(s.writes.size() == 2 && s.writes[0].index == 8 && s.writes[0].value == 0x50u &&
                       s.writes[1].index == 8 && s.writes[1].value == 0x10u,
                   "P[8] bit 6 asserted with bit 4 kept, then released");
        s.configured = false;
        s.writes.clear();
        MIB_EXPECT(!control.resetReceiver(std::chrono::milliseconds(100), &err) && !control.ingressStatus(st, &err) &&
                       s.writes.empty(),
                   "no reset and no status read while the PL is blank");
    }

    struct Run {
        int lockAfterClears{-1}; // the preview arrives after this many resets (-1: never)
        bool overflow{true};
        bool clearFails{false};
        bool slowStart{false};
        int clears{0};
        long long holdMs{0};
        std::vector<int> attempts;
    };
    const auto run = [](Run& r) {
        pz::AlignLockHooks h;
        h.waitPreview = [&](std::chrono::milliseconds wait) {
            if (r.slowStart) return wait.count() > 5000;
            return r.lockAfterClears >= 0 && r.clears >= r.lockAfterClears && r.clears > 0;
        };
        h.readStatus = [&](pz::IngressStatus& st) {
            const bool cleared = r.lockAfterClears > 0 && r.clears >= r.lockAfterClears;
            st.status = r.overflow && !cleared ? 0x0001FF20u : 0x00000020u;
            st.errors = 11;
            st.resyncs = 15;
            return true;
        };
        h.clearFlags = [&](std::chrono::milliseconds hold) {
            ++r.clears;
            r.holdMs = hold.count();
            return !r.clearFails;
        };
        h.onAttempt = [&](int attempt, const pz::IngressStatus&) { r.attempts.push_back(attempt); };
        h.pause = [](std::chrono::milliseconds) {};
        return pz::awaitAlignLock(h);
    };

    {   // A healthy start: the first wait is enough, nothing is read or reset.
        bool touched = false;
        pz::AlignLockHooks h;
        h.waitPreview = [](std::chrono::milliseconds) { return true; };
        h.readStatus = [&](pz::IngressStatus&) { touched = true; return true; };
        h.clearFlags = [&](std::chrono::milliseconds) { touched = true; return true; };
        h.pause = [](std::chrono::milliseconds) {};
        const auto out = pz::awaitAlignLock(h);
        MIB_EXPECT(out.locked && !out.recovered && out.clears == 0 && !touched, "no recovery on a healthy start");
    }
    {   // Stuck flags, the first reset clears them: held 100 ms, one attempt logged.
        Run r;
        r.lockAfterClears = 1;
        const auto out = run(r);
        MIB_EXPECT(out.locked && out.recovered && out.clears == 1 && r.holdMs == 100 && r.attempts == std::vector<int>{1},
                   "one 100 ms reset recovers");
    }
    {   // Each reset is a fresh try: the third one locks.
        Run r;
        r.lockAfterClears = 3;
        const auto out = run(r);
        MIB_EXPECT(out.locked && out.clears == 3 && r.attempts == (std::vector<int>{1, 2, 3}), "third reset locks");
    }
    {   // The seventh reset locks (the board needed up to four): still inside the eight.
        Run r;
        r.lockAfterClears = 7;
        const auto out = run(r);
        MIB_EXPECT(out.locked && out.clears == 7, "seventh reset locks");
    }
    {   // It never locks: eight attempts, then the operator error with the readings.
        Run r;
        const auto out = run(r);
        MIB_EXPECT(!out.locked && out.clears == 8 && r.attempts.size() == 8, "eight attempts");
        MIB_EXPECT(out.error.find("Align preview not locking") == 0 && out.error.find("lane overflow 0xFF") != std::string::npos &&
                       out.error.find("errors 11") != std::string::npos && out.error.find("8 receiver resets") != std::string::npos,
                   "operator error names the flags and the attempts: " + out.error);
    }
    {   // A refused reset ends the recovery with a plain reason.
        Run r;
        r.clearFails = true;
        const auto out = run(r);
        MIB_EXPECT(!out.locked && out.clears == 1 && out.error.find("reset was refused") != std::string::npos, "refused reset");
    }
    {   // No overflow flags: a slow start gets the rest of the wait and no reset is issued.
        Run r;
        r.overflow = false;
        r.slowStart = true;
        auto out = run(r);
        MIB_EXPECT(out.locked && out.clears == 0 && r.clears == 0, "slow start without flags: no reset");
        Run q;
        q.overflow = false;
        out = run(q);
        MIB_EXPECT(!out.locked && q.clears == 0 && out.error.find("no Align preview arrived") == 0, "no preview, no flags: plain error");
    }
}

// results9's PL receiver self-heal (RXH1 at P[256]): with the block present the PL does the resets;
// the host waits for it first, and runs its own recovery as the backstop when the block gives up.
void testRxSelfHeal() {
    {
        FakeState s;
        seedCellImage(s);
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        std::string err;
        pz::RxHealStatus heal;
        MIB_REQUIRE(control.rxHealStatus(heal, &err) && !heal.present, "results8: no RXH1 block (reads 0)");
        s.live[256] = 0x52584831u;
        s.live[257] = 0x64140801u;
        s.live[258] = (0xFFu << 16) | 0x200u | 3u; // 3 tries, flags set, all lanes
        s.live[259] = 7;
        s.live[260] = 0x0301u;
        MIB_REQUIRE(control.rxHealStatus(heal, &err) && heal.present, "results9: RXH1 present");
        MIB_EXPECT(heal.tries() == 3 && heal.flagsSet() && !heal.gaveUp() && heal.laneFlags() == 0xFF && heal.autoResets == 7 &&
                       heal.control == 0x64140801u && heal.lastPulse == 0x0301u,
                   "status, count and last pulse words");
        s.live[258] |= 0x100u;
        MIB_EXPECT(control.rxHealStatus(heal, &err) && heal.gaveUp(), "gave-up bit");
        MIB_EXPECT(!heal.v2 && heal.flagClears == 0, "v1: no v2 counters");
        s.live[256 + pz::kRxHealV2Word] = 0x03e80001u; // heal v2: CTRL2
        s.live[261] = 6;
        s.live[262] = 5;
        s.live[263] = 2;
        MIB_EXPECT(control.rxHealStatus(heal, &err) && heal.v2 && heal.flagClears == 6 && heal.episodes == 5 &&
                       heal.failedEpisodes == 2 && heal.autoResets == 7,
                   "v2: flag clears, episodes and failed episodes beside the receiver-reset count");
        s.live[256 + pz::kRxHealV2Word] = 0;
        s.writes.clear();
        (void)control.rxHealStatus(heal, &err);
        MIB_EXPECT(s.writes.empty(), "reading the block writes nothing");
    }
    struct Run {
        bool present{true};
        int previewAfterPolls{-1}; // the PL heal locks the preview after this many heal polls (-1 never)
        int gaveUpAfterPolls{-1};
        int lockAfterClears{-1};   // the host's resets: the preview arrives after this many (-1 never)
        int polls{0}, clears{0}, plGaveUps{0};
    };
    const auto run = [](Run& r) {
        pz::AlignLockHooks h;
        const auto hostLocked = [&] { return r.lockAfterClears > 0 && r.clears >= r.lockAfterClears; };
        h.waitPreview = [&](std::chrono::milliseconds wait) {
            if (hostLocked()) return true;
            if (wait.count() >= 1000) return false; // the first 1 s wait: nothing yet
            ++r.polls;
            return r.previewAfterPolls >= 0 && r.polls > r.previewAfterPolls;
        };
        h.readStatus = [&](pz::IngressStatus& st) {
            st.status = hostLocked() ? 0x00000020u : 0x0001FF20u;
            st.errors = 11;
            st.resyncs = 15;
            return true;
        };
        h.readHeal = [&](pz::RxHealStatus& heal) {
            heal = pz::RxHealStatus{};
            heal.present = r.present;
            heal.status = (0xFFu << 16) | 0x200u | 4u | (r.gaveUpAfterPolls >= 0 && r.polls >= r.gaveUpAfterPolls ? 0x100u : 0u);
            return true;
        };
        h.onPlGaveUp = [&](const pz::RxHealStatus&, bool, long long) { ++r.plGaveUps; };
        h.clearFlags = [&](std::chrono::milliseconds) { ++r.clears; return true; };
        h.pause = [](std::chrono::milliseconds) {};
        return pz::awaitAlignLock(h);
    };
    {
        Run r;
        r.previewAfterPolls = 3;
        const auto out = run(r);
        MIB_EXPECT(out.locked && out.healedByPl && !out.plGaveUp && r.clears == 0 && r.plGaveUps == 0,
                   "the PL heal locks the preview: the host sends no reset");
    }
    {
        Run r; // the PL gives up (4 of 10 flips on results10): the host recovery takes over and locks
        r.gaveUpAfterPolls = 2;
        r.lockAfterClears = 3;
        const auto out = run(r);
        MIB_EXPECT(out.locked && out.plGaveUp && !out.healedByPl && r.clears == 3 && r.plGaveUps == 1,
                   "after the PL gave up, the host resets lock the preview and the gave-up is still reported");
    }
    {
        Run r; // the PL gives up and the host cannot lock either: the operator error names both
        r.gaveUpAfterPolls = 2;
        const auto out = run(r);
        MIB_EXPECT(!out.locked && out.plGaveUp && r.clears == 8 && out.error.find("the PL self-heal gave up after 4 tries") != std::string::npos &&
                       out.error.find("8 receiver resets") != std::string::npos,
                   "gave-up, then eight host resets, then the operator error: " + out.error);
    }
    {
        Run r; // never heals, never gives up: bounded by healWait, then the host recovery
        const auto out = run(r);
        MIB_EXPECT(!out.locked && out.plGaveUp && r.clears == 8 && out.error.find("did not finish") != std::string::npos,
                   "a heal that never finishes ends with the host recovery and an error");
        MIB_EXPECT(r.polls <= 6000 / 250 + 1, "the wait is bounded");
    }
    {
        Run r; // no RXH1 block: the host recovery as before
        r.present = false;
        const auto out = run(r);
        MIB_EXPECT(!out.locked && !out.plGaveUp && r.clears == 8, "results8: eight host resets");
    }
}

// Service start writes the standing RXH1 v1 CTRL value (persist 250 ms) and reads it back; v2 builds,
// images without the block and a blank PL are left alone.
void testRxHealCtrl() {
    using Kind = pz::PzInstrumentControl::RxHealCtrlResult::Kind;
    const auto ctrlWrites = [](const FakeState& s) {
        size_t n = 0;
        for (const auto& w : s.writes) n += w.index == 257;
        return n;
    };
    pz::PzInstrumentControl::RxHealCtrlResult r;
    std::string err;
    {   // results8: no block, no write
        FakeState s;
        seedCellImage(s);
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        s.writes.clear();
        MIB_REQUIRE(control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err), err);
        MIB_EXPECT(r.kind == Kind::Absent && s.writes.empty(), "no RXH1 block: nothing written");
    }
    {   // v1 at the default CTRL: one write of word 1, read back
        FakeState s;
        seedCellImage(s);
        s.live[256] = pz::kRxHealId;
        s.live[257] = 0x64140801u;
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        s.writes.clear();
        MIB_REQUIRE(control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err), err);
        MIB_EXPECT(r.kind == Kind::Written && r.before == 0x64140801u && r.after == 0x64FA0801u && s.live[257] == 0x64FA0801u,
                   "v1 default CTRL becomes 0x64FA0801 (persist 250 ms), read back");
        MIB_EXPECT(s.writes.size() == 1 && ctrlWrites(s) == 1, "exactly one register written: RXH1 CTRL");
        s.writes.clear();
        MIB_REQUIRE(control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err), err);
        MIB_EXPECT(r.kind == Kind::AlreadySet && s.writes.empty(), "already set: no write");
    }
    {   // a CTRL that does not hold the value is reported
        FakeState s;
        seedCellImage(s);
        s.live[256] = pz::kRxHealId;
        s.live[257] = 0x64140801u;
        s.onWrite = [&](unsigned i, uint32_t) { if (i == 257) s.live[257] = 0x64140801u; };
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        MIB_REQUIRE(control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err), err);
        MIB_EXPECT(r.kind == Kind::Mismatch && r.after == 0x64140801u, "a read-back mismatch is reported");
    }
    {   // heal v2 (word 24 set): CTRL left alone
        FakeState s;
        seedCellImage(s);
        s.live[256] = pz::kRxHealId;
        s.live[257] = 0x64140801u;
        s.live[256 + pz::kRxHealV2Word] = 2;
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        s.writes.clear();
        MIB_REQUIRE(control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err), err);
        MIB_EXPECT(r.kind == Kind::V2 && s.writes.empty() && s.live[257] == 0x64140801u, "v2: CTRL untouched");
    }
    {   // a blank PL: nothing read or written
        FakeState s;
        seedCellImage(s);
        s.live[256] = pz::kRxHealId;
        pz::PzInstrumentControl control(std::make_unique<FakeControl>(s));
        s.configured = false;
        const auto reads = s.reads;
        s.writes.clear();
        MIB_EXPECT(!control.applyRxHealCtrl(pz::kRxHealCtrlPersist250, r, &err) && err.find("PCFG_DONE") != std::string::npos &&
                       s.writes.empty() && s.reads == reads,
                   "blank PL: refused, no access");
    }
}

void testLedLimits() {
    using pz::InstrumentMode;
    MIB_EXPECT(pz::checkLed(InstrumentMode::Run, pz::kRunLed).empty(), "Run preset within Run limits");
    MIB_EXPECT(pz::checkLed(InstrumentMode::Align, pz::kAlignLed).empty(), "Align preset within Align limits");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Run, {7.0, 90.0}).empty(), "Run width above 80 µs refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Align, {0.0, 40.0}).empty(), "Align width below 60 µs refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Run, {-1.0, 60.0}).empty(), "negative delay refused");
    MIB_EXPECT(!pz::checkLed(InstrumentMode::Unknown, pz::kRunLed).empty(), "no LED without a mode");
}

const backend::app::ReadinessGate* gateOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id) {
    return r.gate(id);
}

void testModeSequence(const mib::test::TempDir& td) {
    const auto frames = td.path() / "frames";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 512, 96), "mock frames");
    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", "none");
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "science on the PL");

    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((td.path() / "data").string()), "facade initializes");
    MIB_EXPECT(!backend.instrumentControlAvailable(), "no control without the board provider");

    FakeState s;
    seedCellImage(s);
    // The results10 PL (RXH1 v1 at its default CTRL): the one write at service start is its CTRL word.
    s.live[256] = pz::kRxHealId;
    s.live[257] = 0x64140801u;
    // The start-up write allow-list: exactly one register, RXH1 CTRL word 1 = P[257], and nothing to the
    // LED strobe (S[0..5], P[128..133]), P[8] (command / U-Net enable), the cell path (S[46]) or the SPI.
    const auto onlyRxHealCtrlWritten = [&] {
        return s.writes.size() == 1 && s.writes[0].index == 257 && s.writes[0].value == pz::kRxHealCtrlPersist250;
    };
    // The rule the producer imposes: the cell path never goes on while the camera stream runs.
    bool cellOnWhileStreaming = false;
    s.onWrite = [&](unsigned i, uint32_t v) {
        const bool cellOn = (i == S0 + 46 && v == 1) || (i == 8 && (v & 0x10u));
        if (cellOn && backend.capture().lifecycleSnapshot().isActive()) cellOnWhileStreaming = true;
    };
    backend.setInstrumentControlForTesting(std::make_unique<FakeControl>(s));
    MIB_REQUIRE(backend.instrumentControlAvailable(), "control injected");
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Unknown && onlyRxHealCtrlWritten(),
               "the only write before a mode is chosen is RXH1 CTRL word 1 (persist 250 ms): no LED, P[8], cell path or SPI");
    MIB_EXPECT(s.live[257] == pz::kRxHealCtrlPersist250, "the CTRL write took");
    {
        // Raw Record is refused on a PL-science instrument (#651 G9): the producer's preview frames are not
        // the 5 kHz results, and the RAM root has no free-space guard.
        std::string recordError;
        MIB_EXPECT(!backend.startFrameRecording((td.path() / "clip.h5").string(), &recordError) &&
                       recordError.find("PL-science") != std::string::npos,
                   "raw recording is refused on a PL-science instrument: " + recordError);
        MIB_EXPECT(!std::filesystem::exists(td.path() / "clip.h5"), "and creates no file");
    }
    {
        // Everything the UI polls at load, with no operator action: reads only (the standing
        // YOFO Studio unit on the board must not touch the PL or the pump bus before an operator
        // does; serial ports are opened only by the pump/stage/discovery commands, which the UI
        // sends from buttons).
        (void)facade.fetchPlatformInfoJson();
        (void)facade.fetchInstrumentStatusJson();
        (void)facade.fetchCameraGeometryJson();
        backend::bridge::BackendPumpStatus pump;
        (void)facade.fetchPumpStatus(0, pump);
        (void)facade.fetchPumpStatus(1, pump);
        backend::bridge::BackendStageStatus stage;
        (void)facade.fetchStageStatus(stage);
        MIB_EXPECT(onlyRxHealCtrlWritten(), "the status polls the UI makes at load write no PL register (still only the CTRL write)");
    }

    {
        // PL science never probes or opens the nanopositioner, pulse-generator or ZC300 ports (the
        // RS485 bus carries the pumps): the backend refuses, with a reason the UI shows.
        for (const auto kind : {backend::discovery::DeviceKind::Nanopositioner, backend::discovery::DeviceKind::PulseGenerator,
                                backend::discovery::DeviceKind::MotionStage}) {
            backend::discovery::DiscoveryRequest request;
            request.kinds = {kind};
            const auto started = backend.deviceDiscovery().startDiscovery(request);
            MIB_EXPECT(!started.accepted && started.reason.find("runs science on the PL") != std::string::npos,
                       "serial hardware discovery is refused on a PL-science instrument");
        }
        backend::discovery::DiscoveryRequest cameras;
        cameras.kinds = {backend::discovery::DeviceKind::Camera};
        const auto cameraJob = backend.deviceDiscovery().startDiscovery(cameras);
        MIB_EXPECT(cameraJob.accepted, "camera discovery still works: " + cameraJob.reason);
        if (cameraJob.accepted) backend.deviceDiscovery().cancelDiscovery(cameraJob.jobId);
        backend::bridge::StageCommand connect;
        connect.action = backend::bridge::StageCommandAction::Connect;
        connect.portName = "/dev/ttyPS1";
        connect.modbusAddress = 5;
        const auto stage = facade.dispatch(connect);
        MIB_EXPECT(!stage.ok && stage.message.find("runs science on the PL") != std::string::npos,
                   "ZC300 stage connect is refused on a PL-science instrument: " + stage.message);
    }

    std::string err;
    // Align: the camera streams, cell path off, LED 0/125.
    MIB_EXPECT(!backend.instrumentRunWindowSet() &&
                   facade.fetchInstrumentStatusJson().find("\"run_set\":false") != std::string::npos,
               "no run window is set before the first Run switch");
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align: " + err);
    MIB_EXPECT(!backend.instrumentRunWindowSet(), "Align alone does not set the run window");
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Align, "mode Align");
    MIB_EXPECT(backend.capture().lifecycleSnapshot().cameraReady, "the camera streams in Align");
    MIB_EXPECT(s.live[S0 + 46] == 0 && (s.live[8] & 0x10u) == 0, "cell path off in Align");
    MIB_EXPECT(s.live[S0 + 0] == 1 && s.live[S0 + 1] == 0 && s.live[S0 + 2] == 12500, "LED Align 0/125");
    std::vector<uint8_t> preview;
    MIB_EXPECT(!backend.fetchRunPreview(preview, &err), "no run preview in Align");

    const auto out = (td.path() / "run.h5").string();
    auto readiness = backend.experiment().evaluateReadiness(out, "pl");
    MIB_EXPECT(gateOf(readiness, "instrument.mode") &&
                   gateOf(readiness, "instrument.mode")->status == backend::app::GateStatus::Fail,
               "a run needs Run mode");

    // Run at an off-grid window: snapped to x % 8, y % 4; camera stopped; cell path on; LED 7/60.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 157, 203, &err), "Run: " + err);
    MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Run, "mode Run");
    MIB_EXPECT(backend.instrumentRunOffset() == std::make_pair(152, 200), "window snapped to (152, 200)");
    {
        // The UI restores its window from the status after a page reload (#501).
        const auto status = facade.fetchInstrumentStatusJson();
        MIB_EXPECT(backend.instrumentRunWindowSet() && status.find("\"run_set\":true") != std::string::npos &&
                       status.find("\"run_x\":152") != std::string::npos &&
                       status.find("\"run_y\":200") != std::string::npos,
                   "the status reports the window the Run switch applied: " + status);
    }
    MIB_EXPECT(!backend.capture().lifecycleSnapshot().isActive(), "the producer stream is stopped in Run");
    MIB_EXPECT(s.live[S0 + 46] == 1 && (s.live[8] & 0x10u) != 0, "cell path on in Run");
    MIB_EXPECT(s.live[S0 + 0] == 1 && s.live[S0 + 1] == 700 && s.live[S0 + 2] == 6000, "LED Run 7/60");
    MIB_EXPECT(!cellOnWhileStreaming, "the cell path never went on while the camera streamed");

    MIB_EXPECT(backend.fetchRunPreview(preview, &err) && preview.size() > 32 + 49152,
               "run preview from the cell capture: " + err);

    backend::bridge::CameraCommand start;
    start.action = backend::bridge::CameraCommandAction::StartCapture;
    const auto refused = facade.dispatch(start);
    MIB_EXPECT(!refused.ok && refused.message.find("Run mode") != std::string::npos, "Start Camera refused in Run");
    MIB_EXPECT(!backend.capture().lifecycleSnapshot().isActive(), "and the stream stays stopped");
    MIB_EXPECT(!backend.setCameraOverview(true, &err), "the overview switch is refused in Run");

    readiness = backend.experiment().evaluateReadiness(out, "pl");
    MIB_EXPECT(gateOf(readiness, "instrument.mode") &&
                   gateOf(readiness, "instrument.mode")->status == backend::app::GateStatus::Pass,
               "Run passes the instrument gate");
    {
        const auto* persistent = gateOf(readiness, "storage.persistent");
        const bool onRam = backend::app::recordingTarget(out).ram;
        MIB_EXPECT(persistent && persistent->status == (onRam ? backend::app::GateStatus::Warn
                                                              : backend::app::GateStatus::Pass),
                   "storage.persistent follows the destination's filesystem");
        std::error_code ec;
        if (std::filesystem::is_directory("/dev/shm", ec) && backend::app::recordingTarget("/dev/shm").ram) {
            const auto ramOut = "/dev/shm/mib_gate_" + std::to_string(processId()) + ".h5";
            const auto ramReadiness = backend.experiment().evaluateReadiness(ramOut, "pl");
            const auto* warn = gateOf(ramReadiness, "storage.persistent");
            MIB_EXPECT(warn && warn->status == backend::app::GateStatus::Warn &&
                           warn->reason.rfind("Recording to RAM: lost on power-off.", 0) == 0,
                       "a RAM destination warns at run start");
            MIB_EXPECT(!warn->blocksStart(), "and does not block the run");
        }
    }
    MIB_EXPECT(!gateOf(readiness, "camera.session") && !gateOf(readiness, "camera.geometry") &&
                   !gateOf(readiness, "camera.deliveryMode"),
               "the live-camera gates do not apply in Run");
    MIB_EXPECT(gateOf(readiness, "telemetry.transportLoss") &&
                   gateOf(readiness, "telemetry.transportLoss")->status == backend::app::GateStatus::Pass,
               "transport loss comes from the PL records in Run");

    // Raw LED: Service mode only, within the Run limits.
    MIB_EXPECT(!backend.setInstrumentLed(5.0, 70.0, &err) && err.find("Service") != std::string::npos,
               "raw LED refused outside Service mode (backend-side)");
    backend.setServiceMode(true);
    MIB_EXPECT(!backend.setInstrumentLed(7.0, 90.0, &err), "a Run width above 80 µs is refused");
    MIB_EXPECT(backend.setInstrumentLed(5.0, 70.0, &err) && s.live[S0 + 1] == 500 && s.live[S0 + 2] == 7000,
               "5/70 µs applied in Service mode: " + err);
    backend.setServiceMode(false);

    // Back to Align: cell path off before the camera starts again.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "back to Align: " + err);
    MIB_EXPECT(s.live[S0 + 46] == 0 && backend.capture().lifecycleSnapshot().cameraReady, "Align again, streaming");
    MIB_EXPECT(!cellOnWhileStreaming, "still never on while streaming");

    // The unattended safe state (the server asks for it when the last client has been gone for its
    // grace time): LED off, cell path off, camera released; resumed by the next mode switch.
    {
        MIB_EXPECT(s.live[S0 + 0] == 1 && !backend.instrumentIdle(), "Align is lit before the idle rule");
        auto idle = facade.setInstrumentMode("idle", 0, 0);
        MIB_EXPECT(idle.ok, "idle: " + idle.message);
        MIB_EXPECT(s.live[S0 + 0] == 0 && s.live[S0 + 46] == 0 && (s.live[8] & 0x10u) == 0,
                   "idle: LED off, cell path off");
        MIB_EXPECT(backend.instrumentMode() == pz::InstrumentMode::Unknown && backend.instrumentIdle() &&
                       !backend.capture().lifecycleSnapshot().isActive(),
                   "idle: mode unknown, camera released");
        MIB_EXPECT(facade.fetchInstrumentStatusJson().find("\"idle\":true") != std::string::npos, "status reports idle");
        MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run after idle: " + err);
        MIB_EXPECT(!backend.instrumentIdle() && s.live[S0 + 0] == 1 && s.live[S0 + 46] == 1, "a mode switch ends idle");
        idle = facade.setInstrumentMode("idle", 0, 0);
        MIB_EXPECT(idle.ok && s.live[S0 + 0] == 0 && s.live[S0 + 46] == 0, "idle from Run turns the LED and cell path off");
        MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align after idle: " + err);
    }

    // A blank PL refuses the switch and writes nothing.
    s.configured = false;
    const auto writes = s.writes.size();
    MIB_EXPECT(!backend.setInstrumentMode(pz::InstrumentMode::Run, 0, 0, &err) && err.find("PCFG_DONE") != std::string::npos,
               "mode switch refused with the PL blank");
    MIB_EXPECT(s.writes.size() == writes, "no register written");
    // Shutdown ends the session with the LED off (found on the PZ7035 on 2026-10-08: Studio left
    // the strobe pulsing, S[0] = 1, after the process exited).
    s.configured = true;
    MIB_EXPECT(s.live[S0 + 0] == 1, "the Align LED is on before shutdown");
    facade.shutdown();
    backend.shutdown(); // the server's bridge.shutdown() destroys the backend, which runs this
    MIB_EXPECT(s.live[S0 + 0] == 0, "shutdown switches the LED off");
}

// PL results in Run without an experiment (#651 G5): the provider runs while the instrument is in Run, stops
// for Align, idle and an experiment's hand-over, resumes afterwards, and restarts with a recompiled profile
// when a setting changes.
void testLiveResults(const mib::test::TempDir& td) {
    const auto frames = td.path() / "frames_live";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 512, 96), "mock frames");
    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", "none");
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((td.path() / "data_live").string()), "facade initializes");
    FakeState s;
    seedCellImage(s);
    backend.setInstrumentControlForTesting(std::make_unique<FakeControl>(s));
    auto provider = std::make_unique<FakeProvider>();
    FakeProvider* fake = provider.get();
    backend.setExecutionProviderForTesting(std::move(provider));
    std::string err;

    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align: " + err);
    MIB_EXPECT(!fake->running && fake->starts == 0 && !backend.liveResultsActive(), "Align: no live results");

    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run: " + err);
    MIB_EXPECT(fake->running && fake->configures == 1 && fake->starts == 1 && backend.liveResultsActive(),
               "Run: the provider is configured and started once, without an experiment");
    const auto firstPage = fake->lastPage;

    // The hand-over to an experiment, and back.
    backend.stopLiveResults();
    MIB_EXPECT(!fake->running && fake->stops == 1 && !backend.liveResultsActive(), "hand-over: the session stops");
    backend.stopLiveResults();
    MIB_EXPECT(fake->stops == 1, "stopping twice is harmless");
    backend.resumeLiveResults();
    MIB_EXPECT(fake->running && fake->starts == 2 && backend.liveResultsActive(), "after the run: the session resumes in Run");

    // A setting the PL implements changes: the profile is recompiled and the provider restarts with it.
    auto config = backend.processing().getProcessingConfig();
    config.area_threshold_max = config.area_threshold_max + 40;
    backend.processing().setProcessingConfig(config);
    const auto restarted = [&] {
        for (int i = 0; i < 40; ++i) {
            if (fake->starts == 3) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }();
    MIB_EXPECT(restarted && fake->running && fake->lastPage != firstPage, "a changed gate restarts the session with the new profile page");
    // A change the PL profile does not see (no page difference) does not restart it.
    const int startsBefore = fake->starts;
    backend.processing().setProcessingConfig(config); // the same values: a version bump, the same page
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    MIB_EXPECT(fake->starts == startsBefore, "an unchanged profile page does not restart the provider");

    // Align and idle stop it.
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align again: " + err);
    MIB_EXPECT(!fake->running && !backend.liveResultsActive(), "Align stops the live session");
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run again: " + err);
    MIB_EXPECT(fake->running, "Run starts it again");
    MIB_EXPECT(facade.setInstrumentMode("idle", 0, 0).ok && !fake->running && !backend.liveResultsActive(), "idle stops it");
    backend.resumeLiveResults();
    MIB_EXPECT(!fake->running, "no resume outside Run");
    facade.shutdown();
    backend.shutdown();
}

// Stop in Run holds the every-frame ring for playback and resume re-arms (#649 v1): only in Run, only with a ring, the
// provider stops (the freeze itself is the provider's, waited for there), the LED goes off, a mode switch ends the stopped Run,
// and the readiness gates say why an experiment cannot start from a stopped Run or with a ring that does not fit.
void testRingPlayback(const mib::test::TempDir& td) {
    const auto frames = td.path() / "frames_ring";
    MIB_REQUIRE(mib::test::writeFrames(frames, 8, 512, 96), "mock frames");
    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frames.string().c_str());
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,autofocus,trigger,playback");
    setEnv("MIB_EXECUTION_PROVIDER", "none");
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((td.path() / "data_ring").string()), "facade initializes");
    FakeState s;
    seedCellImage(s);
    backend.setInstrumentControlForTesting(std::make_unique<FakeControl>(s));
    auto provider = std::make_unique<FakeProvider>();
    FakeProvider* fake = provider.get();
    backend.setExecutionProviderForTesting(std::move(provider));
    std::string err;

    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run: " + err);
    MIB_EXPECT(!backend.freezeRun(&err) && err.find("no frame ring") != std::string::npos && !backend.runFrozen() && fake->running,
               "no ring configured: Stop is refused with the reason and Run goes on");

    fake->ringFrames = 100;
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align: " + err);
    MIB_EXPECT(!backend.freezeRun(&err) && err.find("applies in Run") != std::string::npos, "Stop is a Run action");
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run again: " + err);
    MIB_EXPECT(fake->running && s.live[S0 + 0] == 1, "Run: the provider runs and the LED is on");

    fake->ring.valid = true;
    fake->ring.records = 100;
    fake->ring.recordBytes = pz::kRingRecordBytes;
    fake->ring.head = 99;
    fake->ring.final = 100;
    fake->ring.lo = 0;
    fake->ring.frozen = true;
    const int stopsBefore = fake->stops;
    MIB_REQUIRE(backend.freezeRun(&err), "freeze: " + err);
    MIB_EXPECT(backend.runFrozen() && !fake->running && fake->stops == stopsBefore + 1 && s.live[S0 + 0] == 0 && !backend.liveResultsActive(),
               "Stop: the provider stopped, the LED is off, the Run is held");
    MIB_EXPECT(backend.freezeRun(&err) && fake->stops == stopsBefore + 1, "stopping twice is harmless");
    // The bridge view (ABI 34): the status JSON, the `ring` block of the instrument status, a frame, and the commands.
    {
        const auto j = nlohmann::json::parse(facade.fetchRingStatusJson());
        MIB_EXPECT(j["available"] == true && j["frozen"] == true && j["invalid"] == false && j["run_frozen"] == true &&
                       j["count"] == 100 && j["first_seq"] == 0 && j["last_seq"] == 99 && j["capacity_frames"] == 100,
                   "fetch_ring_status: a frozen ring of 100 frames, readable 0 to 99");
        const auto instrument = nlohmann::json::parse(facade.fetchInstrumentStatusJson());
        MIB_EXPECT(instrument.contains("ring") && instrument["ring"]["run_frozen"] == true, "the instrument status carries the ring block");
        std::string frameError;
        const auto packet = facade.fetchRingFramePacket(5, &frameError);
        MIB_EXPECT(packet.size() > 48 && std::memcmp(packet.data(), "MIBR", 4) == 0 && frameError.empty(), "fetch_ring_frame: a MIBR packet");
        MIB_EXPECT(facade.fetchRingFramePacket(500, &frameError).empty() && frameError.find("out_of_range") != std::string::npos,
                   "fetch_ring_frame: a sequence out of range says so");
        fake->ring.fault = true;
        fake->ring.frozen = false;
        fake->ring.invalidReason = "the ring stalled (RING_STALLED): re-arm";
        const auto bad = nlohmann::json::parse(facade.fetchRingStatusJson());
        MIB_EXPECT(bad["invalid"] == true && bad["frozen"] == false && bad["reason"].get<std::string>().find("RING_STALLED") != std::string::npos,
                   "an invalid ring is reported with the reason");
        // STOP_STUCK: not frozen, not invalid, flagged stop incomplete; the frames below FINAL stay readable.
        fake->ring.faultCleared = 0x104;
        MIB_EXPECT(nlohmann::json::parse(facade.fetchRingStatusJson())["fault_cleared"] == 0x104, "a PL fault cleared at Run start is in the ring status");
        fake->ring.faultCleared = 0;
        fake->ring.fault = false;
        fake->ring.frozen = false;
        fake->ring.stopStuck = true;
        fake->ring.stopIncomplete = true;
        fake->ring.invalidReason.clear();
        const auto stuck = nlohmann::json::parse(facade.fetchRingStatusJson());
        MIB_EXPECT(stuck["stop_incomplete"] == true && stuck["invalid"] == false && stuck["frozen"] == false && stuck["count"] == 100 &&
                       stuck["restore_needed"] == false,
                   "STOP_STUCK: stop incomplete, not invalid, the range stays");
        MIB_EXPECT(facade.fetchRingFramePacket(7, &frameError).size() > 48, "STOP_STUCK: a frame below FINAL is still served");
        fake->ring.stopStuck = false;
        fake->ring.stopIncomplete = false;
        // A stop that never reached IDLE: restore needed.
        fake->ring.restoreNeeded = true;
        fake->ring.valid = false;
        fake->ring.why = "the frame ring never reached idle after STOP: the PL needs a restore";
        const auto restore = nlohmann::json::parse(facade.fetchRingStatusJson());
        MIB_EXPECT(restore["restore_needed"] == true && restore["reason"].get<std::string>().find("restore") != std::string::npos,
                   "a stop that never reached IDLE is reported as restore needed");
        fake->ring.restoreNeeded = false;
        fake->ring.valid = true;
        fake->ring.why.clear();
        fake->ring.frozen = true;
        fake->placementProblem = "frame ring: boot with a smaller mem=";
        const auto nofit = nlohmann::json::parse(facade.fetchRingStatusJson());
        MIB_EXPECT(nofit["available"] == false && nofit["reason"].get<std::string>().find("mem=") != std::string::npos, "a ring that does not fit is unavailable with the remedy");
        fake->placementProblem.clear();
    }
    {
        const auto st = backend.ringStatus();
        MIB_EXPECT(st.valid && st.count() == 100 && st.frozen, "the ring's status reaches the backend");
        std::vector<uint8_t> packet;
        MIB_EXPECT(backend.ringFrame(42, packet, &err) == pz::RingRead::Ok && packet.size() > 48 && std::memcmp(packet.data(), "MIBR", 4) == 0 &&
                       packet[8] == 42,
                   "a frame comes back as a MIBR packet of that sequence");
        MIB_EXPECT(backend.ringFrame(100, packet, &err) == pz::RingRead::OutOfRange, "a sequence beyond FINAL is out of range");
    }
    // An experiment cannot start from a stopped Run, and a ring that does not fit says why.
    {
        const auto out = (td.path() / "ring_exp.h5").string();
        auto readiness = backend.experiment().evaluateReadiness(out, "pl");
        const auto* frozen = readiness.gate("run.frozen");
        MIB_EXPECT(frozen && frozen->blocksStart(), "a stopped Run blocks an experiment (run.frozen)");
        fake->placementProblem = "frame ring: Linux owns all the DDR below the PL area: boot with a smaller mem=";
        readiness = backend.experiment().evaluateReadiness(out, "pl");
        const auto* placement = readiness.gate("ring.placement");
        MIB_EXPECT(placement && placement->blocksStart() && placement->reason.find("mem=") != std::string::npos,
                   "a ring that does not fit blocks with the remedy (ring.placement)");
        fake->placementProblem.clear();
    }
    // A resume whose start fails: the LED stays off, the Run stays held, and the intact frozen ring is still readable.
    {
        fake->failStart = true;
        MIB_EXPECT(!backend.resumeRun(&err) && err.find("restore the PL") != std::string::npos, "a failed start fails the resume with its reason");
        MIB_EXPECT(backend.runFrozen() && s.live[S0 + 0] == 0 && !backend.liveResultsActive(), "a failed resume keeps the Run held with the LED off (never lit without frames)");
        std::vector<uint8_t> packet;
        MIB_EXPECT(backend.ringFrame(42, packet, &err) == pz::RingRead::Ok && backend.ringStatus().frozen, "the frozen ring is still readable after a failed resume");
        fake->failStart = false;
    }
    // Resume re-arms: a new ring, the LED back on.
    const int startsBefore = fake->starts;
    MIB_REQUIRE(facade.resumeRun().ok, "resume through the facade");
    MIB_EXPECT(!backend.runFrozen() && fake->running && fake->starts == startsBefore + 1 && s.live[S0 + 0] == 1 && backend.liveResultsActive(),
               "Resume: the session is armed again and the LED is on");
    MIB_EXPECT(backend.resumeRun(&err) && fake->starts == startsBefore + 1, "resuming a running Run is harmless");
    // A mode switch ends a stopped Run.
    MIB_REQUIRE(facade.freezeRun().ok, "freeze again through the facade");
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Align, 0, 0, &err), "Align from a stopped Run: " + err);
    MIB_EXPECT(!backend.runFrozen(), "Align ends the stopped Run");
    MIB_REQUIRE(backend.setInstrumentMode(pz::InstrumentMode::Run, 152, 200, &err), "Run: " + err);
    MIB_REQUIRE(backend.freezeRun(&err), "freeze before idle: " + err);
    MIB_EXPECT(facade.setInstrumentMode("idle", 0, 0).ok && !backend.runFrozen(), "idle ends it too");
    facade.shutdown();
    backend.shutdown();
}

// The Run window survives a restart (#501): a Run switch writes <data>/instrument_run_window.json,
// the next initialize() reads it; a file that is not a window the switch could have applied is
// ignored. testModeSequence left (152, 200) there.
void testRunWindowPersists(const mib::test::TempDir& td) {
    const auto data = td.path() / "data";
    const auto file = data / "instrument_run_window.json";
    MIB_REQUIRE(std::filesystem::exists(file), "the Run switch saved the window");
    const auto restart = [&]() {
        backend::AppBackend backend;
        backend::bridge::BackendFacade facade(backend);
        MIB_REQUIRE(facade.initialize(data.string()), "facade initializes");
        const auto set = backend.instrumentRunWindowSet();
        const auto offset = backend.instrumentRunOffset();
        facade.shutdown();
        return std::make_pair(set, offset);
    };
    auto r = restart();
    MIB_EXPECT(r.first && r.second == std::make_pair(152, 200), "a restart restores the Run window");

    const auto write = [&](const std::string& text) { std::ofstream(file, std::ios::trunc) << text; };
    write("{\"x\":157,\"y\":200,\"set\":true}");
    MIB_EXPECT(!restart().first, "an off-grid x is ignored");
    write("{\"x\":152,\"y\":600,\"set\":true}");
    MIB_EXPECT(!restart().first, "a window off the sensor is ignored");
    write("not json");
    MIB_EXPECT(!restart().first, "garbage is ignored");
    write("{\"x\":152,\"y\":200,\"set\":false}");
    MIB_EXPECT(!restart().first, "a file that says 'not set' is ignored");
    std::filesystem::remove(file);
    MIB_EXPECT(!restart().first, "no file: no window");
}

// Both states of the persistence warning (#501): a RAM-backed destination (tmpfs, like the JTAG
// RAM root) warns with the operator text; a disk destination does not.
void testRecordingTarget(const mib::test::TempDir& td) {
    namespace fs = std::filesystem;
    const auto disk = backend::app::recordingTarget((td.path() / "runs" / "run.h5").string());
    MIB_EXPECT(disk.exists && disk.writable, "a temp dir on disk is writable: " + disk.path);
    std::error_code ec;
    const fs::path shm = "/dev/shm";
    if (fs::is_directory(shm, ec) && backend::app::recordingTarget(shm.string()).filesystem == "tmpfs") {
        const auto dir = shm / ("mib_target_" + std::to_string(processId()));
        fs::create_directories(dir, ec);
        const auto ram = backend::app::recordingTarget((dir / "run.h5").string());
        const auto warning = backend::app::recordingTargetWarning(ram);
        MIB_EXPECT(ram.ram && ram.writable, "tmpfs is RAM");
        MIB_EXPECT(warning.rfind("Recording to RAM: lost on power-off. Copy data off before shutdown. ", 0) == 0 &&
                       warning.find(" GB free.") != std::string::npos,
                   "the operator warning: " + warning);
        fs::remove_all(dir, ec);
    } else {
        std::fprintf(stderr, "instrument_modes: /dev/shm is not tmpfs here; RAM state checked by construction only\n");
    }
    backend::app::RecordingTarget ram;
    ram.ram = true;
    ram.freeBytes = 2'500'000'000ull;
    MIB_EXPECT(backend::app::recordingTargetWarning(ram) ==
                   "Recording to RAM: lost on power-off. Copy data off before shutdown. 2.5 GB free.",
               "warning wording");
    if (!disk.ram) MIB_EXPECT(backend::app::recordingTargetWarning(disk).empty(), "a disk destination does not warn");
}

} // namespace

int main() {
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("instrument_modes");
    testControl();
    testLedLimits();
    testAlignIngressRecovery();
    testRxSelfHeal();
    testRxHealCtrl();
    testModeSequence(td);
    testLiveResults(td);
    testRingPlayback(td);
    testRunWindowPersists(td);
    testRecordingTarget(td);
    return mib::test::exitCode();
}
