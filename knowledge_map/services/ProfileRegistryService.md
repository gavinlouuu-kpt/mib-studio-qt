# ProfileRegistryService

Source: `include/backend/profiles/`, `src/backend/profiles/`;
Qt transport `src/frontend/system/RegistryHttpTransport.cpp`.

## Responsibility

Central method registry for #398: provider-neutral revisions, Supabase RPC and
Auth clients, immutable per-user local cache, explicit sync operations, and the
backend-owned `ProfileRegistryWorker` that runs them on one thread. Owned by
[[../architecture/AppBackend]] (`profileRegistry()`); the Qt shell injects the
HTTPS transport; the React Central Methods panel (through `BackendFacade` registry calls, bridge ABI 15,
see [[../architecture/Rust-Bridge]]) sign in, refresh and list cached
revisions. Not yet in a method picker, Apply/Verify or run provenance.

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

## Gotchas

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

Tests: `profiles.registry` (integrity/cache/provider), `profiles.registry_worker`
(fake Supabase: auth, rotation, outage, restart offline, user switch, sign-out,
cancel, bounds, concurrent snapshot traffic + shutdown), `profiles.registry_backend`
(AppBackend wiring; a hung registry leaves mock capture running; shutdown aborts
it), `profiles.registry_facade` (facade mapping + contract integers + shutdown abort),
the bridge `registry_*` cargo tests, `desktop/src/registry.test.ts`,
`frontend.registry_http_transport` (Qt transport timeout/cancel/https-only on a
worker thread), and the PGlite SQL suite.

Setup, current scope, tests and recovery: `supabase/README.md`.
Execution plan: `docs/exec-plans/active/2026-09-10-central-profile-registry.md`.
