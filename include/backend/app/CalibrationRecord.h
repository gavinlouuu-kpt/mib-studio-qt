// The calibration run (#716 A4a, ADR 0015 item 7, bridge ABI 42): the method's purpose and
// calibration policy, the derivation of a calibration record from a closed run of a calibration
// method, the record itself, and the `calibration.record` readiness gate. Qt-free.
//
// - The method document (config.json, the profile registry's method, #398) may declare
//   `purpose` ("measurement" | "calibration"), `calibration_policy` ("none" | "warn" | "required"),
//   `calibration_max_age_hours` (default 24) and, for a calibration method, its reference
//   `calibration_reference` {nominal_diameter_um, diameter_tolerance_um, nominal_e_kpa?,
//   max_cv_percent? (default 10)}. Absent: a measurement method with policy none (legacy methods
//   are unchanged). A malformed declaration refuses the whole config document.
// - The derivation reads the run's own file (the normal core's valid objects; no separate
//   acquisition path). Nothing is fabricated: focus is "not recorded" unless the file records a
//   focus metric, E is compared only where both the file and the method have one, and the event
//   rate needs a timestamp clock the file declares. A record that fails a minimum-evidence gate is
//   `insufficient` and can never be applied.
// - Records are instrument-scoped and versioned: CalibrationStore keeps them append-only (a status
//   change is a new version). Only pixel-to-micron is ever applied, explicitly, by
//   `calibration_apply`; the deformation offset and E against its nominal are reported only.
#pragma once

#include "backend/app/ExperimentReadiness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace backend::app {

// Contract-pinned (bridge-contract.json calibration_records; static_asserts in shim.cpp).
inline constexpr std::size_t kCalibrationRecordIdMaxBytes = 128;
inline constexpr std::size_t kCalibrationMinValidObjects = 200;
inline constexpr double kCalibrationDefaultMaxCvPercent = 10.0;
inline constexpr double kCalibrationDefaultMaxAgeHours = 24.0;
inline constexpr double kCalibrationRateWindowSeconds = 1.0;
inline constexpr std::size_t kCalibrationRateMinWindows = 3;
inline constexpr std::size_t kCalibrationStoreMaxVersions = 2048;
inline constexpr std::uint32_t kCalibrationRecordSchemaVersion = 1;
// A factor is "the same" as another within this relative tolerance (compare-and-set and the
// in-effect check): a JSON round trip of a double never moves it further.
inline constexpr double kCalibrationFactorRelativeTolerance = 1e-9;
inline constexpr const char* kCalibrationNotTracked = "not tracked";
inline constexpr const char* kCalibrationNotRecorded = "not recorded";

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
};

// Parse the calibration members of a config.json document. Empty text or absent members: the
// legacy defaults (measurement, none, 24 h). False with `why` naming the key for anything
// malformed (wrong type, unknown value, out of range, a calibration method without its
// reference, or a reference on a measurement method).
bool parseCalibrationMethodSpec(const std::string& configJson, CalibrationMethodSpec& out,
                                std::string* why = nullptr);

// Contract-pinned (bridge-contract.json calibration_record_statuses); append only.
enum class CalibrationStatus { Proposed = 0, Applied = 1, Superseded = 2, Invalidated = 3, Insufficient = 4 };
const char* toString(CalibrationStatus status);
std::optional<CalibrationStatus> calibrationStatusFromString(const std::string& text);

// The identity a record was derived under. Chip and objective are "not tracked" until the setup
// declares them. Unknown (empty camera or ROI) never invalidates anything and never matches.
struct CalibrationIdentity {
    std::string camera;
    std::string roi;
    std::string chip{kCalibrationNotTracked};
    std::string objective{kCalibrationNotTracked};
    bool known() const { return !camera.empty() && !roi.empty(); }
    bool operator==(const CalibrationIdentity& o) const
    {
        return camera == o.camera && roi == o.roi && chip == o.chip && objective == o.objective;
    }
    bool operator!=(const CalibrationIdentity& o) const { return !(*this == o); }
};
// "camera mock:frames -> egrabber:Coaxlink 0; ROI ..." (only the members that differ).
std::string describeIdentityChange(const CalibrationIdentity& from, const CalibrationIdentity& to);

