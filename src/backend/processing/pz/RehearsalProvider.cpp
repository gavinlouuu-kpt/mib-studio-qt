#include "backend/processing/pz/RehearsalProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace backend::processing::pz {

namespace {
namespace fs = std::filesystem;
using backend::pz::RingCell;
using backend::pz::RingFrame;
using backend::pz::RingRead;
using backend::pz::RingStatus;

constexpr unsigned kW = 512, kH = 96;
constexpr uint32_t kS0 = 128; // S[i] = P[128 + i]

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// tools/pzrec_to_h5/synthetic_records.py `cell(object_id, reason)`: the same numbers, so the stored record and the ring packet carry equal words.
RingCell makeCell(uint32_t objectId, uint32_t reason, uint16_t index) {
    const auto q = [](double x) { return static_cast<uint32_t>(std::lround(x * 65536.0)); };
    RingCell c;
    c.x = 161;
    c.y = 13;
    c.width = 46;
    c.height = 20;
    c.valid = reason == 0;
    c.index = index;
    c.payloadValidity = 0x7FFF;
    c.payload = {objectId | reason << 16, q(600.0), q(630.0), q(100.0), q(1.5), static_cast<uint32_t>(0.19 * 65536) | 1u << 24, q(120.0), q(185.1), q(24.6), q(50.0), q(24.6),
                 static_cast<uint32_t>(300.5 * 256), 0u, 385u | 18u << 16, static_cast<uint32_t>(25.25 * 256)};
    return c;
}

class RehearsalRegisters final : public backend::pz::IPzControlRegisters {
public:
    RehearsalRegisters() {
        live_[kS0 + 41] = 0x43454C32u; // 'CEL2'
        live_[kS0 + 10] = 100000;      // strobe clock, kHz
        live_[kS0 + 43] = 777;         // capture tag
        live_[kS0 + 44] = 2u | (0x5u << 8) | (3u << 16);
        live_[kS0 + 45] = 4u | (1u << 16);
    }
    uint32_t live(unsigned i) override {
        std::lock_guard<std::mutex> l(m_);
        auto it = live_.find(i);
        return it == live_.end() ? 0 : it->second;
    }
    void setLive(unsigned i, uint32_t v) override {
        std::lock_guard<std::mutex> l(m_);
        live_[i] = v;
        if (i == kS0 + 36) live_[kS0 + 42] = 1; // a cell capture finishes at once
    }
    uint32_t window(unsigned w) override {
        if (w < 12288) return 0x04030201u + w;
        if (w < 13824) return 0xFF00FF00u;
        const unsigned cell = (w - 13824) / 32, k = (w - 13824) % 32;
        if (k == 15) return (10u + cell) << 16 | (100u + 40 * cell);
        if (k == 16) return 20u << 16 | 30u;
        if (k == 17) return 2u << 24 | cell << 16 | 1u;
        return k;
    }
    void sleepUs(unsigned) override {}

private:
    std::mutex m_;
    std::map<unsigned, uint32_t> live_;
};
} // namespace

std::unique_ptr<backend::pz::IPzControlRegisters> makeRehearsalRegisters() { return std::make_unique<RehearsalRegisters>(); }

RehearsalProvider::RehearsalProvider(double fps, uint32_t ringFrames, std::string dir) : fps_(fps), ringFrames_(ringFrames), dir_(std::move(dir)) {}

RehearsalProvider::~RehearsalProvider() = default;

void RehearsalProvider::fillFrame(uint32_t epoch, uint64_t id, double fps, RingFrame& out) {
    (void)epoch; // the pixels do not depend on the epoch (a re-ARM restarts the ids at 1, like the PL)
    out = RingFrame{};
    out.frameId = id;
    out.timestampTicks = kTickBase + static_cast<uint64_t>(std::llround(static_cast<double>(id) * kTickHz / fps));
    out.frameFlags = 0;
    out.width = kW;
    out.height = kH;
    out.maskPresent = true;
    out.gray.resize(static_cast<size_t>(kW) * kH);
    out.mask.assign(static_cast<size_t>(kW) * kH / 8, 0);
    for (unsigned y = 0; y < kH; ++y) {
        for (unsigned x = 0; x < kW; ++x) {
            out.gray[y * kW + x] = static_cast<uint8_t>((x * 3 + y * 5 + id * 7) & 0xFF);
            if (((x ^ y ^ id) & 15) == 0) out.mask[(y * kW + x) / 8] |= static_cast<uint8_t>(1u << (x % 8));
        }
    }
    const uint64_t k = id % 3;
    if (k == 0) {
        out.cells.push_back(makeCell(1, 0, 0));
        out.cells.push_back(makeCell(2, 3, 1));
    } else if (k == 1) {
        out.cells.push_back(makeCell(1, 0, 0));
    }
}

