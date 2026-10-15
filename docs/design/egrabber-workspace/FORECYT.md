# Lessons from plate-based high-throughput cytometry software

Status: design note; decisions recorded in [ADR 0015](../../decisions/0015-experiment-run-chapter-identity.md)
and the [execution plan](../../exec-plans/active/2026-10-14-egrabber-workspace.md) decision log.

On 2026-10-14 we studied a mature high-throughput cytometry package, Sartorius iQue Forecyt, from
its public product pages, reference guide, brochures and demo videos. The aim was to learn workflow
patterns that work at scale. This note records:
- which of those patterns fit MIB;
- where each one belongs in the Prepare / Align / Experiment / Review workspace;
- which ones we rejected.

It is a design input, not a vendor specification. No vendor assets or text are reproduced.

## What carries over

The other product does three things well:

1. **One application from setup to answer.** There is no export/import step between acquisition,
   analysis and the per-sample summary.
2. **Run metadata is first-class.** Sample annotations become groups, colours, table columns and
   export columns automatically.
3. **Every analysis change propagates live.** Moving a gate updates every downstream plot, table,
   hit count and fit.

MIB adopts the three, within its own rules:
- backend truth only;
- additive contracts;
- immutable recordings;
- analysis never actuates hardware;
- full review on a PC for the PZ7035.

## Where each idea lands

