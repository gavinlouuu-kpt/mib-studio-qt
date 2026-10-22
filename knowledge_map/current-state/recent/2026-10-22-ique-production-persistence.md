## 2026-10-22 — iQue production metadata save and reopen

#807 now mounts [[services/WorkspaceStore]] in [[services/SqliteService]], exposed
through BackendFacade, cxx ABI 45, shared commands, Tauri and the WebSocket server.
The production Plan view saves and reopens immutable sample-sheet revisions.
Explicit actions only; controller-only writes, CAS conflicts and exact-command
retry. Native rollback/restart and live WebSocket permission checks pass. Recipe
science, plan activation, full workspace lifecycle and TSan acceptance remain open.
See [[architecture/Desktop-Shell]] and the production integration evidence.
