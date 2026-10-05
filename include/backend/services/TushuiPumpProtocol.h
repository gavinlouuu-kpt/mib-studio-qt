// Register map and unit conversions for the Tushui (惠州徒水) peristaltic pump,
// "精简版本" (simplified) command set of the vendor's Modbus protocol V2.21.
// Pure functions, no device I/O, so the protocol is unit-tested without a port.
//
// Wire facts (protocol V2.21, verified on the PZ7035 RS485 port 2026-10-04):
// 8N1, default 115200 baud, FC 03/06/10, CRC low byte first. The pump on the
// instrument answers at slave address 3 (vendor default 1).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace backend::services::tushui {

// Holding registers 100-107 (one 16-bit word each unless noted).
inline constexpr uint16_t kRegSpeed = 100;      // rpm x100, 1..50000 (0.01-500.00 rpm)
inline constexpr uint16_t kRegDirection = 101;  // 0 = clockwise, 1 = counter-clockwise
inline constexpr uint16_t kRegTurnsHigh = 102;  // turns x1000, 32 bit over 102 (high) + 103 (low);
inline constexpr uint16_t kRegTurnsLow = 103;   //   0 = run until stopped
inline constexpr uint16_t kRegRunState = 104;   // write 0 stop / 1 run; read also 2-4 below
inline constexpr uint16_t kRegBaudHigh = 105;   // baud, 32 bit over 105 + 106
inline constexpr uint16_t kRegBaudLow = 106;
inline constexpr uint16_t kRegSlaveId = 107;    // 1..254
inline constexpr uint16_t kStatusFirst = kRegSpeed;
inline constexpr uint16_t kStatusCount = 8;     // one FC03 read covers 100-107

enum class RunState : uint16_t { Stopped = 0, Running = 1, SuckBack = 2, Timing = 3, Paused = 4 };

inline constexpr uint16_t kClockwise = 0;
inline constexpr uint16_t kCounterClockwise = 1;

inline constexpr double kMinRpm = 0.01;
inline constexpr double kMaxRpm = 500.0; // protocol ceiling; the fitted model may be lower

// Flow calibration. 0.4 rpm delivers 10 µL/min on the instrument tubing
// (operator figure 2026-10-04, to be replaced by a measured calibration).
inline constexpr double kDefaultMicrolitersPerRev = 25.0;

// Purge runs the head at a fixed speed instead of a flow rate.
inline constexpr double kPurgeRpm = 100.0;

struct Status {
    double speedRpm{0.0};
    uint16_t direction{kClockwise};
    double turns{0.0}; // 0 = continuous
    RunState runState{RunState::Stopped};
    uint16_t rawRunState{0};
    uint32_t baudRate{0};
    uint16_t slaveId{0};
};

// Registers 100-107 as returned by FC03 (big-endian byte pairs, 16 bytes).
inline bool decodeStatus(const std::vector<uint8_t>& data, Status& out)
{
    if (data.size() < kStatusCount * 2u) return false;
    std::array<uint16_t, kStatusCount> r{};
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = static_cast<uint16_t>((data[2 * i] << 8) | data[2 * i + 1]);
    out.speedRpm = r[0] / 100.0;
    out.direction = r[1];
    out.turns = ((static_cast<uint32_t>(r[2]) << 16) | r[3]) / 1000.0;
    out.rawRunState = r[4];
    out.runState = r[4] <= 4 ? static_cast<RunState>(r[4]) : RunState::Running;
    out.baudRate = (static_cast<uint32_t>(r[5]) << 16) | r[6];
    out.slaveId = r[7];
    return true;
}

inline bool isMoving(RunState state)
{
    return state == RunState::Running || state == RunState::SuckBack || state == RunState::Timing;
}

// Speed register value for an rpm, clamped to the protocol range.
inline uint16_t rpmToRegister(double rpm)
{
    const double clamped = std::fmin(std::fmax(rpm, kMinRpm), kMaxRpm);
    return static_cast<uint16_t>(std::lround(clamped * 100.0));
}

inline double flowToRpm(double microlitersPerMinute, double microlitersPerRev)
{
    return microlitersPerRev > 0.0 ? microlitersPerMinute / microlitersPerRev : 0.0;
}

inline double rpmToFlow(double rpm, double microlitersPerRev) { return rpm * microlitersPerRev; }

// FC10 payload for registers 102-103 (turns x1000, high word first).
inline std::vector<uint8_t> turnsPayload(double turns)
{
    const auto v = static_cast<uint32_t>(std::lround(std::fmax(turns, 0.0) * 1000.0));
    return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

} // namespace backend::services::tushui
