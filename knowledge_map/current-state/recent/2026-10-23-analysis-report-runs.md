## 2026-10-23 — Immutable analysis report runs

AnalysisReports handles saved exact-reference layouts and all-or-nothing immutable
runs through the existing SQLite workspace owner. AnalysisReport renders the frozen
artifact in Studio/Review and exports standalone escaped HTML or evidence JSON.
Native tests cover layout CAS, run collision/replay and restart independence; real
tailnet backend two-block build/reopen passes. UI regression tests cover lost reply
and title escaping. Browser renderer/export visual verification remains unavailable.
See docs/design/flowjo-analysis/production-api.md for schemas and limits.
