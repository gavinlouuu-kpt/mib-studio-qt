#pragma once

// Processing Contract versioning, schema-compatibility, and v1->v2 migration.
//
// Qt-free by design: this lives in `mib_processing` so the backend-only CTest
// lane exercises it. The Qt `ProfileManager` calls into it for schema-aware
// profile loading and copy-upgrade. See ADR 0001 and
// docs/architecture/processing-contract-compatibility.md.

#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace backend::processing::contract {

// Two independent version axes (see the compatibility doc). Both are bumped to
// 2 for Contract v2; they are matched by equality, never ordering.
inline constexpr int kProcessingContractVersionV1 = 1;
inline constexpr int kProcessingContractVersionV2 = 2;
inline constexpr int kConfigSchemaVersionV1 = 1;
inline constexpr int kConfigSchemaVersionV2 = 2;

// The schema version this build treats as current.
inline constexpr int kCurrentConfigSchemaVersion = kConfigSchemaVersionV2;

// Canonical vs legacy config keys for the background-difference threshold.
inline constexpr char kDifferenceThresholdKey[] = "difference_threshold";        // v2 canonical
inline constexpr char kLegacyBgSubtractThresholdKey[] = "bg_subtract_threshold"; // v1 legacy

// Runtime contract selection helpers (ProcessingConfig::processing_contract_version).
inline constexpr bool isSupportedProcessingContract(int contract) noexcept {
    return contract == kProcessingContractVersionV1 || contract == kProcessingContractVersionV2;
}
// Contract 2 compares against the background with cv::absdiff; Contract 1 keeps
// saturating cv::subtract. Every difference site must route through this.
inline constexpr bool contractUsesAbsoluteDifference(int contract) noexcept {
    return contract >= kProcessingContractVersionV2;
}
// Ring width is a Contract-1 metric only: under Contract 2 it is NaN and its
// gate is ignored.
inline constexpr bool contractHasRingWidth(int contract) noexcept {
    return contract < kProcessingContractVersionV2;
}

enum class SchemaCompatibility {
    Same,          // merge missing defaults in memory; do not rewrite the file
    UpgradeNeeded, // older source: preserve untouched, offer explicit copy-upgrade
    Incompatible,  // newer/unknown: fail closed with a diagnostic
};

// Classify a source document's schema against a target schema (the schema this
// build understands). Non-positive or newer-than-target source -> Incompatible.
SchemaCompatibility classifyConfigSchema(int sourceSchema, int targetSchema);

// A profile of `profileContract` executes only against an implementation
// (core / bundled kernel / wheel) of the SAME contract. Unknown (<=0) versions
// are never compatible.
bool isProfileImplementationCompatible(int profileContract, int implContract);

// Read the difference threshold from the object that directly holds the keys
// (e.g. the `image_processing` block, or the flat Python-contract dict),
// preferring the canonical v2 key and falling back to the legacy key. Returns
// `fallback` when neither key is present or numeric.
int resolveDifferenceThreshold(const nlohmann::json& keyHolder, int fallback);

// Produce a Contract-2 config document from a Contract-1 config document.
// Returns nullopt and sets *error on failure. Preserves unrelated values,
// removes ring thresholds and their enable flag, renames the difference
// threshold to its canonical key, installs an identity preprocessing chain,
// leaves the Laplacian-variance gate disabled, and never selects/activates a
// core. `error` may be null.
std::optional<nlohmann::json> migrateProfileConfigV1ToV2(const nlohmann::json& v1Config,
                                                         std::string* error);

} // namespace backend::processing::contract
