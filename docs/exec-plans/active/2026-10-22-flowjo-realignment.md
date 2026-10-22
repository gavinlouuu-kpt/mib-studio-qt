# FlowJo shared analysis fold-in — #808

Status: active; audit deliverable prepared, shared implementation awaits contract ownership agreement.

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
`develop` at `0a58e201614bdb1eeb412544be60859f93220de7`. Backend work belongs
on develop. UI audit uses the freshly fetched `feat/egrabber-workspace` at
`7133749621defac01a80985fe6a3a5fc3f13eae1`; future view slices target that branch.
The latter happens still to equal the historical audit snapshot; no checkout
was reset. Existing dirty checkout and other worktrees are untouched.

Current ownership is limited to `docs/design/flowjo-analysis/**`, this plan,
`tools/flowjo_acceptance/**` and a new recent-work note. No shared ABI numbers,
production services, UI kit or host composition files claimed.

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
  Shared App.tsx ownership and final composition are outside this change.
- #523/#314 retain the full scientific/research scope. WORK-SPLIT's unscheduled
  R4/R5 and October-15 presentation trim are history, not the new scope ceiling.
  ADR 0015's one-file-per-sample and explicit application rules still apply;
  no inferred chapter boundaries or silent amendment of acquisition policy.
- #520 is reusable per-detection FCS export; #553 retains React export,
  time-precision and qualified per-cell obligations. No duplicate issue created.

## Explicit blockers / resume conditions

- **Shared contracts:** #806/#807 and #512/#717 owners acknowledge or amend
  F-contracts/1 and concrete production file ownership before shared edits.
- **Worker integration:** pinned Cytoflow 1.3.2 builds and runs in an isolated
  Python 3.12 environment after building Logicle with SWIG and initializing the
  pinned fcsparser submodule. The configured package index did not resolve the
  version directly. Bare RangeOp rejects equal endpoints; boolean inversion admits
  missing-value placeholders. The MIB adapter must implement its declared scalar
  and three-valued composition semantics; no production adapter is implemented.
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
