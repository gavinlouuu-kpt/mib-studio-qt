#include "backend/pz/PzPlatformMonitor.h"

#include "backend/pz/AlignLock.h"

#include "pz_mib_abi.h" // vendored bundle (register offsets)

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef MIB_PINNED_UNET_PROFILE_ID
#define MIB_PINNED_UNET_PROFILE_ID ""
#endif

namespace backend::pz {

namespace {

// docs/YOFO_HOST_INTERFACE.md (pz7035-imx426 PR #10, 5b30761).
constexpr uint64_t kLivePageBase = 0x40100000u;   // P[i]; the strobe window S[i] is at +0x200
constexpr uint64_t kBridgePageBase = 0x40101000u; // PZ_MIB_REG_*
constexpr unsigned kStrobeWindowWord = 0x200 / 4;
constexpr uint64_t kDevcfgPage = 0xF8007000u; // Zynq-7000 DEVCFG (PS)
constexpr unsigned kDevcfgIntStsWord = 0x00C / 4;
constexpr uint32_t kPcfgDone = 1u << 2;

constexpr unsigned kLiveDropped = 6, kLiveBadFrames = 7, kLiveIngressErrors = 12, kLiveResyncs = 14;
constexpr unsigned kStrobeControl = 0, kStrobeDelay = 1, kStrobeWidth = 2, kStrobeStatus = 12, kStrobeGuard = 13;
constexpr unsigned kXvsPeriod = 9, kIngressGeometry = 29;
constexpr double kHostClockHz = 100e6; // the XVS period counts 100 MHz host clocks
constexpr unsigned kLatencyLast = 47, kLatencyMax = 49, kLatencyOverBudget = 50, kLatencyFrames = 51;
constexpr double kStrobeClockMHz = 100.0;  // S[10] kHz = 100,000 on these images
constexpr double kLatencyClockMHz = 175.0;

// LED presets (cycles of the 100 MHz strobe clock).
constexpr uint32_t kRunDelay = 700, kRunWidth = 6000;   // 7 / 60 µs
constexpr uint32_t kAlignDelay = 10000, kAlignWidth = 13500;    // 100 / 135 µs (whole frames, results8 on)
constexpr uint32_t kAlignBandsDelay = 0, kAlignBandsWidth = 12500; // 0 / 125 µs (banded fallback)

std::string coreId(uint32_t id3, uint32_t id2, uint32_t id1, uint32_t id0) {
    if ((id0 | id1 | id2 | id3) == 0) return {};
    char hex[33];
    std::snprintf(hex, sizeof(hex), "%08x%08x%08x%08x", id3, id2, id1, id0);
    return hex;
}

IdMatch compare(const std::string& actual, const std::string& expected) {
    if (expected.empty()) return IdMatch::Unknown;
    return actual == expected ? IdMatch::Match : IdMatch::Mismatch;
}

// Counter delta per second; a decrease means the PL was reloaded or reset.
double rate(uint32_t now, uint32_t before, double seconds) {
    if (now < before || seconds <= 0.0) return 0.0;
    return static_cast<double>(now - before) / seconds;
}

#if defined(__linux__)
class DevMemPlatformRegisters final : public IPzPlatformRegisters {
public:
    ~DevMemPlatformRegisters() override {
        if (livePage_) munmap(const_cast<uint32_t*>(livePage_), 0x1000);
        if (bridgePage_) munmap(const_cast<uint32_t*>(bridgePage_), 0x1000);
        if (devcfgPage_) munmap(const_cast<uint32_t*>(devcfgPage_), 0x1000);
        if (fd_ >= 0) close(fd_);
    }
    bool open(std::string* error) {
        fd_ = ::open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd_ < 0) return fail(error, std::string("/dev/mem: ") + std::strerror(errno));
        livePage_ = map(kLivePageBase);
        bridgePage_ = map(kBridgePageBase);
        devcfgPage_ = map(kDevcfgPage);
        if (!livePage_ || !bridgePage_ || !devcfgPage_) return fail(error, "mmap PZ7035 register pages");
        return true; // no PL register is read here: the PL may be blank
    }
    bool plConfigured(std::string* why) override {
        if ((devcfgPage_[kDevcfgIntStsWord] & kPcfgDone) != 0) return true;
        if (why) *why = "PL not configured (DEVCFG PCFG_DONE = 0): load the PL image";
        return false;
    }
    uint32_t live(unsigned index) override { return livePage_[index]; }
    uint32_t strobe(unsigned index) override { return livePage_[kStrobeWindowWord + index]; }
    uint32_t bridge(uint32_t offset) override { return bridgePage_[offset / 4]; }

private:
    volatile uint32_t* map(uint64_t base) {
        void* p = mmap(nullptr, 0x1000, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(base));
        return p == MAP_FAILED ? nullptr : static_cast<volatile uint32_t*>(p);
    }
    static bool fail(std::string* error, std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    }
    int fd_{-1};
    volatile uint32_t* livePage_{nullptr};
    volatile uint32_t* bridgePage_{nullptr};
    volatile uint32_t* devcfgPage_{nullptr};
};
#endif

} // namespace