void RehearsalProvider::writeState(bool running, int64_t stopNs) const {
    char buf[512];
    std::snprintf(buf, sizeof buf, "{\"epoch\":%u,\"fps\":%.6f,\"tick_hz\":%u,\"tick_base\":%llu,\"t0_ns\":%lld,\"stop_ns\":%lld,\"running\":%s,\"ring_frames\":%u}\n", epoch_, fps_, kTickHz,
                  static_cast<unsigned long long>(kTickBase), static_cast<long long>(t0Ns_), static_cast<long long>(stopNs), running ? "true" : "false", ringFrames_);
    const fs::path tmp = fs::path(dir_) / "emu.json.tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << buf;
    }
    std::error_code ec;
    fs::rename(tmp, fs::path(dir_) / "emu.json", ec);
}

int64_t RehearsalProvider::headLocked(int64_t now) const {
    if (!armed_) return 0;
    const int64_t end = running_ ? now : stopNs_;
    return std::max<int64_t>(0, static_cast<int64_t>(std::floor(static_cast<double>(end - t0Ns_) * 1e-9 * fps_)));
}

bool RehearsalProvider::start(uint64_t, std::string* error) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!fs::is_directory(dir_)) {
        if (error) *error = "MIB_REHEARSAL_DIR is not a directory: " + dir_;
        return false;
    }
    ++epoch_;
    armed_ = true;
    running_ = true;
    t0Ns_ = nowNs();
    stopNs_ = 0;
    writeState(true, 0);
    return true;
}

void RehearsalProvider::stop() {
    std::lock_guard<std::mutex> l(mutex_);
    if (!running_) return;
    running_ = false;
    stopNs_ = nowNs();
    writeState(false, stopNs_);
}

ProviderStatus RehearsalProvider::status() const {
    std::lock_guard<std::mutex> l(mutex_);
    ProviderStatus s;
    s.running = running_;
    s.frames = static_cast<uint64_t>(headLocked(nowNs()));
    s.profileCommitted = true;
    s.epoch = epoch_;
    return s;
}

RingStatus RehearsalProvider::ringStatus() {
    std::lock_guard<std::mutex> l(mutex_);
    RingStatus s;
    if (!armed_) {
        s.valid = false;
        s.why = "the rehearsal ring is not armed yet";
        return s;
    }
    const int64_t head = headLocked(nowNs());
    s.valid = true;
    s.state = running_ ? 3u : 0u;
    s.records = ringFrames_;
    s.recordBytes = backend::pz::kRingRecordBytes;
    s.epoch = epoch_;
    s.generation = 1;
    s.head = head - 1; // newest sequence started: sequence = frame id - 1
    s.final = static_cast<uint64_t>(head);
    s.lo = head > static_cast<int64_t>(ringFrames_) ? static_cast<uint64_t>(head) - ringFrames_ : 0;
    s.frozen = !running_;
    return s;
}

RingRead RehearsalProvider::ringRead(uint64_t seq, RingFrame& out, std::string* why) {
    uint32_t epoch;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!armed_) {
            if (why) *why = "not armed";
            return RingRead::Unavailable;
        }
        const int64_t head = headLocked(nowNs());
        const uint64_t lo = head > static_cast<int64_t>(ringFrames_) ? static_cast<uint64_t>(head) - ringFrames_ : 0;
        if (seq < lo || seq >= static_cast<uint64_t>(head)) {
            if (why) *why = "out of range";
            return RingRead::OutOfRange;
        }
        epoch = epoch_;
    }
    fillFrame(epoch, seq + 1, fps_, out);
    out.seq = seq;
    return RingRead::Ok;
}

} // namespace backend::processing::pz
