#pragma once
// What the client declares about a run when it starts it (#712 PR 2, ADR 0015 items 1, 4, 5 and 7,
// bridge ABI 39): the experiment / run / sample identity, the selected operator, the applied
// calibration record, and the planned sample annotation.
//
// - The identity is recorded in the run snapshot v3 (`run_snapshot_json`), next to the facts the
//   backend measures itself (RunProvenanceRecord.h).
// - The annotation is written once, at Start, into its own versioned block,
//   `/experiment_annotation` (@annotation_json, @annotation_schema_version). It is the sample as
//   planned; corrections live in the workspace store and never rewrite the file (ADR 0015 item 4).
//   It also carries the declared channel, medium and temperature: the plan treats them as
//   annotation, not as free-text snapshot fields (exec plan 2026-10-15).
//
// The request is checked strictly: unknown keys, duplicate keys, wrong types, empty strings,
// control characters, invalid UTF-8 and anything over a bound are refused with the key named,
// never truncated or dropped. Not declared is null (or a missing key) and reads back as
// std::nullopt. The operator is the one the client selected (#393 phase 1), not an authenticated
// identity, and is recorded as such ("assurance": "selected").
//
// The bounds are part of the bridge contract (crates/mib-bridge/contract/bridge-contract.json,
// `experiment_start_request`) and pinned by static_asserts in crates/mib-bridge/src/shim.cpp.
// Qt-free; part of mib_processing.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace backend::recording {

inline constexpr std::size_t kStartRequestMaxBytes = 16384;   // the whole startRequest document
inline constexpr std::size_t kStartIdMaxBytes = 128;          // sample_ref, operator id/name, calibration record id
inline constexpr std::size_t kAnnotationTextMaxBytes = 256;   // sample, condition, experimental_unit, channel, medium
inline constexpr std::size_t kAnnotationNotesMaxBytes = 2048; // notes (may hold \n, \r, \t)
inline constexpr std::size_t kAnnotationUnitMaxBytes = 32;    // concentration unit
inline constexpr std::size_t kAnnotationFreeMaxEntries = 32;  // free key/values
inline constexpr std::size_t kAnnotationFreeKeyMaxBytes = 64;
inline constexpr std::size_t kAnnotationFreeValueMaxBytes = 256;
inline constexpr int kAnnotationTemperatureMinC = -50;        // a declared temperature outside is refused
inline constexpr int kAnnotationTemperatureMaxC = 150;
inline constexpr uint32_t kExperimentAnnotationSchemaVersion = 1;

// The operator the client selected (#393 phase 1: a local selection, not authentication).
struct OperatorRecord {
    std::optional<std::string> id;    // the workspace store's operator id, when it has one
    std::optional<std::string> name;  // the name shown when it was selected
};

// Identity members of the run snapshot v3. Each is nullopt when the client did not declare it.
struct RunStartIdentity {
    std::optional<std::string> experimentId;          // UUID, lowercase canonical form
    std::optional<uint32_t> runIndex;                 // the run's index within the experiment
    std::optional<std::string> sampleRef;             // into the experiment's sample sheet
    std::optional<OperatorRecord> selectedOperator;   // id or name (at least one)
    std::optional<std::string> calibrationRecordId;   // the applied calibration record (ADR 0015 item 7)

    bool empty() const
    {
        return !experimentId && !runIndex && !sampleRef && !selectedOperator && !calibrationRecordId;
    }
};

struct ConcentrationRecord {
    double value{0.0};   // finite, >= 0
    std::string unit;    // e.g. "uM", "ug/mL"
};

// The planned sample annotation (ADR 0015 item 4 and the plan's sample sheet), written once into
// /experiment_annotation. Every fixed member is nullopt when not declared.
struct ExperimentAnnotation {
    std::optional<std::string> sample;            // the sample as named in the sample sheet
    std::optional<std::string> condition;         // the planned condition
    std::optional<std::string> experimentalUnit;  // e.g. the donor
    std::optional<ConcentrationRecord> concentration;
    std::optional<std::string> channel;           // declared channel
    std::optional<std::string> medium;            // declared medium
    std::optional<double> temperatureC;           // declared temperature, degrees Celsius
    std::optional<std::string> notes;
    // Free key/values, strings only, keys unique. Read back (and parsed) sorted by key: a JSON object
    // keeps no order with the nlohmann_json the wheel build uses.
    std::vector<std::pair<std::string, std::string>> free;
};

// The identity part of an experiment start request.
struct StartRequestIdentity {
    RunStartIdentity identity;
    std::optional<ExperimentAnnotation> annotation;   // nullopt: no annotation block is written
};

// Parse the bridge's startRequest document (empty text = nothing declared). Strict: false with
// `why` naming the key for anything the contract does not allow. On success `out` is valid.
bool parseStartRequestJson(const std::string& text, StartRequestIdentity& out, std::string* why);

// The same bounds for a request built in C++ (the coordinator checks every Start with it).
bool validateStartRequest(const StartRequestIdentity& request, std::string* why);

// The annotation document stored as /experiment_annotation @annotation_json.
std::string experimentAnnotationToJson(const ExperimentAnnotation& annotation);

// Read an annotation document back. Tolerant, as every file reader: a fixed member that is absent,
// null or of the wrong type is nullopt, a free value that is not a string is skipped. nullopt only
// when the text is not a JSON object.
std::optional<ExperimentAnnotation> parseExperimentAnnotation(const std::string& json, std::string* why = nullptr);

} // namespace backend::recording
