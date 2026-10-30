# FlowJo shared analysis fold-in — #808

Status: active; end-to-end integration is owned by F in `feat/808-flowjo-realignment`, including facade, bridges and both product mounts. No main/develop merge.

## Current implementation status (2026-10-23)

The older continuation sections below are historical checkpoints, not current stop
gates. F owns branch-local integration, with E producer a26d26ba and I metadata
baseline5e9ec3c5 pinned. Current ABI47 / Review9 preserve earlier reservations.

Implemented and locally validated: E live/archive fences, retained snapshots,
source-bound immutable strategies and exact method lookup, durable real Cytoflow
jobs, manual tri-state QC, ordered parameter transforms,2–16-feature t-SNE,
applications/overrides, native tables/statistics, descriptive comparisons/findings,
plots/multisource overlays/reports, libraries/discovery and form/scatter graph editing.
Intervals/rectangles/axis-aligned ellipses/quadrants/Boolean parents share native
membership, exact gallery identities and worker eligibility. Tailnet app remains
available; source fixtures and mock camera are explicitly labeled.

Latest evidence and remaining implementation obligations are in the capability
ledger and app-qa.md. Browser connector inventory is empty, TSan cannot start on
this host, actual hardware-budget qualification and E image locators remain open.
These do not block other independent implementation. Full F01–F16 is NOT accepted;
interchange, native polygon/rotated geometry, broader workspace/batch/statistical
work and qualification still need completion. No main/develop merge is authorized.

## Goal

One full F01–F16 scientific analysis module used by Studio and offline Review,
including the #314 research loop. Export to FlowJo is not a substitute.
The user authorized execution of the coordinator prompt on 2026-10-22.

## First deliverable

- [Capability ledger](../../design/flowjo-analysis/capability-ledger.md)
- [Pinned evidence catalogue](../../design/flowjo-analysis/evidence-catalogue.json)
- [Contract proposals and owners](../../design/flowjo-analysis/contracts.md)

## Base and ownership

Dedicated branch `feat/808-flowjo-realignment`, based on freshly fetched
`develop` at `0a58e201614bdb1eeb412544be60859f93220de7`. This branch owns backend and UI work together (no merge into develop is authorized). Historical UI audit uses the freshly fetched `feat/egrabber-workspace` at
`7133749621defac01a80985fe6a3a5fc3f13eae1`; this is an audit reference, not a current UI branch gate.
The latter happens still to equal the historical audit snapshot; no checkout
was reset. Existing dirty checkout and other worktrees are untouched.

Current ownership additionally includes new `src/backend/analysis/**`, matching
headers, `tests/analysis/**`, `workers/cytoflow/**`, `crates/mib-analysis-worker/**`,
`env/analysis-worker/**`, the new analysis CI workflow and matching vault notes.
The coordinator lifted shared-file allocation gates for this branch. F owns necessary
production wiring and generated contracts here; other tracks may edit the same paths
in their own branches. Preserve bridge 45 / Review 7 reservations; this slice adds
bridge 46 / Review 8 without implementing or claiming the reserved capabilities.

## Bounded dependency plan

1. F-A / #523 A01–A03: agree versioned records and source ownership; native
   bounded source projection and fingerprint/relink; portable manifest and
   revisioned command history; qualified real Cytoflow adapter and supervisor.
   Validate exact keys, faulted saves, lost notifications, cancel/crash/restart
   and memory limits. Distribution qualification is a separate release gate.
2. F-B / FJ04/FJ06/FJ08: population graph/unknown logic, existing LUT semantics,
   exact membership/images and exact reductions. Test boundaries, cyclic graphs,
   mismatched units, >2^53 identities, absent images and zero denominators.
3. F-C / L01/S01–S03: frozen unbiased windows, overrides/resync and saved findings
   linked to explicit next-run drafts. Depends on #806 source and #807 metadata
   contracts; analysis must not block finalization. Concurrency/TSan and actual
   acquisition-budget evidence are required, not implied by process separation.
