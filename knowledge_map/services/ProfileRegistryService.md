# ProfileRegistryService

Source: `include/backend/profiles/`, `src/backend/profiles/`;
Qt transport `src/frontend/system/RegistryHttpTransport.cpp`.

## Responsibility

Central method registry for #398: provider-neutral revisions, Supabase RPC and
Auth clients, immutable per-user local cache, explicit sync operations, and the
backend-owned `ProfileRegistryWorker` that runs them on one thread. Owned by
[[../architecture/AppBackend]] (`profileRegistry()`); the Qt shell injects the
HTTPS transport; the React Central Methods panel (through `BackendFacade` registry calls, bridge ABI 25,
see [[../architecture/Rust-Bridge]]) sign in, refresh and list cached
revisions. M2a (backend): the worker materializes a cached revision's files and
records operator-confirmed local validations; [[../architecture/ExperimentCoordinator]]
matches the applied config.json to a cached revision, gates Start on it
(`method.revision`) and freezes it into `/run_provenance`. M2b/M2c: Apply
(`planMethodApply`, then `app::applyCentralMethod` /
`applyConfigDocument` in `include/backend/app/ConfigDocumentApply.h`, the one
validated applier shared with local profiles: bounded values, the ROI rule
(captured frame, else camera window, else pending until the first frame),
realtime enabled/drop_frames, stage block; the exact text is recorded; the applied revision is the startup configuration,
re-applied after a restart by `app::restoreStartupConfiguration`, bridge ABI 32) and
"Mark validated" (`AppBackend::requestMethodValidation` accepts only a
test run whose `/run_provenance` names the revision on this instrument under
the current context — `checkValidationEvidence`). See
`include/backend/app/MethodApply.h`.

## Key APIs

- `canonicalMethod` / `verifyRevision`: existing config schema 1 and camera script
  wrapped with core/contract/hardware declarations; exact canonical SHA-256.
- `ProfileRegistry`: list/fetch/submit/transition; typed network/auth/conflict errors.
- `ProfileCache`: origin/user-scoped SQLite, immutable content, versioned state,
  local validation keyed by instrument + context + exact content hash.
- `ProfileRegistryService`: download/sync one page, separate registry health;
  a failed request does not erase previously cached data. A listed revision that
  fails hash/canonical verification comes back in `RevisionPage::rejected`, is
  never cached, and is counted in `RegistryHealth::rejectedRevisions`; the cursor
  still advances past it.
- `SupabaseProfileRegistry`: injected user access token and bounded HTTP request
  contract. Transport must enforce TLS/no redirects/response cap/timeout and
  abort when `RegistryHttpRequest::cancelled` turns true.
- `SupabaseAuth`: password sign-in, refresh-token rotation, best-effort sign-out.
- `ProfileRegistryWorker`: `requestSignIn/SignOut/Refresh/Download` enqueue and
  return a job ID (0 = refused); `cancelAll()`; `snapshot()` (value copy:
  session SignedOut/SignedIn/CachedOffline, health, projects, cached revisions,
  corrupt IDs, last job); `job(id)`. A refresh covers every member project,
  bounded by `maxPagesPerRefresh` / `refreshBudget` (else Partial). Tokens are
  rotated before `tokenRefreshMargin` and once on a 401, then retried once.
- `ProfileRegistryWorker::requestMaterialize(revisionId)` (M2): writes the
  verified revision to `<dataDir>/methods/<revisionId>/` — `config.json`
  (pretty), `egrabberConfig.js`, `method.canonical.json` (byte-exact
  envelope) — read-only, staged then swapped in; idempotent; tampered files are
  rewritten; works from the offline cache. Only plain-token revision IDs may
  name a directory (`../x` is refused). The snapshot reports
  `materializedDir` per revision (verified scan on cache open).
- `ProfileRegistryWorker::requestRecordValidation(LocalValidationRequest)` (M2):
  needs a **signed-in** session (the validator is the authenticated user, not
  a remembered name) and a published/superseded revision; hashes the evidence
  test-run file (cancellable, `processing::fileSha256`) and stores who, the
  instrument UUID, `methodContextHash` (core version + core SHA-256 + camera
  source), the content hash and evidence JSON (`run_file`, `run_file_sha256`,
  bytes, instrument name, validator email, UTC time). The latest outcome per
  revision/instrument/context replaces earlier ones. Snapshot: `validations`.
- `canonicalConfigSha256(configJson)` / `revisionConfigSha256(envelope)` (M2):
  key-order/whitespace/integral-double independent config hash; every
  `CachedRevisionSummary` carries `configSha256`.
