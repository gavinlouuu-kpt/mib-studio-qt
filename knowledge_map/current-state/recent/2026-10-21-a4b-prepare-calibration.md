## 2026-10-21 — Prepare's calibration card (#716 A4b)

The workspace's Prepare stage gets a Calibration card over the A4a commands (bridge ABI 42; no
backend or contract change). It shows the `calibration.record` gate and its Fix, the record in
effect (id, factor, age, method, identity, source run), an applied record that is no longer in
effect and why, every proposed record (current → proposed, applicable here or not, with the
reason), the newest insufficient records with their failed gates (never applicable), and a plain
list of every record (`calibration_records_list`, polled with the readiness while Prepare or the
Plan is shown and no run is active; `calibration_record_get` for a selected record's versions).
Apply / Re-apply is its one action: `calibration_apply(record_id, expected_current_factor)` with
the factor the page last read; the reply's previous → new is shown, every refusal (stale factor,
run active, insufficient, another setup, unknown identity, viewer) is said in plain words and
changes nothing but the re-read, a list read older than the apply's answer is dropped, a viewer's
button is disabled with the shared control block, and navigation, listing and polling never send
it (IPC calls counted). After a restart a `required` method's gate says "re-apply record <id>":
the card offers that Re-apply right beside the Fix (PR #771 ruling). The Plan (single and queue)
declares `calibration_record_id` only as the record it showed as in effect, nothing when none is,
and says "The applied calibration changed since this page read it" when the backend refuses the
Start for a mismatch, keeping the plan. Running a calibration stays the normal Plan/Start run;
the card says what is derived when it closes. Gap: the bridge reports no method purpose, so the
workspace cannot say itself whether the applied method is a calibration method (the card says so;
the gate row carries the backend's words). See [[architecture/Desktop-Shell]] (Opt-in workspace),
[[services/CalibrationRecords]]. Guards: `calibrationModel.test.ts`, `CalibrationCard.test.tsx`,
`planModel.test.ts`, `queueModel.test.ts`, `PlanView.test.tsx`, `QueuePlan.test.tsx`,
`navigation.test.tsx`.
