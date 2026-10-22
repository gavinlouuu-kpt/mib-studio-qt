# SqliteService

Owns the app metadata SQLite connection at `<dataDir>/app.sqlite3`. AppBackend
creates and initializes it; BackendFacade routes workspace requests to it.

**Source:** `src/backend/database/SqliteService.cpp`,
`include/backend/database/SqliteService.h`.

## API and threading

`initialize(path)` opens once, enables WAL / FULL synchronous / foreign keys,
sets a bounded busy timeout and initializes [[WorkspaceStore]]. Failure leaves
workspace commands unavailable. Reinitializing an open service is refused.
`workspaceRequest(request, write)` serializes the complete request/transaction on
one mutex. Read-only requests cannot mutate. `execute(sql)` is internal-only,
serialized, and is not exposed by the bridge. Destruction closes the connection;
AppBackend owns lifetime and must finish command access first.

No capture-thread calls, new worker threads, method-cache tables, or scientific
worker journals. The shared command layer also serializes bridge access; metadata
writes can occupy that command path up to SQLite's busy timeout. This is a bounded
first integration, not an asynchronous metadata-worker performance qualification.

## Contract and evidence

Bridge ABI 45 has `workspace_read` and `workspace_write`. The WebSocket host
requires the controller for writes; this is transport control, not authenticated
operator attribution. Store errors map to explicit statuses. Successful command
replay returns its original revision; unknown writes must retry exactly.

`backend.workspace_host` tests real persistence, restart, rollback, replay and
stale-CAS refusal. See [[WorkspaceStore]] and
`docs/design/ique-workflow/PRODUCTION-INTEGRATION.md` for limits.
