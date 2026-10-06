// Zolix ZC300 Modbus RTU protocol (vendor manual V1.15; evidence in
// docs/integration/zc300-z-stage.md). Pure functions only — no I/O — so the
// register map, frames and decoding are unit tested without a controller.
//
// Register numbers are the manual's 1-based 3xxxx numbers; the wire address is
// number - 1. 30001..30049 are input registers (FC04 only); 30050.. are
// holding registers (FC03 read, FC16 write). 32-bit values are big-endian,
// high word first.
#pragma once

#include "backend/services/ModbusRtu.h"
#include "backend/stage/StageProfiles.h"
#include "backend/stage/StageTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace backend::stage::zc300 {

using Frame = services::modbus::Frame;

inline constexpr int kAxisCount = 3;
inline constexpr int kBaudRate = 115200; // fixed by the controller

// Input registers (FC04).
inline constexpr int kRegModel = 30001;      // 7 registers, ASCII
inline constexpr int kRegSerial = 30008;     // LONG
inline constexpr int kRegFirmware = 30010;   // version x 10
inline constexpr int kRegMoving = 30012;     // + axis
inline constexpr int kRegSwitches = 30015;
inline constexpr int kRegPositionRo = 30016; // float, + 2 * axis
inline constexpr int kStatusBlockStart = kRegMoving;
inline constexpr int kStatusBlockCount = 10; // 30012..30021: moving x3, switches, positions x3

// Holding registers (FC03 / FC16).
inline constexpr int kRegOpcode = 30050; // then param 1..3 at 30051..30053
inline constexpr int kRegScratch = 30054; // reserved, volatile: power-up token
inline constexpr int kRegPosition = 30059;     // float, + 2 * axis; writing redefines position
inline constexpr int kRegUnit = 30072;         // + axis
inline constexpr int kRegPulsesPerRev = 30075; // LONG, + 2 * axis
inline constexpr int kRegStageType = 30081;    // + axis
inline constexpr int kRegLead = 30084;         // float, + 2 * axis
inline constexpr int kRegStepDistance = 30114; // float, + 2 * axis; also the absolute target
inline constexpr int kRegSpeed = 30129;        // float, + 2 * axis
inline constexpr int kRegAccel = 30135;        // float, + 2 * axis

enum class Unit : std::uint16_t { Pulses = 0, Millimetres = 1, Degrees = 2 };
enum class StageType : std::uint16_t { Linear = 0, Rotary = 1 };

enum class Opcode : std::uint16_t {
    MoveAbsolute = 0x64,
    MoveRelative = 0x65,
    Jog = 0x66,
    DecelerateStop = 0x67,
    Stop = 0x68,
    Home = 0x69,
    FactoryReset = 0x6C,
    Save = 0x6D,
};

inline constexpr std::uint16_t kDirPositive = 0x50; // 'P'
inline constexpr std::uint16_t kDirNegative = 0x4E; // 'N'

inline constexpr int reg16(int base, int axis) { return base + axis; }
inline constexpr int reg32(int base, int axis) { return base + 2 * axis; }
inline constexpr std::uint16_t wireAddress(int reg) { return static_cast<std::uint16_t>(reg - 1); }
inline constexpr bool isInputRegister(int reg) { return reg < kRegOpcode; }
inline constexpr std::uint16_t axisCode(int axis) { return static_cast<std::uint16_t>(0x31 + axis); }

// FC04 for input registers, FC03 for holding registers.
Frame buildRead(std::uint8_t address, int reg, std::uint16_t count);
// FC16 write of raw register bytes (big-endian words).
Frame buildWrite(std::uint8_t address, int reg, const Frame& registerBytes);
Frame buildWriteU16(std::uint8_t address, int reg, std::uint16_t value);
Frame buildWriteFloat(std::uint8_t address, int reg, float value);
Frame buildWriteLong(std::uint8_t address, int reg, std::int32_t value);
// Opcode plus its used parameters (FC16 at 30050); `paramCount` is 0..2.
Frame buildOpcode(std::uint8_t address, Opcode op, int paramCount = 0,
                  std::uint16_t param1 = 0, std::uint16_t param2 = 0);

std::uint16_t decodeU16(const std::uint8_t* data);
std::int32_t decodeLong(const std::uint8_t* data);
float decodeFloat(const std::uint8_t* data);
std::string decodeAscii(const Frame& registerBytes); // trailing NUL/space trimmed

struct SwitchState {
    bool limitPositive{false};
    bool limitNegative{false};
    bool home{false};
    bool driverAlarm{false};
    bool emergencyStop{false};
};
SwitchState decodeSwitches(std::uint16_t raw, int axis);

// Exception codes 01..0A from the vendor manual.
StageError errorFromException(std::uint8_t code);

struct ControllerConfig {
    Unit unit{Unit::Pulses};
    StageType stageType{StageType::Linear};
    double leadMm{0.0};
    std::int32_t pulsesPerRev{0};
};

bool configMatches(const ControllerConfig& config, const StageProfile& profile);
AxisCalibration calibrationFor(const ControllerConfig& config);

// Micrometres -> the millimetre value written to a distance register. The
// controller truncates to 0.001 mm and float32 can drop the last µm, so the
// encoding adds a 0.01 µm guard. Returns nullopt for targets that are not a
// whole number of micrometres (OffGrid).
std::optional<float> encodeMicronsAsMm(double um);

// Controller position (in its current unit) -> micrometres.
double positionToMicrons(float raw, const ControllerConfig& config);

} // namespace backend::stage::zc300