4. F-D / FJ05/FJ08/FJ10/FJ11/FJ12: gallery, reusable tables, overlays, portable
   preferences and reports with immutable runs. Round-trip/fault/cancel/batch
   name collision tests; exact versus histogram-derived statistics labeled.
5. F-E / FJ02/FJ03/FJ07/FJ09/I01: qualified QC, compensation, reproducible
   embedding, design-aware comparison/fits and FCS/GatingML interchange.
   Unsupported channels/constructs are explicit; advanced scope remains open.
6. F-F: two immutable recordings → gate → exact images → strategy + override →
   compare → save/reopen → report, then full ledger acceptance including live,
   invalidation and fault cases. Separate module demo and integration handoff.
   No final three-track composition, merge or deployment under this prompt.

## Reconciliation of existing tasks

- #801: reuse R2 per-run table/small multiples from `review_run_statistics`;
  retain quick-look parity. Temporary worker Unavailable labels are truthful
  milestones, not permission to leave F04/F08/F09 unavailable indefinitely.
- #802: reuse R6 export controls/jobs. File export does not satisfy F10 reports
  or F15 import/interchange. Preserve existing options.
- #803: retain attachable facade bridge / shared ReviewSession and bridge-45
  reservation; no second reader or independently reserved ABI. Live snapshots
  need a distinct safe provider, not removal of acquisition guards.
- #804: retain Review-7 passthrough options; add analysis jobs only through a
  later coordinated contract, not mislabeling reanalysis as population analysis.
- #805: retain eventual one-module host mount and prerequisites #803/#804.
  App.tsx and backend/Review mounts are now in scope within this branch; final multi-track merge is not.
- #523/#314 retain the full scientific/research scope. WORK-SPLIT's unscheduled
  R4/R5 and October-15 presentation trim are history, not the new scope ceiling.
  ADR 0015's one-file-per-sample and explicit application rules still apply;
  no inferred chapter boundaries or silent amendment of acquisition policy.
- #520 is reusable per-detection FCS export; #553 retains React export,
  time-precision and qualified per-cell obligations. No duplicate issue created.

## Explicit blockers / resume conditions

- **Source semantics:** live E adapter still needs epoch-qualified event keys, typed
  batch watermarks, unknown-denominator propagation and measurement/QC separation.
  These are implementation requirements, not ownership/permission blockers.
- **Worker integration:** pinned Cytoflow 1.3.2 builds and runs in an isolated
  Python 3.12 environment after building Logicle with SWIG and initializing the
  pinned fcsparser submodule. The configured package index did not resolve the
  version directly. Bare RangeOp rejects equal endpoints; boolean inversion admits
  missing-value placeholders. The MIB adapter implements native scalar and three-valued composition semantics.
  The supervised Cytoflow worker still needs a source-snapshot job submission path from the app.
- **Shipping:** owner/release maintainer records distribution decision, notices,
  source/patch obligations and fixture rights. Not resolved by this audit.
- **Live acceptance:** unbiased committed provider + safe acquisition coexistence
  and authorized hardware evidence; current Review Start exclusion is preserved.
- **Design evidence:** live page revision pinned, but projection excludes prototype
  interactions; historical 214 interactions are not fresh live verification.

## Acceptance criteria

- [x] All F01–F16 retain explicit disposition, owner package and remaining evidence.
- [x] Original research and 20-board/three-flow inventory recovered and hash-pinned.
- [x] #801–#805 reconciled without taking over shared files or ABI reservations.
- [ ] Peer contracts and ownership agreed.
- [ ] Real adapter and module independently demonstrated.
- [ ] Two-source milestone passes; all F01–F16 have full acceptance evidence.
- [ ] Review findings resolved and integration handoff delivered.

## Decision log

- 2026-10-22: Preserve Cytoflow and full scope; retain old native primitives,
  current acquisition guards and both research evidence versions. No production
  authority implemented from proprietary renderer/schema declarations.
- 2026-10-22: Historical tests are recorded as inspected, not rerun. A local
  reference verifier may prove the bounded v10 evidence independently; it cannot
  establish MIB module implementation, v11 parity or general histogram estimators.
