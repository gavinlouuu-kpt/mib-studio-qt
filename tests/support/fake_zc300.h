// Fake Zolix ZC300 controller (#464) behind an ISerialPort, for the stage
// driver and service tests. Simulates axis X of the register map, time-based
// motion between two limit switches, the vendor exception codes, and the
// quirks observed on the bench unit (docs/integration/zc300-z-stage.md):
//  - one request is ignored right after a move finishes (dropAfterMove);
//  - the save opcode acknowledges after ~1.06 s (saveDelayMs);
//  - in mm mode distances are truncated to 0.001 mm, then rounded to pulses.
// Every frame, write and opcode is counted so tests can assert exactly what
// reached the wire. The physical position (limits, home sensor) is kept apart
// from the controller's counter, which setPosition and power cycles re-zero.
// Header-only; include as "support/fake_zc300.h".
#pragma once

#include "backend/services/ISerialPort.h"
#include "backend/services/ModbusRtu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mib::test {

// Axis X configuration; the defaults are the bench unit as saved for the
// TBZF6-60 on 2026-09-30.
struct FakeZc300Config {
    std::uint16_t unit{1};      // 0 pp, 1 mm, 2 deg
    std::uint16_t stageType{0}; // 0 linear
    float leadMm{0.7f};
    std::int32_t pulsesPerRev{1600};
};

class FakeZc300 {
    // Declared first: every accessor below runs under the device mutex.
    template <typename F>
    auto locked(F f) -> decltype(f())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return f();
    }

