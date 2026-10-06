// Supervised limit-switch verification (#464, ADR 0013 §6).
//
// Home trusts the limit switches: it searches for each one and zeroes the
// stage between them. On a stage whose switches are unwired, swapped or
// broken, that search ends at the mechanical hard stop. So Home is refused
// until this procedure has passed for the controller, with someone watching
// the stage. Only the bench tool runs it (`zc300ctl verify-limits
// --supervised`); no application, bridge or server path can write the record.
#pragma once

#include "backend/stage/IMotionStage.h"

#include <functional>
#include <optional>
#include <string>

namespace backend::stage {

// What the procedure proved for one controller.
struct LimitsVerifiedRecord {
    std::string controllerSerial;
    std::string verifiedAtUtc; // ISO 8601
    double negativeUm{0.0};    // switch positions in the controller's frame at the time
    double positiveUm{0.0};
    double spanUm{0.0};
    std::string procedure;     // e.g. "zc300ctl verify-limits"
};

// JSON file {"controllers": {"<serial>": {...}}}, written via temp + rename;
// saving one controller keeps the others.
class LimitsVerificationStore {
public:
    explicit LimitsVerificationStore(std::string path);
    std::optional<LimitsVerifiedRecord> find(const std::string& controllerSerial) const;
    bool save(const LimitsVerifiedRecord& record);
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// Operator-paced: before each direction the procedure states the position
// and the planned travel and needs `confirm` to return true; it then moves in
// controller-bounded steps of at most `stepUm`, reporting the live position,
// and stops immediately when `cancelled` turns true (Enter or Ctrl-C at the
// bench). Total travel toward each switch is capped at `maxTravelUm`.
struct LimitVerificationOptions {
    double searchSpeedUmS{200.0}; // deliberately slower than Home's search
    double accelUmS2{1000.0};
    double stepUm{500.0};         // per controller-bounded step
    double maxTravelUm{6500.0};   // per direction: expected span + margin
    double expectedSpanUm{6000.0};
    double spanToleranceUm{300.0};
    int pollMs{20};
    std::function<bool(const std::string& prompt)> confirm; // required; false aborts
    std::function<bool()> cancelled;                         // polled continuously
    std::function<void(const std::string&)> report;          // text for the operator
};

struct LimitVerificationResult {
    StageError error{StageError::None};
    std::string detail;
    bool cancelled{false};
    bool returnedToStart{false};
    double negativeUm{0.0};
    double positiveUm{0.0};
    double spanUm{0.0};
    bool passed() const { return error == StageError::None && !cancelled; }
};

// 1. confirmed, stepped, bounded slow moves toward −: the − switch must trip
//    and the + switch must not;
// 2. the same toward +;
// 3. the span must match within tolerance;
// 4. return to the start position (the operator's focus).
// Any failure or cancel stops the axis. The caller records a passing result.
LimitVerificationResult verifyLimits(IMotionStage& stage, const LimitVerificationOptions& options);

} // namespace backend::stage
