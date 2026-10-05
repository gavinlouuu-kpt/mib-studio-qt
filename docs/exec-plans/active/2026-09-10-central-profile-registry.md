# Central profile registry (#398)

Status: active — foundation implemented; application integration pending

## Goal

Distribute immutable approved methods through Supabase without putting network
access on the instrument-control path. Preserve existing local-only profiles and
freeze exact revision identity/content into historical runs.

## Inventory and decisions

- 2026-09-10: `frontend::ProfileManager` owns current local config/script files and
  catalog downloads. There is no existing authoritative backend Method aggregate.
  Preserve this implementation during the foundation PR; do not copy its mutable
  active-profile behavior into the central registry.
- The current backend is Qt-free (the older overview's allowance for Qt describes
  historical layering). New registry code is plain C++17. Reuse the existing SHA-256
  implementation through an extracted dependency-light declaration header.
- Wrap existing config schema 1 + camera script in a versioned envelope. The central
  revision has a separate UUID/hash from the local profile identity. Do not silently
  normalize or upload legacy profiles.
- Authentication/HTTP are injected behind the provider, following the existing LUT
  fetcher boundary. Native Auth/HTTP adapters are not part of this foundation.
- SQLite cache is worker-confined and scoped to origin/subject. No polling, shared
  worker state, acquisition callbacks or automatic selection is introduced.
- Project ownership is the initial subset. Organization/operator ownership and
  cross-project distribution remain later milestones.

## Progress and acceptance

### Foundation delivered in the first PR

- [x] Provider-neutral registry/revision/error types.
- [x] Versioned canonical method envelope and shared SHA-256.
- [x] Supabase RPC provider with bounded request contract and typed failures.
- [x] Immutable SQLite revision cache and exact instrument/context validation.
- [x] Explicit paged download/sync service with separate registry health.
- [x] PostgreSQL organization/project/membership/method/revision/review/audit schema.
- [x] Read RLS plus guarded submit/approve/reject/publish/archive/revoke functions.
- [x] C++ integrity/offline/fault tests and PostgreSQL RLS/lifecycle harness.
- [x] Setup and backup/recovery constraints documented in `supabase/README.md`.

### Remaining #398 milestones (not claimed complete)

- [ ] M0 completion: administrator method/membership APIs and audited permission
  changes; organization/operator ownership; canonical cross-language vectors.
- [x] M1 (backend, PR stacked on #402): shell-injected HTTPS transport (Qt
  `makeQtRegistryHttpTransport`, ADR 0002 seam instead of a backend HTTP client),
  Supabase password sign-in / refresh rotation / sign-out, in-memory tokens,
  AppBackend-owned `ProfileRegistryWorker` with bounded/cancellable refresh across
  member projects (`registry_list_projects`), offline reopen of the last user's cache.
- [ ] M1: refresh-token persistence in the OS keychain (shell-owned seam) so a
  restart does not require a password; today a restart is CachedOffline until sign-in.