public:
    using Clock = std::chrono::steady_clock;
    using Config = FakeZc300Config;

    // Identity and wiring.
    std::string portName{"ttyFAKE0"};
    std::uint8_t address{1};
    std::string model{"ZC300-1A"};
    std::int32_t serial{26017};
    std::uint16_t firmware{12};

    explicit FakeZc300(Config config = Config{}) : config_(config), flash_(config) {}

    // --- test controls (thread-safe) -------------------------------------
    void setPulsesPerSecond(double pps) { locked([&] { pulsesPerSecond_ = pps; }); }
    void setLimits(std::int64_t negPulse, std::int64_t posPulse)
    {
        locked([&] { negLimit_ = negPulse; posLimit_ = posPulse; });
    }
    // Physical position; the counter keeps its current offset.
    void setPositionPulses(std::int64_t p) { locked([&] { position_ = p; }); }
    // The controller stops pulsing when its e-stop input trips.
    void setEmergencyStop(bool on)
    {
        locked([&] {
            advance();
            estop_ = on;
            if (on) moving_ = jogging_ = false;
        });
    }
    void setDriverAlarm(bool on) { locked([&] { alarm_ = on; }); }
    void setEnabled(bool on) { locked([&] { enabled_ = on; }); }
    void setDropAfterMove(bool on) { locked([&] { dropAfterMove_ = on; }); }
    void setSaveDelayMs(int ms) { locked([&] { saveDelayMs_ = ms; }); }
    // Every reply arrives this late: models a stalled host poll (load, OS
    // sleep granularity, USB latency). Motion keeps running meanwhile.
    void setReplyDelayMs(int ms) { locked([&] { replyDelayMs_ = ms; }); }
    // Next motion opcode executes but its reply is never sent.
    void dropNextMotionAck() { locked([&] { dropMotionAck_ = true; }); }
    // Next motion opcode is ignored entirely (no execution, no reply).
    void swallowNextMotion() { locked([&] { swallowMotion_ = true; }); }
    // Next stop opcode executes but its reply is never sent.
    void dropNextStopAck() { locked([&] { dropStopAck_ = true; }); }
    // Power cycle: volatile state cleared, configuration reloaded from flash.
    void powerCycle()
    {
        locked([&] {
            config_ = flash_;
            offset_ = position_; // the counter restarts at 0 wherever the stage is
            moving_ = false;
            jogging_ = false;
            scratch_ = 0;
            stepDistance_ = 0.0f;
        });
    }

    // --- observations (thread-safe) --------------------------------------
    std::int64_t positionPulses() { return locked([&] { advance(); return position_; }); } // physical
    std::int64_t counterPulses() { return locked([&] { advance(); return position_ - offset_; }); }
    int lastMoveDirection() { return locked([&] { return static_cast<int>(lastDirection_); }); }
    bool moving() { return locked([&] { advance(); return moving_; }); }
    int frames() { return locked([&] { return frames_; }); }
    int writes() { return locked([&] { return writes_; }); }       // FC06/FC16 frames
    int opcodes() { return locked([&] { return static_cast<int>(opcodeLog_.size()); }); }
    int opcodeCount(std::uint16_t op)
    {
        return locked([&] { return static_cast<int>(std::count(opcodeLog_.begin(), opcodeLog_.end(), op)); });
    }
    int droppedRequests() { return locked([&] { return dropped_; }); }
    int saves() { return locked([&] { return saves_; }); }
    // Speed register (mm/s) in force at each motion opcode, in order.
    std::vector<float> motionSpeeds() { return locked([&] { return motionSpeeds_; }); }
    // Motion opcodes (0x64/0x65/0x66) in order.
    std::vector<std::uint16_t> motionLog() { return locked([&] { return motionLog_; }); }
    Config config() { return locked([&] { return config_; }); }
    Config flash() { return locked([&] { return flash_; }); }
    std::uint16_t scratch() { return locked([&] { return scratch_; }); }
    float stepDistance() { return locked([&] { return stepDistance_; }); }

    // --- called by FakeZc300Port on the bus I/O thread -------------------
    struct Reply {
        bool send{false};
        std::vector<std::uint8_t> frame;
        Clock::time_point readyAt{};
    };

    Reply handle(const std::vector<std::uint8_t>& q)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        advance();
        Reply reply;
        if (q.size() < 8 || q[0] != address || !backend::services::modbus::responseCrcValid(q)) {
            return reply; // not for us, or corrupt: silence
        }
        ++frames_;
        if (pendingDrop_) {
            pendingDrop_ = false;
            ++dropped_;
            return reply;
        }
        const std::uint8_t func = q[1];
        const int start = ((q[2] << 8) | q[3]) + 1; // manual register number
        const int count = (q[4] << 8) | q[5];
        reply.send = true;
        reply.readyAt = Clock::now() + std::chrono::milliseconds(replyDelayMs_);

        if (func == 0x03 || func == 0x04) {
            const bool input = start < 30050;
            if (input != (func == 0x04) || count < 1 || count > 125) {
                reply.frame = exception(func, 0x02);
                return reply;
            }
            reply.frame = {address, func, static_cast<std::uint8_t>(count * 2)};
            for (int r = start; r < start + count; ++r) {
                const std::uint16_t v = readRegister(r);
                reply.frame.push_back(static_cast<std::uint8_t>(v >> 8));
                reply.frame.push_back(static_cast<std::uint8_t>(v & 0xFF));
            }
            backend::services::modbus::appendCrc(reply.frame);
            return reply;
        }
        if (func != 0x10) {
            reply.frame = exception(func, 0x01);
            return reply;
        }
        ++writes_;
        if (q.size() < 9 + static_cast<size_t>(q[6]) || q[6] != count * 2 || start < 30050) {
            reply.frame = exception(func, 0x02);
            return reply;
        }
        std::vector<std::uint16_t> words;
        for (int i = 0; i < count; ++i) words.push_back(static_cast<std::uint16_t>((q[7 + 2 * i] << 8) | q[8 + 2 * i]));

        std::uint8_t exc = 0;
        bool suppressReply = false;
        if (start == 30050) {
            exc = opcode(words, suppressReply);
            if (words[0] == 0x6D && exc == 0) reply.readyAt += std::chrono::milliseconds(saveDelayMs_);
        } else {
            exc = writeRegisters(start, words);
        }
        if (suppressReply) {
            reply.send = false;
            return reply;
        }
        if (exc) {
            reply.frame = exception(func, exc);
        } else {
            reply.frame.assign(q.begin(), q.begin() + 6);
            backend::services::modbus::appendCrc(reply.frame);
        }
        return reply;
    }

