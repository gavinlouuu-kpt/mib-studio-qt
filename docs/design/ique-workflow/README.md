# iQue experiment workflow — capability ledger

Coordination: #807. Owner direction: 2026-10-22. Status: implementation in progress;
**no complete capability or operational acceptance claimed**. I13 alone is deferred.

## Current own-branch integration

The active dev branch is `feat/807-ique-workspace-core`; it already consolidates
all published I slices. Shared-named production files are owned here for end-to-end
integration. See [production evidence and concrete remaining work](PRODUCTION-INTEGRATION.md).
The following baseline and allocation entries are historical, not current gates.

## Baseline and preservation

Current remote workspace head verified on 2026-10-22:
`7133749621defac01a80985fe6a3a5fc3f13eae1`. This happens to equal the historical
audit baseline; it was selected because it contains the reusable O/R workspace,
not because the prompt requested a reset. Current develop was `0a58e201` and
contains later #792/#793 transport/race fixes; those remain a dependency for live
qualification, not discarded work. Branch `feat/807-ique-workflow` targets
`feat/egrabber-workspace`. No promotion, deployment or three-track merge.

The main checkout has unrelated uncommitted work and is untouched. Existing
worktrees/sessions were not resumed, cancelled or replaced. Open PR inventory
had no iQue implementation PR; #769 and other unrelated work remain untouched.

## Source catalogue and design pin

The recovered Forecyt study dated 2026-10-14 comprises synthesis, reference-guide
notes, document notes, video notes, original seven concepts, and the subsequent
fold-in review. This is public-document/video research, **not native Forecyt
runtime verification**. Raw vendor bundles and private source locations are not
published here. Source labels below refer to this authorized evidence catalogue:

- RG: reference-guide study, sections Design/annotation, Metrics, Profile Map,
  Panorama, curve fitting, QC history, templates and experiment export.
- V: video-study workflows, especially Profile Maps, precise metric building,
  Panorama and dose-response; automated transcripts are secondary to frames.
- C: original seven concepts, recovered live file
  `6a808aca-eb00-48c2-a75a-9b5d7058a42b`, revision **71**, vern 0, page
  `202e7953-c6e1-8109-8008-c9e9185c8df0`.
- F/O/R: fold-in, Operation and Review pages in live file
  `2c3c14b3-8959-40f7-a29c-2e6202e341b6`, revision **284**, vern 0.
  [design-pin.json](design-pin.json) records the native board inventory read.
  This does not validate rendered appearance or native prototype navigation.

Recovered discrepancy: FORECYT.md's old Live board UUID is superseded by the
live inventory's `368c693c-68ea-42be-92a0-2553f704b158`. Historical documents are
preserved, not rewritten as if they had always carried the restored scope.

## Capability ledger

Paths below are relative to the repository (UI paths under `desktop/src/`).
Existing tests are evidence anchors, not claims that they ran in this audit.

