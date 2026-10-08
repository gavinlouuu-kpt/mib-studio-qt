#include "backend/pz/PzInstrumentControl.h"

#include "backend/pz/PzPlatformMonitor.h"

#include <cmath>
#include <cstring>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace backend::pz {

namespace {

// docs/YOFO_HOST_INTERFACE.md, docs/LED_STROBE.md, tools/pzcell/pzcell.c.
constexpr unsigned kCommand = 8;              // P[8]
constexpr uint32_t kUnetEnable = 0x10u;       // P[8] bit 4, a level
constexpr uint32_t kReceiverReset = 0x40u;    // P[8] bit 6, a pulse
constexpr unsigned kIngressErrors = 12, kIngressStatus = 13, kIngressResyncs = 14; // P[12..14]
constexpr unsigned kStrobeControl = 0, kStrobeDelay = 1, kStrobeWidth = 2, kStrobeEvery = 4, kStrobeGate = 5;
constexpr unsigned kStrobeClockKhz = 10;
constexpr unsigned kCaptureArm = 36, kCellImage = 41, kCaptureDone = 42, kCaptureTag = 43, kCaptureListing = 44,
                   kCaptureCounts = 45, kCellMode = 46, kLatencyClear = 47;
constexpr uint32_t kCellImageMagic = 0x43454C32u; // 'CEL2'
constexpr unsigned kGrayWords = kCaptureWidth * kCaptureHeight / 4;     // 12288
constexpr unsigned kMaskWords = kCaptureWidth * kCaptureHeight / 32;    // 1536
constexpr unsigned kCellsWord = kGrayWords + kMaskWords;                // 13824
constexpr unsigned kCellStride = 32, kCellWords = 18;

void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    put16(b, at, v & 0xFFFFu);
    put16(b, at + 2, v >> 16);
}

#if defined(__linux__)
class DevMemControlRegisters final : public IPzControlRegisters {
public:
    ~DevMemControlRegisters() override {
        if (live_) munmap(const_cast<uint32_t*>(live_), 0x1000);
        if (window_) munmap(const_cast<uint32_t*>(window_), 0x10000);
        if (fd_ >= 0) close(fd_);
    }
    bool open(std::string* error) {
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) return fail(error, std::string("/dev/mem: ") + std::strerror(errno));
        live_ = map(0x40100000u, 0x1000);
        window_ = map(0x400E0000u, 0x10000);
        if (!live_ || !window_) return fail(error, "mmap PZ7035 live page / capture window");
        return true; // no PL register is touched here: the PL may be blank
    }
    uint32_t live(unsigned i) override { return i < 1024 ? live_[i] : 0; }
    void setLive(unsigned i, uint32_t v) override {
        if (i < 1024) live_[i] = v;
    }
    uint32_t window(unsigned w) override { return w < 0x4000 ? window_[w] : 0; }
    bool plConfigured(std::string* why) override { return pzPlConfigured(why); }

private:
    volatile uint32_t* map(off_t base, size_t bytes) {
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, base);
        return p == MAP_FAILED ? nullptr : static_cast<volatile uint32_t*>(p);
    }
    static bool fail(std::string* error, std::string msg) {
        if (error) *error = std::move(msg);
        return false;
    }
    int fd_{-1};
    volatile uint32_t* live_{nullptr};
    volatile uint32_t* window_{nullptr};
};
#endif

} // namespace