- [~] M1 (Qt): a Settings → Central Methods… dialog was built (#475) and dropped
  unmerged: ADR 0011 makes Qt fixes-only, so the registry UI is React/Tauri only.
  The shared fake Supabase it introduced (`tests/support/fake_supabase.h`) stays.
- [x] M1 (bridge + React): `BackendFacade` registry commands/snapshot, bridge ABI
  24 (`registry_*` contract groups, `set_registry_transport` with polled cancel
  handles), Tauri `ureq` HTTPS transport (desktop-only `registry` module), React
  Central Methods panel over a pure view model.
- [x] M2a (backend): instrument identity (UUID + name); worker Materialize and
  RecordValidation jobs; the applied config.json matched to a cached revision by
  canonical config hash; `method.revision` gate (unvalidated → Warn, validated
  here → Pass, revoked/not published → Fail) bound into the readiness
  generation; exact revision, content hash, validation and evidence hash frozen
  into `/run_provenance` (schema v2) with no network lookup.
- [ ] M2b: Apply (materialized config → config.json with confirmation) and
  "Mark validated" (pick the test-run file) in React; bridge commands for
  materialize / record validation; method row in the readiness panels.
- [ ] M2: compatibility validator against declared hardware compatibility and
  calibration context; camera script compared as well as config.json; explicit
  update selection when a newer revision is published.
- [ ] M2: hardware run continues unchanged during update and registry outage
  (mock-run and HDF5 reopen are covered by `e2e.method_gate`; hardware not yet).
- [ ] M3: local drafts, authoring/submission UI, conflict comparison/branch handling,
  release notes and review/publication management UI.
- [ ] M4: operator execution entitlements, project distribution, role management,
  independent approval display and attributed execution/session journal integration.
- [ ] M5: automatic quarantine/refetch recovery, retained historical/offline pins,
  schema migrations, hosted Auth/RLS/PostgREST smoke tests and restore rehearsal.
- [ ] Windows and actual Qt/Tauri E2E; lifecycle/concurrency/TSan tests when worker
  and experiment integration are added. Current test is registry-layer only.

## Progress log

- 2026-10-02: Merged `develop` (153 commits) into PR #402; conflicts were vault notes
  only. First full `linux-backend-only` build + CTest of the registry sources: all
  pass (`profiles.registry` included). Fixed a sync stall: a noncanonical revision
  threw out of `listRevisions`, so the one-revision page never advanced and every
  later revision/revocation stayed invisible. Now reported as `RevisionPage::rejected`
  and counted in `RegistryHealth::rejectedRevisions`. `profile-registry-ci.yml` now
  runs only the PGlite SQL suite (the C++ test runs in backend-ci; no inline apt list).

- 2026-10-02 (M1 backend): decisions — (1) ADR 0002 forbids a backend HTTP
  client, so "native HTTP adapter" is the shell-injected `RegistryHttpTransport`
  with a `cancelled` predicate; (2) tokens stay in worker memory, nothing
  token-bearing is written; `last_session.json` keeps only origin/subject/email so
  offline continuity survives a restart; (3) explicit sign-out closes the user's
  cache and forgets the last session (shared instruments); (4) a refresh restarts
  every member project's scan and is bounded by pages + time (Partial, never
  silently truncated); (5) cancelled requests never count as outages.
  Tests: `profiles.registry_worker`, `profiles.registry_backend` (hung registry
  vs running mock capture; shutdown abort), `frontend.registry_http_transport`;
  all three behaviour mutations of the worker were caught; TSan clean (3 repeats).

- 2026-10-04 (M1 bridge + React): found and fixed on the way — `BackendFacade::
  shutdown()` did not stop the registry worker, so a hung registry request outlived
  facade shutdown (caught by the new `profiles.registry_facade` test); the facade
  now stops it first. ABI 15 was free on `develop`; the review-scatter plan also
  names "14 → 15", so whichever lands second takes the next number.
- 2026-10-05 (renumber): the registry surface was built as a provisional ABI 15 and
  rode develop's number through the restack. #495 took 23 for the instrument line,
  so after merging develop (23) the stack takes **24** in one commit (contract,
  `shim.cpp`, contract test, generated `bridgeContract.ts`, registry comments); 15
  and 19-23 are never reused. The Tauri commands moved to `mib-app-commands` on
  develop; the registry commands stay in the desktop crate (`registry` module)
  because they need the shell's transport and a sign-in carries a password that must
  not cross the YOFO Studio WebSocket.

- 2026-10-04 (M2a backend): decisions — (1) operator choices on #398: an
  unvalidated central revision warns and Start is allowed; validation is an
  explicit operator confirmation (who = signed-in registry user, when, instrument
  UUID + context hash, test-run file + SHA-256); a known-revoked revision blocks
  Start; instrument identity = generated UUID + optional name. (2) The Qt
  `AppConfigWatcher` stays the applier of config.json, so the backend recognises
  the applied method by canonical config hash instead of tracking an "applied"
  flag that could drift from what is really loaded; any local edit makes it a
  local method. (3) The coordinator reads only the worker's value snapshot
  (memoized on registry generation), keeping the network and SQLite off the
  readiness path. (4) A failed local validation warns rather than blocks (not
  decided on the issue; easy to tighten). (5) Validation needs an authenticated
  session; the offline cache can materialize but not validate. (6) Context =
  instrument + core version/SHA + camera source; the free-form camera label is
  excluded because it is not stable across sessions.

## Validation

`profiles.registry` runs in backend CTest (`linux-backend-only` preset; built and
passing as of 2026-10-02) against the production registry sources, shared SHA-256
and SQLite. PostgreSQL tests use pinned PGlite with an auth identity stub
(`npm test --prefix supabase`, `profile-registry-ci.yml`). Windows build not yet
exercised. No Supabase project has been deployed or modified.
