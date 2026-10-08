#include "backend/processing/ProcessingContract.h"

#include <string>

#include <nlohmann/json.hpp>

namespace backend::processing::contract {

using nlohmann::json;

SchemaCompatibility classifyConfigSchema(int sourceSchema, int targetSchema) {
    if (sourceSchema <= 0 || targetSchema <= 0) {
        return SchemaCompatibility::Incompatible;
    }
    if (sourceSchema == targetSchema) {
        return SchemaCompatibility::Same;
    }
    if (sourceSchema < targetSchema) {
        return SchemaCompatibility::UpgradeNeeded;
    }
    // Source declares a schema newer than this build understands: fail closed.
    return SchemaCompatibility::Incompatible;
}

bool isProfileImplementationCompatible(int profileContract, int implContract) {
    if (profileContract <= 0 || implContract <= 0) {
        return false;
    }
    // Contract match is equality, not ordering: the science differs between
    // versions, so a Contract-2 profile is not "acceptable" to a Contract-1
    // implementation, nor the reverse.
    return profileContract == implContract;
}

int resolveDifferenceThreshold(const json& keyHolder, int fallback) {
    if (!keyHolder.is_object()) {
        return fallback;
    }
    const auto readInt = [&keyHolder](const char* key, int& out) -> bool {
        const auto it = keyHolder.find(key);
        if (it == keyHolder.end()) {
            return false;
        }
        if (it->is_number_integer() || it->is_number_unsigned()) {
            out = it->get<int>();
            return true;
        }
        if (it->is_number_float()) {
            out = static_cast<int>(it->get<double>());
            return true;
        }
        return false;
    };

    int value = fallback;
    if (readInt(kDifferenceThresholdKey, value)) { // canonical wins
        return value;
    }
    if (readInt(kLegacyBgSubtractThresholdKey, value)) {
        return value;
    }
    return fallback;
}

std::optional<json> migrateProfileConfigV1ToV2(const json& v1Config, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<json> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    if (!v1Config.is_object()) {
        return fail("Config root must be a JSON object.");
    }

    int sourceSchema = kConfigSchemaVersionV1;
    if (const auto it = v1Config.find("config_schema_version");
        it != v1Config.end() && it->is_number()) {
        sourceSchema = it->get<int>();
    }
    if (sourceSchema != kConfigSchemaVersionV1) {
        return fail("migrateProfileConfigV1ToV2 expects a config_schema_version=1 document, got " +
                    std::to_string(sourceSchema) + ".");
    }

    json out = v1Config; // Start from a full copy so unrelated values are preserved.

    // Stamp both version axes.
    out["config_schema_version"] = kConfigSchemaVersionV2;
    out["processing_contract_version"] = kProcessingContractVersionV2;

    json& imageProcessing = out["image_processing"];
    if (!imageProcessing.is_object()) {
        imageProcessing = json::object();
    }

    // Canonical difference threshold: read from the legacy (or already-canonical)
    // key, then keep only the canonical key.
    const int differenceThreshold = resolveDifferenceThreshold(imageProcessing, /*fallback=*/8);
    imageProcessing.erase(kLegacyBgSubtractThresholdKey);
    imageProcessing[kDifferenceThresholdKey] = differenceThreshold;

    // Ring width / ring ratio is removed from the v2 science contract.
    imageProcessing.erase("ring_ratio_min");
    imageProcessing.erase("ring_ratio_max");

    // Install a no-op (identity) preprocessing chain. V2-2 adds real stages;
    // the identity chain is deliberately equivalent to the omitted baseline.
    imageProcessing["preprocessing"] = json{
        {"input", json::array({json{{"stage", "identity"}}})},
        {"difference", json::array({json{{"stage", "identity"}}})},
    };

    // Laplacian-variance gate: present but disabled and uncalibrated. Thresholds
    // are placeholders until V2-7 calibration; the gate stays off in the
    // meantime, so the placeholder values are never consulted.
    imageProcessing["laplacian_variance_min"] = 0.0;
    imageProcessing["laplacian_variance_max"] = 0.0;

    json& filters = imageProcessing["filters"];
    if (!filters.is_object()) {
        filters = json::object();
    }
    filters.erase("enable_ring_ratio_check");
    filters["enable_laplacian_variance_check"] = false;

    return out;
}

} // namespace backend::processing::contract
