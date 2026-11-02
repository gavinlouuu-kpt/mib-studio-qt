# WorkspaceStore

Native metadata persistence core for #807 / #393 / #395. Mounted in AppBackend
through [[SqliteService]] and BackendFacade (bridge ABI 45); not mounted in
ReviewSession. The host owns connection lifetime and serialization; transport
controller authorization is not operator authentication.

## Responsibility and APIs

`include/backend/workspace/WorkspaceStore.h` and matching source take an injected
`sqlite3*`. `initialize`, exact/latest `read`, bounded `history`, expected-revision
`commit` and new-revision `restore`. SQLite transaction atomically stores an
immutable logical document and successful idempotency receipt. No hardware,
scientific evaluator, source HDF5, file opening or per-feature database authority.

## Gotchas

Initialize successfully first; unknown schemas fail closed. Serialize one
connection on the host metadata worker, never on acquisition paths. Do not call
inside a host transaction. Host owns SQLite durability pragmas and DB lifecycle.
Successful command replay is exact; failures do not consume command IDs.
Scientific payload validation remains F-owned. JSON envelope validity is not
scientific validation or operator authentication.

## Evidence and remaining work

Standalone `tools/ique_acceptance` harness covers round-trip, injected rollback,
restart, history, restore, command replay and two-connection CAS. ASan/UBSan passed;
TSan startup failed with unexpected memory mapping, so sanitizer acceptance is
open. See `docs/design/ique-workflow/WORKSPACE-EVIDENCE.md`. Export/relink, scientific provider integration and complete workspace loading remain required; this is not full I12 acceptance.

**Up:** [[_MOC]] · [[../architecture/Desktop-Shell]]

## Production recovery controls (2026-10-23)

Experiment / Plan exposes history (first 1000 revisions) and CAS restore as a new
immutable revision. Explicit Load as new experiment copies the saved sheet into
usePlan with a fresh UUID and cleared queue/reference/result, refusing locked or
pending plans. Recipe references persist in the same document; no provider results
are fabricated. Pending write bytes are stored in sessionStorage before dispatch
and reconciled only by explicit retry after reload. Closing the tab loses that
client journal; SQLite snapshots/receipts remain authoritative.

## Lifecycle and transfer (2026-10-23 continuation)

`list` pages latest heads in lexical ID order. Titles/archive flags are revisioned
metadata, not destructive deletion. The client retains all document payloads when
saving the sheet, including findings/definitions/annotation-display/journal links.
`exportSnapshot` materializes an immutable `I-workspace/1` manifest and SHA256 in
`workspace_exports` within a SQLite transaction. Successful job IDs replay exactly;
`exportStatus` recovers them after restart. Completion is durable manifest generation,
not browser download completion. Import validates format/digest and creates a new
workspace only, using the normal successful-command receipt/CAS transaction.

Source attach computes full SHA256/size from regular files (2 GiB/10-second bound).
Relink preserves content identity and appends only a changed path after verification;
mismatch never updates the original. Receipt replay precedes filesystem access, so
moving/deleting the old path does not prevent retry of an already committed action.
A verification result describes that check only; evaluation must revalidate source
bytes. Facade source operations hold the existing idle-configuration guard and refuse
raw recording. No source-file write, automatic acquisition, or method apply is added.

`tools/ique_acceptance/workspace_ws.mjs` exercises the real app/SQLite lifecycle and
accepts `recover` to verify export/import/relink receipts after an owned server restart.
Use isolated QA data and do not take control from another client's runtime.

## Native next-run derivation (2026-10-30)

[[../current-state/recent/2026-10-23-workspace-transfer]] is extended by the native
`NextRunPlanning` consumer of F `dbdcfd82`. Exact finding/comparison, retained source,
strategy and method references resolve before a create-only draft. `derive` and
`derivedReceipt` are native-only dispatcher actions, not public JSON capabilities.
The same SQLite transaction fences the source head, writes the next-run document,
and records an exact origin-request receipt. Retry precedes re-resolution so a lost
reply survives later method/source changes. Production UI journals the entire request
before send and never auto-replays. No hardware apply/start is involved.

Exports traverse up to 64 finding/source/strategy dependencies. Public import still
requires native provisioning; native hashes/sample content are checked, including
when callers omit dependency entries. Reopen shows historical provenance; explicit
`verify_next_run` is required for a current check. See
`docs/design/ique-workflow/PRODUCTION-INTEGRATION.md` for literal request contracts.
