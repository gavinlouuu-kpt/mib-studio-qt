## 2026-10-23 — workspace lifecycle and transfer

I adds real-app workspace listing, title/archive controls, complete-document
round-trip, persisted annotation grouping, pinned export jobs, validated create-only
import and full-SHA256/size-checked source relink. Reuses the metadata SQLite owner,
receipt/CAS transaction and native SHA implementation; sources remain read-only.

Native host tests and a real WebSocket/SQLite probe cover damaged import, duplicate
jobs, mismatched relink, missing old path replay and server restart recovery.
Scientific integration remains a separate pinned-provider task, not implied by
valid recipe syntax or successful source hashing. See [[services/WorkspaceStore]]
and `docs/design/ique-workflow/CONTINUATION-LEDGER.md`.