- Authoring (M3a backend; M3b UI in the React Central Methods panel, see
  [[../architecture/Desktop-Shell]]):
  `requestSaveDraft(MethodDraft, copyFromRevisionId)` stores a local draft in
  the per-user cache (`registry_drafts`; works offline; IDs for draft, method
  and revision pre-generated with `generateUuidV4()` so a retried submit is
  idempotent); with a source revision the config, camera script, core,
  compatibility, method and base are copied from it — only the content fields
  the caller left empty, so `AppBackend::currentConfigDraft()` can put the
  applied config.json on top of a revision. The draft must
  canonicalize. `requestDeleteDraft`. `requestSetDraftNotes(id, notes)` (a
  SaveDraft job) changes only the notes of the worker's current, unsubmitted
  draft, so a delete queued before it wins (the facade's
  `registrySetDraftNotes` uses it; a full save of a snapshot copy would
  recreate the discarded draft). `requestSubmitDraft(id, asBranch)`
  (signed in): creates the method for a new-method draft, otherwise reads the
  method head (`listMethods`) and, when it is not the draft's base, stops with
  `snapshot().submitConflict` (base, head, upstream and draft-vs-head key
  changes) without sending anything; `asBranch` submits with the base as
  parent. A submitted draft is read-only. `requestTransition(id, state,
  reason)` (Approved/Rejected/Published/Archived/Revoked; reason required;
  cached metadata version, stale = Conflict); after a publish the worker
  re-downloads the method's other Published revisions so the superseded state
  shows at once. `requestHistory(id)` → `snapshot().history` (reviews + audit
  events). Refresh also lists each project's methods (`snapshot().methods`
  with heads). Summaries carry `parentRevisionId` and `releaseNotes` (cache
  column added in place, immutable once known).
  `app::newerPublishedRevision()` answers "update available".
- `InstrumentIdentity` (M2): UUID v4 in `<dataDir>/instrument_identity.json`
  plus `MIB_INSTRUMENT_NAME`; a corrupt file is moved to `.corrupt-<n>` and
  replaced (old validations stop matching, the gate warns).

## Gotchas

Close a file before renaming or moving it: Windows refuses to rename a file
that is still open (Linux allows it). `InstrumentIdentity` reads the identity
file in its own scope so a corrupt file can be moved to `.corrupt-<n>`
(`profiles.instrument_identity` caught this on the first MSVC run).

Everything except `ProfileRegistryWorker`'s public API is confined to the
worker thread. Snapshots never wait on SQLite or the network (the cache is read
outside the lock). A cancelled/aborted request is not counted as an outage.
Tokens are in memory only; `last_session.json` (origin, subject, email) reopens
the last user's cache as CachedOffline after a restart, and explicit sign-out
removes it and closes that cache (another operator then sees nothing). A
rejected refresh token drops to CachedOffline: cached revisions stay listable.
All provider/cache objects are worker-confined. No background thread or execution readiness is
owned here. Cache eligibility does not mean applied, hardware verified, or allowed
to Start. New central revisions never select themselves. Known revocation is sticky;
archived/revoked content remains readable for history. Refresh scans must restart:
UUID cursors do not detect mutable metadata changes. One unverifiable revision (any project author can submit
noncanonical bytes; the server checks only the hash) must not stall sync for every
later revision, so `listRevisions` reports it instead of throwing; transport, auth
and cross-project errors still fail the whole page. Corrupt cache rows fail closed;
automatic repair and historical pins remain pending.
Matching an applied config to a revision is by canonical config.json only; any
local edit (including default-key merge or camera delivery-mode write-back by
`AppConfigWatcher`) makes it a local method. Explicit sign-out closes the cache,
so the coordinator then cannot recognise (or know revocation of) central
methods: runs are recorded as local, never silently as validated.

Tests: `profiles.registry` (integrity/cache/provider), `profiles.registry_worker`
(fake Supabase: auth, rotation, outage, restart offline, user switch, sign-out,
cancel, bounds, concurrent snapshot traffic + shutdown), `profiles.registry_backend`
(AppBackend wiring; a hung registry leaves mock capture running; shutdown aborts
it), `profiles.registry_facade` (facade mapping + contract integers + shutdown abort),
the bridge `registry_*` cargo tests, `desktop/src/registry.test.ts`,
`profiles.registry_method`
(canonical config hash, materialize, record validation, restart, cancel while hashing),
`profiles.instrument_identity`, `backend.method_provenance`, `e2e.method_gate`,
`profiles.registry_authoring` (M3 lifecycle, conflict, branch, viewer refusal, old
cache migration), `frontend.registry_http_transport` (Qt transport timeout/cancel/https-only on a
worker thread), and the PGlite SQL suite.

Setup, current scope, tests and recovery: `supabase/README.md`.
Execution plan: `docs/exec-plans/active/2026-09-10-central-profile-registry.md`.
