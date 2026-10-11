#pragma once

// Host rehearsal of the PZ7035 frame ring and the SSD run (#693). NOT a product feature: selected only by
// MIB_EXECUTION_PROVIDER=rehearsal:<fps>[:<ring frames>] on a development host (the board's unit sets `pz`), it lets a
// Studio on the host run the whole slot chain (Run, an SSD run, Stop, ring playback reads, export, ring_vs_ssd)
// against a fake `pzrec` (tools/ring_vs_ssd/rehearsal/fake_pzrec.py) with no board.
//
// The frames are a pure function of (epoch, frame id) and the ids a pure function of the monotonic clock (CLOCK_MONOTONIC, shared by the two processes) since the ARM:
// head = floor((min(now, stop) - t0) * fps). The provider has no producer thread; the fake pzrec reads the same
// state file (MIB_REHEARSAL_DIR/emu.json) and derives the same ids, epoch, ticks and pixels, so ring and SSD agree
// the way the real PL's two taps do. The content formulas are mirrored in fake_pzrec.py; keep them in step.

#include "backend/processing/IExecutionProvider.h"
#include "backend/pz/PzInstrumentControl.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace backend::processing::pz {

class RehearsalProvider final : public IExecutionProvider {
public:
    // `fps`: the sensor rate; `ringFrames`: the ring size; `dir`: where emu.json is kept (must exist).
    RehearsalProvider(double fps, uint32_t ringFrames, std::string dir);
    ~RehearsalProvider() override;
    std::string name() const override { return "rehearsal"; }
    void setSink(Sink) override {}
    bool configure(const CompiledProfile&, std::string*) override { return true; }
    bool start(uint64_t runId, std::string* error) override;
    void stop() override;
    ProviderStatus status() const override;
    uint32_t ringFramesWanted() const override { return ringFrames_; }
    backend::pz::RingStatus ringStatus() override;
    backend::pz::RingRead ringRead(uint64_t seq, backend::pz::RingFrame& out, std::string* why) override;
    uint32_t ringTickHz() override { return kTickHz; }

    static constexpr uint32_t kTickHz = 100000000u;
    static constexpr uint64_t kTickBase = 1000000u;
    // The content of frame `frameId` (shared with the fake pzrec).
    static void fillFrame(uint32_t epoch, uint64_t frameId, double fps, backend::pz::RingFrame& out);

private:
    void writeState(bool running, int64_t stopNs) const;
    int64_t headLocked(int64_t nowNs) const; // newest frame id started, 0: none

    const double fps_;
    const uint32_t ringFrames_;
    const std::string dir_;
    mutable std::mutex mutex_;
    bool running_{false}, armed_{false};
    uint32_t epoch_{2};
    int64_t t0Ns_{0}, stopNs_{0};
};

// The register space of the cell image with nothing behind it: the LED and mode writes land in a map. Used with the rehearsal provider.
std::unique_ptr<backend::pz::IPzControlRegisters> makeRehearsalRegisters();

} // namespace backend::processing::pz
