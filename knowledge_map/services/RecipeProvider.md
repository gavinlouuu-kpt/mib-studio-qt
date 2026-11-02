# RecipeProvider

Native recipe authority for #807 goal **I02** (fold-in slice I-1). A recipe is an
immutable pair of an acquisition method revision and an F analysis-plan revision,
with optional parent-recipe and finding lineage. Bridge ABI 50 (provisional:
47 F, 48 I workspace, 49 E feature control).

**Source:** `include/backend/workspace/RecipeProvider.h`,
`src/backend/workspace/RecipeProvider.cpp`; facade host in
`BackendFacade::recipeRequestJson` (`src/backend/app/BackendFacade.cpp`).

## Responsibility

- **Storage.** Through [[WorkspaceStore]] in `app.sqlite3`, namespace
  `recipe/<id>`, via `SqliteService::recipeStoreRequest` (read, catalog and
  create-only commit only). Every recipe has exactly revision `1`. The public
  `workspace_write` endpoint refuses the namespace, so no client can revise,
  restore or import over a recipe. The document carries `origin {command_id,
  request_sha256}`: the same command and request replay the stored recipe; any
  other request for an existing id is a conflict.
- **Create / derive.** The method must resolve through the method authority
  (registry cache or local profile store) and the plan revision through F's
  analysis authority; a plan that binds a method must bind this one. Derive
  ("base on previous") writes a **new** recipe with `lineage.recipe` = parent;
  the parent's bytes never change. Each recipe records the calibration `basis`
  (record in effect, E-LUT sha256) it was created under. A `finding` must be
  known to F's analysis authority (`RecipeHost::finding`, analysis
  `readFinding`); an unknown one refuses the derive with `not_found`. The child
  (and an adopted parent) are validated before anything is written: a refused
  derive writes nothing, the adopted parent included (#825 review).
- **Diff** (`recipe_diff`, against `"instrument"` or another recipe): four items,
  each `equal | different | unknown` with a reason.
  - *method*: pinned identity vs the coordinator's applied `MethodProvenance`
    (central revision or local profile id+revision; a modified local config is
    different; an unnamed applied config is unknown), then the method content:
    the same revision with another `content_sha256` (two recipes' recorded
    hashes, or a recipe's recorded hash vs the method authority's current one
    for the instrument) is different, "method content changed for this
    revision"; a missing hash is unknown. `left`/`right` carry `content_sha256`.
  - *profile*: canonical config.json hash (registry summary `configSha256`; local
    profile `canonicalConfigSha256(config.json)`) vs the applied config hash.
  - *core*: the method's declared `processing_core_id` / `processing_contract_version`
    (registry envelope from the materialized `method.canonical.json`; local
    `profile.meta.json` contract) vs the active processing core.
  - *calibration*: the recipe's basis (record id, E-LUT) vs the record in effect
    and the loaded E-LUT, plus the method's calibration policy
    (`parseCalibrationMethodSpec`).
  Unknown is only returned with the missing fact named (unresolved method, not
  materialized, no declared contract, unreadable calibration store, adopted
  recipe without a recorded basis).
- **Apply preview** (`recipe_apply_preview`): the diff, the exact changes, and the
  method-selection request (`registry_apply_method` or `profile_command` apply
  with the profile's baseline) that the method authority must accept explicitly.
  Core and calibration are listed under `not_changed_by_apply`. Always
  `applied: false`, `hardware_actuated: false`.
- **Findings.** `NextRunPlanning` `draft_from_finding` derives the next-run recipe
  here (adopting the workspace's inline recipe once, with an unknown basis) and
  refuses without the provider.
- **Adoption is checked** (#825 review). Workspace documents are user-written
  (`workspace_write`), so an adopted inline recipe becomes provider truth only
  after: its pair validates as `recipe_create`; `lineage.recipe` is kept only
  when it is already a provider recipe (a self-parent or the id being derived
  refuses `invalid`; a cycle cannot form, because a parent must exist first);
  `lineage.finding` is kept only when the analysis authority knows it. Each
  dropped reference is recorded in `adoption_notes` ("lineage not adopted:
  parent not in provider (...)", "lineage not adopted: finding ... is not known
  to the analysis authority (...)").

## Gotchas

- Stateless: all facts come through `RecipeHost` (store, method, plan, finding,
  instrument, now); tests inject stubs. It lives in `mib_review_core` beside `NextRunPlanning`,
  so it must not depend on `mib_backend` code (the facade parses calibration
  declarations and passes the policy in `RecipeMethodFacts`).
- The instrument facts come from `ExperimentCoordinator::evaluateReadiness()`; a
  diff names the readiness generation it used.
- Registry revisions that are not materialized give *core* and *calibration
  policy* unknown with that reason.

## Tests

`tests/workspace/recipe_provider_test.cpp` (`workspace.recipe_provider`),
`analysis_host_test` (finding → draft lineage), `ws.rs`
`recipe_provider_create_derive_diff_apply_preview` (real backend over the
WebSocket), `contract.rs` `recipes_are_described`, `RecipeLibrary.test.tsx`.

**Up:** [[_MOC]] · [[WorkspaceStore]] · [[SqliteService]] · [[../architecture/Rust-Bridge]]