#if defined(__linux__)
bool pzPlConfigured(std::string* error, uint32_t* intSts) {
    const int fd = ::open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        if (error) *error = std::string("/dev/mem: ") + std::strerror(errno);
        return false;
    }
    void* p = mmap(nullptr, 0x1000, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(kDevcfgPage));
    if (p == MAP_FAILED) {
        close(fd);
        if (error) *error = "mmap DEVCFG";
        return false;
    }
    const uint32_t sts = static_cast<volatile uint32_t*>(p)[kDevcfgIntStsWord];
    munmap(p, 0x1000);
    close(fd);
    if (intSts) *intSts = sts;
    if ((sts & kPcfgDone) == 0) {
        if (error) *error = "PL not configured (DEVCFG PCFG_DONE = 0): load the PL image";
        return false;
    }
    return true;
}

std::unique_ptr<IPzPlatformRegisters> openDevMemPlatformRegisters(std::string* error) {
    auto regs = std::make_unique<DevMemPlatformRegisters>();
    if (!regs->open(error)) return nullptr;
    return regs;
}
#endif

bool ExpectedCore::has(const std::string& feature) const {
    for (const auto& f : features)
        if (f == feature) return true;
    return false;
}

std::optional<ExpectedCore> parseExpectedCore(const std::string& text, std::string* error) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "expected-core.json is not a JSON object";
        return std::nullopt;
    }
    ExpectedCore c;
    c.buildId = j.value("build_id", std::string());
    c.profileId = j.value("profile_id", std::string());
    c.commit = j.value("commit", std::string());
    c.image = j.value("image", std::string());
    if (const auto abi = j.find("abi"); abi != j.end() && abi->is_object()) {
        c.abiMajor = abi->value("major", 0);
        c.abiMinor = abi->value("minor", 0);
    }
    c.scienceProfile = j.value("science_profile", 0);
    c.profileVersion = j.value("profile_version", 0);
    if (const auto f = j.find("features"); f != j.end() && f->is_array()) {
        for (const auto& v : *f)
            if (v.is_string()) c.features.push_back(v.get<std::string>());
    }
    if (c.buildId.size() != 32) {
        if (error) *error = "expected-core.json has no 32-hex build_id";
        return std::nullopt;
    }
    return c;
}

std::optional<ExpectedCore> loadExpectedCore(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "no " + path;
        return std::nullopt;
    }
    return parseExpectedCore(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
                             error);
}

const char* pinnedUnetProfileId() { return MIB_PINNED_UNET_PROFILE_ID; }

const char* idMatchName(IdMatch m) {
    switch (m) {
    case IdMatch::Match: return "match";
    case IdMatch::Mismatch: return "mismatch";
    case IdMatch::Unknown: break;
    }
    return "unknown";
}

PzPlatformMonitor::PzPlatformMonitor(std::unique_ptr<IPzPlatformRegisters> registers, std::string unavailableReason,
                                     std::string expectedCorePath)
    : registers_(std::move(registers)), unavailableReason_(std::move(unavailableReason)),
      expectedCorePath_(std::move(expectedCorePath)) {}

