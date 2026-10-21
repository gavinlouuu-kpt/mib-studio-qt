// The applied method's calibration declarations (#716 A4a, ADR 0015 item 7): its purpose,
// calibration policy, maximum record age and, for a calibration method, its reference sample.
// Split from CalibrationRecord.h (#776, bridge ABI 43) so the readiness report
// (ExperimentReadiness.h, which CalibrationRecord.h includes) can carry them. Qt-free.
#pragma once

#include <optional>

namespace backend::app {

// Contract-pinned (bridge-contract.json calibration_records; static_asserts in shim.cpp).
inline constexpr double kCalibrationDefaultMaxCvPercent = 10.0;
inline constexpr double kCalibrationDefaultMaxAgeHours = 24.0;

enum class MethodPurpose { Measurement, Calibration };
enum class CalibrationPolicy { None, Warn, Required };
const char* toString(MethodPurpose purpose);
const char* toString(CalibrationPolicy policy);

// The reference sample a calibration method declares (rigid size beads, ADR 0015 item 7).
struct CalibrationReference {
    double nominalDiameterUm{0.0};          // > 0
    double diameterToleranceUm{0.0};        // >= 0
    std::optional<double> nominalEKpa;      // check-only, > 0 when declared
    double maxCvPercent{kCalibrationDefaultMaxCvPercent}; // the diameter CV limit, (0, 100]
};

struct CalibrationMethodSpec {
    MethodPurpose purpose{MethodPurpose::Measurement};
    CalibrationPolicy policy{CalibrationPolicy::None};
    double maxAgeHours{kCalibrationDefaultMaxAgeHours};
    std::optional<CalibrationReference> reference; // set exactly when purpose is Calibration
    // The method declares at least one calibration member (#776): false for legacy methods and
    // when no method is applied, whose values above are then the defaults (measurement, none).
    bool declared{false};
};

} // namespace backend::app
