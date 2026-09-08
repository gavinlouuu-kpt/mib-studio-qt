# Controllers

> Thin `QObject`-based controllers that translate UI actions into
> [[../architecture/AppBackend]] calls and emit Qt signals for the UI.

**Source:** `src/frontend/controllers/`, `include/frontend/controllers/`

## CameraController

`CameraController(AppBackend& backend)` — **the single authoritative UI
command path for camera acquisition** (issue #360). Owned by [[MainWindow]];
every presentation (tab-bar corner buttons, Space-bar toggle in
`PlaybackPanel`, the screenshot tour's `onStartCapture`, script-apply
restarts) dispatches through it and derives its enabled/text state from one
`CameraActionState` snapshot.

- `setOperationGuard(fn)` — read-only provider of `CameraOperationBlock
  {blocked, reason}` from the actual experiment/recording/flush owner
  (MainWindow's `experimentActive_`/`flushInProgress_`/`isFrameRecording()`).
  It runs on **every** stop request, including direct dispatch while the
  button is disabled — no shortcut can bypass experiment finalization. The
  old `force` parameter is gone.
- `requestStart()` / `requestStop()` / `requestToggle()` → typed
  `CameraCommandResult {Accepted | AlreadyInState | Blocked | Failed,
  message}`. One command in flight at a time; rapid duplicates are
  `Blocked`/`AlreadyInState` and issue no backend command.
- `state()` → `CameraActionState {phase: Idle | Starting | Running |
  Stopping | Failed | NotConfigured, generation, startEnabled, stopEnabled,
  stopBlockedReason, failureMessage}` projected from
  `CaptureService::lifecycleSnapshot()` (authoritative — start acceptance is
  not hardware readiness) by a bounded poll (250 ms; 20 ms while a session is
  Starting/Stopping) plus an immediate refresh after each command. A camera
  that fails or disconnects on its own shows as `Failed` with the backend's
  structured `lastFailureMessage`.
- `startAction()` / `stopAction()` — shared `QAction`s (`startCameraAct`,
  `stopCameraAct`; texts "Start Live View" / "Stop Camera") whose enabled
  state and tooltip (the blocked reason) follow the snapshot. Buttons bind to
  them; they must not carry their own guard logic.
- Signals: `stateChanged(CameraActionState)`, `cameraStarted`,
  `cameraStopped`, `commandFailed(QString)` (refused/failed command, operator
  text).
- Legacy wrappers `startCapture(QString*)` / `stopCapture(QString*)` map to
  the typed API.
- Test: `tests/frontend/camera_action_state_test.cpp`
  (`frontend.camera_action_state`) — identical outcome for every route,
  duplicate burst → one backend start, blocked stop during experiment on all
  routes, failed start message, device loss projected as Failed.

## ExperimentController

`ExperimentController(AppBackend& backend)` — owns the experiment state
machine and the HDF5 file path for the current run.

- `enum class State { Idle, Starting, Active, Stopping }`
- `startExperiment(hdf5FilePath, errorMsg)`,
  `stopExperiment(errorMsg)`
- `startTimeNs()`, `endTimeNs()` accessors for metadata
- Signals: `stateChanged(State)`,
  `experimentStarted(startTimeNs)`,
  `experimentStopped(endTimeNs, validFrames, invalidFrames)`,
  `error(QString)`

## Related

- [[MainWindow]] owns both controllers.
- [[../services/ProcessingService]] reacts to
  `startExperiment`/`endExperiment` — the controller calls into the backend.
- HDF5 flushes at stop-time are coordinated through
  [[../services/Hdf5Service]] and `ProcessingService::flushBufferedFrames`.
