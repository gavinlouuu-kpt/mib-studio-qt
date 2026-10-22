# iQue / Forecyt realignment (#807)

Status: I-A1 bounded annotation slice implemented; complete fold-in blocked on
shared analysis, workspace persistence and acquisition qualification. No scope
removed. I13 remains the explicit chapter extension.

The [capability ledger and dependency plan](../../design/ique-workflow/README.md)
is the execution record, including all I01–I12, source/design pins, existing
implementation anchors, v1 contract proposals, file ownership and blocker owners.
The [demo and evidence limits](../../design/ique-workflow/DEMO.md) distinguish
UI fixtures from real backend/hardware acceptance.

## Decision log — 2026-10-22

- Owner executed the coordinator prompt. Restored recipes, annotations,
  criteria, comparisons and QC trends are required, not reference-only.
- Verified current workspace head `71337496`; preserve current develop
  `0a58e201` fixes as qualification dependencies. No final composition/merge.
- Recovered original concept file revision 71 and live fold-in inventory 284.
- Published full ledger and contracts on #807; coordinated exact file scope
  with #806/#808/#716. Acknowledge E immutable source/evidence and F-contracts/1
  authority semantics; concrete analysis/store API shapes remain dependencies.
- Implement annotation grouping as a pure projection of existing metadata,
  using existing components and Start serializer. Do not add a statistics
  engine, recorder, identity authority or localStorage workspace store.
- I-A1 tests are UI/contract-projection evidence only. Full I01 acceptance
  also needs persistent annotation presentation and downstream result/export
  linkage; I02–I12 remain covered by the ledger, not silently deferred.

## Validation

Workspace Vitest suite: 519 tests across 23 files passed. Focused annotation
and Plan/Queue suite: 83 passed. TypeScript + Vite production build passed.
`scripts/check_docs.py` and `git diff --check` passed. Existing jsdom navigation
tests emit canvas-not-implemented diagnostics despite passing; these are not
native-webview validation. No hardware/TSan/scientific parity claim.
