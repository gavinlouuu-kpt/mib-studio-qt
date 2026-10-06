// Motorized Z stage service (ADR 0013, #464): connection, status polling,
// operator-initiated Home (mid-travel referencing), soft limits, one-sided
// approach and the once-per-power-up reference. Qt-free.
//
// Threading: one worker thread owns the driver and runs connect/disconnect,
// operations and idle status polling, so the driver sees a single caller.
// stop() is the only call that reaches the driver from another thread; it is
// immediate and idempotent. Public calls never block on motion.
#pragma once

#include "backend/services/StageConfig.h"
#include "backend/stage/IMotionStage.h"
#include "backend/stage/LimitVerification.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace backend::services {

namespace serialbus {
class SerialBusManager;
}

// What survives an application restart: the reference holds only while the
// controller still carries the same power-up token (ADR 0013 §6).
struct StageReferenceRecord {
    std::string controllerSerial;
    std::uint16_t token{0};
    double spanUm{0.0};
};

class IStageReferenceStore {
public:
    virtual ~IStageReferenceStore() = default;
    virtual std::optional<StageReferenceRecord> load() = 0;
    virtual void save(const StageReferenceRecord& record) = 0;
    virtual void clear() = 0;
};

// JSON file under the data directory (written via a temporary + rename).
class FileStageReferenceStore final : public IStageReferenceStore {
public:
    explicit FileStageReferenceStore(std::string path);
    std::optional<StageReferenceRecord> load() override;
    void save(const StageReferenceRecord& record) override;
    void clear() override;

private:
    std::string path_;
};

// In-process store (tests, and services built without a data directory).
class MemoryStageReferenceStore final : public IStageReferenceStore {
public:
    std::optional<StageReferenceRecord> load() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return record_;
    }
    void save(const StageReferenceRecord& record) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record_ = record;
    }
    void clear() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record_.reset();
    }

private:
    std::mutex mutex_;
    std::optional<StageReferenceRecord> record_;
};

class StageService {
public:
    using DriverFactory = std::function<std::unique_ptr<stage::IMotionStage>()>;
    using OperationId = std::uint64_t;

    enum class OperationKind { Move, Reference };
    enum class OperationState { Queued, Running, Completed, Failed, Cancelled, TimedOut };

    struct OperationInfo {
        OperationId id{0};
        OperationKind kind{OperationKind::Move};
        OperationState state{OperationState::Queued};
        stage::StageError error{stage::StageError::None};
        std::string detail;
    };

    struct StartResult {
        OperationId id{0}; // 0 when refused
        stage::StageError error{stage::StageError::None};
        std::string detail;
        bool accepted() const { return id != 0; }
    };

    struct Snapshot {
        bool connected{false};
        bool configured{false};
        bool referenced{false};
        // A supervised limit-switch check (zc300ctl verify-limits) passed for
        // this controller; Home is refused without it.
        bool limitsVerified{false};
        stage::StageIdentity identity;
        std::string systemPort; // resolved port while connected (conflict checks)
        stage::StageStatus status;
        double spanUm{0.0};
        double softMinUm{0.0};
        double softMaxUm{0.0};
        OperationId activeOperation{0};
        std::string lastError;
    };

    // `limits` holds the supervised limit-switch records; without one for the
    // connected controller, Home is refused (null: nothing is verified).
    StageService(serialbus::SerialBusManager& busManager, std::unique_ptr<IStageReferenceStore> store,
                 std::shared_ptr<stage::LimitsVerificationStore> limits = nullptr);
    // Test seam: inject the driver (e.g. a ZC300 driver over a fake port).
    StageService(DriverFactory driverFactory, std::unique_ptr<IStageReferenceStore> store,
                 std::shared_ptr<stage::LimitsVerificationStore> limits = nullptr);
    ~StageService();

    StageService(const StageService&) = delete;
    StageService& operator=(const StageService&) = delete;

    // Refused (false) while connected; reconnect to apply a new endpoint.
    bool setConfig(const StageConfig& config);
    StageConfig config() const;

    // Observe-only: identifies the controller, checks the profile and the
    // power-up token. Writes nothing.
    stage::StageError connect(std::string* detail = nullptr);
    // Stops a running move or Home (it ends Cancelled), then releases the port.
    // Returns promptly: it never waits for the operation to run to its end.
    void disconnect();
    // Start-up policy: connects when enabled; references only when the rig
    // opted in with reference.on_startup (default false: zero motion, zero
    // writes). Returns the connect result.
    stage::StageError startup(std::string* detail = nullptr);

    Snapshot snapshot() const;

