// IMotionStage driver for the Zolix ZC300 over the shared Modbus bus layer.
// See ADR 0013 and docs/integration/zc300-z-stage.md for the controller
// quirks encoded here.
#pragma once

#include "backend/services/SerialBus.h"
#include "backend/stage/IMotionStage.h"
#include "backend/stage/zc300/Zc300Protocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace backend::stage::zc300 {

class Zc300Stage final : public IMotionStage {
public:
    // Timeouts per transaction. Reads and idempotent writes are retried after
    // silence (the controller drops one request right after a move ends);
    // motion opcodes are never re-sent.
    struct Timing {
        int transactionMs{500};
        int saveMs{3000}; // the save acknowledges after ~1.06 s
        int silenceRetries{3};
    };

    explicit Zc300Stage(services::serialbus::SerialBusManager& busManager);
    Zc300Stage(services::serialbus::SerialBusManager& busManager, Timing timing);
    ~Zc300Stage() override;

    StageKind kind() const override { return StageKind::Zc300; }

    StageError connect(const StageEndpoint& endpoint, const StageProfile& profile,
                       StageIdentity& identity, std::string& detail) override;
    void disconnect() override;
    bool isConnected() const override;
    bool isConfigured() const override;
    AxisCalibration calibration() const override;

    StageError readStatus(StageStatus& status) override;
    StageError moveAbsolute(double targetUm) override;
    StageError moveRelative(double deltaUm) override;
    StageError jog(Direction direction) override;
    StageError stop() override;
    StageError setPosition(double positionUm) override;
    StageError setSpeed(double umPerS, double umPerS2) override;
    StageError applyProfile(const StageProfile& profile) override;
    StageError readPowerUpToken(std::uint16_t& token) override;
    StageError writePowerUpToken(std::uint16_t token) override;

    ControllerConfig controllerConfig() const;

private:
    // All require mutex_ held.
    StageError readLocked(int reg, std::uint16_t count, Frame& data);
    StageError writeLocked(const Frame& request, int timeoutMs);
    StageError motionLocked(Opcode op, std::uint16_t direction);
    StageError readStatusLocked(StageStatus& status);
    StageError readConfigLocked(ControllerConfig& config);
    StageError requireMotionLocked() const;
    StageError moveLocked(Opcode op, double um);
    void releaseLocked();

    services::serialbus::SerialBusManager& busManager_;
    const Timing timing_;

    // Bounded, prioritized access (#511: one tight status poller stalled a
    // move for 60 s on a TSan CI runner). Commands register as priority
    // waiters and status polls step aside for them, so back-to-back polls
    // cannot starve a move or a Stop. Commands give up with Busy after
    // kLockTimeout instead of waiting forever.
    class Access;
    static constexpr std::chrono::seconds kLockTimeout{15};
    mutable std::mutex mutex_;
    mutable std::atomic<int> priorityWaiters_{0};
    std::shared_ptr<services::serialbus::ModbusBusSession> bus_;
    std::uint8_t address_{1};
    int axis_{0};
    StageProfile profile_;
    ControllerConfig config_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> configured_{false};
};

} // namespace backend::stage::zc300