| Idea | MIB form | Stage / package |
|---|---|---|
| Plate map with annotation layers | **Sample sheet** with a fixed small schema (sample, condition, experimental unit, concentration + unit, notes, free key/values), not a layer system. Single sample or manual-swap queue (one file per sample); time segments deferred with chapters. Plate/rack formats wait for an autosampler device class. | Experiment → Plan (P4), ADR 0015 |
| Template = acquisition + analysis | Method (acquisition, approved via #398) and analysis plan (gates/metrics, #523) stay separate. "Base on previous experiment" copies the sheet and method reference. Recipe pairing deferred. | P4 |
| Worklist with time estimate | Run plan: stop condition × rate × payload → duration and size. SSD estimates use measured fresh vs sustained rates (#667). | Prepare (P2) |
| Resources sized to the run | Storage budget gate. Sample volume as a labelled *estimate* (no level sensors are fitted). | P2 |
| (MIB, not iQue) live detected-cell strip + click-to-see-cell during a run | **Kept.** Already in Qt Monitoring; React port is a P4 item. Drawn on the Live view. | Experiment (P4), views 02/21 |
| Run dialog with step checklist | Lifecycle stepper Starting → Active → Stopping → Drain → Finalizing → Closed/Error. Separate Stop-acquisition / Freeze / Stop-&-finalize. Next sample. | Experiment (P4) |
| Protocol-scoped overrides | "N settings differ from method X" banner. Requested vs readback. Recorded in snapshot v3. No auto-revert (draft-vs-applied already covers it). | P3 / P4 |
| Automatic gates frozen during acquisition | Adaptive gates are frozen while a run is active and re-evaluated in review, with old → new shown. | P5 |
| Inferred well boundaries | **Rejected.** A sample change is a new run; chapters (recorded, never inferred) are deferred until a mid-run change is actually needed. | ADR 0015 Amendment 1 |
| Linked selection across plots | Full event identity (run, frame, object, revisions) as u64 strings, linking scatter, table, image and mask. Requires the single review implementation (#512). | P5 |
| Small multiples per well | Small multiples per run or annotation group, consumed from the #523 worker. | P5 → #523 FJ05 |
| Multi-criteria hit finding with live hit rate | **Not built in the shell.** The shell keeps polygon AND E with a live "N of M (x %)"; multi-criteria gating is the analysis tool's job. View 05 is reference-only. | #523 FJ04 |
| Metrics builder with inline quality flags | Per-run / per-population n, median, CV from the #523 worker, shown with inline flags (low n, loss, E outside LUT domain, legacy file). The shell computes nothing. | P5 → #523 FJ08 |
| Multi-plate comparison (later given refresh) | **Not in the workspace plan.** Owned by #314 ComparisonSpec / #523 FJ09. The workspace only records the identities that make comparison possible (snapshot v3). View 06 is reference-only. | #314 / #523 |
| Dose-response with "suspect fit" flags | Analysis-tool feature; per-unit fits with declared unit/pairing/weighting and inline suspect-fit reasons when #523 FJ09 is built. | #523 FJ09 |
| Audit trail, lock, e-signatures | Journal, lock and change reasons continue under #393/#310. **E-signatures / Part 11 are out of scope.** | — |
| Bead QC with trend charts | **Adapted as a calibration run**, trimmed. A run of rigid beads produces an instrument-scoped calibration record (size → pixel→µm proposal; offset and E-vs-nominal check-only). The operator applies it explicitly, previous vs new shown; readiness checks its age per method policy. Past records are a plain list — no trend chart. | Prepare (P2), ADR 0015 item 7 |
| Editions (instrument / offline / server) | Already equivalent: Studio on the instrument, YOFO Review offline ([0014](../../decisions/0014-yofo-review-on-react-tauri.md)), central registry and remote viewer. No licence split. | — |

Avoid also:
- dense, icon-only toolbars;
- a separate window for comparison;
- one-way comparison snapshots;
- destructive edits without undo;
- a closed data store that analysis definitions cannot leave.

## Concept boards

Seven concept boards were drawn during the study in the internal Penpot sandbox, as the file
"YOFO Studio – Forecyt-inspired workflow concepts" (Setup, Preflight, Run, Segment, Review,
Compare, plus a concept map). They are sketches in a different visual style and use only
illustrative data.

Where they disagree with this workspace's [README](README.md) or [PENPOT.md](PENPOT.md), the
workspace wins. In particular:
- axis naming: **Deformability**;
- E LUT curves are shown;
- separate stop actions;
- Unknown / Unsupported readiness states.

Access follows the same private-origin rule as PENPOT.md.

## Proposed views in the workspace visual language

After the owner decisions, the concept boards were redrawn in the same visual language as
boards 00–36 of the sandbox source (header, 01–04 stage tabs, toolbar row, card grid, teal/grey
palette, "proposed wireframe / illustrative data" band). They live in a **separate sandbox
file** so the reviewed source file (see [PENPOT.md](PENPOT.md)) is untouched:

| Reference | Value |
|---|---|
| File | **PROPOSED · MIB Studio fold-in (eGrabber + iQue lessons) · 2026-10-14** · `2c3c14b3-8959-40f7-a29c-2e6202e341b6` |
| Page | `202e7953-c6e1-8109-8008-ca01abeab5bf` (revision 54 at 2026-10-15) |
| Access | same private-origin rule as PENPOT.md |

| View | Board UUID |
|---|---|
| 00 · Fold-in map | `8b742b76-d59d-4cf7-ae47-de4f40f3d46f` |
| 01 · Prepare · readiness, run plan, calibration record (P2) | `14a7d154-9465-4525-b781-ec51ed7edc3d` |
| 03 · Experiment · Plan — sample sheet, one file per sample (P4) | `d6bbb202-e10c-4a77-ad10-b49b8ded7c0b` |
| 03 · Experiment · Live — queue run, lifecycle stepper (P4) | `bfba229d-ee75-4fd9-af76-3cf47a3215ce` |
| 04 · Review · Run overview — recorded chapters, per-sample statistics (P5) | `2b5022ad-4791-4710-a352-f3bb4cbb8545` |
| 04 · Review · Scatter inspector + population criteria (P5) | `be010d21-d507-4422-8f1a-61b0e48b6ecc` |
| 04 · Review · Compare — multi-run workspace (P7) | `5cd0129c-264a-4861-bd23-21b7cff7b280` |

All values are illustrative. Where these views and the README/PENPOT.md disagree, the
README wins; the views add the Plan sub-view, the queue/lifecycle elements, the chapter
overview, population criteria and the Compare workspace that ADR 0015 and P7 introduce.
Rendering was checked with `penpot_render`; native prototype links are not installed.

## Audit findings to fix independently

A code audit at `ac2ffba7` found these, independent of the workspace redesign. Findings 5–7 are tracked under #713, finding 10 under #393 and #712:

1. **When the last remote client disconnects, pumps keep running** (#711).
   - The server stops the experiment, stage and recording and idles the instrument, but not the
     pumps (`crates/mib-bridge-server/src/lib.rs`, `stop_and_save`).
   - This must be fixed before unattended queues.
2. **The E-modulus LUT identity is not recorded with runs.** Fixed by snapshot v3 (ADR 0015, #712).
3. **MIB Studio's Review tab does not mount `review/ReviewPanel.tsx`** (noted on #512). The panel's header comment
   says it does. Resolve via #512 before adding review features.
4. **React Prepare does not show the authoritative backend readiness gates** (#713). They are evaluated
   only inside Start, after the file is picked.
5. **Readiness evaluation with a path writes a probe file every time**, and an HDF5 round trip
   every 30 s. It should be cached; any benchmark should be explicit and idle-only.
6. **The Preflight confirmation signature omits** PL build/weights, pump/stage identities, profile
   id and storage path.
7. **The frontend checklist has no Unknown/Unsupported state.** "Not read yet" shows as failed,
   and a required warning blocks.
8. **The PZ7035 ingest drops PL `runId`/`epoch`.** Fixed by ADR 0015 (#712).
9. **FCS export never fills `$MIB_RUN_ID`** (#712).
10. **The operator is always empty in run provenance.** To be fixed with #393 selected operator.
