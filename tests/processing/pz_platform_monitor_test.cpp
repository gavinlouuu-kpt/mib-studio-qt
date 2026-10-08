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

namespace pz = backend::pz;

namespace {

struct FakeRegisters final : pz::IPzPlatformRegisters {
    std::map<unsigned, uint32_t> livePage, strobeWindow;
    std::map<uint32_t, uint32_t> bridgePage;
    bool configured = true;
    int plReads = 0; // reads of the PL window (a blank PL would stall the bus)
    uint32_t live(unsigned i) override { ++plReads; return livePage[i]; }
    uint32_t strobe(unsigned i) override { ++plReads; return strobeWindow[i]; }
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