void IPzControlRegisters::sleepUs(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

#if defined(__linux__)
std::unique_ptr<IPzControlRegisters> openDevMemControlRegisters(std::string* error) {
    auto regs = std::make_unique<DevMemControlRegisters>();
    if (!regs->open(error)) return nullptr;
    return regs;
}
#endif

const char* instrumentModeName(InstrumentMode mode) {
    switch (mode) {
    case InstrumentMode::Align: return "align";
    case InstrumentMode::Run: return "run";
    default: return "unknown";
    }
}

LedLimits ledLimits(InstrumentMode mode) {
    if (mode == InstrumentMode::Align) return {0.0, 400.0, 60.0, 150.0};
    return {0.0, 100.0, 20.0, 80.0};
}

std::string checkLed(InstrumentMode mode, LedSetting s) {
    if (mode == InstrumentMode::Unknown) return "the LED is set per camera mode: switch to Align or Run first";
    const auto l = ledLimits(mode);
    if (!std::isfinite(s.delayUs) || s.delayUs < l.delayMinUs || s.delayUs > l.delayMaxUs) {
        return "delay outside " + std::to_string(static_cast<int>(l.delayMinUs)) + "-" +
               std::to_string(static_cast<int>(l.delayMaxUs)) + " µs for " + instrumentModeName(mode);
    }
    if (!std::isfinite(s.widthUs) || s.widthUs < l.widthMinUs || s.widthUs > l.widthMaxUs) {
        return "width outside " + std::to_string(static_cast<int>(l.widthMinUs)) + "-" +
               std::to_string(static_cast<int>(l.widthMaxUs)) + " µs for " + instrumentModeName(mode);
    }
    return {};
}

std::vector<uint8_t> encodeRunPreview(const PzCellCapture& c) {
    const size_t gray = static_cast<size_t>(kCaptureWidth) * kCaptureHeight, mask = gray / 8;
    std::vector<uint8_t> b(32 + gray + mask + c.list.size() * kCellWords * 4, 0);
    std::memcpy(b.data(), "MIBC", 4);
    put16(b, 4, 1);
    put16(b, 6, 32);
    put32(b, 8, c.frameId);
    put16(b, 12, kCaptureWidth);
    put16(b, 14, kCaptureHeight);
    put16(b, 16, static_cast<uint32_t>(c.list.size()));
    put16(b, 18, c.flags);
    put16(b, 20, c.cells);
    put16(b, 22, c.blemishes);
    put32(b, 24, c.dropped);
    if (c.gray.size() == gray) std::memcpy(b.data() + 32, c.gray.data(), gray);
    if (c.mask.size() == mask) std::memcpy(b.data() + 32 + gray, c.mask.data(), mask);
    size_t at = 32 + gray + mask;
    for (const auto& cell : c.list) {
        for (unsigned k = 0; k < kCellWords; ++k, at += 4) put32(b, at, cell.words[k]);
    }
    return b;
}

PzInstrumentControl::PzInstrumentControl(std::unique_ptr<IPzControlRegisters> registers)
    : registers_(std::move(registers)) {}

bool PzInstrumentControl::readyLocked(std::string* error) {
    if (!registers_) {
        if (error) *error = "no PZ7035 registers (MIB_EXECUTION_PROVIDER=pz on the instrument)";
        return false;
    }
    if (!registers_->plConfigured(error)) return false; // a blank PL can stall the bus
    if (S(kCellImage) != kCellImageMagic) {
        if (error) *error = "the loaded PL image is not the U-Net cell image (S[41] != 'CEL2')";
        return false;
    }
    return true;
}

bool PzInstrumentControl::ledOff(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    setS(kStrobeControl, 0);
    return true;
}

bool PzInstrumentControl::setLed(LedSetting s, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    const uint32_t khz = S(kStrobeClockKhz);
    if (khz == 0) {
        if (error) *error = "the strobe block reports no clock (S[10] = 0)";
        return false;
    }
    const auto cycles = [&](double us) { return static_cast<uint32_t>(std::lround(us * khz / 1000.0)); };
    // Off first so a half-written setting never fires; writing S[0] also clears a guard fault.
    setS(kStrobeControl, 0);
    setS(kStrobeDelay, cycles(s.delayUs));
    setS(kStrobeWidth, cycles(s.widthUs));
    setS(kStrobeEvery, 0);
    setS(kStrobeGate, 0);
    setS(kStrobeControl, 1); // XVS sync, pin 7 = XVS
    return true;
}

bool PzInstrumentControl::setCellPath(bool on, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    setS(kCellMode, on ? 1u : 0u);
    const uint32_t command = registers_->live(kCommand);
    registers_->setLive(kCommand, on ? (command | kUnetEnable) : (command & ~kUnetEnable));
    return true;
}

bool PzInstrumentControl::cellPathOn(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    return S(kCellMode) != 0 && (registers_->live(kCommand) & kUnetEnable) != 0;
}

bool PzInstrumentControl::ingressStatus(IngressStatus& out, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    out.status = registers_->live(kIngressStatus);
    out.errors = registers_->live(kIngressErrors);
    out.resyncs = registers_->live(kIngressResyncs);
    return true;
}

bool PzInstrumentControl::rxHealStatus(RxHealStatus& out, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    out = RxHealStatus{};
    if (registers_->live(kRxHealWindow) != kRxHealId) return true;
    out.present = true;
    out.control = registers_->live(kRxHealWindow + 1);
    out.status = registers_->live(kRxHealWindow + 2);
    // The counter can tear (no CDC in results9): read until two consecutive reads match.
    uint32_t count = registers_->live(kRxHealWindow + 3);
    for (int i = 0; i < 8; ++i) {
        const uint32_t again = registers_->live(kRxHealWindow + 3);
        if (again == count) break;
        count = again;
    }
    out.autoResets = count;
    out.lastPulse = registers_->live(kRxHealWindow + 4);
    return true;
}

bool PzInstrumentControl::resetReceiver(std::chrono::microseconds hold, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    const uint32_t keep = registers_->live(kCommand) & kUnetEnable;
    registers_->setLive(kCommand, keep | kReceiverReset);
    registers_->sleepUs(static_cast<unsigned>(hold.count()));
    registers_->setLive(kCommand, keep);
    return true;
}

bool PzInstrumentControl::clearLatency(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    setS(kLatencyClear, 0);
    return true;
}

bool PzInstrumentControl::captureCell(PzCellCapture& out, std::chrono::milliseconds timeout, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readyLocked(error)) return false;
    if (S(kCellMode) == 0 || (registers_->live(kCommand) & kUnetEnable) == 0) {
        if (error) *error = "the cell path is off (switch to Run)";
        return false;
    }
    setS(kCaptureArm, 0); // any write arms the next frame
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ((S(kCaptureDone) & 1u) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            if (error) *error = "cell capture timed out (is the sensor streaming?)";
            return false;
        }
        registers_->sleepUs(200); // one 5 kHz frame
    }
    out.frameId = S(kCaptureTag);
    const uint32_t listing = S(kCaptureListing), counts = S(kCaptureCounts);
    out.listed = listing & 0xFFu;
    out.flags = (listing >> 8) & 0xFFu;
    out.dropped = listing >> 16;
    out.cells = counts & 0xFFFFu;
    out.blemishes = counts >> 16;
    if (out.listed > kMaxCaptureCells) {
        if (error) *error = "cell capture lists " + std::to_string(out.listed) + " cells (max 16)";
        return false;
    }
    out.gray.resize(kGrayWords * 4);
    out.mask.resize(kMaskWords * 4);
    for (unsigned i = 0; i < kGrayWords; ++i) {
        const uint32_t w = registers_->window(i);
        std::memcpy(out.gray.data() + 4 * i, &w, 4); // little-endian PS: pixel 0 in bits 7:0
    }
    for (unsigned i = 0; i < kMaskWords; ++i) {
        const uint32_t w = registers_->window(kGrayWords + i);
        std::memcpy(out.mask.data() + 4 * i, &w, 4);
    }
    out.list.assign(out.listed, PzCaptureCell{});
    for (unsigned c = 0; c < out.listed; ++c) {
        for (unsigned k = 0; k < kCellWords; ++k) out.list[c].words[k] = registers_->window(kCellsWord + kCellStride * c + k);
    }
    return true;
}

} // namespace backend::pz
