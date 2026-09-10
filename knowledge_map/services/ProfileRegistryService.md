# ProfileRegistryService

Source: `include/backend/profiles/`, `src/backend/profiles/`.

## Responsibility

Foundation for #398: provider-neutral central method revisions, Supabase RPC
serialization, immutable local cache and explicit one-request sync operations.
Not yet wired into AppBackend, Qt, React/Tauri or experiment execution.

## Key APIs

- `canonicalMethod` / `verifyRevision`: existing config schema 1 and camera script
  wrapped with core/contract/hardware declarations; exact canonical SHA-256.
- `ProfileRegistry`: list/fetch/submit/transition; typed network/auth/conflict errors.
- `ProfileCache`: origin/user-scoped SQLite, immutable content, versioned state,
  local validation keyed by instrument + context + exact content hash.
- `ProfileRegistryService`: download/sync one page, separate registry health;
  a failed request does not erase previously cached data.
- `SupabaseProfileRegistry`: injected user access token and bounded HTTP request
  contract. Transport must enforce TLS/no redirects/response cap/timeout.

## Gotchas

All objects are worker-confined. No background thread or execution readiness is
owned here. Cache eligibility does not mean applied, hardware verified, or allowed
to Start. New central revisions never select themselves. Known revocation is sticky;
archived/revoked content remains readable for history. Refresh scans must restart:
UUID cursors do not detect mutable metadata changes. Corrupt cache rows fail closed;
automatic repair and historical pins remain pending.

Setup, current scope, tests and recovery: `supabase/README.md`.
Execution plan: `docs/exec-plans/active/2026-09-10-central-profile-registry.md`.
