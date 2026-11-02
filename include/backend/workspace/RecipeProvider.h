// Native recipe authority (#807 I02, fold-in slice I-1). A recipe is an immutable pair of an
// acquisition method revision and an F analysis-plan revision, with an optional parent recipe
// (base-on-previous lineage), an optional source finding, a creation time and a note. Recipes
// persist through the app metadata host (app.sqlite3, WorkspaceStore) in the reserved
// "recipe/<id>" namespace: create-only, so every recipe has exactly one revision ("1") and no
// public workspace write can touch it. Deriving writes a NEW recipe; the parent's references,
// lineage and stored bytes never change.
//
// The compatibility diff (method revision, processing profile, processing core/contract and
// calibration) is computed here, never in a view. Each item is "equal", "different" or
// "unknown", always with a reason; "unknown" only when the fact cannot be known from the method
// authority, the calibration store or the recipe itself (never as a default).
//
// Apply is a preview only: it validates and returns the exact changes and the method-selection
// request that the existing method authority (the profile registry's registry_apply_method or the
// local profile store's profile_command apply) must accept explicitly. Nothing here applies a
// method, changes calibration, starts acquisition or actuates hardware.
//
// Qt-free. The provider holds no state: every fact comes through RecipeHost, so tests drive it
// with a real SqliteService and stub authorities.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace backend::workspace {

// What the method authority says about one pinned method reference {source, id, revision}.
struct RecipeMethodFacts {
    bool resolved{false};
    std::string reason;          // why it is not resolved, or what is missing
    std::string provider;        // "registry-cache" | "local-profile"
    std::string contentSha256;   // registry content hash, or the local profile revision
    std::string configSha256;    // canonicalConfigSha256 of its config.json; empty: not available
    // The calibration policy its config.json declares ("none" | "warn" | "required", the legacy
    // default "none" when undeclared); nullopt with calibrationReason when not readable.
    std::optional<std::string> calibrationPolicy;
    std::string calibrationReason;
    std::optional<std::string> coreVersion; // declared processing_core_id
    std::optional<int> contractVersion;     // declared processing_contract_version
    std::string coreReason;      // why no core / contract is declared or readable
    std::string localName;       // local profile directory name (for the apply request)
};

// The instrument's current state, as the experiment coordinator, processing service and
// calibration store report it (read-only).
struct RecipeInstrumentState {
    std::string generation;              // readiness generation the facts belong to
    std::string methodSource{"none"};    // "central" | "local" | "none"
    std::string methodId;                // central method id or local profile id
    std::string methodRevision;          // central revision id or local profile revision
    bool methodNamed{false};             // the applied config names a method revision
    bool methodModified{false};          // the local profile's config was changed after apply
    std::string configSha256;            // canonical hash of the applied config; empty: none
    std::string coreVersion;
    int contractVersion{0};
    std::string coreSha256;
    bool calibrationKnown{true};
    std::string calibrationReason;       // why calibration is not knowable
    std::optional<std::string> calibrationRecordId; // record in effect; nullopt: none in effect
    bool lutLoaded{false};
    std::string lutSha256;
};

struct RecipeHost {
    // The metadata host's recipe authority (SqliteService::recipeStoreRequest).
    std::function<nlohmann::json(const nlohmann::json& request, bool write)> store;
    // The method authority (registry cache + local profile store), keyed by the pinned reference.
    std::function<RecipeMethodFacts(const nlohmann::json& methodRef, const std::string& profileBase)>
        method;
    // F's analysis authority: the reply of analysis read {planId, revision}.
    std::function<nlohmann::json(const std::string& planId, const std::string& revision)> plan;
    // F's analysis authority on a finding: the reply of analysis readFinding {findingId, revision}
    // ({status ok, findingId, revision, document} or a refusal). Unset: findings are unavailable.
    std::function<nlohmann::json(const std::string& findingId, const std::string& revision)> finding;
    std::function<RecipeInstrumentState()> instrument;
    std::function<std::string()> now; // UTC ISO-8601
};

class RecipeProvider {
public:
    explicit RecipeProvider(RecipeHost host);

    // Bridge entry (recipe_list, recipe_get, recipe_create, recipe_derive, recipe_diff,
    // recipe_apply_preview). `write` is the control-only endpoint; reads refuse mutations.
    // Never throws: replies {"status": "ok" | "invalid" | "not_found" | "conflict" |
    // "unavailable" | "storage", ...}.
    std::string request(const std::string& requestJson, bool write) const;

    // In-process callers (NextRunPlanning): same actions, plus `parent_document` on
    // recipe_derive, the inline recipe of an exact saved workspace revision, adopted into the
    // provider (create-only, verified against any stored copy) when it is not stored yet.
    nlohmann::json handle(const nlohmann::json& request, bool write, bool internal) const;

private:
    RecipeHost host_;
};

} // namespace backend::workspace
