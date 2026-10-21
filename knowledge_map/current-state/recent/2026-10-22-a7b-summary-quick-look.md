## 2026-10-22 — Workspace Run summary (O5) and Quick look (O6) (#716 A7b)

The opt-in workspace's Experiment · Summary sub-view, on the workspace branch, over A7a's run
quick look (bridge ABI 44 `run_quick_look`). No backend or contract change. Per the issue's A7
rulings:

- **O5 Run summary** for a closed run of the plan (the single sample once its operation reported a
  good end, or a queue row once the backend finalized it): the outcome, the quick look's state,
  descriptive flags as grey chips, the medians as `StatTile`s with the backend's deltas, "% in
  declared target group", the event rate, the counts and accounting, and the provenance from the
  identity. `deferred` reads "computed when the instrument is idle" and is read again when the
  instrument turns idle; a raw recording "Not available until exported to HDF5"; null "Not
  recorded"; a recorded 0 stays 0. No verdict colours and no recommendation text.
- **O6 Quick look**: the plan's closed runs side by side, the operator's reference run (none by
  default; held by `usePlan` with the plan, cleared by New plan, not persisted), the backend's
  deltas only (withheld ones with the reason, `differs` flagged), and the overlay on
  `AreaDeformChart` (the reference in slot 5, chosen runs in their condition's slot, "display
  only"). During a run: earlier runs from the backend's cache and the active run's status counts,
  "numbers at close".
- **Decisions**, each one step: Repeat sample (a new queue event `repeat`: the row goes back to
  planned as the current row, its run kept in `QueueRow.history`, its attempts and lock kept, so
  the next Start sends the same sample_ref with the next run index and a new file; between samples
  only), Adjust next (Align or Plan, navigation only), Continue (confirms the finalized row's close,
  then shows the Plan), Open in Review (the classic Review tab with the file, through App's
  `loadReviewSource(path)`; not during a run). A viewer's acting decisions are disabled with the
  shared control block.
- **Reads, never polled** (`useQuickLook`): on showing the Summary, when the plan's closed runs or
  the reference change, on the idle transition for a deferral, and on Refresh.
- **No client arithmetic**: `quickLookModel.test.ts` reads the model, the view and the hook and
  refuses arithmetic on the quick look's numbers.

Docs: WORK-SPLIT B2 and the FORECYT metrics row now say R2's per-run numbers come from the shared
function (`review_run_statistics`) and the #523 worker keeps populations, CV and fits (A7 ruling
11, Q1 (a)). See [[../../architecture/Desktop-Shell]] (Opt-in workspace → Experiment · Summary) and
[[../../services/RunQuickLook]].