PzPlatformStatus PzPlatformMonitor::sample(uint64_t nowUs) {
    std::scoped_lock lk(mutex_);
    PzPlatformStatus s;
    s.pinnedProfileId = pinnedUnetProfileId();
    if (!registers_) {
        s.error = unavailableReason_.empty() ? "no PZ7035 platform" : unavailableReason_;
        return s;
    }
    auto& r = *registers_;
    std::string why;
    if (!r.plConfigured(&why)) {
        s.error = why; // nothing in the PL window is read while it is blank
        havePrevious_ = false;
        return s;
    }
    if (r.bridge(PZ_MIB_REG_IDENTITY) != PZ_MIB_IDENTITY_MAGIC) {
        s.error = "no PZ-MIB platform bridge loaded (identity)";
        havePrevious_ = false;
        return s;
    }
    s.available = true;

    s.buildId = coreId(r.bridge(PZ_MIB_REG_BUILD_ID3), r.bridge(PZ_MIB_REG_BUILD_ID2), r.bridge(PZ_MIB_REG_BUILD_ID1),
                       r.bridge(PZ_MIB_REG_BUILD_ID0));
    s.profileId = coreId(r.bridge(PZ_MIB_REG_PROFILE_ID3), r.bridge(PZ_MIB_REG_PROFILE_ID2),
                         r.bridge(PZ_MIB_REG_PROFILE_ID1), r.bridge(PZ_MIB_REG_PROFILE_ID0));
    s.abiVersion = r.bridge(PZ_MIB_REG_ABI_VERSION);
    const uint32_t science = r.bridge(PZ_MIB_REG_SCIENCE_PROFILE);
    s.scienceProfile = static_cast<uint16_t>(science & 0xFFFFu);
    s.profileVersion = static_cast<uint16_t>(science >> 16);

    std::string expectedError;
    if (auto expected = loadExpectedCore(expectedCorePath_, &expectedError)) {
        s.expectedPresent = true;
        s.expected = *expected;
    }
    s.buildMatch = s.expectedPresent ? compare(s.buildId, s.expected.buildId) : IdMatch::Unknown;
    s.profileMatch = compare(s.profileId, s.pinnedProfileId);

    const uint32_t control = r.strobe(kStrobeControl);
    const uint32_t delay = r.strobe(kStrobeDelay);
    const uint32_t width = r.strobe(kStrobeWidth);
    s.ledOn = (control & 1u) != 0;
    s.ledDelayUs = delay / kStrobeClockMHz;
    s.ledWidthUs = width / kStrobeClockMHz;
    if (!s.ledOn) s.ledPreset = "off";
    else if (delay == kRunDelay && width == kRunWidth) s.ledPreset = "run";
    else if ((delay == kAlignDelay && width == kAlignWidth) ||
             (delay == kAlignBandsDelay && width == kAlignBandsWidth))
        s.ledPreset = "align";
    else s.ledPreset = "custom";
    const uint32_t guard = r.strobe(kStrobeGuard);
    s.guardFault = (r.strobe(kStrobeStatus) & (1u << 4)) != 0 || (guard & 0x80000000u) != 0;
    s.guardTrips = guard & 0x7FFFFFFFu;

    s.xvsPeriodClocks = r.strobe(kXvsPeriod);
    s.xvsFps = s.xvsPeriodClocks ? kHostClockHz / s.xvsPeriodClocks : 0.0;
    const uint32_t geometry = r.strobe(kIngressGeometry);
    s.geometryHeight = geometry >> 16;
    s.geometryWidth = (geometry & 0xFFu) * 8u;

    s.latencyLastUs = r.strobe(kLatencyLast) / kLatencyClockMHz;
    s.latencyMaxUs = r.strobe(kLatencyMax) / kLatencyClockMHz;
    s.latencyOverBudget = r.strobe(kLatencyOverBudget);
    s.latencyFrames = r.strobe(kLatencyFrames);

    const Counters now{r.live(kLiveIngressErrors), r.live(kLiveResyncs), r.live(kLiveBadFrames),
                       r.live(kLiveDropped)};
    if (nowUs < settleUntilUs_) {
        // Right after a mode switch: no rates, and the next window starts here.
        havePrevious_ = false;
    } else if (havePrevious_ && nowUs > previousUs_) {
        const double seconds = static_cast<double>(nowUs - previousUs_) / 1e6;
        s.ratesValid = true;
        s.ingressErrorsPerS = rate(now.ingressErrors, previous_.ingressErrors, seconds);
        s.resyncsPerS = rate(now.resyncs, previous_.resyncs, seconds);
        s.badFramesPerS = rate(now.badFrames, previous_.badFrames, seconds);
        s.droppedPerS = rate(now.dropped, previous_.dropped, seconds);
    }
    // Sustained loss against the sensor's frame rate: onset when the rate first exceeds the
    // threshold, a warning once it has stayed above it for kSustainedUs.
    s.badFramesWarnPerS = kBadFramesWarnFraction * s.xvsFps;
    s.droppedWarnPerS = kDroppedWarnFraction * s.xvsFps;
    const auto sustained = [&](bool above, uint64_t& since) {
        if (!s.ratesValid || s.xvsFps <= 0.0 || !above) {
            since = 0;
            return false;
        }
        if (since == 0) since = nowUs;
        return nowUs - since >= kSustainedUs;
    };
    const uint32_t bridgeState = r.bridge(PZ_MIB_REG_STATE);
    s.bridgeActive = bridgeState == PZ_MIB_STATE_ARMED || bridgeState == PZ_MIB_STATE_RUNNING;
    s.rxHealPresent = r.live(kRxHealWindow) == kRxHealId;
    if (s.rxHealPresent) {
        const uint32_t heal = r.live(kRxHealWindow + 2);
        s.rxHealTries = heal & 0xFFu;
        s.rxHealGaveUp = (heal & 0x100u) != 0;
        s.rxHealAutoResets = r.live(kRxHealWindow + 3);
    }
    s.badFramesWarn = sustained(s.badFramesPerS > s.badFramesWarnPerS, badSinceUs_);
    s.droppedWarn = sustained(s.bridgeActive && s.droppedPerS > s.droppedWarnPerS, droppedSinceUs_);
    havePrevious_ = true;
    previousUs_ = nowUs;
    previous_ = now;
    return s;
}

std::optional<uint32_t> PzPlatformMonitor::rxHealAutoResets() {
    std::scoped_lock lk(mutex_);
    if (!registers_ || !registers_->plConfigured(nullptr)) return std::nullopt;
    if (registers_->live(kRxHealWindow) != kRxHealId) return std::nullopt;
    return registers_->live(kRxHealWindow + 3);
}

void PzPlatformMonitor::settle(uint64_t nowUs) {
    std::scoped_lock lk(mutex_);
    settleUntilUs_ = nowUs + kModeSettleUs;
    havePrevious_ = false;
}

} // namespace backend::pz
