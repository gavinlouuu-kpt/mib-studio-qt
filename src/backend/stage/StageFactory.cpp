#include "backend/stage/IMotionStage.h"
#include "backend/stage/zc300/Zc300Stage.h"

namespace backend::stage {

const char* toString(StageError error)
{
    switch (error) {
    case StageError::None: return "none";
    case StageError::NotConnected: return "not connected";
    case StageError::WrongDevice: return "wrong device";
    case StageError::Misconfigured: return "controller configuration does not match the stage profile";
    case StageError::Busy: return "busy (axis moving)";
    case StageError::OutOfSoftLimits: return "outside soft limits";
    case StageError::LimitSwitch: return "limit switch active";
    case StageError::EmergencyStop: return "emergency stop active";
    case StageError::DriverAlarm: return "driver alarm";
    case StageError::NotEnabled: return "axis not enabled";
    case StageError::ZeroNotSet: return "zero not set";
    case StageError::OffGrid: return "target is not a whole micrometre";
    case StageError::InvalidArgument: return "invalid argument";
    case StageError::Timeout: return "timeout";
    case StageError::LostAck: return "motion command reply lost";
    case StageError::Protocol: return "protocol error";
    case StageError::Transport: return "transport error";
    case StageError::LimitCheckFailed: return "limit-switch check failed";
    }
    return "unknown";
}

std::unique_ptr<IMotionStage> createStage(StageKind kind,
                                          services::serialbus::SerialBusManager& busManager)
{
    switch (kind) {
    case StageKind::Zc300: return std::make_unique<zc300::Zc300Stage>(busManager);
    }
    return nullptr;
}

} // namespace backend::stage
