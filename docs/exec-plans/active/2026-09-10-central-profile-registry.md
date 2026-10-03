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
- [ ] M1: native HTTP adapter, Supabase user login/refresh/logout, secure token
  custody, AppBackend-owned worker lifecycle and bounded/cancellable refresh jobs.
- [ ] M1: shared backend snapshots/commands through BackendFacade and Rust bridge;
  Qt and React method discovery/details showing separate central/cache states.
- [ ] M2: authoritative selected/applied/verified method aggregate; compatibility
  validator tied to real core/camera/calibration context; explicit update selection.
- [ ] M2: Start readiness binds revision/hash and local execution permission;
  exact canonical content + identities frozen into HDF5 without network lookup.
- [ ] M2: hardware/mock run continues unchanged during update and registry outage;
  reopen historical HDF5 proves exact revision independently of the registry.
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

## Validation

`profiles.registry` runs in backend CTest (`linux-backend-only` preset; built and
passing as of 2026-10-02) against the production registry sources, shared SHA-256
and SQLite. PostgreSQL tests use pinned PGlite with an auth identity stub
(`npm test --prefix supabase`, `profile-registry-ci.yml`). Windows build not yet
exercised. No Supabase project has been deployed or modified.
