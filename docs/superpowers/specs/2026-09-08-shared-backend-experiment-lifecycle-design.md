# Shared backend experiment lifecycle and contract hardening (design)

Status: active
Date: 2026-09-08
Owner: Agent A (reliability backend). Serves issue #372 gaps G2/G3 (and G5 in part), epic #371, epic #246.

## Goal

One backend owns the whole experiment lifecycle so that the Qt window and the
React/Tauri bridge are two clients of the same object. No Qt timer, widget, or
dialog is needed for a run to start, record, stop, finalize, and report its
terminal accounting outcome. The bridge contract then pins that surface.

Evidence this is needed (bench, 2026-09-08): the window-owned finalization
appended stop-time frames outside the accounting (clean run labelled
IntentionallyPartial) and a modal held the HDF5 open for 108 s. Both were fixed
in the window, but the ownership was the defect.

## Non-goals

No new exporter, no configuration seam (G4), no recovery snapshot beyond the
status pull (G5 is only partially served), no science or Contract-v2 changes,
no Qt removal, no merge of `dev/react-tauri` in this work (the migration
coordinator is deleted at that merge, later).

## Architecture

`ExperimentCoordinator` (existing, `include/backend/app/ExperimentCoordinator.h`)
stays the single authority for readiness, Start, and the frozen
`RunConfigurationSnapshot`, and additionally owns:

- a worker thread that runs the periodic flush while Active and the whole
  finalization on Stop, so Stop never blocks the caller;
- a status snapshot and a status callback fired on every transition;
- the fatal save-error funnel;
- idempotent bounded shutdown.

`AppBackend` owns the coordinator (as today). `BackendFacade` and `MainWindow`
call it; neither touches `Hdf5Service` for experiment finalization anymore.

## Coordinator API (additive)

```cpp
enum class ExperimentRunState { Idle = 0, Starting = 1, Active = 2, Stopping = 3, Failed = 4 };
// Values are the bridge contract's experiment_states. Append only.

enum class ExperimentStopOutcome { Accepted, NotActive, Busy };

struct ExperimentStatus {
    ExperimentRunState state;
    uint64_t startGeneration, readinessGeneration, captureGeneration;
    std::string outputPath;
    uint64_t startWallClockNs, endWallClockNs;      // 0 until known
    uint64_t validBuffered, invalidBuffered;        // live, from ProcessingService
    uint64_t persistenceAdmitted, persistenceCommitted, persistenceFailed;
    bool flushing;
    bool cancelled;
    bool terminal;                                  // finalization finished
    bool finalizationOk;                            // every finalize step succeeded
    backend::recording::RunCompletionState completion; // Unknown until terminal
    std::string completionReason;
    std::string faultCode, faultMessage;            // unresolved fault, if any
};
using StatusCallback = std::function<void(const ExperimentStatus&)>;

ExperimentStopOutcome requestStop(bool cancelled);
ExperimentStatus status() const;
void setStatusCallback(StatusCallback cb);        // non-blocking consumers only
void onFatalSaveError(const std::string& message); // from the flush queue
void shutdown();                                   // bounded, idempotent
```

`start()` is unchanged in signature and semantics; on success it starts the
worker's periodic flush loop (interval from `ProcessingService` flush policy,
as the window did) and publishes `Active`.

### Finalization sequence (worker, on `requestStop`)

1. `Stopping` published.
2. `ProcessingService::flushBufferedFrames(hdf5)` then `finishFlush()` (drain
   the async write queue, writer thread stopped).
3. `ProcessingService::endExperiment()`; `resetRealtimeMetrics()`.
4. Remainder that arrived between 2 and 3: `flushBufferedFrames` +
   `finishFlush` again (credits `persistenceCommitted`).
5. `Hdf5Service::flush()`; `writeExperimentInfo(...)` (times, remainder
   counts as today, processing config, ROI, background, core identity);
   `writeRunAccounting(experimentAccountingSnapshot())`; run provenance;
   config JSON.
6. `closeFile()`.
7. `finish()` releases the frozen snapshot; restore the realtime mode if the
   run switched it for multi-image series.
8. Terminal status published: `Idle` with `terminal=true`, `completion` from
   `reconcile()`, `finalizationOk` false if any of 2, 4, 5, 6 failed; a failed
   finalization publishes `Failed` and reports an unresolved fault
   (`experiment.flushFailed` / `experiment.provenanceFailed`) exactly as the
   window did.

`onFatalSaveError` during Active marks `Failed`, then runs the same sequence so
the file is closed and readable; the terminal status keeps `Failed`.