private:
    std::vector<std::uint8_t> exception(std::uint8_t func, std::uint8_t code) const
    {
        std::vector<std::uint8_t> f{address, static_cast<std::uint8_t>(func | 0x80), code};
        backend::services::modbus::appendCrc(f);
        return f;
    }

    static std::pair<std::uint16_t, std::uint16_t> floatWords(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        return {static_cast<std::uint16_t>(bits >> 16), static_cast<std::uint16_t>(bits & 0xFFFF)};
    }
    static float wordsFloat(std::uint16_t hi, std::uint16_t lo)
    {
        const std::uint32_t bits = (static_cast<std::uint32_t>(hi) << 16) | lo;
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    double pulsesPerUnit() const
    {
        if (config_.unit == 0) return 1.0;
        return config_.pulsesPerRev / static_cast<double>(config_.leadMm); // per mm
    }
    float positionInUnit() const { return static_cast<float>((position_ - offset_) / pulsesPerUnit()); }
    // The controller truncates unit distances to 0.001 before converting.
    std::int64_t distancePulses(float value) const
    {
        if (config_.unit == 0) return static_cast<std::int64_t>(std::llround(value));
        const double truncated = std::trunc(static_cast<double>(value) * 1000.0) / 1000.0;
        return static_cast<std::int64_t>(std::llround(truncated * pulsesPerUnit()));
    }
    bool atPositiveLimit() const { return position_ >= posLimit_; }
    bool atNegativeLimit() const { return position_ <= negLimit_; }

    std::uint16_t readRegister(int r) const
    {
        const auto pos = floatWords(positionInUnit());
        switch (r) {
        case 30008: return static_cast<std::uint16_t>(static_cast<std::uint32_t>(serial) >> 16);
        case 30009: return static_cast<std::uint16_t>(static_cast<std::uint32_t>(serial) & 0xFFFF);
        case 30010: return firmware;
        case 30011: return 0x07;
        case 30012: return moving_ ? 1 : 0;
        case 30015: {
            std::uint16_t s = 0;
            if (atPositiveLimit()) s |= 1u << 0;
            if (atNegativeLimit()) s |= 1u << 1;
            if (position_ == 0) s |= 1u << 2;
            if (estop_) s |= 1u << 9;
            if (alarm_) s |= 1u << 10;
            return s;
        }
        case 30016: case 30059: return pos.first;
        case 30017: case 30060: return pos.second;
        case 30022: case 30072: return config_.unit;
        case 30054: return scratch_;
        case 30066: return enabled_ ? 1 : 0;
        case 30075: return static_cast<std::uint16_t>(static_cast<std::uint32_t>(config_.pulsesPerRev) >> 16);
        case 30076: return static_cast<std::uint16_t>(static_cast<std::uint32_t>(config_.pulsesPerRev) & 0xFFFF);
        case 30081: return config_.stageType;
        case 30084: return floatWords(config_.leadMm).first;
        case 30085: return floatWords(config_.leadMm).second;
        case 30114: return floatWords(stepDistance_).first;
        case 30115: return floatWords(stepDistance_).second;
        default: break;
        }
        if (r >= 30001 && r <= 30007) {
            const size_t i = static_cast<size_t>(r - 30001) * 2;
            const auto ch = [&](size_t k) { return k < model.size() ? static_cast<std::uint8_t>(model[k]) : 0; };
            return static_cast<std::uint16_t>((ch(i) << 8) | ch(i + 1));
        }
        const auto it = extra_.find(r);
        return it == extra_.end() ? 0 : it->second;
    }

    std::uint8_t writeRegisters(int start, const std::vector<std::uint16_t>& w)
    {
        const auto f = [&] { return wordsFloat(w[0], w[1]); };
        if (w.size() == 2) {
            switch (start) {
            case 30059: offset_ = position_ - static_cast<std::int64_t>(std::llround(f() * pulsesPerUnit())); return 0;
            case 30075: config_.pulsesPerRev = static_cast<std::int32_t>((static_cast<std::uint32_t>(w[0]) << 16) | w[1]); return 0;
            case 30084: config_.leadMm = f(); return 0;
            case 30114: stepDistance_ = f(); return 0;
            case 30129: speed_ = f(); return 0;
            default: break;
            }
        }
        if (w.size() == 1) {
            switch (start) {
            case 30054: scratch_ = w[0]; return 0;
            case 30066: enabled_ = w[0] != 0; return 0;
            case 30072: config_.unit = w[0]; return 0;
            case 30081: config_.stageType = w[0]; return 0;
            default: break;
            }
        }
        for (size_t i = 0; i < w.size(); ++i) extra_[start + static_cast<int>(i)] = w[i];
        return 0;
    }

    std::uint8_t opcode(const std::vector<std::uint16_t>& w, bool& suppressReply)
    {
        const std::uint16_t op = w[0];
        opcodeLog_.push_back(op);
        const bool isMotion = op == 0x64 || op == 0x65 || op == 0x66;
        if (isMotion) {
            motionLog_.push_back(op);
            motionSpeeds_.push_back(speed_);
        }
        if (isMotion && swallowMotion_) {
            swallowMotion_ = false;
            suppressReply = true;
            return 0;
        }
        if (op == 0x67 || op == 0x68) {
            moving_ = false;
            jogging_ = false;
            if (dropStopAck_) {
                dropStopAck_ = false;
                suppressReply = true;
            }
            return 0;
        }
        if (op == 0x6D) {
            if (moving_) return 0x04;
            flash_ = config_;
            ++saves_;
            return 0;
        }
        if (!isMotion) return 0x0A;
        if (w.size() < 3 || w[1] != 0x31) return 0x05; // only axis X is modelled
        if (estop_) return 0x08;
        if (!enabled_) return 0x09;
        if (moving_) return 0x06;
        const bool positive = w[2] == 0x50;
        std::int64_t target = position_;
        if (op == 0x64) { // absolute targets are counter values
            target = distancePulses(stepDistance_) * (positive ? 1 : -1) + offset_;
        } else if (op == 0x65) {
            target = position_ + distancePulses(stepDistance_) * (positive ? 1 : -1);
        }
        const bool towardPositive = op == 0x66 ? positive : target > position_;
        if ((towardPositive && atPositiveLimit()) || (!towardPositive && atNegativeLimit())) {
            if (op == 0x66 || target != position_) return 0x07;
        }
        if (op == 0x66) {
            jogging_ = true;
            jogDirection_ = positive ? 1 : -1;
            moving_ = true;
        } else if (target != position_) {
            target_ = target;
            moving_ = true;
        }
        lastAdvance_ = Clock::now();
        if (dropMotionAck_) {
            dropMotionAck_ = false;
            suppressReply = true;
        }
        return 0;
    }

    // Moves the simulated axis forward to "now".
    void advance()
    {
        const auto now = Clock::now();
        if (!moving_) {
            lastAdvance_ = now;
            return;
        }
        const double dt = std::chrono::duration<double>(now - lastAdvance_).count();
        auto steps = static_cast<std::int64_t>(dt * pulsesPerSecond_);
        if (steps <= 0) return;
        lastAdvance_ = now;
        const std::int64_t direction = jogging_ ? jogDirection_ : (target_ > position_ ? 1 : -1);
        lastDirection_ = direction;
        while (steps-- > 0 && moving_) {
            position_ += direction;
            const bool hitLimit = (direction > 0 && atPositiveLimit()) || (direction < 0 && atNegativeLimit());
            if (hitLimit || (!jogging_ && position_ == target_)) {
                moving_ = false;
                jogging_ = false;
                if (dropAfterMove_) pendingDrop_ = true;
            }
        }
    }

    std::mutex mutex_;
    Config config_;
    Config flash_;
    double pulsesPerSecond_{200000.0};
    std::int64_t position_{0}; // physical
    std::int64_t offset_{0};   // counter = position_ - offset_
    std::int64_t target_{0};
    std::int64_t lastDirection_{0};
    std::int64_t negLimit_{-6858}; // about -3000 µm at 0.4375 µm/pulse
    std::int64_t posLimit_{6858};
    bool moving_{false};
    bool jogging_{false};
    std::int64_t jogDirection_{1};
    Clock::time_point lastAdvance_{Clock::now()};
    bool estop_{false};
    bool alarm_{false};
    bool enabled_{true};
    float stepDistance_{0.0f};
    std::uint16_t scratch_{0};
    std::map<int, std::uint16_t> extra_;
    bool dropAfterMove_{false};
    bool pendingDrop_{false};
    int saveDelayMs_{0};
    int replyDelayMs_{0};
    float speed_{3.5f}; // mm/s, the bench unit's saved cruise speed
    std::vector<float> motionSpeeds_;
    std::vector<std::uint16_t> motionLog_;
    bool dropMotionAck_{false};
    bool swallowMotion_{false};
    bool dropStopAck_{false};
    int frames_{0};
    int writes_{0};
    int dropped_{0};
    int saves_{0};
    std::vector<std::uint16_t> opcodeLog_;
};

