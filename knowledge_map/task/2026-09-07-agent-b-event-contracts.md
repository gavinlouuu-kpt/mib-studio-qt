# Agent B: exact event contracts

Branch: agent-b/event-contracts, stacked after the atomic frame slice (#374).
Separate worktree/build outputs; no Agent A/backend-domain/Qt file edits.

Regression-first: the existing production client receiving a native JSON number
returned operation ID 9007199254740992 instead of 9007199254740993. The passing
adapter now validates decimal strings and delivers named event variants.

Implemented: cxx ABI 12 exact companions preserving old slots; versioned event
JSON; command/experiment/seek/cancel identity preservation; explicit non-finite
processing metrics; centralized component event decoding; no inferred elapsed
clock or false Complete from saved-row counts. Experiment Start is disabled
with the missing-authoritative-readiness reason; existing stop remains available. Native fixtures use the production
marshaler through a test-only feature, never a frontend fallback.

Evidence/limits: `docs/architecture/event-json-v1.md`. The previous #374 native
Desktop CI is green (run 34085079050, 12 Rust tests). This branch has 116 passing
frontend tests and a passing production build locally. New native fixture tests
must run on this branch's CI before native contract acceptance.

Remaining owner dependency: authoritative readiness/config/finalization/recovery
snapshot/retained-outcome handoff from Agent A. Additional Review/monitoring/
autofocus DTO integers remain to migrate; do not claim all M1 integers complete.
M2 operation reconciliation and M3 workflow/layout/drafts remain independent next
work. Accepted-backend native M4 experiment and Qt comparison remain blocked.

Related: [[../architecture/Desktop-Shell]], [[../architecture/Rust-Bridge]].

Delivery: draft PR #375, published implementation commit
`f66a111f1433c52aed6a351000a143eebedb7489` (tree
`08da89e96bee95ee6a8d8b2eb1ad074a51a2c28a`, identical to local
`540e18b72cf008a7e0828207c0cd88b8a6802d30`). Desktop CI run
34086418024 is pending. Bridge CI now also targets staging and develop, so the
existing full cxx contract suite runs on these branches rather than only the
Tauri crate tests. No test categories are substituted by this trigger change.
