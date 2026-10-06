// Method provenance + the "method.revision" readiness gate (#398 M2).
//
// Which experiment method a run used, resolved from backend state only: the
// canonical hash of the applied config.json is matched against the cached
// central revisions in the registry worker's snapshot (value copy; no network,
// no SQLite on the caller's thread). A config that matches no cached revision
// is a local method. Matching is by config.json alone; the camera script is
// not compared (recorded as `match` = "config_json").
//
// Gate policy (operator decisions on #398):
//   local / no config applied          -> NotRequired
//   central, revoked                   -> Fail (review of existing runs unaffected)
//   central, not published/superseded  -> Fail
//   central, failed local validation   -> Warn
//   central, not validated here        -> Warn (Start allowed)
//   central, validated here            -> Pass
// "Here" means this instrument UUID and this method context (processing core
// build + camera source); see profiles::MethodContext.
#pragma once

#include "backend/app/ExperimentReadiness.h"

#include <cstdint>
#include <string>

namespace backend::profiles {
struct RegistryWorkerSnapshot;
struct MethodContext;
} // namespace backend::profiles

namespace backend::app {

// Resolve the run's method. `appliedConfigCanonicalSha256` is
// profiles::canonicalConfigSha256 of the applied config.json (empty: none
// applied or unparsable).
MethodProvenance resolveMethodProvenance(const std::string& appliedConfigCanonicalSha256,
                                         const profiles::RegistryWorkerSnapshot& registry,
                                         const profiles::MethodContext& context,
                                         const std::string& instrumentName);

ReadinessGate methodRevisionGate(const MethodProvenance& method);

// Stable comparable summary of everything the gate depends on (readiness
// invalidation input).
std::string methodInvalidationKey(const MethodProvenance& method);

// The JSON object written as "method" in run_snapshot_json.
std::string methodProvenanceToJson(const MethodProvenance& method);

} // namespace backend::app
