## 2026-11-02 — Native recipe provider, bridge ABI 50 (#807 I02, fold-in slice I-1)

Branch `claude/807-i1-recipes` on `claude/815-ique-on-810`. Goal **I02**: recipes are immutable
pairs of an acquisition method revision and an analysis-plan revision; base-on-previous creates a
new draft; the compatibility diff is computed in the backend; apply is explicit and never changes
hardware.

- **Authority:** [[services/RecipeProvider]] (`src/backend/workspace/RecipeProvider.cpp`), stored
  through [[services/WorkspaceStore]] in `app.sqlite3` under `recipe/<id>`, create-only via the new
  `SqliteService::recipeStoreRequest` authority; `workspace_write` refuses the namespace.
- **Diff dimensions:** method (coordinator method provenance), profile (canonical config hash),
  core (method-declared core/contract vs the active core), calibration (recorded basis vs record in
  effect and E-LUT, plus the method's policy); each equal / different / unknown with a reason.
- **Apply preview:** changes plus the `registry_apply_method` / `profile_command` request the method
  authority must accept; core and calibration are never changed; `applied` and `hardware_actuated`
  are always false.
- **Findings:** `NextRunPlanning` derives the next-run recipe through the provider (adopting the
  workspace's inline recipe once) and refuses without it.
- **Bridge ABI 50** (provisional; 49 is E feature control): `recipe_list`, `recipe_get`,
  `recipe_create`, `recipe_derive`, `recipe_diff`, `recipe_apply_preview`
  ([[architecture/Rust-Bridge]]).
- **View:** `RecipeLibrary` mounted in Experiment → Plan, rendering `RecipePairing` from the
  provider's replies ([[architecture/Desktop-Shell]]).

Evidence class: **real backend, headless** (native `workspace.recipe_provider` on the real metadata
host; `ws.rs` against the real backend with the mock camera). No rig run; registry-cache methods
are exercised only through stubs.
