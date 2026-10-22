# E01–E09 instrument-workspace fold-in (#806)

Status: active — first inventory and contract proposal; no full acceptance claimed.

## Baseline and ownership

- Verified remote workspace head: `7133749621defac01a80985fe6a3a5fc3f13eae1`.
  This happens to equal the historical audit baseline; it was selected by fetching
  `origin/feat/egrabber-workspace`, not by assuming the old audit is current.
- Also inspected current develop `0a58e201614bdb1eeb412544be60859f93220de7`.
  The workspace contains 144 commits outside that develop and lacks six develop
  commits. Do not reset it or promote it to develop. #716/#799 reserve promotion
  for Gavin. This slice targets the workspace branch, with no ABI change.
- Branch `feat/806-egrabber-fold-in`, dedicated worktree; shared checkout and all
  existing worktrees preserved. Existing open PRs checked on 2026-10-22.
- First implementation ownership: `desktop/src/discovery.ts`, its adjacent test,
  the discovery service vault note, this plan, the uniquely named E capability
  ledger and recent-work fragment. No App.tsx, review, kit, generated contracts,
  server, recorder, or classic-view files are claimed.
- #796–#798 remain externally delegated under #799. #807 owns experiment
  orchestration; #808/#717 own analysis and the shared scientific inspector.
  #806 owns device/frame truth, not a second recorder or inspector.

## First deliverable

[Capability ledger and proposed contracts](../../design/egrabber-workspace/E-CAPABILITY-LEDGER.md)
records all nine IDs, all 37 design-view assignments, code/test anchors, known
implementation owners, evidence limitations and unresolved dependencies.

## Bounded dependency plan

1. E-A1: validate discovery response identity before accepting completion. Reuse
   the existing asynchronous job adapter, keep IDs lossless, test stale job and
   generation replies before production edits. No new service, ABI or UI design.
2. E-A2/E-B: agree the SourceCapabilities/typed feature extension with #716 and
   the bridge coordinator. Add the provider-qualified service within the existing
   camera session owner, then its bridge and typed React controls. Do not open a
   second SDK device or implement selector exploration as UI write loops.
3. E-C: qualify existing acquisition/preview/writer and ring paths. Agree missing
   state/retention fields before extending them. Prove independent freeze,
   buffer allocation stop-without-auto-resume, finalization, export and faults.
4. E-D: finish #713's backend no-path/no-write proof; integrate externally owned
   #796–#798 without recreating them. Capability-qualified LED/focus/storage and
   firmware operations require real adapters and existing authorization.
5. E-E: after the shared contracts are acknowledged, run the complete scenario
   with a real backend and authorized supported hardware. Hand off exact evidence
   identities to #808 and outcomes to #807; no final three-track composition,
   deployment or cross-track merge in this prompt.

## Contract coordination gate

The ledger's proposals are reviewable field/semantic requirements, not assigned
ABI numbers. Existing ABI and reader authorities remain unchanged. Await
acknowledgement from #716/bridge coordinator and #807/#808 before editing their
shared contract files. Local adapter corrections using existing contracts may
proceed independently. Missing acknowledgement blocks shared-contract changes,
not the local inventory, regression tests or adapter fix.

## Validation

- Regression first: discovery fault cases must fail at the pinned base.
- Run focused discovery and workspace tests, the full desktop Vitest suite,
  TypeScript/Vite build, reference prototype checks and `scripts/check_docs.py`.
- No C++/thread/save changes in E-A1; backend lifecycle/TSan/storage gates belong
  to later changes that touch those authorities, not a claim made by TS fixtures.
- Full acceptance additionally requires contract round-trip/fault, pipeline and
  concurrency accounting, latency/invariants, and qualified hardware runs.
- Independent code review is a separate milestone; self-review is not labelled
  independent review. No hardware action is authorized merely by this plan.

## Decision log

- 2026-10-22: use the verified workspace head after checking current ownership;
  preserve the live-beta promotion hold. Classic view remains the default;
  this task does not choose final Studio navigation.
- 2026-10-22: October 15 trims are history, not removal of required functionality.
  Chapters are capability-qualified under E06; R4/R5 and scientific evaluation
  stay with #808, not reference-only forever and not reimplemented by E.
- 2026-10-22: original REA is available and read; it establishes static evidence,
  not hardware behavior. Penpot live metadata still reports revision 124/0,
  5,830 shapes and 37 root children. Native routing remains unverified.

## E-A1 result and continuation

- Inventory/plan/proposals delivered, with ownership notices on #806/#716 and
  consumer notices on #807/#808. No shared DTO or ABI was changed.
- E-A1 production fix: wrong-job/generation replies rejected losslessly; a
  coalesced waiter's timeout no longer cancels another caller's discovery.
- Six regression cases proved failing before their fixes. Final validation:
  1,442 desktop tests passed, one skipped; 15 discovery tests passed; TypeScript
  and Vite build, 14 offline prototype checks, docs and whitespace checks passed.
- Self-review complete for the narrow slice; independent review and CI are not
  inferred from local test results. Draft PR remains unmerged.
- **Full fold-in blocked:** session-owned feature contract/ABI agreement
  (#716/bridge coordinator), peer evidence/outcome compatibility (#807/#808),
  externally owned classic-view delivery (#799), and supported hardware
  qualification (operator + #806). E02, E07 and all other named gaps remain in
  the ledger; no capability has been removed or declared accepted.
- Resume from this worktree/branch after the corresponding owner acknowledgement
  or evidence arrives. Do not restart existing coordinators or replace their work.