| ID | Research / board | Existing code, PR and tests | Disposition / owned issue | Acceptance gap |
|---|---|---|---|---|
| I01 | RG Design; C Setup; F Plan; O3 | `workspace/planModel.ts`, `PlanView.tsx`, `QueuePlan.tsx`; #736/#739; `planModel.test.ts`, `PlanView.test.tsx`; backend `run_start_identity_test.cpp` | Extend #807 with #314 metadata semantics | Planned annotation grouping/table/JSON projection and durable layer selection implemented; native source-bound findings now persist. Broader acquired-result continuity remains partial. Independent-unit identity must never be inferred from rows/cells. |
| I02 | RG templates; C Setup; O3 | `profiles.tsx`, `CentralMethodsPanel.tsx`, backend `MethodApply.cpp`; #398; no recipe pairing | Restore #807/#398; consume #808 plan references | Versioned association/lineage, persistence, exact method lookup and pinned F evaluation implemented; acquisition compatibility remains unknown and applying methods is separate. |
| I03 | RG Protocol/worklist; C Preflight; O1/O3 | `prepareModel.ts`, `usePrepareReadiness.ts`, `planParts.tsx`; #727/#548; `prepareModel.test.ts` | Extend #807; readiness authority #806 | Readiness exists. Time/storage/volume forecasts, provenance/freshness and complete worklist not accepted. |
| I04 | RG Run; C Run; O3/O4 | `queueModel.ts`, `usePlan.ts`, `QueuePlan.tsx`, `experimentStart.ts`; #739/#744/#750; queue model/view tests | Reuse/extend #807; #741 closed | Manual one-file-per-sample queue and explicitly saved historical run observations exist. Automatic restart resumption is not supported; live fault qualification outstanding. |
| I05 | RG Run + linked selection; C Run; O4 | `LiveView.tsx`, `useLiveCells.ts`, `liveImages.ts`; #749; `LiveView.test.tsx`, `liveModel.test.ts` | Reuse/extend #807, image authority #806 | Strip/pins/lifecycle exist; full revision-qualified live answers, reconnect and hardware proof outstanding. Develop #793 must be incorporated before qualification. |
| I06 | RG analysis propagation; C Review; O5/O6/R2 | `QuickLookView.tsx`, `useQuickLook.ts`, `runQuickLook.ts`, backend `RunStatistics.cpp`; `run_quick_look_test.cpp`, `review_run_statistics_test.cpp`, `summaryViews.test.tsx` | Extend #807; science #808/#523 | Existing results are closed-run descriptive answers, not population/live analysis. Keep their backend authority; add linked next-action persistence. |
| I07 | RG Metrics; V metric builder; C Review; R4 | `review/metricsColumns.ts`, `components/ResultMetricCells.tsx`; #523 FJ08 | Restore/missing #807 UX; #808 evaluator | No population × parameter × statistic builder, typed expression validator/evaluator or revision-bound result API. |
| I08 | RG Profile Map; V criteria; F criteria/R4 | Analysis criteria UI and common evaluator absent; existing acquisition target is NOT a substitute | Restore #807 UX; #808/#523 FJ04 evaluation | Polygon AND E AND ranges, declared missing policy, linked selected values and exact hit denominator all required. Never deploy sort gates. |
| I09 | RG Panorama; V multi-plate; C Compare; F Compare/R5 | Single-file review and batch exports; no refreshable ComparisonSpec authority | Restore #807/#314 UX; #808 science | Compatible-run picker with reasons, refresh, small multiples/heatmaps and scale choice absent. |
| I10 | RG curve fitting; V dose response; C Compare/R5 | No qualified fit result API; #523 FJ09 | Missing #807 presentation; #808 fit authority | Unit/pairing/weighting/model and suspect-fit reasons required; no frontend fit or invented curve. |
| I11 | RG QC history; C Preflight; O1 | `CalibrationCard.tsx`, `calibrationRecords.ts`, backend `CalibrationRecord.cpp`; #716 A4; calibration model/view and backend tests | Extend/restore #807 history UX; backend owner #716/#806 | Recorded history/setup-filter/selection implemented in #813; existing explicit CAS apply reused. Versioned trend thresholds/reversible exclusions missing; focus remains not-recorded when absent. |
| I12 | RG experiment export/templates; C Review; O6/R6 | `usePlan.ts` deliberately holds plan/reference in memory; #393/#395 store not implemented here | Missing #807 continuity; shared persistence #393/#395 and #808 | Native SQLite lifecycle, production save/reopen/CAS restore, full-document preservation, findings/run observations, atomic definition export/import, source relink and pending-write reload recovery implemented; see CONTINUATION-LEDGER.md. Rendered browser/Tauri qualification remains open. No replacement localStorage authority. |
| I13 | C Segment | ADR 0015 Amendment 1 | Explicitly deferred #314 | Requires real mid-run use case and additive writer contract; never infer boundaries from event rate. |

## Scope reconciliation

The October 15 trim and WORK-SPLIT's reference-only criteria/Compare are historical,
not this delivery's ceiling. Owner execution of #807 restores annotation layers,
recipe pairing, functional criteria/Compare, QC trends and continuity. Backend
scientific ownership stays unchanged. No automatic override reversion,
autosampler fiction, e-signature commitment, or implicit hardware apply is added.

## Bounded dependency plan and ownership