- 2026-10-22: Owner directed continued execution without check-ins. Corrected
  the over-broad stop: at that time shared interface/file edits awaited coordination (superseded below).
  F-owned code now provides native inclusive scalar/three-valued composition,
  exact-key/revision acceptance, real Cytoflow polygon adapter, Unix bounded
  process supervision and immutable terminal job recovery. The worker-private
  journal is not the shared workspace store. Native ASan/UBSan, eight Python
  adapter tests and six Rust tests (including real engine/native persistence)
  pass locally; CI/independent review and full host/module acceptance remain open.

### Native graph continuation

Added bounded dependency-ordered PopulationPlan evaluation over qualified masks:
parent and Boolean edges validated together, exact revisions/keys, QC clipping,
cycle/dangling/arity/stale-input refusal and no hidden subsampling. Two native
executables pass normally and with ASan/UBSan. This extends F04, not full gate
editing/application override/undo acceptance. I workspace boundary acknowledged;
source read-lease/generation gap sent directly to E with precise fields.

### Clean-run qualification

Analysis CI run 39762481872 passed at `1278cfe5` after correcting pip-isolated
SWIG native-binary/library discovery and adding FastLogicle import preflight.
Existing macOS Review parity test failed at `review_bridge.rs:694` with only
0.038 vs 0.038000000000000006 and 0.053 vs 0.053000000000000005 overlay differences;
exact evidence sent to the E/F/I coordinator for shared #716 ownership, without
editing/regenerating gold references. Source seam is now specified in
`docs/design/flowjo-analysis/event-snapshot-contract.md`; lease/generation
allocation remains the immediate integration dependency, not missing scope.

### Continued implementation during coordinator QA

QA handoff is not a stop condition. Added independent F08 native population
reductions with revision-qualified feature/unit columns, exact-key alignment,
explicit membership/missingness accounting, raw min/max/mean/median and bounded
input refusal. Three native executables pass locally under ASan/UBSan. No shared
files, ABI, source authority or host mounts changed; no integrated UI claim.
App exposure remains blocking required scope. Typed-feature wiring and committed live EventSnapshot integration remain implementation
work; the coordinator subsequently removed the shared-file allocation gate. Offline pagination still requires an injected
whole-read generation fence; per-call ReviewSession locking is insufficient.
Reusable tables/export, percentiles/CV, comparison and the remaining F01–F16
acceptance are still open, not discharged by this native milestone.

### End-to-end branch ownership and first app integration

Coordinator direction: F integrates in its own existing dev branch, including
shared-named files. All prior F commits are ancestors; no cherry-pick or reset was
needed. This continuation mounts the same bounded native interval/statistics UI
in Studio and YOFO Review. ReviewSession reads both cached metadata sets under
one lock; ReviewJobs excludes active job lanes; BackendFacade additionally uses
the existing idle transaction and raw-recording exclusion. #722 is unchanged.
Bridge 46 and Review 8 are additive; reserved 45/7 are not repurposed.

The first app slice intentionally exposes **stored** area (px²) and deformability,
not inferred E/calibration or fabricated completeness. It refuses unreadable sets,
header/row count mismatch, raw recordings, >10,000 rows and stale source paths.
Results identify local evaluations only: fingerprint null, durable false. Absent metadata columns and nonfinite values are unknown, never default zero.
Detailed per-event missing reasons remain unexposed. It is not F01–F16 acceptance, saved workspace/DAG editing,
Cytoflow polygon execution, live evidence consumption or verified relinking.

See [app QA handoff](../../design/flowjo-analysis/app-qa.md) for real runtime and
synthetic fixture distinction. Scope remains the original capability ledger, not
a reduced interval-only replacement.


## 2026-10-30 continuation

Native polygon vertical slice implemented and tested through persisted strategy,
React form events, Review reopen/gallery and real Studio/Cytoflow WSS parity.
Current evidence and remaining scope are in capability-ledger.md. Continue
interchange/workspace-batch work; this checkpoint does not complete F01–F16.
