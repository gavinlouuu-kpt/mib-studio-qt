# Central profile registry foundation (#398)

These migrations and the C++ provider/cache/worker implement the foundation and
the backend half of M1 of
[issue #398](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/398): desktop
sign-in, explicit refresh/download and an offline-capable per-user cache. They are
**not yet connected to a method picker, Apply/Verify, or run provenance**. Existing
local profiles are unchanged and are never uploaded. Do not enable this as an
instrument execution path yet.

## Setup

1. Create a development Supabase project. Apply every file in `migrations/` in
   filename order with the Supabase CLI or SQL editor.
2. Configure Supabase Auth (email + password) and create test user identities.
   Bootstrap organization, project, membership and method rows through a trusted
   administrator connection. Membership management and method creation UI/RPC are
   not implemented yet.
3. Point the desktop app at the project: `MIB_PROFILE_REGISTRY_URL=https://<ref>.supabase.co`
   and `MIB_PROFILE_REGISTRY_PUBLISHABLE_KEY=sb_publishable_...` in the environment
   (never in a tracked file). Without both, the registry worker stays inert.
   Service-role/secret keys are refused as API keys.
4. The shell supplies the HTTPS POST (`AppBackend::setProfileRegistryTransport`,
   ADR 0002). The Qt transport (`src/frontend/system/RegistryHttpTransport.cpp`)
   uses POST, platform TLS verification, no redirects, the request timeout, the
   response byte cap during transfer, prompt abort on cancel, and logs no headers
   or bodies. A Tauri shell must meet the same contract.
5. `ProfileRegistryWorker` signs in with Supabase Auth (password grant), keeps
   access/refresh tokens **in memory only**, rotates the refresh token before
   expiry or on a 401, and opens one SQLite cache per origin + user under
   `<dataDir>/profile_registry/`. `last_session.json` (origin, user ID, email; no
   token) lets the last user's cached revisions be listed offline after a restart;
   explicit sign-out removes it. Per-user files prevent accidental account reuse;
   they are not encryption or protection against the OS user who owns them.
   Persisting a refresh token in the OS keychain is a later, shell-owned step.

Use the [Supabase RLS guide](https://supabase.com/docs/guides/database/postgres/row-level-security)
when reviewing grants and policies. Both table grants and RLS are required. Desktop
clients receive read grants and narrow RPC access; privileged transitions are checked
inside fixed-search-path functions against `auth.uid()` and project roles.

## Protocol

`registry_list_projects` (the caller's memberships and roles),
`registry_fetch_revision`, `registry_list_revisions`, `registry_submit` and
`registry_transition` correspond to the provider interface. A worker refresh lists
the user's projects, then scans each from its first page, bounded by a page count
and a time budget (exceeding either ends the job as Partial; the cache stays valid). One full revision per
page bounds responses for methods of up to 1 MiB. The cursor is a UUID keyset cursor,
**not a synchronization watermark**: every explicit refresh restarts the scan to
observe changed revocation/supersession metadata. A concurrent insertion before the
cursor is discovered on the next scan. The service performs one page per call; the
future worker must cap pages/time per user operation and support continuation.

The content envelope contains existing `config.json` schema 1 and optional camera
script text, plus explicit processing core/contract and hardware declarations.
`canonicalMethod` freezes the envelope's canonical bytes; SHA-256 is over the UTF-8
bytes, not the display name. Canonicalization v1 uses ordered object keys, compact
nlohmann JSON 3.x encoding and normalizes integral doubles within the safe integer
range. This is **not RFC 8785/JCS**. New-language publishers must use golden vectors
or call the same C++ implementation. The server verifies the submitted hash and
structural schema; it stores the exact text without reserializing it. The client
also rejects noncanonical bytes, unsupported envelopes and duplicate keys before use.
During a list scan such a revision is skipped, never cached, and counted in registry
health (`rejectedRevisions`); the scan continues so one bad submission cannot hide
later revisions or revocations.
This structural validation does not validate hardware configuration or camera script
safety; the local instrument validation and existing Apply/Verify gates must do so.

Submission is idempotent by caller-assigned revision UUID plus exact immutable
content, author, method and parent. A changed central head produces HTTP 409.
Publication rechecks the parent against the locked method head. Reviews apply to the
exact hash and require someone other than the author. Review/publication mutations
require the last-seen metadata version and a reason. A lost transition response must
be resolved by fetching current state before retrying, never blindly replaying a
publication. Revocation is terminal; supersession retains historical eligibility,
while archived and revoked revisions are excluded by the local eligibility policy.

## Tests

```sh
npm ci --prefix supabase
npm test --prefix supabase
```

The pinned PGlite harness executes the migration and role/lifecycle tests in an
isolated PostgreSQL engine, stubbing only `auth.users` and `auth.uid()`. It tests
actual grants, RLS, functions and transactions. It does not exercise hosted
Supabase Auth, PostgREST HTTP status mapping, networking, or deployment configuration.
Run `tests/profile_registry.sql` against a disposable Supabase development database
as the database owner too; fixtures roll back. Never run test fixtures in production.

The C++ `profiles.registry` CTest verifies canonicalization/hash vectors, immutable
cache contents, persisted reopen, account isolation, local validation context,
offline/expiry/reconnect, stale metadata, terminal revocation and disk corruption.
It is registered with backend CTest and requires no hardware or live credentials.

## Recovery and pilot operations

- Keep experiment HDF5, per-frame data and large artifacts outside this registry.
- Export the registry tables, schema/functions/policies and required Auth identity
  mappings using a trusted PostgreSQL backup connection. Store encrypted backups
  outside the hosted project, for example on protected NAS/object storage. Never
  put backup credentials in MIB Studio.
- Before production, rehearse restoring into an isolated project, restoring user
  identity mappings, verifying revision hashes, and rerunning RLS tests. Define the
  backup owner, schedule, retention, recovery point and recovery time explicitly.
- Free-tier quotas are deployment constraints; no quota is hardcoded in the client.
- Cache integrity failures fail closed. This milestone intentionally cannot repair
  a corrupt immutable row in place. Preserve/quarantine the database for diagnostics;
  rebuild a separate cache by verified refetch, and repeat local validation. Offline
  runs using a corrupt revision are unavailable until recovery. Automatic recovery,
  retained historical pins and cache migrations are later work.
- An offline central service changes registry health only. Cached eligible values
  remain readable; this is not automatic authorization to execute, Apply, or Start.