1. **I-A1 (#807):** annotation grouping/table projection over existing sample
   sheet fields and free key/value metadata. Own new `workspace/annotation*`
   files, narrow mounts in `PlanView.tsx`/`QueuePlan.tsx`, their tests, this
   directory, a recent-work fragment and Desktop-Shell vault update. No ABI,
   writer, shared kit, App, scientific or lifecycle changes.
2. **I-A2 / I-E (#807, #398, #393/#395):** agree recipe/finding/workspace
   persistence and analysis-plan reference contracts; implement draft lineage,
   atomic save/reopen/export, fault handling and journal through that authority.
3. **I-B (#807/#806):** worklist forecasts from backend facts, qualified freshness,
   expected request/run identity and durable reconciliation. Preserve coordinator
   start/stop; test unknown start, disconnect, finalize failures and restart.
4. **I-C (#807/#808):** shared result adapter then live answers, metrics and
   criteria views. Test stale replies and exact identities with versioned
   fixtures, then real backend parity; no frontend scientific evaluator.
5. **I-D (#807/#808/#716):** comparison, fit rendering and QC trend workflow over
   shared qualified results and calibration history; test missing/incompatible
   units, low independent n, reversible exclusions, no implicit apply.
6. Review each slice; run three-sample/two-condition scenario first on fixtures,
   then real acquisition/analysis adapters. Include fault/restart and hardware
   evidence. Only then hand off to final Studio composition (outside #807).

## Contract proposals — v1, not ABI reservations

These adapt existing authorities. Peer agreement is required before shared-file
implementation; publication here does not mean the peers have accepted them.

- **AcquisitionPlan/RunOutcome (#806):** retain ABI 40 start outcome and expected
  run stop, snapshot-v3 experiment/sample/unit metadata. Add durable request-key
  reconciliation via the coordinator, with accepted/refused/unknown distinct;
  no automatic retry of an unknown start. Need source generation, finalization,
  accounting and authoritative freshness for worklist estimates.
- **AnalysisPlan/Result (#808):** opaque id+revision plan reference, result
  revision, source fingerprint/run/session/epoch + committed watermark,
  processing/calibration revisions, exact membership handle, universe,
  population/metric identities and qualified values/null reasons. Counts and
  64-bit identities remain strings. Criteria and expression validation belongs
  to F. A reply is usable only for the requested snapshot and plan revision.
- **Recipe/Finding (#807/#398/#393/#395):** versioned association of immutable
  acquisition method id+revision and separate analysis plan id+revision; source
  finding/result references, new draft identity and lineage. Copy never applies
  hardware or overwrites original references. Compatibility is shown before an
  explicit method apply through the existing authority.
- **ComparisonSpec (#807/#808):** versioned selected runs, declared experimental
  units, condition/pairing/weighting, concentration units, metric/model refs,
  missing-value policy and pinned result revisions. F returns eligibility and
  exclusion reasons, summaries/fits and suspect reasons; I renders and refreshes.
- **Workspace/ExportJob (#393/#395/#808):** one versioned store for definitions,
  references, findings, annotation display groups, exclusions and journal;
  expected-revision writes, atomic failure semantics, relink and terminal export
  recovery. No analysis writes to source HDF5 and no second client-only store.
- **QC (#716/#806/#808):** reuse calibration list/get/apply and compare-and-set;
  add versioned threshold/exclusion definitions to the agreed workspace store.
  Scientific trend classification uses the shared evaluator, not TS.

## Blocking dependencies and acceptance status

Continuation after I-A1: I11's recorded-measurement history now consumes the
existing calibration list/get path, with per-record limits and verdicts, missing
focus/flow, setup/method-revision filtering and linked record selection. No
scientific recomputation or actuation. Persistent exclusions/new policies remain
required. [Native qualification](NATIVE-EVIDENCE.md) now supplies 9 passing
real-backend checks with synthetic/mock inputs, distinct from hardware evidence.

The owner directed continued execution while actionable work remains. Dependency
gaps below limit full acceptance, not independent I-owned implementation/testing.

I-A1 is now implemented in the claimed files, with a version-1 synthetic
annotation fixture, real Plan/Queue mounts and [reproducible demo](DEMO.md).
Workspace suite: 519/519 tests, 23 files; focused tests: 83/83. TypeScript/Vite
and docs validation pass. This is partial I01 evidence, **0/12 complete
capability acceptances**, with no native-webview/hardware/scientific claim.
See also the [user guide](../../manual/ique-annotation-groups.md).

The I coordinator has acknowledged the E immutable snapshot/evidence and
F-contracts/1 authority semantics on the peer issues. Exact callable API shapes and real adapters
are still required. Existing #803 bridge 45 / #804 Review 7 reservations are
preserved; this slice reserves no ABI.

- **B-F, #808/#523:** required shared analysis plan/result, metrics/criteria,
  comparison/fit interfaces and real adapter are absent at this baseline. Owner:
  FlowJo coordinator. Resume when versioned contracts/fixtures and callable
  backend are supplied. I07–I10 and full I06 cannot pass meanwhile.
- **B-W, #393/#395 with cross-track coordinator:** isolated injected-SQLite core
  and I persistence ownership are established; actual host metadata lifecycle,
  operator authorization, facade/provider allocation and document validation remain
  unbound. Export/relink require shared job/source seams, not a second exporter.
  Blocks complete I02/I12 and durable I04/annotation presentation state. TSan
  startup mapping failure remains a native acceptance blocker.
- **B-E, #806/#716:** authoritative estimates, full source-generation contract,
  live reconciliation and hardware qualification remain. Develop's later fixes
  must reach the qualification base. Blocks complete I03–I05 acceptance.
- **B-Q, #716/#808:** threshold/exclusion persistence and qualified trend result
  API missing. Existing calibration application is not complete I11 acceptance.

No missing dependency removes an ID. A fixture demo is not operational proof.
The I-A1 slice can progress independently because it only projects existing
annotations and neither evaluates science nor creates a persistence authority.

## Recipe consumer milestone

The isolated `recipeModel.ts` / `RecipePairing.tsx` slice implements immutable
method/analysis associations, fresh unsaved draft copy with lineage, and exact
pair/setup-revision compatibility matching. It never saves, evaluates, applies or
starts a run. Version-1 fixture/demo and malformed/stale/copy tests accompany it.
The [workspace proposal](WORKSPACE-CONTRACT.md) specifies one #393/#395 authority;
actual store and live provider binding remain open, so I02/I12 are not accepted.
Annotation PR #811 passed all four CI checks; QC PR #813 has only the gold guard
triggered on its stacked base, so local checks are reported separately from CI.

## Native persistence core milestone

The isolated [WorkspaceStore qualification](WORKSPACE-EVIDENCE.md) now implements
real SQLite snapshot CAS/history/restore/idempotency on an injected host connection.
Round-trip, rollback, restart, concurrent CAS and ASan/UBSan pass. TSan cannot start
on this host (mapping failure); host binding, export/relink, schema validation and
full I12 acceptance remain open. The cross-track coordinator owns shared facade
allocation; no shared host/build/ABI files were edited.
