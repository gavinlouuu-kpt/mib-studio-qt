// V2-1 proof: the Qt-free Processing Contract module classifies config schemas,
// enforces contract-equality compatibility, resolves the canonical vs legacy
// difference-threshold key, and migrates a Contract-1 config document into a
// Contract-2 document without losing unrelated values or leaving ring science
// behind. See ADR 0001 and docs/architecture/processing-contract-compatibility.md.
#include "backend/processing/ProcessingContract.h"
#include "support/assert.h"

#include <nlohmann/json.hpp>

#include <string>

namespace contract = backend::processing::contract;
using nlohmann::json;

namespace {

// A representative Contract-1 profile config, mirroring the nested shape of
// resources/defaults/config.json (only the fields the migration touches plus a
// few unrelated ones that must survive verbatim).
json makeV1Config() {
    return json{
        {"config_schema_version", 1},
        {"buffer_threshold", 1000},
        {"image_processing",
         json{
             {"area_threshold_min", 60},
             {"area_threshold_max", 290},
             {"ring_ratio_min", 15.0},
             {"ring_ratio_max", 25.0},
             {"bg_subtract_threshold", 8},
             {"empty_frame_pixel_threshold", 100},
             {"filters",
              json{
                  {"enable_area_range_check", true},
                  {"enable_ring_ratio_check", true},
                  {"enable_border_check", true},
              }},
             {"target_group",
              json{
                  {"enabled", false},
                  {"area_min", 72},
              }},
         }},
        {"autofocus_focus_setpoint", 20.0},
        {"pixel_to_micron_factor", 0.4886},
    };
}

void testSchemaClassification() {
    using contract::SchemaCompatibility;
    MIB_EXPECT(contract::classifyConfigSchema(1, 1) == SchemaCompatibility::Same,
               "equal schema is Same");
    MIB_EXPECT(contract::classifyConfigSchema(2, 2) == SchemaCompatibility::Same,
               "equal v2 schema is Same");
    MIB_EXPECT(contract::classifyConfigSchema(1, 2) == SchemaCompatibility::UpgradeNeeded,
               "older source needs upgrade");
    MIB_EXPECT(contract::classifyConfigSchema(3, 2) == SchemaCompatibility::Incompatible,
               "newer-than-target fails closed");
    MIB_EXPECT(contract::classifyConfigSchema(0, 2) == SchemaCompatibility::Incompatible,
               "unknown/zero source fails closed");
    MIB_EXPECT(contract::classifyConfigSchema(-1, 2) == SchemaCompatibility::Incompatible,
               "negative source fails closed");
}

void testContractCompatibility() {
    MIB_EXPECT(contract::isProfileImplementationCompatible(1, 1), "1<->1 compatible");
    MIB_EXPECT(contract::isProfileImplementationCompatible(2, 2), "2<->2 compatible");
    MIB_EXPECT(!contract::isProfileImplementationCompatible(1, 2), "1<->2 refused");
    MIB_EXPECT(!contract::isProfileImplementationCompatible(2, 1), "2<->1 refused");
    MIB_EXPECT(!contract::isProfileImplementationCompatible(0, 1), "unknown contract refused");
}

void testResolveDifferenceThreshold() {
    // Legacy-only document.
    json legacy = json{{"bg_subtract_threshold", 12}};
    MIB_EXPECT(contract::resolveDifferenceThreshold(legacy, 8) == 12, "legacy key read");

    // Canonical-only document.
    json canonical = json{{"difference_threshold", 20}};
    MIB_EXPECT(contract::resolveDifferenceThreshold(canonical, 8) == 20, "canonical key read");

    // Both present: canonical wins.
    json both = json{{"difference_threshold", 33}, {"bg_subtract_threshold", 7}};
    MIB_EXPECT(contract::resolveDifferenceThreshold(both, 8) == 33, "canonical wins over legacy");

    // Neither present: fallback.
    json none = json::object();
    MIB_EXPECT(contract::resolveDifferenceThreshold(none, 8) == 8, "fallback when absent");

    // Non-object holder: fallback.
    json notObject = json::array({1, 2, 3});
    MIB_EXPECT(contract::resolveDifferenceThreshold(notObject, 8) == 8, "fallback for non-object");
}

void testMigrationHappyPath() {
    std::string error;
    const auto migrated = contract::migrateProfileConfigV1ToV2(makeV1Config(), &error);
    MIB_REQUIRE(migrated.has_value(), std::string("migration should succeed: ") + error);
    const json& v2 = *migrated;

    // Both version axes stamped.
    MIB_EXPECT(v2.at("config_schema_version") == 2, "config_schema_version -> 2");
    MIB_EXPECT(v2.at("processing_contract_version") == 2, "processing_contract_version -> 2");

    const json& ip = v2.at("image_processing");

    // Canonical difference threshold carried the legacy value; legacy key gone.
    MIB_EXPECT(ip.contains("difference_threshold"), "canonical difference_threshold present");
    MIB_EXPECT(ip.at("difference_threshold") == 8, "difference_threshold carried legacy value");
    MIB_EXPECT(!ip.contains("bg_subtract_threshold"), "legacy bg_subtract_threshold removed");

    // Ring science removed.
    MIB_EXPECT(!ip.contains("ring_ratio_min"), "ring_ratio_min removed");
    MIB_EXPECT(!ip.contains("ring_ratio_max"), "ring_ratio_max removed");
    MIB_EXPECT(!ip.at("filters").contains("enable_ring_ratio_check"),
               "enable_ring_ratio_check removed");

    // Identity preprocessing chain installed for both phases.
    MIB_REQUIRE(ip.contains("preprocessing"), "preprocessing block present");
    const json& pp = ip.at("preprocessing");
    MIB_EXPECT(pp.at("input").is_array() && pp.at("input").size() == 1, "one input stage");
    MIB_EXPECT(pp.at("input").at(0).at("stage") == "identity", "input stage is identity");
    MIB_EXPECT(pp.at("difference").at(0).at("stage") == "identity",
               "difference stage is identity");

    // Laplacian gate present but disabled.
    MIB_EXPECT(ip.contains("laplacian_variance_min"), "laplacian_variance_min added");
    MIB_EXPECT(ip.contains("laplacian_variance_max"), "laplacian_variance_max added");
    MIB_EXPECT(ip.at("filters").at("enable_laplacian_variance_check") == false,
               "laplacian gate disabled");

    // Unrelated values preserved verbatim.
    MIB_EXPECT(v2.at("buffer_threshold") == 1000, "buffer_threshold preserved");
    MIB_EXPECT(v2.at("autofocus_focus_setpoint") == 20.0, "autofocus setpoint preserved");
    MIB_EXPECT(v2.at("pixel_to_micron_factor") == 0.4886, "pixel factor preserved");
    MIB_EXPECT(ip.at("area_threshold_min") == 60, "area_threshold_min preserved");
    MIB_EXPECT(ip.at("filters").at("enable_area_range_check") == true,
               "unrelated filter flag preserved");
    MIB_EXPECT(ip.at("target_group").at("area_min") == 72, "nested target_group preserved");
}

void testMigrationDeterministicAndIdempotentOutputShape() {
    std::string errA;
    std::string errB;
    const auto a = contract::migrateProfileConfigV1ToV2(makeV1Config(), &errA);
    const auto b = contract::migrateProfileConfigV1ToV2(makeV1Config(), &errB);
    MIB_REQUIRE(a.has_value() && b.has_value(), "both migrations succeed");
    MIB_EXPECT(a->dump() == b->dump(), "migration is deterministic");
}

void testMigrationRejectsNonV1() {
    std::string error;

    // Already v2.
    json alreadyV2 = makeV1Config();
    alreadyV2["config_schema_version"] = 2;
    const auto r1 = contract::migrateProfileConfigV1ToV2(alreadyV2, &error);
    MIB_EXPECT(!r1.has_value(), "refuses a schema-2 document");

    // Not an object.
    const auto r2 = contract::migrateProfileConfigV1ToV2(json::array({1, 2}), &error);
    MIB_EXPECT(!r2.has_value(), "refuses a non-object root");
}

void testMigrationCanonicalThresholdWinsWhenBothPresent() {
    json cfg = makeV1Config();
    cfg["image_processing"]["difference_threshold"] = 42; // hand-authored canonical
    std::string error;
    const auto migrated = contract::migrateProfileConfigV1ToV2(cfg, &error);
    MIB_REQUIRE(migrated.has_value(), "migration succeeds");
    MIB_EXPECT(migrated->at("image_processing").at("difference_threshold") == 42,
               "canonical threshold preferred over legacy during migration");
    MIB_EXPECT(!migrated->at("image_processing").contains("bg_subtract_threshold"),
               "legacy threshold still removed");
}

} // namespace

int main() {
    testSchemaClassification();
    testContractCompatibility();
    testResolveDifferenceThreshold();
    testMigrationHappyPath();
    testMigrationDeterministicAndIdempotentOutputShape();
    testMigrationRejectsNonV1();
    testMigrationCanonicalThresholdWinsWhenBothPresent();
    return mib::test::exitCode();
}