// The identity from the readiness candidate: camera = effective source (+ ":" label), unknown when
// no source is known; ROI "x,y,w,h" or "full", unknown while an applied ROI is pending.
CalibrationIdentity calibrationIdentityOf(const RunConfigurationSnapshot& candidate);

// One valid object of the run's file, as the processing core recorded it.
struct CalibrationSample {
    uint64_t frameIndex{0};
    uint64_t timestamp{0};   // the row's raw timestamp (ticks of the file's declared clock)
    double areaPx{0.0};      // object area in px^2
    double deformation{0.0};
    double laplacianVariance{std::numeric_limits<double>::quiet_NaN()};
    double youngsModulusKpa{std::numeric_limits<double>::quiet_NaN()};
};

// What the run's file gives the derivation.
struct CalibrationEvidence {
    std::vector<CalibrationSample> objects; // valid objects only
    bool focusFieldRecorded{false};         // the file has a laplacianVariance column
    bool eFieldRecorded{false};             // the file has a youngsModulus column
    uint64_t ticksPerSecond{0};             // the row timestamp clock; 0 = not declared (no event rate)
};

// Reads the valid objects (metadata only, no images) and the declarations above. False with `why`
// when the file cannot be opened or its valid-object metadata cannot be read.
bool readCalibrationEvidence(const std::string& path, CalibrationEvidence& out, std::string* why = nullptr);

struct CalibrationEvidenceGate {
    std::string id;        // "min_valid_objects" | "max_cv_percent" | "run_finalized" | "file_readable"
    bool pass{false};
    double value{0.0};
    double limit{0.0};
    std::string reason;    // empty on pass
};

// Measurements of one run. Optional members are absent when not computed; *Status says why.
struct CalibrationResults {
    uint64_t validObjects{0};      // objects with a finite, positive area
    uint64_t excludedObjects{0};   // valid rows with an unusable area (not finite or <= 0)
    std::optional<double> medianDiameterPx;  // equivalent diameter 2*sqrt(area/pi)
    std::optional<double> madDiameterPx;     // median absolute deviation, unscaled
    std::optional<double> cvDiameterPercent; // sample standard deviation / mean * 100
    std::optional<double> proposedPixelToMicron; // nominal um / median px
    std::optional<double> runPixelToMicron;      // the factor the run used (snapshot)
    std::optional<double> measuredDiameterUmAtRunFactor;
    std::optional<bool> runFactorWithinTolerance; // |measured - nominal| <= tolerance
    std::optional<double> deformationMedian;      // report only, never subtracted
    std::string eventRateStatus{kCalibrationNotRecorded}; // "computed" | "not recorded" | "too short" | "not monotonic"
    uint64_t eventRateWindows{0};
    std::optional<double> eventRateMeanPerS;
    std::optional<double> eventRateCvPercent;
    std::string focusStatus{kCalibrationNotRecorded}; // "recorded" | "not recorded"
    std::optional<double> focusLaplacianMedian;
    uint64_t focusObjects{0};
    std::string eStatus{"not declared"}; // "compared" | "not declared" | "not recorded"
    std::optional<double> eMedianKpa;
    std::optional<double> eNominalKpa;
    std::optional<double> eRatio;        // median / nominal, report only
    uint64_t eObjects{0};
};

struct CalibrationDerivation {
    CalibrationResults results;
    std::vector<CalibrationEvidenceGate> gates;
    bool sufficient() const
    {
        for (const auto& g : gates) if (!g.pass) return false;
        return !gates.empty();
    }
};

