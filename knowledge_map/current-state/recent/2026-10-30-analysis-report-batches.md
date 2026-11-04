## 2026-10-30 — Durable cooperative report batches

AnalysisReports owns1–16 pinned layout queues in the existing SQLite authority.
Each step produces one immutable run then CAS progress; deterministic run IDs allow
recovery after report publication but before progress commit. Cancelled/completed
states do not advance. Cancellation is between bounded native calls, not preemptive.
No restart scheduling, worker thread or separate database is introduced.

Real React controls drive/reopen native progress. Tests cover complete/cancel,
stale revisions, unavailable pins, SQLite reopen, injected progress-commit failure,
and cancellation interleaved with a build. Live WSS proves1/2 cancelled with absent
pending run and2/2 completed with exact hashes. Native6, Review8, sanitizer4, UI29
pass. Full F scope and rendered-browser/platform qualification remain open.
