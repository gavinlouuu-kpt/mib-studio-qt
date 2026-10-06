#include "backend/stage/zc300/Zc300Protocol.h"

#include <cmath>

namespace backend::stage::zc300 {

namespace modbus = services::modbus;

Frame buildRead(std::uint8_t address, int reg, std::uint16_t count)
{
    return isInputRegister(reg) ? modbus::buildReadInputRequest(address, wireAddress(reg), count)
                                : modbus::buildReadRequest(address, wireAddress(reg), count);
}

Frame buildWrite(std::uint8_t address, int reg, const Frame& registerBytes)
{
    return modbus::buildWriteMultipleRequest(address, wireAddress(reg), registerBytes);
}

Frame buildWriteU16(std::uint8_t address, int reg, std::uint16_t value)
{
    return buildWrite(address, reg,
                      Frame{static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value & 0xFF)});
}

Frame buildWriteFloat(std::uint8_t address, int reg, float value)
{
    return buildWrite(address, reg, modbus::floatToRegisters(value));
}

Frame buildWriteLong(std::uint8_t address, int reg, std::int32_t value)
{
    const auto v = static_cast<std::uint32_t>(value);
    return buildWrite(address, reg,
                      Frame{static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
                            static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
}

Frame buildOpcode(std::uint8_t address, Opcode op, int paramCount, std::uint16_t param1,
                  std::uint16_t param2)
{
    Frame regs;
    auto push = [&regs](std::uint16_t v) {
        regs.push_back(static_cast<std::uint8_t>(v >> 8));
        regs.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    push(static_cast<std::uint16_t>(op));
    if (paramCount >= 1) push(param1);
    if (paramCount >= 2) push(param2);
    return buildWrite(address, kRegOpcode, regs);
}

std::uint16_t decodeU16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

std::int32_t decodeLong(const std::uint8_t* data)
{
    const std::uint32_t v = (static_cast<std::uint32_t>(data[0]) << 24) |
                            (static_cast<std::uint32_t>(data[1]) << 16) |
                            (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
    return static_cast<std::int32_t>(v);
}

float decodeFloat(const std::uint8_t* data)
{
    return modbus::registersToFloat(data);
}

std::string decodeAscii(const Frame& registerBytes)
{
    std::string text(registerBytes.begin(), registerBytes.end());
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();
    return text;
}

SwitchState decodeSwitches(std::uint16_t raw, int axis)
{
    SwitchState s;
    s.limitPositive = (raw >> (3 * axis)) & 1;
    s.limitNegative = (raw >> (3 * axis + 1)) & 1;
    s.home = (raw >> (3 * axis + 2)) & 1;
    s.emergencyStop = (raw >> 9) & 1;
    s.driverAlarm = (raw >> (10 + axis)) & 1;
    return s;
}

StageError errorFromException(std::uint8_t code)
{
    switch (code) {
    case 0x04: // save failed: an axis was moving
    case 0x06: return StageError::Busy;
    case 0x07: return StageError::LimitSwitch;
    case 0x08: return StageError::EmergencyStop;
    case 0x09: return StageError::NotEnabled;
    case 0x05: return StageError::InvalidArgument;
    default: return StageError::Protocol; // 01/02/03 framing, 0A bad opcode
    }
}

bool configMatches(const ControllerConfig& config, const StageProfile& profile)
{
    if (profile.kind != AxisKind::Linear) return false; // rotary stages are not supported yet
    return config.unit == Unit::Millimetres && config.stageType == StageType::Linear &&
           std::abs(config.leadMm - profile.leadMm) < 1e-4 &&
           config.pulsesPerRev == profile.pulsesPerRev;
}

AxisCalibration calibrationFor(const ControllerConfig& config)
{
    AxisCalibration c;
    c.kind = config.stageType == StageType::Rotary ? AxisKind::Rotary : AxisKind::Linear;
    if (config.pulsesPerRev > 0) c.umPerPulse = config.leadMm * 1000.0 / config.pulsesPerRev;
    c.commandQuantumUm = 1.0; // mm unit mode: the controller resolves 0.001 mm
    return c;
}

std::optional<float> encodeMicronsAsMm(double um)
{
    if (!std::isfinite(um)) return std::nullopt;
    const double whole = std::round(um);
    if (std::abs(um - whole) > 1e-6) return std::nullopt;
    return static_cast<float>(std::abs(whole) / 1000.0 + 1e-5);
}

double positionToMicrons(float raw, const ControllerConfig& config)
{
    switch (config.unit) {
    case Unit::Millimetres: return static_cast<double>(raw) * 1000.0;
    case Unit::Pulses: return static_cast<double>(raw) * calibrationFor(config).umPerPulse;
    case Unit::Degrees: break;
    }
    return static_cast<double>(raw); // rotary: degrees are passed through unchanged
}

} // namespace backend::stage::zc300
