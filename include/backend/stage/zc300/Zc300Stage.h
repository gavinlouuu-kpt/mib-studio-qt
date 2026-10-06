// IMotionStage driver for the Zolix ZC300 over the shared Modbus bus layer.
// See ADR 0013 and docs/integration/zc300-z-stage.md for the controller
// quirks encoded here.
#pragma once

#include "backend/services/SerialBus.h"
#include "backend/stage/IMotionStage.h"
#include "backend/stage/zc300/Zc300Protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    StageError moveAbsolute(double targetUm, std::uint64_t expectedStopGeneration = kAnyStopGeneration) override;
    StageError moveRelative(double deltaUm) override;
    StageError jog(Direction direction) override;
    StageError stop() override;
    std::uint64_t stopGeneration() const override { return stopGeneration_.load(); }
    StageError setPosition(double positionUm) override;
    StageError setSpeed(double umPerS, double umPerS2) override;
    StageError applyProfile(const StageProfile& profile) override;
    StageError readPowerUpToken(std::uint16_t& token) override;
    StageError writePowerUpToken(std::uint16_t token) override;

    ControllerConfig controllerConfig() const;
    // Calls currently queued for the driver (diagnostics and tests).
    std::size_t waitingCalls() const;

private:
    // All require the driver held (an owned Access).
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

    // Fair, prioritized, bounded access to the driver (#511, then #516).
    // `std::mutex` is not fair, and a try_lock + sleep loop is worse: a thread
    // that re-locks within nanoseconds keeps the driver while sleepers lose
    // every race. So waiters queue, and a release hands the driver straight to
    // the next waiter: commands first (FIFO), then status polls (FIFO).
    // Nobody can jump the queue, so tight pollers cannot starve each other
    // or a Stop. Commands and polls give up with Busy after kLockTimeout;
    // disconnect (teardown) waits its turn however long it takes.
    // Stop has its own queue served before everything else, so it is sent right
    // after the call in flight, never behind queued moves or teardown. To keep
    // a Stop storm from starving a waiting Disconnect forever, at most
    // kMaxConsecutiveStops are granted in a row while anything else waits.
    class Access;
    struct Waiter {
        bool granted{false}; // set by the releasing thread, under gate_
    };
    static constexpr std::chrono::seconds kLockTimeout{15};
    static constexpr int kMaxConsecutiveStops{4};
    mutable std::mutex gate_; // guards held_ and the queues; never held during bus I/O
    mutable std::condition_variable gateCv_;
    mutable bool held_{false};
    mutable std::deque<Waiter*> stopQueue_;    // Stop only: served first
    mutable std::deque<Waiter*> commandQueue_; // commands and teardown
    mutable int consecutiveStops_{0};
    mutable std::deque<Waiter*> pollQueue_;
    std::shared_ptr<services::serialbus::ModbusBusSession> bus_;
    std::uint8_t address_{1};
    int axis_{0};
    StageProfile profile_;
    ControllerConfig config_;
    std::atomic<std::uint64_t> stopGeneration_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> configured_{false};
};

} // namespace backend::stage::zc300
