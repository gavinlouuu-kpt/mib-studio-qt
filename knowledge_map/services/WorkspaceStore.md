# WorkspaceStore

Isolated native metadata persistence core for #807 / #393 / #395. Not yet mounted
in AppBackend or ReviewSession. Connection ownership, operator authorization and
migration scheduling remain with the shared metadata host.

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
open. See `docs/design/ique-workflow/WORKSPACE-EVIDENCE.md`. Export/relink and live
provider/host integration remain required; this is not full I12 acceptance.

**Up:** [[_MOC]] · [[../architecture/Desktop-Shell]]