### Concurrency rules

- `start`, `requestStop`, `shutdown` may be called from any one command thread;
  they take the coordinator mutex briefly and never wait on the worker while
  holding it.
- Duplicate `requestStop` while Stopping returns `Busy`; when Idle returns
  `NotActive`. `start` while Stopping returns `Busy` (existing outcome).
- The status callback is invoked outside the mutex and must not block (same
  rule as the facade event sink).
- `shutdown()` requests a stop if Active, waits for the worker (bounded by the
  final flush), and joins it. Called from `AppBackend::shutdown()`.

## Facade surface (`BackendFacade`)

```cpp
enum class ExperimentCommandAction { EvaluateReadiness = 0, Start = 1, Stop = 2, Status = 3 };
struct ExperimentCommand {
    ExperimentCommandAction action;
    std::string outputPath;          // Start
    uint64_t readinessGeneration{0}; // Start
    std::string profileId;           // Start / EvaluateReadiness
    bool acknowledgeLatestFrameDrops{false};
    bool cancelled{false};           // Stop
};
struct ExperimentStatusEvent { ExperimentStatus status; };  // BackendEvent kind 8
bool fetchExperimentReadiness(ExperimentReadinessSnapshot& out, const std::string& outputPath = {}, const std::string& profileId = {}) const;
bool fetchExperimentStatus(ExperimentStatus& out) const;
```

`BackendCommandResult` gains `experimentStartOutcome` and
`experimentStopOutcome` (typed enums, `Unknown` default) so acceptance,
rejection, and completion are distinct. `BackendCommandType::Experiment = 6`
matches the contract. The facade subscribes to the coordinator status callback
in `initialize()` and forwards each status as an `ExperimentStatusEvent`.

## Qt window as client

- `onStartExperiment`: unchanged preflight and dialogs; calls
  `coordinator.start()` as today.
- `onStopExperiment`: `requestStop(false)`; UI shows "Stopping…".
- New slot `onExperimentStatus(const ExperimentStatus&)` (queued from the
  callback to the GUI thread): updates the run-status model and status label;
  on `terminal`, raises alerts / latches failure for `Failed` or
  `IncompleteLoss`, shows the deferred accounting dialog, restores buttons.
- Removed from the window: the finalize `QFutureWatcher`, every
  `Hdf5Service` call in the stop path, the realtime-mode restore (moved to the
  coordinator), the periodic flush timer if the window still owns one.
- `closeEvent` calls `AppBackend::shutdown()` which calls
  `coordinator.shutdown()`; the "closing during experiment" confirmation stays.

## Contract hardening (bridge branch, second PR)

Append to `crates/mib-bridge/contract/bridge-contract.json` (ABI 13):
`experiment_command_actions`, `experiment_start_outcomes`,
`experiment_stop_outcomes`, `run_completion_states`,
`readiness_gate_statuses`; `experiment_states` unchanged. Shim
`static_assert`s pin the C++ enums; `contract.rs` pins Rust; the generator
renders TS. `ExperimentStatusEvent` and the readiness snapshot are carried as
typed JSON payloads with decimal-string u64 identities (PR #375 convention),
never `u0..u5` reinterpretation. The bridge `experiment_*` commands map onto
`ExperimentCommand`; the Rust side stops calling the migration coordinator.

## Testing

- `backend.experiment_readiness` (extend): start → requestStop → terminal
  Complete with the stop-time remainder committed and 0 pending; fatal save
  error → Failed with a readable, closed file; requestStop when Idle →
  NotActive; second requestStop while Stopping → Busy; start while Stopping →
  Busy; shutdown while Active finalizes.
- `backend.facade_boundary` (extend): EvaluateReadiness → Start → status
  events (Starting, Active, Stopping, terminal) → Stop → fetchExperimentStatus,
  with the fake lifecycle camera and no Qt.
- Frontend lane (19) and `kin6_mib_app_capture_proof` unchanged and green.
- Bench: HF mock recording run reconciles Complete through the window;
  bridge `experiment_lifecycle_end_to_end` passes on Windows after the
  contract PR.

## Documentation

`knowledge_map/architecture/ExperimentCoordinator.md`,
`knowledge_map/architecture/BackendFacade` (or `Data-Flow`) note,
`knowledge_map/frontend/MainWindow.md`, `Recent-Work`, and
`docs/exec-plans/active/2026-09-08-agent-a-handoff-372.md` (G2, G3 closed;
G5 partially).