// The pure derivation (no I/O). `runPixelToMicron` <= 0 means the run's factor is unknown.
CalibrationDerivation deriveCalibration(const CalibrationEvidence& evidence, const CalibrationReference& reference,
                                        double runPixelToMicron);

// One version of a record (the store keeps every version; the newest version of an id is its
// current state).
struct CalibrationRecord {
    uint32_t schemaVersion{kCalibrationRecordSchemaVersion};
    std::string recordId;           // <= kCalibrationRecordIdMaxBytes
    uint32_t version{1};            // 1 = derived; +1 per status change
    uint64_t sequence{0};           // the store's append sequence (assigned on append)
    CalibrationStatus status{CalibrationStatus::Proposed};
    std::string statusReason;
    uint64_t statusAtUnixMs{0};
    uint64_t createdAtUnixMs{0};
    // The source run.
    std::string runId;
    std::string file;
    std::string fileSha256;         // empty when it could not be hashed
    uint64_t startGeneration{0};
    std::string backgroundSha256;   // the background the run processed with (empty: none)
    // The method and revision the run used (MethodProvenance at Start).
    std::string methodSource;       // "central" | "local" | "none"
    std::string methodId;
    std::string revisionId;
    uint64_t revisionNumber{0};
    std::string methodDisplayName;
    std::string configSha256;
    // The inputs.
    CalibrationReference reference;
    uint64_t minValidObjects{kCalibrationMinValidObjects};
    double rateWindowSeconds{kCalibrationRateWindowSeconds};
    CalibrationIdentity identity;
    CalibrationDerivation derivation;
    // Apply (present on the version that applied it).
    std::optional<double> appliedPreviousFactor;
    std::optional<double> appliedNewFactor;
    std::string supersededBy;       // on a superseded version: the record that replaced it

    // The record's own evidence passes the minimum-evidence gates, re-checked from its results
    // (never trusted from the stored status alone): every gate passed, at least
    // kCalibrationMinValidObjects valid objects, a diameter CV within the method's limit and a
    // finite positive proposal. A stored record that fails it is never applied or in effect.
    bool evidenceSufficient() const;
    bool applicable() const { return status == CalibrationStatus::Proposed && evidenceSufficient(); }
    std::optional<double> proposedFactor() const { return derivation.results.proposedPixelToMicron; }
};

std::string calibrationRecordToJson(const CalibrationRecord& record);
// Strict enough for the store: false with `why` when a required member is missing or of the
// wrong type, or the status is unknown.
bool calibrationRecordFromJson(const std::string& json, CalibrationRecord& out, std::string* why = nullptr);

// "cal-20261021T093000Z-1a2b3c4d" from a wall-clock time and a random suffix.
std::string newCalibrationRecordId(uint64_t unixMs);
std::string utcIsoFromUnixMs(uint64_t unixMs);

// True when two factors are the same within kCalibrationFactorRelativeTolerance.
bool sameCalibrationFactor(double a, double b);

// --- the readiness gate `calibration.record` ------------------------------------------------
struct CalibrationGateInput {
    CalibrationMethodSpec spec;          // of the method currently applied
    std::string specError;               // the applied config's calibration members are malformed
    std::optional<CalibrationRecord> applied; // the store's applied record (after invalidation)
    std::vector<CalibrationRecord> proposed;  // proposed records (newest first)
    CalibrationIdentity current;
    double currentFactor{0.0};
    uint64_t nowUnixMs{0};
    std::string storeError;              // the store is unreadable
    std::string invalidatedNote;         // what this evaluation invalidated, if anything
};

struct CalibrationGateResult {
    ReadinessGate gate;
    // The applied record in effect (identity matches and its factor is the current factor): the
    // id the run snapshot records and the one a declared id must equal. nullopt otherwise.
    std::optional<std::string> inEffectRecordId;
    // Everything the gate depends on, for the readiness invalidation key.
    std::string invalidationKey;
};

CalibrationGateResult evaluateCalibrationGate(const CalibrationGateInput& input);

} // namespace backend::app
