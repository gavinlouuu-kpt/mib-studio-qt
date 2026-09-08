Title: Backend-owned experiment coordinator + bridge lifecycle API (BE-4, issue #274)

Context:
- Moves the experiment orchestration the Qt frontend performed in
  `MainWindow::onStart/onStopExperiment` + `ExperimentController` behind one
  backend command/state model, so React never re-implements safety-critical
  save behavior. Bridge schema **v5** (additive). Runs on the BE-1
  operation-state primitive.

Implementation Notes:
- **`backend::ExperimentCoordinator`** (`src/backend/app/ExperimentCoordinator.cpp`,
  header in `include/backend/app/`): state machine Idle→Active→Stopping→Idle/
  Failed. `start()` validates preconditions atomically with Qt-parity
  messages (processing-core pin, camera running, HDF5 openable), performs the
  multi-image async_batch→inline realtime-mode override (restored at stop),
  and spawns a worker thread. The worker runs the periodic flush loop
  (buffered ≥ flush interval → `flushBufferedFrames`) and, on stop/cancel/
  fatal, the finalization sequence: final flush → `finishFlush` (drain write
  queue, writer stopped) → append remainder → `H5Fflush` → mandatory
  metadata/provenance (`writeExperimentInfo`, only after data flush) → config
  JSON → close. Cancel finalizes the file too (marked cancelled). Fatal save
  errors funnel via `onFatalSaveError` → Failed. `shutdown()` is idempotent
  and joins the worker so close never corrupts the file.
- **Accounting** — `ProcessingService` gained `totalInvalidFlushed_` (exact,
  incremented on the writer thread) so `writeExperimentInfo` totals are
  flushed + remainder (the Qt path wrote remainder only); status exposes
  buffered/saved/dropped counts (captured = processed + explicitly dropped).
- **Facade** — `ExperimentCommand{Start,Stop,Cancel,Status}` (command type 6),
  `ExperimentStatusEvent` (event kind 8), `fetchExperimentStatus` pull; the
  running experiment is a tracked operation (result carries `operationId`,
  terminal Completed/Failed/Cancelled emitted via the BE-1 registry); fatal
  save errors emit `BackendError{Experiment}`; camera StopCapture is rejected
  while an experiment is active (Qt parity); facade shutdown finalizes the
  experiment before cancelling remaining operations.
- **Bridge/Tauri/TS** — ABI **v5**: `experiment_start/stop/cancel`,
  `fetch_experiment_status`; Tauri commands + typed TS client; the shell's
  Start/Stop Experiment buttons, telemetry sidebar Experiment section, and
  status bar are now backed by real backend state.

Verification:
- `ctest e2e.experiment_coordinator` — facade-driven e2e + fault injection:
  Qt-parity precondition messages, double start/stop, camera-stop blocked,
  cancel path, fatal-save injection → Failed with readable file, shutdown
  during active experiment leaves a loadable HDF5.
- `mib-bridge cargo test experiment_lifecycle_end_to_end` — mock E2E start →
  accumulate → stop → reopen via `load_recording`; operation Completed event.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups:
- Experiment export variants land with the BE-6 job APIs.
- Windows TSan/stress lanes remain the CI gate for the threaded paths.
