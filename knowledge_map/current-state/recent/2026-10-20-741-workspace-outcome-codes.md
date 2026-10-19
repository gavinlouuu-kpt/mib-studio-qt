## 2026-10-20 — Workspace queue and Plan on ABI 40 outcome codes (#741)

Part 2 of #741, on `feat/egrabber-workspace` (track A, desktop only; no contract change). **Start:**
the Plan's and the queue's start handling branches on the reply's `start_outcome`
(`experimentStart.ts` `startReplyState` / `START_OUTCOME_STATES`), not on its message. Started →
started; StaleReadiness, AlreadyActive, Busy, InvalidRequest → refused (returned before the
coordinator begins the run: no generation, no file; the identity and the run index may be sent
again); NotReady, StorageFailed, ProvenanceFailed → unknown with the code, which locks the identity
like a lost reply, because the coordinator can return each after it opened the run's HDF5 file
(StorageFailed from `initializeDatasets` leaves the file; ProvenanceFailed removes it best effort;
NotReady also covers the PZ7035 "execution provider did not start" after the file and snapshot were
written). A code that contradicts `ok`, or an unmapped one, is unknown. Without a code (a backend
before ABI 40) `ok` decides and the `invalid start request: ` prefix stays the InvalidRequest
fallback. A locking code is said "Not started · its file may exist" (single plan: locked, New plan;
queue: chip "Not started · file may exist", Retry as the next run index). **Stop:** the queue's Next
sample / Stop last sample (`usePlan.stopCurrent` → App `stopForPlan`) pass the row's own
`start_generation` as `expectedStartGeneration`; the classic Stop Experiment sends none, and the
single plan has no stop. A `stop_outcome` RunMismatch stopped nothing: the row is interrupted at the
stop ("Interrupted · not stopped", the backend's reason) and nothing more is sent for it; a later
status of the row's own run Stopping or terminal is still followed to Finalized. Busy, NotActive and
a reply without a code keep the "stop was not accepted" handling. See [[architecture/Desktop-Shell]]
("Opt-in workspace"). Guards: `experimentStart.test.ts` (every contract code, written out),
`queueModel.test.ts`, `QueuePlan.test.tsx`, `PlanView.test.tsx`, `navigation.test.tsx` (the App over
a mocked ABI 40 backend: exact stop arguments, RunMismatch, the classic Stop, a reply without codes).
Mutation: dropping the generation in `usePlan` fails 8 tests, in App's `stopForPlan` 4.
