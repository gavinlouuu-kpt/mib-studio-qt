## 2026-10-22 — Run summary (O5) and Quick look (O6) follow their Penpot boards (#716 A7c)

A UI-only follow-up to A7b on the workspace branch, once the O5/O6 boards were readable again (the
sandbox file's Operation app page). No backend or contract change.

- **Separate sub-views.** The Experiment sub-nav is Plan · Live · Run summary · Quick look
  (`RunSummaryView.tsx`, `QuickLookView.tsx`, shared parts in `summaryParts.tsx`). Each reads only
  on its own transitions and Refresh: the Run summary its one run (no reference), Quick look the
  plan's closed runs with the reference. The run they inspect is held by `ExperimentView`.
  Toolbars as the boards: O5 Next sample · End queue; O6 Next sample · Open in Review.
- **O5 Outcome**: a lifecycle checklist (Start requested · Acknowledged · first frame seen · Active ·
  recording · Stopping · Drain · finalizing · Confirmed closed), each step from what the page
  observed: the queue now records, per attempt, what the run's own statuses showed (`RunSeen`:
  Active, a first frame, Stopping, an accepted Next sample; observations only, no transition uses
  them). A step not observed reads unknown, never ticked; a single sample (followed by its
  operation only) has its status steps unknown. The stop reason as observed, the duration only from
  the terminal status's recorded start and end ("Not recorded" otherwise), the file, the experiment
  / run, the writer.
- **O5 without medians**: Outcome · Counts · Provenance written, then the run's display-only
  snapshot ("display only, the Review app does the analysis") and Next. The board's "no statistics
  on the instrument (#501)" now reads "statistics: Quick look (descriptive) and the Review app".
- **O5 Next**: the next sample by name, Next sample, Repeat this sample, End queue (the queue's own
  `end`, after its Confirm closed on a finalized row; no new queue event). For a calibration run (the
  method purpose from the Start's readiness read, ABI 43, now carried on the start outcome; or a
  record naming the run) the record derived from it, matched by `source.run_id`: current →
  proposed, the CV and the gates; Apply and Dismiss lead to Prepare's Calibration card; nothing is
  applied from O5. Without a record: "No calibration record was derived for this run" and why.
- **O6**: the board in plan order with grey "Queued" placeholders (no numbers) for runs not yet made,
  mini scatters from the backend's overlay points, and "Refine the experiment" beside the overlay
  (Continue · load S<n>, Repeat, Adjust next sample…, Open in Review (PC)), descriptive helper text
  only.

**Kept deviations from the boards, and why** (rulings, unchanged):

- The overlay is the backend's point subsample, not KDE contours (A7 Q4 a: no density estimate on
  the instrument).
- "% in declared target group", not "% in Population A" (A7 Q2 a: the run's own declared group, no
  polygon).
- No recommendation text (A7 ruling 6): the board's "Run 03 looks different from the vehicle and
  clean" is not shown; a test greps the sources and the rendered views for assessment wording.
- No client-side arithmetic on the quick look's values (the pin now covers both views and their
  shared parts); deltas only from the backend.

See [[../../architecture/Desktop-Shell]] (Opt-in workspace → Experiment · Run summary and Quick
look) and [[../../services/RunQuickLook]].
