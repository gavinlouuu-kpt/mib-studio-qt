// PzPlatformMonitor (#501 P0) against fake PZ7035 registers: identity in the
// `pzres id` order, the expected core and the pinned weights, LED presets and
// the guard, link-counter rates (a decrease is a reset), latency in µs.
#include "backend/pz/PzPlatformMonitor.h"

#include "pz_mib_abi.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <cmath>
#include <fstream>
#include <map>
#include <vector>

namespace pz = backend::pz;

namespace {

struct FakeRegisters final : pz::IPzPlatformRegisters {
    std::map<unsigned, uint32_t> livePage, strobeWindow;
    std::map<uint32_t, uint32_t> bridgePage;
    bool configured = true;
    int plReads = 0; // reads of the PL window (a blank PL would stall the bus)
    std::vector<uint32_t> tornReads; // queued values for P[tornIndex] before the page value (a torn counter)
    unsigned tornIndex = 0;
    uint32_t live(unsigned i) override {
        ++plReads;
        if (i == tornIndex && !tornReads.empty()) {
            const uint32_t v = tornReads.front();
            tornReads.erase(tornReads.begin());
            return v;
        }
        return livePage[i];
    }
    std::vector<uint32_t> tornStrobeReads; // queued values for S[tornStrobeIndex] before the window value
    unsigned tornStrobeIndex = 0;
    uint32_t strobe(unsigned i) override {
        ++plReads;
        if (i == tornStrobeIndex && !tornStrobeReads.empty()) {
            const uint32_t v = tornStrobeReads.front();
            tornStrobeReads.erase(tornStrobeReads.begin());
            return v;
        }
        return strobeWindow[i];
    }
    uint32_t bridge(uint32_t off) override { ++plReads; return bridgePage[off]; }
    bool plConfigured(std::string* why) override {
        if (!configured && why) *why = "PL not configured (DEVCFG PCFG_DONE = 0): load the PL image";
        return configured;
    }
};

// results6 (pz7035-imx426 76aea76) with the release weights.
void loadResults6(FakeRegisters& r) {
    r.bridgePage[PZ_MIB_REG_IDENTITY] = PZ_MIB_IDENTITY_MAGIC;
    r.bridgePage[PZ_MIB_REG_ABI_VERSION] = 0x00010002u;
    r.bridgePage[PZ_MIB_REG_SCIENCE_PROFILE] = (2u << 16) | 2u;
    r.bridgePage[PZ_MIB_REG_BUILD_ID3] = 0x76aea765u;
    r.bridgePage[PZ_MIB_REG_BUILD_ID2] = 0x5189e35bu;
    r.bridgePage[PZ_MIB_REG_BUILD_ID1] = 0x1b629289u;
    r.bridgePage[PZ_MIB_REG_BUILD_ID0] = 0xf363df5eu;
    r.bridgePage[PZ_MIB_REG_PROFILE_ID3] = 0xeea09a3fu;
    r.bridgePage[PZ_MIB_REG_PROFILE_ID2] = 0x9cbf552cu;
    r.bridgePage[PZ_MIB_REG_PROFILE_ID1] = 0x749fa66eu;
    r.bridgePage[PZ_MIB_REG_PROFILE_ID0] = 0x205ef961u;
    r.strobeWindow[0] = 1;    // XVS sync
    r.strobeWindow[1] = 700;  // 7 µs
    r.strobeWindow[2] = 6000; // 60 µs
}

constexpr const char* kExpectedResults6 = R"({
  "build_id": "76aea7655189e35b1b629289f363df5e",
  "profile_id": "eea09a3f9cbf552c749fa66e205ef961",
  "commit": "76aea7655189e35b1b629289f363df5ea4c6d31f",
  "abi": {"major": 1, "minor": 2},
  "science_profile": 2,
  "profile_version": 2,
  "image": "pz_live_results6"
})";

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream(p) << text;
}

} // namespace

