#pragma once

// Read-only health and identity of the PZ7035 PL for the instrument UI
// (mib-studio-qt #501 P0; pz7035-imx426 docs/YOFO_HOST_INTERFACE.md, PR #10
// 5b30761). It never writes a register: sensor timing, the command word and
// the LED belong to the one backend owner of mode switches (P0b), and the
// bridge belongs to the execution provider.
//
// Sources:
// - bridge identity registers (PZ_MIB_REG_BUILD_ID*, PROFILE_ID*, ABI_VERSION,
//   SCIENCE_PROFILE), ID3 first as `pzres id` prints them;
// - the expected core, /etc/yofo/expected-core.json (written by
//   pz7035-imx426 scripts/pz_install_core.sh; missing = BUILD_ID "unknown");
// - the pinned weights: PROFILE_ID must equal the first 32 hex of the
//   unet-c4-multiline-v1 .npz sha256 in env/assets.json (compiled in);
// - the strobe window S[i] (LED control, delay, width, guard, latency);
// - the live page P[i] (cumulative link counters; rates from deltas, a
//   decrease is a PL reset, not a negative rate).
// Qt-free.

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backend::pz {

// Register reads of one PZ7035 image. Implementations: /dev/mem on the PS,
// a fake in tests.
class IPzPlatformRegisters {
public:
    virtual ~IPzPlatformRegisters() = default;
    // Live page P[i] = 0x40100000 + 4*i.
    virtual uint32_t live(unsigned index) = 0;
    // Strobe/cell window S[i] = 0x40100200 + 4*i.
    virtual uint32_t strobe(unsigned index) = 0;
    // Bridge register at byte offset `offset` from 0x40101000.
    virtual uint32_t bridge(uint32_t offset) = 0;
    // Whether the PL is configured. Reads of the PL window while it is blank
    // (power-up, a JTAG reload) stall the AXI bus, so callers check this
    // before every read. Fakes are always configured unless they say not.
    virtual bool plConfigured(std::string* why) {
        (void)why;
        return true;
    }
};

#if defined(__linux__)
// DEVCFG INT_STS (0xF800700C) bit 2, PCFG_DONE: the PL is configured. Read
// only; never written (write-1-to-clear). Reads 0x00020004 under Linux after
// a JTAG load. False with `error` when /dev/mem fails or the PL is blank.
bool pzPlConfigured(std::string* error, uint32_t* intSts = nullptr);

// Maps the live, bridge and DEVCFG pages read-only through /dev/mem (O_SYNC).
// Mapping touches no PL register, so it succeeds with the PL blank; reads
// check PCFG_DONE first. Null with `error` when /dev/mem is unavailable.
std::unique_ptr<IPzPlatformRegisters> openDevMemPlatformRegisters(std::string* error);
#endif

// /etc/yofo/expected-core.json (pz7035-imx426 scripts/pz_core_json.py).
struct ExpectedCore {
    std::string buildId;   // 32 hex
    std::string profileId; // 32 hex
    std::string commit;
    std::string image;
    int abiMajor{0};
    int abiMinor{0};
    int scienceProfile{0};
    int profileVersion{0};
    // Build features (pz7035-imx426 scripts/pz_core_json.py, from git ancestry), e.g.
    // "align_whole_frame_preview" for results8 on (a781ec5).
    std::vector<std::string> features;
    bool has(const std::string& feature) const;
};
inline constexpr const char* kFeatureAlignWholeFrame = "align_whole_frame_preview";
std::optional<ExpectedCore> parseExpectedCore(const std::string& json, std::string* error);
std::optional<ExpectedCore> loadExpectedCore(const std::string& path, std::string* error);
inline constexpr const char* kExpectedCorePath = "/etc/yofo/expected-core.json";

// The weights the PL must run: first 32 hex of the pinned .npz sha256.
const char* pinnedUnetProfileId();

enum class IdMatch { Match, Mismatch, Unknown };
const char* idMatchName(IdMatch m);

// Rates below these are the known baselines (docs/YOFO_HOST_INTERFACE.md):
// ingress errors 0.2/s dark, ~1-2/s with the LED at 7/60 in bursts of 8-13 (Run at 5 kHz, measured
// 2026-10-08: 1.12/s); resyncs ~0.02-0.1/s. Ingress errors are judged as a kSustainedUs average
// (one burst must not warn, a link that really degrades stays above the threshold).
inline constexpr double kIngressErrorWarnPerS = 10.0;
inline constexpr double kResyncWarnPerS = 1.0;
// Bad and dropped frames are judged against the sensor's frame rate, and only when the rate stays
// above the threshold for kSustainedUs (a drop in Run is data loss; a blip is not a warning).
inline constexpr double kBadFramesWarnFraction = 0.01;   // of the XVS frame rate
inline constexpr double kDroppedWarnFraction = 0.001;    // of the XVS frame rate
inline constexpr uint64_t kSustainedUs = 5'000'000;

struct PzPlatformStatus {
    bool available{false};
    std::string error;

    // Identity read from the bridge.
    std::string buildId;
    std::string profileId;
    uint32_t abiVersion{0};
    uint16_t scienceProfile{0};
    uint16_t profileVersion{0};
    // Against the expected core and the pinned weights.
    bool expectedPresent{false};
    ExpectedCore expected;
    std::string pinnedProfileId;
    IdMatch buildMatch{IdMatch::Unknown};
    IdMatch profileMatch{IdMatch::Unknown};

    // LED strobe (S[0..2], S[12] bit 4, S[13]); 100 MHz strobe clock.
    bool ledOn{false};
    double ledDelayUs{0.0};
    double ledWidthUs{0.0};
    std::string ledPreset; // "run", "align", "custom" or "off"
    bool guardFault{false};
    uint32_t guardTrips{0};

    // Sensor link (P[12] errors, P[14] resyncs, P[7] bad frames, P[6] dropped).
    bool ratesValid{false}; // false until two samples
    double ingressErrorsPerS{0.0};
    // Ingress errors averaged over the last kSustainedUs, and the warning from it: valid only once
    // a full window has been seen since the last mode-switch settle.
    double ingressErrorsAvgPerS{0.0};
    bool ingressErrorsWarn{false};
    double resyncsPerS{0.0};
    double badFramesPerS{0.0};
    double droppedPerS{0.0};
    // Sustained loss, from the rates relative to the sensor's frame rate (never while the sensor is
    // closed or inside the mode-switch settle window).
    bool badFramesWarn{false};
    bool droppedWarn{false};
    // The results bridge is ARMED or RUNNING, i.e. something consumes the frames. P[6] counts every
    // frame as dropped while nothing does (Run without an experiment, a stopped Align), so dropped
    // frames are judged only while this is true.
    bool bridgeActive{false};
    // The PL receiver self-heal block (results9; RXH1 at P[256]): absent on results8.
    bool rxHealPresent{false};
    bool rxHealGaveUp{false};
    uint32_t rxHealTries{0};
    uint32_t rxHealAutoResets{0}; // since the PL reset (receiver resets)
    // results12 (frames start only after a FrameStart line): FrameStarts seen, frames dropped because no
    // FS was seen (a mid-frame join or a lost FS line: lost data in a run), and the lines dropped with them.
    // fsPresent: the build counts them (fs_seen is non-zero once a frame has come); all 0 otherwise.
    bool rxFsPresent{false};
    uint32_t rxFsSeen{0}, rxNoFsFrames{0}, rxNoFsLines{0};
    bool rxHealV2{false};         // heal v2 (CTRL2 at word 24 non-zero): the counters below exist
    uint32_t rxHealFlagClears{0}, rxHealEpisodes{0}, rxHealFailedEpisodes{0};
    double badFramesWarnPerS{0.0};
    double droppedWarnPerS{0.0};

    // Sensor: S[9] reads the XVS period in 100 MHz host clocks (0 while the sensor is closed);
    // S[29] reads the ingress geometry {lines, OB lines, slots of 8 pixels} written at the last
    // receiver reset (0x00602040 = the 512x96 default, 0x02702066 = 816x624 full field).
    uint32_t xvsPeriodClocks{0};
    double xvsFps{0.0};
    uint32_t geometryWidth{0};
    uint32_t geometryHeight{0};

    // Latency monitor (S[47..51], 175 MHz clocks), SOF -> last result.
    double latencyLastUs{0.0};
    double latencyMaxUs{0.0};
    uint32_t latencyOverBudget{0};
    uint32_t latencyFrames{0};
};

class PzPlatformMonitor {
public:
    // `registers` may be null: every sample is then !available with `error`.
    PzPlatformMonitor(std::unique_ptr<IPzPlatformRegisters> registers, std::string unavailableReason,
                      std::string expectedCorePath = kExpectedCorePath);

    // One sample at host time `nowUs` (monotonic). Thread-safe.
    PzPlatformStatus sample(uint64_t nowUs);

    // A camera mode switch resets the receiver and the sensor: the link counters jump for about a
    // second (docs/YOFO_HOST_INTERFACE.md). Rates are reported invalid (`ratesValid` false) until
    // `kModeSettleUs` after `nowUs`, and the first valid rate starts after that window.
    void settle(uint64_t nowUs);
    // The PL's auto-reset count (RXH1 P[259]); nullopt on an image without the block or a blank PL.
    std::optional<uint32_t> rxHealAutoResets();
    // Frames dropped because no FrameStart was seen (RXH1 word 33), tear-safe; nullopt on a build
    // without the counters (fs_seen, word 32, still zero), without the block, or with a blank PL.
    std::optional<uint32_t> rxNoFsFrames();
    // The counters are zeroed by a receiver reset (a heal or P[8] bit 6) as well as by a clear, so
    // "end minus start" under-counts when a reset lands mid-run. Over a run the monitor accumulates the
    // deltas instead: every status sample and the end read add (value - last) or, when the value fell,
    // the new value. begin returns false (and nothing is tracked) on a build without the counters;
    // end returns the frames dropped for want of a FrameStart during the run.
    bool beginNoFsRun();
    std::optional<uint64_t> endNoFsRun();
    static constexpr uint64_t kModeSettleUs = 1'500'000;

private:
    struct Counters {
        uint32_t ingressErrors{0}, resyncs{0}, badFrames{0}, dropped{0};
    };
    std::unique_ptr<IPzPlatformRegisters> registers_;
    std::string unavailableReason_;
    std::string expectedCorePath_;
    std::mutex mutex_;
    bool havePrevious_{false};
    uint64_t settleUntilUs_{0};
    uint64_t badSinceUs_{0}, droppedSinceUs_{0}; // 0 = not above its threshold
    std::deque<std::pair<uint64_t, uint32_t>> errorHistory_; // (host time, P[12]) over the window
    struct DeltaSum {
        bool have{false};
        uint32_t last{0};
        uint64_t total{0};
        void add(uint32_t value) {
            if (have) total += value >= last ? value - last : value; // fell: a reset zeroed it
            have = true;
            last = value;
        }
    };
    DeltaSum noFsRun_;
    bool noFsRunActive_{false};
    uint64_t previousUs_{0};
    Counters previous_{};
};

} // namespace backend::pz
