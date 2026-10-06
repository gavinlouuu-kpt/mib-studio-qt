#pragma once

// The one writer of the PZ7035 live registers the instrument UI drives (#501 P1): the LED strobe,
// the U-Net cell path and the one-frame cell capture (pz7035-imx426 docs/YOFO_HOST_INTERFACE.md,
// docs/LED_STROBE.md; reference implementation tools/pzcell/pzcell.c). Sensor timing, the ROI and
// the receiver reset are applied by the GenTL producer at AcquisitionStart; AppBackend sequences
// the two (setInstrumentMode). The bridge belongs to the execution provider; PzPlatformMonitor
// only reads.
//
// Every operation checks that the PL is configured (DEVCFG PCFG_DONE) and that the image is the
// U-Net cell image (S[41] = 'CEL2') before touching a PL register, and all of them are serialised
// by one mutex: P[8] is read-modify-write (bit 4 is the U-Net enable level).
// Qt-free.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace backend::pz {

class IPzControlRegisters {
public:
    virtual ~IPzControlRegisters() = default;
    // Live page P[i] = 0x40100000 + 4*i; the strobe/cell window S[i] is P[128 + i].
    virtual uint32_t live(unsigned index) = 0;
    virtual void setLive(unsigned index, uint32_t value) = 0;
    // Live-view / cell-capture window at 0x400E0000, 32-bit words.
    virtual uint32_t window(unsigned word) = 0;
    virtual bool plConfigured(std::string* why) {
        (void)why;
        return true;
    }
    // Polling delays; fakes return at once.
    virtual void sleepUs(unsigned us);
};

#if defined(__linux__)
// Read-write /dev/mem mapping of the live page and the capture window. Mapping reads no PL register.
std::unique_ptr<IPzControlRegisters> openDevMemControlRegisters(std::string* error);
#endif

enum class InstrumentMode { Unknown, Align, Run };
const char* instrumentModeName(InstrumentMode mode);

struct LedSetting {
    double delayUs{0.0};
    double widthUs{0.0};
};
// Presets (docs/YOFO_HOST_INTERFACE.md "LED presets"), each straddling the exposure start: Run at
// 5 kHz; Align at 400 fps full field (HMAX 232, results8 on); the banded fallback for older images
// at 500 fps (HMAX 116).
inline constexpr LedSetting kRunLed{7.0, 60.0};
inline constexpr LedSetting kAlignLed{100.0, 135.0};
inline constexpr LedSetting kAlignBandsLed{0.0, 125.0};

// Service-mode limits per mode (the bench viewer's LED_LIMITS): at 5 kHz the width stays <= 80 µs
// (40 % duty; the PL clamps at 50 %); in Align the driver needs > ~75 µs to light at all.
struct LedLimits {
    double delayMinUs, delayMaxUs, widthMinUs, widthMaxUs;
};
LedLimits ledLimits(InstrumentMode mode);
// Empty when `s` is inside the limits of `mode`, else why not.
std::string checkLed(InstrumentMode mode, LedSetting s);

// One listed cell of the capture: payload words 0-14 (unet_cells_v2), bbox and listing.
struct PzCaptureCell {
    uint32_t words[18]{};
    uint16_t x() const { return static_cast<uint16_t>(words[15]); }
    uint16_t y() const { return static_cast<uint16_t>(words[15] >> 16); }
    uint16_t width() const { return static_cast<uint16_t>(words[16]); }
    uint16_t height() const { return static_cast<uint16_t>(words[16] >> 16); }
    bool valid() const { return (words[17] & 0xFFFFu) != 0; }
};

inline constexpr unsigned kCaptureWidth = 512, kCaptureHeight = 96;
inline constexpr unsigned kMaxCaptureCells = 16;

// One frame of the cell path as fed to the U-Net: gray, the U-Net mask (1 bit/px, LSB first,
// row-major) and the listed cells, all of the same frame.
struct PzCellCapture {
    uint32_t frameId{0};
    unsigned listed{0};
    unsigned flags{0};
    unsigned cells{0};
    unsigned blemishes{0};
    unsigned dropped{0};
    std::vector<uint8_t> gray;  // 512*96
    std::vector<uint8_t> mask;  // 512*96/8
    std::vector<PzCaptureCell> list;
};

// Run preview packet for the UI (fetch_run_preview): 32-byte little-endian header
//   "MIBC" | u16 version 1 | u16 header 32 | u32 frame id | u16 width | u16 height
//   | u16 listed | u16 flags | u16 cells | u16 blemishes | u32 dropped | u32 0
// then gray (width*height), mask (width*height/8), listed x 18 u32 cell words.
std::vector<uint8_t> encodeRunPreview(const PzCellCapture& capture);

class PzInstrumentControl {
public:
    explicit PzInstrumentControl(std::unique_ptr<IPzControlRegisters> registers);

    bool ledOff(std::string* error);
    // S[0] = 0; S[1], S[2] in strobe-clock cycles; S[4] = S[5] = 0; S[0] = 1 (XVS sync).
    bool setLed(LedSetting setting, std::string* error);
    // S[46] and P[8] bit 4 (the U-Net enable level), together.
    bool setCellPath(bool on, std::string* error);
    bool cellPathOn(std::string* error);
    // Latency monitor clear (any write to S[47]) at the start of a run.
    bool clearLatency(std::string* error);
    // Arm S[36], wait for S[42] bit 0, read the frame from the window. Cell path must be on.
    bool captureCell(PzCellCapture& out, std::chrono::milliseconds timeout, std::string* error);

private:
    bool readyLocked(std::string* error);
    uint32_t S(unsigned i) { return registers_->live(128 + i); }
    void setS(unsigned i, uint32_t v) { registers_->setLive(128 + i, v); }

    std::mutex mutex_;
    std::unique_ptr<IPzControlRegisters> registers_;
};

} // namespace backend::pz
