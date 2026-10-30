## 2026-10-22 — isolated iQue workspace persistence core

#807 adds injected-connection SQLite CAS snapshots, immutable history, new-revision
restore and successful command replay. Real rollback/restart/two-connection CAS
and ASan/UBSan harness pass. TSan startup fails on host mapping; not accepted.
No live host/facade/ABI integration or separate database authority.
See [[services/WorkspaceStore]] and the iQue workspace evidence document.