    // Operations run one at a time on the worker. Validation (connection,
    // configuration, reference, soft limits, whole micrometres) happens
    // before anything is queued, and again when the operation starts.
    StartResult moveTo(double targetUm);
    StartResult moveBy(double deltaUm);
    // Home: probe both limits, zero at mid-travel. Refused with
    // LimitsUnverified unless a supervised limit check passed for the
    // connected controller (re-read on every request).
    StartResult reference();

    // Cancels the active operation and stops the axis. Always allowed.
    stage::StageError stop();
    bool cancel(OperationId id);
    std::optional<OperationInfo> operation(OperationId id) const;
    // True when the operation reached a terminal state within `timeout`.
    bool waitForOperation(OperationId id, std::chrono::milliseconds timeout) const;

    // The only controller-configuration write: applies and saves the stage
    // profile. Refused at once (Busy) while an operation is active.
    stage::StageError applyProfile();

    // Cancels any operation, stops the axis, joins the worker, disconnects.
    // Idempotent; the destructor calls it.
    void shutdown();

private:
    struct Job {
        enum class Type { Connect, Disconnect, ApplyProfile, Operation } type{Type::Connect};
        OperationId operation{0};
        OperationKind kind{OperationKind::Move};
        double targetUm{0.0};
        bool absolute{true};
        std::shared_ptr<std::promise<std::pair<stage::StageError, std::string>>> reply;
    };

    std::pair<stage::StageError, std::string> runSync(Job::Type type);
    // Connect / ApplyProfile: refused with Busy while an operation is active
    // or another exclusive job is queued, instead of queuing behind a move.
    // The bridge runs one command at a time and Stop needs the same lock, so a
    // call that waits for a running move would hold Stop for its duration.
    std::pair<stage::StageError, std::string> runExclusive(Job::Type type);
    StartResult enqueueOperation(OperationKind kind, double targetUm, bool absolute);
    stage::StageError validateMoveLocked(double targetUm, bool absolute, std::string& detail) const;

    // Worker thread only.
    void workerLoop();
    std::pair<stage::StageError, std::string> doConnect();
    void doDisconnect();
    void runOperation(const Job& job);
    stage::StageError runMove(OperationId id, double targetUm, bool absolute, std::string& detail);
    stage::StageError runReference(OperationId id, std::string& detail);
    stage::StageError moveAndWait(OperationId id, double targetUm, double speedUmS, std::string& detail);
    stage::StageError approachAndWait(OperationId id, double targetUm, double minUm, double maxUm,
                                      std::string& detail);
    // Controller-bounded search: a relative move of at most expected_span +
    // search_margin toward the switch (never an open-ended jog).
    stage::StageError searchLimit(OperationId id, stage::Direction direction, double& positionUm,
                                  std::string& detail);
    stage::StageError waitIdle(OperationId id, std::chrono::steady_clock::time_point deadline,
                               stage::StageStatus& status, std::string& detail,
                               const std::function<bool(const stage::StageStatus&)>& abortIf = {});
    stage::StageError applySpeed(double umPerS);
    bool cancelled(OperationId id) const;
    void pollStatus();
    void publishStatus(const stage::StageStatus& status);
    void finishOperation(OperationId id, OperationState state, stage::StageError error, std::string detail);
    void invalidateReference(const char* why);
    std::shared_ptr<stage::IMotionStage> driver() const;

    bool limitsVerifiedFor(const std::string& serial) const;

    DriverFactory driverFactory_;
    std::unique_ptr<IStageReferenceStore> store_;
    std::shared_ptr<stage::LimitsVerificationStore> limits_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    StageConfig config_;
    std::shared_ptr<stage::IMotionStage> driver_; // replaced only by the worker
    Snapshot snapshot_;
    std::deque<Job> jobs_;
    std::map<OperationId, OperationInfo> operations_;
    OperationId nextOperation_{1};
    OperationId cancelRequested_{0};
    // Bumped after every stop() reached the driver. An operation whose start
    // predates a bump was stopped by someone, even if stop() raced with its
    // first command, so it ends Cancelled instead of "stopped short".
    std::atomic<std::uint64_t> stopEpoch_{0};
    std::uint64_t operationStopEpoch_{0}; // worker only
    double appliedSpeedUmS_{0.0}; // worker only
    bool stopping_{false};
    bool shutDown_{false};
    int exclusivePending_{0};   // queued or running Connect / ApplyProfile jobs
    // Disconnects queued or running. While any is pending, operations end and
    // no new operation, Connect or ApplyProfile is admitted. A counter, not a
    // flag: with two concurrent disconnect() calls the first to finish must not
    // reopen admission while the second is still queued.
    int pendingDisconnects_{0};
    std::thread worker_;
};

const char* toString(StageService::OperationState state);

} // namespace backend::services
