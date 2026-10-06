// Driver seam for motorized stages (ADR 0013). Drivers are literal: they
// convert units and enforce the controller's own rules, while soft limits,
// referencing and backlash approach belong to StageService.
#pragma once

#include "backend/stage/StageProfiles.h"
#include "backend/stage/StageTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace backend::services::serialbus {
class SerialBusManager;
}

namespace backend::stage {

class IMotionStage {
public:
    virtual ~IMotionStage() = default;

    virtual StageKind kind() const = 0;

    // Observe-only: opens the port, identifies the controller, reads its
    // configuration and checks it against `profile`. Never writes. A
    // controller that does not match the profile stays connected read-only
    // (isConfigured() == false) and refuses motion with Misconfigured.
    virtual StageError connect(const StageEndpoint& endpoint, const StageProfile& profile,
                               StageIdentity& identity, std::string& detail) = 0;
    // Stops the axis first if it is moving; otherwise writes nothing.
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool isConfigured() const = 0;
    virtual AxisCalibration calibration() const = 0;

    virtual StageError readStatus(StageStatus& status) = 0;

    // Counts every stop() call, bumped when the call starts (before it waits for
    // the driver). A caller that read it before deciding to move can pass it to
    // moveAbsolute: a Stop that arrived in between, even one still queued, makes
    // the move fail with Stopped instead of starting motion after the Stop.
    static constexpr std::uint64_t kAnyStopGeneration = ~std::uint64_t{0};
    virtual std::uint64_t stopGeneration() const = 0;

    // Motion returns once the controller accepted the command; poll
    // readStatus() for completion. Targets off the command grid are rejected
    // with OffGrid, never rounded. `expectedStopGeneration` (see above) is
    // checked under the driver lock, immediately before the opcode.
    virtual StageError moveAbsolute(double targetUm,
                                    std::uint64_t expectedStopGeneration = kAnyStopGeneration) = 0;
    virtual StageError moveRelative(double deltaUm) = 0;
    // Open-ended: runs until stop() or a limit switch, so its extent depends
    // on the caller stopping it in time. Prefer a bounded moveRelative; the
    // service's Home search does not use jog (#464).
    virtual StageError jog(Direction direction) = 0;
    virtual StageError stop() = 0;                   // immediate, idempotent, retried

    // Redefines the current position without motion.
    virtual StageError setPosition(double positionUm) = 0;
    // Volatile until the controller parameters are saved.
    virtual StageError setSpeed(double umPerS, double umPerS2) = 0;
    // The only configuration write: unit, stage type, lead and pulses/rev,
    // then save to the controller's flash. Refused while moving.
    virtual StageError applyProfile(const StageProfile& profile) = 0;

    // Volatile scratch value that the controller clears at power-up; lets
    // StageService keep a reference across application restarts (ADR 0013 §6).
    virtual StageError readPowerUpToken(std::uint16_t& token) = 0;
    virtual StageError writePowerUpToken(std::uint16_t token) = 0;
};

std::unique_ptr<IMotionStage> createStage(StageKind kind,
                                          services::serialbus::SerialBusManager& busManager);

} // namespace backend::stage