class FakeZc300Port final : public backend::services::ISerialPort {
public:
    explicit FakeZc300Port(FakeZc300& device) : device_(device) {}

    bool open(int, int) override { return false; }
    bool openNamed(const std::string& name, const backend::services::SerialSettings&) override
    {
        opened_ = name == device_.portName;
        return opened_;
    }
    void close() override { opened_ = false; }
    bool isOpen() const override { return opened_; }
    int lastSystemError() const override { return opened_ ? 0 : 2; }
    std::string lastError() const override { return opened_ ? "" : "no such fake port"; }

    int write(const std::vector<std::uint8_t>& data) override
    {
        const auto reply = device_.handle(data);
        if (reply.send) {
            rx_ = reply.frame;
            readyAt_ = reply.readyAt;
        }
        return static_cast<int>(data.size());
    }
    bool waitForBytesWritten(int) override { return true; }
    bool waitForReadyRead(int ms) override
    {
        const auto deadline = FakeZc300::Clock::now() + std::chrono::milliseconds(ms);
        if (rx_.empty()) {
            std::this_thread::sleep_until(deadline);
            return false;
        }
        std::this_thread::sleep_until(std::min(deadline, readyAt_));
        return FakeZc300::Clock::now() >= readyAt_;
    }
    std::vector<std::uint8_t> readAll() override
    {
        if (rx_.empty() || FakeZc300::Clock::now() < readyAt_) return {};
        auto out = std::move(rx_);
        rx_.clear();
        return out;
    }

private:
    FakeZc300& device_;
    bool opened_{false};
    std::vector<std::uint8_t> rx_;
    FakeZc300::Clock::time_point readyAt_{};
};

} // namespace mib::test
