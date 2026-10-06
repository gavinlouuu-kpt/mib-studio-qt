// Value types for motorized stages (ADR 0013). Qt-free, vendor-free: every
// position and distance above a driver is in micrometres.
#pragma once

#include <cstdint>
#include <string>

namespace backend::stage {

enum class StageKind { Zc300 }; // append-only

enum class AxisKind { Linear, Rotary };

enum class Direction { Negative, Positive };

enum class MoveState { Idle, Moving, Homing, Faulted };

enum class StageError {
    None,
    NotConnected,
    WrongDevice,     // the endpoint answered, but it is not the expected controller
    Misconfigured,   // controller unit/type/lead/pulses disagree with the stage profile
    Busy,            // controller refused: axis already moving (or saving while moving)
    OutOfSoftLimits, // StageService only
    LimitSwitch,     // controller refused: limit switch active in that direction
    EmergencyStop,
    DriverAlarm,
    NotEnabled,
    ZeroNotSet,      // StageService only: the operator has not set zero this power-up
    OffGrid,         // target is not a whole multiple of the command quantum
    InvalidArgument,
    Timeout,
    LostAck,         // a motion command got no reply and status cannot prove it started
    Protocol,
    Transport,
    LimitCheckFailed, // verify-limits: wiring, polarity or span did not check out
};

const char* toString(StageError error);

// Where a stage lives. `usbSerial` re-resolves the port after a rename
// (ttyUSB0 -> ttyUSB1); `systemPort` wins when both are set.
struct StageEndpoint {
    std::string systemPort;
    std::string usbSerial;
    std::uint8_t modbusAddress{1};
    int axis{0}; // 0 = X
};

struct StageIdentity {
    std::string model;    // "ZC300-1A"
    std::string serial;   // "26017"
    std::string firmware; // "1.2"
};

struct StageStatus {
    MoveState state{MoveState::Idle};
    double positionUm{0.0};
    bool zeroSet{false}; // the operator set zero this power-up; StageService fills it, drivers report false
    bool limitPositive{false};
    bool limitNegative{false};
    bool home{false};
    bool emergencyStop{false};
    bool driverAlarm{false};
    std::int64_t sampledAtNs{0}; // steady clock
};

struct AxisCalibration {
    AxisKind kind{AxisKind::Linear};
    double umPerPulse{0.0};       // 0.4375 for a TBZF6-60 at 1600 pulses/rev
    double commandQuantumUm{1.0}; // targets must be whole multiples of this
};

} // namespace backend::stage
