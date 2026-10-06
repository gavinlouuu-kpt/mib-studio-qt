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
- [x] M2b: "Mark validated" in React (evidence must be a run of that revision
  on this instrument/context) and Materialize; bridge
  `registry_materialize` / `registry_record_validation` /
  `registry_local_validation`. The readiness panels list `method.revision` like
  any gate. A Qt Apply (exact bytes through `AppConfigWatcher`, with a backup)
  was built and dropped with the Qt UI (ADR 0011).
- [x] M2c: Apply in the React shell through the backend config.json applier
  (`app::applyConfigDocument`: the watcher's section semantics, staged and
  fail-closed, exact text recorded; dot_grid / display_fps reported as Qt-only;
  refused while a run is in flight).
- [ ] M2c: reconcile `app::applyConfigDocument` with develop's local-profile
  apply (`app/ProfileStore.cpp`, arrived with #450): it also refuses while
  capture/realtime/autofocus run, bounds every value, validates the ROI against a
  captured frame and applies `realtime_processing.enabled` / `drop_frames`;
  share one validated applier, keeping exact-text recording for central methods.
- [ ] M2c: persist the applied method across Tauri restarts (today it lives in
  the backend for the session; the Qt shell persists through config.json).
- [ ] M2: compatibility validator against declared hardware compatibility and
  calibration context; camera script compared as well as config.json; explicit
  update selection when a newer revision is published.
- [ ] M2: hardware run continues unchanged during update and registry outage
  (mock-run and HDF5 reopen are covered by `e2e.method_gate`; hardware not yet).
- [x] M3a (backend): Supabase authoring RPCs (create method, immutable release
  notes, method heads, revision history) with PGlite tests; local drafts; submit
  with a pre-submit base-vs-head check that stops with a compared conflict and an
  explicit branch option; independent review / publish / archive / revoke with
  reasons; history; update-available helper. `profiles.registry_authoring`.
- [x] M3b: authoring/review UI in React (Drafts view, review actions by role
  with reasons, details/history, "rN available"; rules in `registry.ts`), bridge
  authoring functions inside the registry ABI (25). The Qt version was
  dropped (ADR 0011).
- [ ] M3: template drafts (a new method from a bundled template), a per-key
  draft editor beyond "current config.json", and release-note display in a
  method picker / context bar (#312).
- [x] M3 core: local drafts, authoring/submission UI, conflict comparison/branch handling,
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

- 2026-10-04 (M2b): decisions — (1) Apply replaces config.json exactly, never
  merged, because only exact bytes are recognisable as the revision; instrument
  keys travel with the method, so the confirmation lists every changed key and
  the old file is backed up. (2) "Mark validated" requires the test run's frozen
  provenance to name the revision, this instrument and the current context, so
  a validation cannot be attached to an unrelated file. (3) React Apply stays
  disabled with the reason instead of approximating it through the
  processing-config document.

- 2026-10-04 (M3a): decisions — (1) methods are created by authors through an
  audited, idempotent RPC (M0 left method creation to administrators, which made
  authoring impossible from the app); (2) release notes are immutable revision
  metadata outside the content hash; (3) the worker checks the head before
  submitting so a conflict costs no server write and shows both diffs; the server's
  own 409 remains the authority; (4) drafts live in the per-user cache, so they
  follow the registry account, not the instrument.

- 2026-10-04 (ABI hold): ADR 0011 (PR #484) freezes bridge-ABI bumps on `develop`
  until #450 (`dev/react-tauri`) lands, then renumbers once. #474/#475 do not touch
  the bridge; #477 introduces the registry surface as ABI 15 and #478-#482 extend it
  inside that unreleased 15, so none of them lands before #450. After the
  renumbering the stack is rebased (version + changelog only; contract groups and
  function names stay). ADR 0011 also limits Qt to fixes until #450 reaches parity,
  which bears on the Qt-only parts of #479 (Apply) and #482 (authoring UI); owner's
  call.
- 2026-10-05 (restack): merge coordination decided to drop the Qt parts. #475 (Qt
  dialog) closes unmerged; #477 is based directly on `develop` (bridge ABI 19 after
  #450) by merging `develop` in (published branches are never rebased), and the Qt
  dialog, Qt Apply and Qt authoring UI were removed from #477/#479/#482; the
  fake Supabase test support stays. Until the stack lands, ABI conflicts resolve
  to the highest number (the registry rides 19). The stack lands after
  `feat/yofo-remote-server` and `feat/yofo-pl-results` (merged together as #495).
- 2026-10-05 (renumber): the registry surface was built as a provisional ABI 15 and
  rode develop's number through the restack. #495 took 23 for the instrument line
  and #502 took 24 (#501 P0), so the stack is **25** (contract, `shim.cpp`, contract
  test, generated `bridgeContract.ts`, registry comments): a bump to 24 was first
  committed (17b08e71), then set to 25 when develop reached 24 (merge commit
  resolving the ABI lines). 15 and 19-24 are never reused. The Tauri commands moved to `mib-app-commands` on
  develop; the registry commands stay in the desktop crate (`registry` module)
  because they need the shell's transport and a sign-in carries a password that must
  not cross the YOFO Studio WebSocket.
- 2026-10-06 (M3b ABI): the authoring surface (#482) changes the bridge FFI, so it
  takes its own number. Merge coordination assigned **28** (26 = the ZC300 stage
  bridge #513, 27 = #501 P1 #510); the bump is its own commit, and the ABI lines
  resolve to 28 when `develop` reaches 27. #510 landed (develop at 27) and the
  follow-up merge resolved the ABI lines to 28.
- 2026-10-06 (M2c ABI): the React Apply (#493: `registry_plan_apply`,
  `registry_apply_method`) changes the bridge FFI too; merge coordination assigned
  **29**, bumped in its own commit on top of #482's 28.

## Validation

`profiles.registry` runs in backend CTest (`linux-backend-only` preset; built and
passing as of 2026-10-02) against the production registry sources, shared SHA-256
and SQLite. PostgreSQL tests use pinned PGlite with an auth identity stub
(`npm test --prefix supabase`, `profile-registry-ci.yml`). Windows build not yet
exercised. No Supabase project has been deployed or modified.
