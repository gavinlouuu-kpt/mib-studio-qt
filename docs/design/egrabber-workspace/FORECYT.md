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
| Plate map with annotation layers | **Sample sheet**: single sample, manual-swap queue (one file per sample), or time segments (chapters). Typed layers: condition, experimental unit, concentration + unit. One colour registry. Plate/rack formats wait for an autosampler device class. | Experiment → Plan (P4), ADR 0015 |
| Template = acquisition + analysis | Method (acquisition, approved via #398) and analysis plan (gates/metrics, #523) stay separate. A recipe pairs them by reference. Add "new draft from this run's recorded configuration". | P4 / P7 |
| Worklist with time estimate | Run plan: stop condition × rate × payload → duration and size. SSD estimates use measured fresh vs sustained rates (#667). | Prepare (P2) |
| Resources sized to the run | Storage budget gate. Sample volume as a labelled *estimate* (no level sensors are fitted). | P2 |
| Run dialog with step checklist | Lifecycle stepper Starting → Active → Stopping → Drain → Finalizing → Closed/Error. Separate Stop-acquisition / Freeze / Stop-&-finalize. Next sample. | Experiment (P4) |
| Protocol-scoped overrides | "N settings differ from method X" banner. Requested vs readback. Recorded in snapshot v3. Reverted only after the run ends. | P3 / P4 |
| Automatic gates frozen during acquisition | Adaptive gates are frozen while a run is active and re-evaluated in review, with old → new shown. | P5 |
| Inferred well boundaries | **Rejected.** Chapters are recorded at acquisition. Review flags them (loss, low n) but never moves them. | ADR 0015 |
| Linked selection across plots | Full event identity (run, frame, object, revisions) as u64 strings, linking scatter, table, image and mask. Requires the single review implementation (#512). | P5 |
| Small multiples per well | Small multiples per run, chapter or annotation group. | P5 |
| Multi-criteria hit finding with live hit rate | Extra AND terms in the population rule: polygon AND E interval AND feature ranges, with a live "N of M (x %)". Analysis-only unless explicitly deployed to a sort. | P5 |
| Metrics builder with inline quality flags | Per-population / per-chapter n, median, mean, CV of area, deformability and E, computed by the #523 Cytoflow worker. Inline flags: low n, loss in chapter, E outside LUT domain, legacy file. | P5 |
| Multi-plate comparison (later given refresh) | **Compare**: a multi-run workspace, live by default. Incompatible runs are greyed out with the reason (chip, LUT, contract, metric definition). | P7 |
| Dose-response with "suspect fit" flags | Fits on per-unit summaries only, with declared unit, pairing and weighting. Suspect-fit reasons shown inline. | P7 |
| Audit trail, lock, e-signatures | Journal, lock and change reasons continue under #393/#310. **E-signatures / Part 11 are out of scope.** | — |
| Bead QC with trend charts | **Not adopted.** That instrument needs bead runs to check optics and fluidics. MIB readiness is already covered by calibration (pixel-to-micron, background) and method-validation gates. | — |
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

## Audit findings to fix independently

A code audit at `ac2ffba7` found these, independent of the workspace redesign:

1. **When the last remote client disconnects, pumps keep running.**
   - The server stops the experiment, stage and recording and idles the instrument, but not the
     pumps (`crates/mib-bridge-server/src/lib.rs`, `stop_and_save`).
   - This must be fixed before unattended queues.
2. **The E-modulus LUT identity is not recorded with runs.** Fixed by snapshot v3 (ADR 0015).
3. **MIB Studio's Review tab does not mount `review/ReviewPanel.tsx`.** The panel's header comment
   says it does. Resolve via #512 before adding review features.
4. **React Prepare does not show the authoritative backend readiness gates.** They are evaluated
   only inside Start, after the file is picked.
5. **Readiness evaluation with a path writes a probe file every time**, and an HDF5 round trip
   every 30 s. It should be cached; any benchmark should be explicit and idle-only.
6. **The Preflight confirmation signature omits** PL build/weights, pump/stage identities, profile
   id and storage path.
7. **The frontend checklist has no Unknown/Unsupported state.** "Not read yet" shows as failed,
   and a required warning blocks.
8. **The PZ7035 ingest drops PL `runId`/`epoch`.** Fixed by ADR 0015.
9. **FCS export never fills `$MIB_RUN_ID`.**
10. **The operator is always empty in run provenance.** To be fixed with #393 selected operator.
