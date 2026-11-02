## 2026-10-23 — production workspace recovery controls

I branch adds explicit history/CAS restore and saved-sheet loading into a fresh
experiment identity. Loading refuses a locked/starting single plan or in-progress
queue, resets queue/reference/result state, and never invokes acquisition commands.
Pending writes are journaled to per-tab sessionStorage before transmission; reload
and panel remount offer byte-identical explicit retry, never automatic replay.
Storage failure prevents transmission. This journal is not a database substitute
and does not survive closing the browser tab.

Pinned recipe references now round-trip with the production SQLite document and
are editable as fresh recipe identities with lineage to the opened saved recipe.
Compatibility remains unknown: this branch has no scientific plan resolver or
provider integration. No physical readiness or scientific compatibility inferred.

See [[services/WorkspaceStore]] and `docs/design/ique-workflow/PRODUCTION-INTEGRATION.md`.
