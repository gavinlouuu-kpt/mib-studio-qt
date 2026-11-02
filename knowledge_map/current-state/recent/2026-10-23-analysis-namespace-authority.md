## 2026-10-23 — Reserve analysis metadata authority

SqliteService separates generic workspace writes from native analysis namespace
writes. This closes the cross-track integration route where generic version1 JSON
could impersonate retained sources or evidence under analysis prefixes. F typed
hosts use analysisWorkspaceRequest; the generic RPC must never expose that method.
Native shared-store/host tests pass including reserved namespace write attempts.
Exact boundary handed to I before final multi-track integration.