int main() {
    MIB_REQUIRE(std::string(pz::pinnedUnetProfileId()) == "eea09a3f9cbf552c749fa66e205ef961",
                "pinned weights id compiled from env/assets.json");

    mib::test::TempDir td("pz_platform_monitor");
    const auto expectedPath = (td.path() / "expected-core.json").string();

    // No platform: unavailable, with the reason.
    {
        pz::PzPlatformMonitor monitor(nullptr, "MIB_EXECUTION_PROVIDER is not pz", expectedPath);
        const auto s = monitor.sample(1);
        MIB_EXPECT(!s.available && s.error == "MIB_EXECUTION_PROVIDER is not pz", "no registers: unavailable");
    }

    // results6, no expected-core.json: build unknown, weights checked anyway.
    auto regs = std::make_unique<FakeRegisters>();
    auto* r = regs.get();
    loadResults6(*r);
    pz::PzPlatformMonitor monitor(std::move(regs), {}, expectedPath);
    {
        const auto s = monitor.sample(1'000'000);
        MIB_EXPECT(s.available, "bridge present");
        MIB_EXPECT(s.buildId == "76aea7655189e35b1b629289f363df5e" &&
                       s.profileId == "eea09a3f9cbf552c749fa66e205ef961",
                   "ids read ID3 first, as pzres id");
        MIB_EXPECT(s.abiVersion == 0x00010002u && s.scienceProfile == 2 && s.profileVersion == 2, "abi and profile");
        MIB_EXPECT(!s.expectedPresent && s.buildMatch == pz::IdMatch::Unknown, "no expected-core.json: build unknown");
        MIB_EXPECT(s.profileMatch == pz::IdMatch::Match, "release weights match the pin");
        MIB_EXPECT(s.ledOn && s.ledPreset == "run" && std::abs(s.ledDelayUs - 7.0) < 1e-9 &&
                       std::abs(s.ledWidthUs - 60.0) < 1e-9,
                   "LED Run preset 7/60");
        MIB_EXPECT(!s.guardFault && s.guardTrips == 0, "guard clear");
        MIB_EXPECT(!s.ratesValid, "first sample: no rates yet");
    }

    // With the expected core: build matches; another image mismatches.
    write(expectedPath, kExpectedResults6);
    {
        const auto s = monitor.sample(2'000'000);
        MIB_EXPECT(s.expectedPresent && s.expected.image == "pz_live_results6" && s.expected.abiMinor == 2,
                   "expected core parsed");
        MIB_EXPECT(s.buildMatch == pz::IdMatch::Match, "build id matches expected-core.json");
    }
    r->bridgePage[PZ_MIB_REG_BUILD_ID0] = 0x00000001u;
    r->bridgePage[PZ_MIB_REG_PROFILE_ID0] = 0x00000001u;
    {
        const auto s = monitor.sample(3'000'000);
        MIB_EXPECT(s.buildMatch == pz::IdMatch::Mismatch && s.profileMatch == pz::IdMatch::Mismatch,
                   "another image and other weights mismatch");
    }
    loadResults6(*r);

    // Link rates from counter deltas over 2 s; a decrease reads as a reset (0).
    r->livePage[12] = 1000; // ingress errors
    r->livePage[14] = 10;   // resyncs
    r->livePage[7] = 5;     // bad frames
    (void)monitor.sample(10'000'000);
    r->livePage[12] = 1004;
    r->livePage[14] = 30;
    r->livePage[7] = 5;
    {
        const auto s = monitor.sample(12'000'000);
        MIB_EXPECT(s.ratesValid && std::abs(s.ingressErrorsPerS - 2.0) < 1e-9 && std::abs(s.resyncsPerS - 10.0) < 1e-9 &&
                       s.badFramesPerS == 0.0,
                   "rates per second from deltas");
        MIB_EXPECT(s.resyncsPerS > pz::kResyncWarnPerS && s.ingressErrorsPerS < pz::kIngressErrorWarnPerS,
                   "thresholds: baseline errors pass, 10 resyncs/s warns");
    }
    r->livePage[12] = 3; // PL reloaded: counters restart
    {
        const auto s = monitor.sample(13'000'000);
        MIB_EXPECT(s.ratesValid && s.ingressErrorsPerS == 0.0, "a counter decrease is a reset, not a negative rate");
    }

    // Guard trip, Align preset, LED off, latency.
    r->strobeWindow[12] = 1u << 4;
    r->strobeWindow[13] = 0x80000003u;
    r->strobeWindow[49] = 19425; // 111 µs at 175 MHz
    r->strobeWindow[50] = 0;
    r->strobeWindow[51] = 300118;
    {
        const auto s = monitor.sample(14'000'000);
        MIB_EXPECT(s.guardFault && s.guardTrips == 3, "guard fault and trips");
        MIB_EXPECT(std::abs(s.latencyMaxUs - 111.0) < 1e-9 && s.latencyFrames == 300118, "latency in µs");
    }
    r->strobeWindow[1] = 0;
    r->strobeWindow[2] = 12500;
    MIB_EXPECT(monitor.sample(15'000'000).ledPreset == "align", "LED Align preset 0/125");
    r->strobeWindow[1] = 10000;
    r->strobeWindow[2] = 13500;
    MIB_EXPECT(monitor.sample(15'500'000).ledPreset == "align", "LED Align preset 100/135 (results8 whole frames)");
    r->strobeWindow[0] = 0;
    MIB_EXPECT(monitor.sample(16'000'000).ledPreset == "off", "LED off");

    // PL blank (power-up, JTAG reload): unavailable, and nothing in the PL
    // window is read; rates restart once it is back.
    r->configured = false;
    r->plReads = 0;
    {
        const auto s = monitor.sample(16'500'000);
        MIB_EXPECT(!s.available && s.error.find("PCFG_DONE") != std::string::npos, "blank PL: unavailable, says why");
        MIB_EXPECT(r->plReads == 0, "blank PL: no PL register is read");
    }
    r->configured = true;
    {
        const auto first = monitor.sample(16'700'000);
        MIB_EXPECT(first.available && !first.ratesValid, "configured again: available, rates restart");
        MIB_EXPECT(monitor.sample(16'800'000).ratesValid, "rates resume on the second sample");
    }

    // Sensor readout: S[9] is the XVS period in 100 MHz clocks, S[29] the ingress geometry.
    {
        const auto closed = monitor.sample(17'000'000);
        MIB_EXPECT(closed.xvsPeriodClocks == 0 && closed.xvsFps == 0.0, "no XVS while the sensor is closed");
        r->strobeWindow[9] = 249966;       // 400 fps, as measured on the board
        r->strobeWindow[29] = 0x02702066u; // 624 lines, 32 OB lines, 102 slots of 8 pixels
        const auto open = monitor.sample(17'100'000);
        MIB_EXPECT(open.xvsPeriodClocks == 249966 && std::abs(open.xvsFps - 400.0544) < 1e-3, "XVS period as fps");
        MIB_EXPECT(open.geometryWidth == 816 && open.geometryHeight == 624, "ingress geometry 816x624");
        r->strobeWindow[29] = 0x00602040u; // the 512x96 default
        const auto run = monitor.sample(17'200'000);
        MIB_EXPECT(run.geometryWidth == 512 && run.geometryHeight == 96, "default geometry 512x96");
    }

    // The latency words can tear (no CDC from the 175 MHz domain): a torn read is repeated.
    {
        r->strobeWindow[50] = 7; // over budget
        r->tornStrobeReads = {0x0000FFFFu, 7u, 7u};
        r->tornStrobeIndex = 50;
        const auto s = monitor.sample(17'800'000);
        MIB_EXPECT(s.latencyOverBudget == 7, "a torn latency word is read again until two reads match");
        r->strobeWindow[50] = 0;
        r->tornStrobeIndex = 0;
    }

    // The PL receiver self-heal block (results9, RXH1 at P[256]): absent on results8, then present.
    {
        const auto before = monitor.sample(17'900'000);
        MIB_EXPECT(!before.rxHealPresent && !monitor.rxHealAutoResets().has_value(), "no RXH1 block: absent, no count");
        r->livePage[256] = 0x52584831u;
        r->livePage[258] = 0x100u | 5u; // gave up after 5 tries
        r->livePage[259] = 9;
        const auto after = monitor.sample(17'950'000);
        MIB_EXPECT(after.rxHealPresent && after.rxHealGaveUp && after.rxHealTries == 5 && after.rxHealAutoResets == 9,
                   "RXH1 status words");
        MIB_EXPECT(monitor.rxHealAutoResets().value_or(0) == 9, "auto-reset count for the run accounting");
        // The counter crosses clock domains with no CDC and can tear: read again until two reads match.
        r->tornReads = {0x00FF00FFu, 0x00000009u, 0x00000009u};
        r->tornIndex = 259;
        MIB_EXPECT(monitor.rxHealAutoResets().value_or(0) == 9, "a torn counter read is repeated until two reads match");
        // Heal v2 (CTRL2 at word 24 non-zero): flag clears, episodes and failed episodes are read.
        MIB_EXPECT(!monitor.sample(17'955'000).rxHealV2, "v1: no v2 counters");
        r->livePage[280] = 0x03e80001u;
        r->livePage[261] = 4;  // flag clears
        r->livePage[262] = 3;  // episodes
        r->livePage[263] = 1;  // failed episodes
        const auto v2 = monitor.sample(17'956'000);
        MIB_EXPECT(v2.rxHealV2 && v2.rxHealFlagClears == 4 && v2.rxHealEpisodes == 3 && v2.rxHealFailedEpisodes == 1 &&
                       v2.rxHealAutoResets == 9,
                   "v2 counters beside the receiver-reset count");
        r->livePage[280] = 0;
        // results12: frames start only after a FrameStart line. No counters (fs_seen 0): absent, no count.
        MIB_EXPECT(!monitor.sample(17'957'000).rxFsPresent && !monitor.rxNoFsFrames().has_value(), "no FS counters: absent");
        r->livePage[288] = 5000; // fs_seen
        r->livePage[289] = 3;    // nofs_frames
        r->livePage[290] = 21;   // nofs_lines
        const auto fs = monitor.sample(17'958'000);
        MIB_EXPECT(fs.rxFsPresent && fs.rxFsSeen == 5000 && fs.rxNoFsFrames == 3 && fs.rxNoFsLines == 21, "FS counter words 32-34");
        r->tornReads = {0x00FF00FFu, 3u, 3u};
        r->tornIndex = 289;
        MIB_EXPECT(monitor.rxNoFsFrames().value_or(99) == 3, "a torn nofs_frames read is repeated until two reads match");
        r->configured = false;
        MIB_EXPECT(!monitor.rxNoFsFrames().has_value(), "no read while the PL is blank");
        r->configured = true;
        r->livePage[288] = r->livePage[289] = r->livePage[290] = 0;
        r->configured = false;
        MIB_EXPECT(!monitor.rxHealAutoResets().has_value(), "no read while the PL is blank");
        r->configured = true;
        r->livePage[256] = 0;
        r->livePage[258] = 0;
        r->livePage[259] = 0;
        (void)monitor.sample(17'960'000);
    }

    // Ingress errors are judged as a 5 s average: a burst of 13 inside the window does not warn,
    // 12/s sustained does, and the window restarts after a mode-switch settle.
    {
        r->strobeWindow[9] = 250000;
        r->livePage[12] = 1000;
        monitor.settle(18'000'000);
        (void)monitor.sample(18'000'000);
        (void)monitor.sample(19'600'000); // past the settle window: history starts
        auto at = [&](uint64_t us, uint32_t errors) {
            r->livePage[12] = errors;
            return monitor.sample(us);
        };
        MIB_EXPECT(!at(20'000'000, 1000).ingressErrorsWarn, "no full window yet");
        const auto burst = at(23'000'000, 1013); // a burst of 13 in 3 s
        MIB_EXPECT(!burst.ingressErrorsWarn && burst.ingressErrorsAvgPerS == 0.0, "window still filling");
        const auto after = at(25'000'000, 1013); // 5.4 s after the first sample: the average is 13 / 5.4 ≈ 2.4/s
        MIB_EXPECT(!after.ingressErrorsWarn && after.ingressErrorsAvgPerS > 2.0 && after.ingressErrorsAvgPerS < 3.0,
                   "a burst of 13 averages to about 2.4/s: no warning");
        MIB_EXPECT(after.ingressErrorsPerS == 0.0 || after.ingressErrorsPerS < 10.0, "instantaneous rate for the readout only");
        // 12/s sustained: 12 more every second for 6 s.
        uint32_t errors = 1013;
        uint64_t t = 25'000'000;
        pz::PzPlatformStatus last;
        for (int i = 0; i < 6; ++i) {
            t += 1'000'000;
            errors += 12;
            last = at(t, errors);
        }
        MIB_EXPECT(last.ingressErrorsAvgPerS > 11.5 && last.ingressErrorsWarn, "12/s sustained for 5 s warns");
        // The link recovers: the warning clears once the window no longer holds the errors.
        for (int i = 0; i < 6; ++i) {
            t += 1'000'000;
            last = at(t, errors);
        }
        MIB_EXPECT(!last.ingressErrorsWarn && last.ingressErrorsAvgPerS == 0.0, "clean for 5 s: no warning");
        // A mode switch restarts the window.
        errors += 600;
        monitor.settle(t + 100'000);
        const auto switched = at(t + 1'000'000, errors);
        MIB_EXPECT(!switched.ingressErrorsWarn && !switched.ratesValid, "inside the settle window: no warning");
    }

    // Bad and dropped frames are judged against the frame rate (400 fps here: bad above 4/s, dropped
    // above 0.4/s) and warn only once sustained for 5 s; a blip does not, a closed sensor never does.
    {
        r->bridgePage[PZ_MIB_REG_STATE] = PZ_MIB_STATE_RUNNING; // dropped frames count only while the bridge is consumed
        r->strobeWindow[9] = 250000; // 400 fps
        r->livePage[7] = 0;
        r->livePage[6] = 0;
        (void)monitor.sample(30'000'000);
        r->livePage[7] = 100; // 100 bad frames in 10 s = 10/s: above 1% of 400
        r->livePage[6] = 2;   // 0.2 dropped/s: below 0.1% of 400 (0.4)
        auto s0 = monitor.sample(40'000'000);
        MIB_EXPECT(s0.ratesValid && s0.badFramesWarnPerS == 4.0 && std::abs(s0.droppedWarnPerS - 0.4) < 1e-9, "thresholds follow the frame rate");
        MIB_EXPECT(!s0.badFramesWarn && !s0.droppedWarn, "above the threshold, but not yet for 5 s");
        r->livePage[7] = 150;
        r->livePage[6] = 3;
        auto s1 = monitor.sample(43'000'000); // 16.7 bad/s for 3 s
        MIB_EXPECT(!s1.badFramesWarn, "3 s above the threshold is not sustained");
        r->livePage[7] = 250;
        r->livePage[6] = 4;
        auto s2 = monitor.sample(46'000'000);
        MIB_EXPECT(s2.badFramesWarn, "6 s above the threshold is sustained");
        MIB_EXPECT(!s2.droppedWarn, "0.33 dropped/s is below 0.1% of 400");
        // The rate drops back: the warning clears at once and the clock restarts.
        auto s3 = monitor.sample(47'000'000);
        MIB_EXPECT(!s3.badFramesWarn, "back under the threshold: no warning");
        r->livePage[6] = 30;  // a burst of drops: 26/3 s ≈ 8.7/s
        (void)monitor.sample(48'000'000);
        r->livePage[6] = 60;
        (void)monitor.sample(53'500'000);
        r->livePage[6] = 90;
        MIB_EXPECT(monitor.sample(59'000'000).droppedWarn, "sustained dropped frames warn");
        // Nothing consumes the bridge (stopped): P[6] counts every frame as dropped, which is not loss.
        r->bridgePage[PZ_MIB_REG_STATE] = PZ_MIB_STATE_IDLE;
        r->livePage[6] = 4000;
        (void)monitor.sample(60'200'000);
        r->livePage[6] = 8000;
        (void)monitor.sample(66'200'000);
        r->livePage[6] = 12000;
        const auto idle = monitor.sample(72'200'000);
        MIB_EXPECT(!idle.bridgeActive && idle.droppedPerS > idle.droppedWarnPerS && !idle.droppedWarn,
                   "dropped frames do not warn while the bridge is stopped, however high the counter runs");
        r->bridgePage[PZ_MIB_REG_STATE] = PZ_MIB_STATE_ARMED;
        r->livePage[6] = 16000;
        (void)monitor.sample(73'200'000);
        r->livePage[6] = 20000;
        (void)monitor.sample(79'200'000);
        r->livePage[6] = 24000;
        MIB_EXPECT(monitor.sample(85'200'000).droppedWarn, "armed bridge: the same drops warn once sustained");
        r->bridgePage[PZ_MIB_REG_STATE] = PZ_MIB_STATE_RUNNING;
        // Sensor closed: thresholds are 0 and nothing warns.
        r->strobeWindow[9] = 0;
        r->livePage[6] = 500;
        MIB_EXPECT(!monitor.sample(60'000'000).droppedWarn, "no warning while the sensor is closed");
        r->strobeWindow[9] = 249966;
    }

    // A mode switch resets the receiver: rates are invalid for the settle window, then resume from
    // the end of it (a spike during the switch never shows).
    r->livePage[12] = 100;
    (void)monitor.sample(18'000'000);
    r->livePage[12] = 5000; // the counters jump during the switch
    monitor.settle(18'100'000);
    MIB_EXPECT(!monitor.sample(18'200'000).ratesValid && !monitor.sample(19'000'000).ratesValid,
               "no rates inside the settle window");
    {
        const auto after = monitor.sample(19'700'000); // past 18'100'000 + 1.5 s
        MIB_EXPECT(after.ratesValid && after.ingressErrorsPerS == 0.0,
                   "rates resume after the window, measured from inside it: the switch's spike never shows");
        r->livePage[12] = 5002;
        const auto next = monitor.sample(20'700'000);
        MIB_EXPECT(next.ratesValid && std::abs(next.ingressErrorsPerS - 2.0) < 1e-9, "and are normal afterwards");
    }

    // No bridge (PL not loaded): unavailable.
    r->bridgePage[PZ_MIB_REG_IDENTITY] = 0;
    {
        const auto s = monitor.sample(17'000'000);
        MIB_EXPECT(!s.available && !s.error.empty(), "unloaded PL: unavailable");
    }

    // expected-core.json validation.
    std::string error;
    MIB_EXPECT(!pz::parseExpectedCore("[]", &error) && !error.empty(), "not an object");
    MIB_EXPECT(!pz::parseExpectedCore(R"({"build_id":"abc"})", &error), "short build id refused");
    return mib::test::exitCode();
}
