## 2026-10-23 — Analysis applications and findings

AnalysisHost now derives source-bound strategies from exact upstream revisions and
explicit interval overrides. It persists immutable descriptive comparison findings
in the existing SQLite authority. AnalysisComparison uses native unit-level endpoints,
not cell rows as experimental replicates; duplicate source identity is refused.
Production controls live in AnalysisWorkspace. API and evidence are documented in
`docs/design/flowjo-analysis/production-api.md` and `app-qa.md`.
Native host and UI tests/build pass; actual tailnet backend two-source round trip
passes. Browser automation unavailable; no hardware or inferential qualification.
