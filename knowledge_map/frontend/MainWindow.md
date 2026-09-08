# MainWindow

> `QMainWindow` subclass at the root of the UI. Holds a reference to
> `backend::AppBackend&` and owns the tab widget, sidebar, and corner
> actions.

**Source:** `src/frontend/core/MainWindow.cpp`,
`include/frontend/core/MainWindow.h`
**Related:** [[Controllers]], [[ConnectTab]], [[../architecture/AppBackend]],
[[System-Utilities]] (`AutoUpdater`, `DeviceInitManager`, `SidebarWidget`,
`PlaybackPanel`)

## Slots and lifecycle

- `onStartCapture` / `onStopCapture` — thin wrappers over the owned
  [[Controllers]] `CameraController` (`cameraController()` accessor). The
  controller's operation guard reads `experimentActive_`, `flushInProgress_`
  and `AppBackend::isFrameRecording()`; its `commandFailed` signal shows the
  warning dialog + status text, and `onCameraStateChanged` arms/disarms
  `statsTimer_` for every route (the sampling/flush scheduling in
  `onUpdateStats` no longer depends on which button started the camera) and
  writes the phase/failure text to the status label. The Preview page's
  `PlaybackPanel::captureToggleRequested` (Space) is wired to
  `requestToggle()` (issue #360).
- `onStartExperiment` / `onStopExperiment` — Start is a backend
  transaction (issue #369, [[../architecture/ExperimentCoordinator]]):
  a destination-less preflight (`evaluateReadiness()`) first; any blocking
  gate other than `storage.output` is explained by `explainReadiness()`
  (status, reason, remediation per gate; when the only blocker is
  `lifecycle.fault` the dialog offers "Acknowledge fault and re-check",
  which calls `clearUnresolvedFault()`). Then the LatestFrame
  acknowledgement, the file dialog, a final `evaluateReadiness(path)` and
  `start()` with **that** generation. Outcomes are typed: `Started` (status
  bar says "(simulated camera)" for a mock run), `StaleReadiness` ("the
  configuration changed … start again"), `NotReady` (gate dialog),
  `StorageFailed` / `ProvenanceFailed` (critical), `AlreadyActive` / `Busy`.
  The window no longer opens the HDF5 file, initializes datasets, sets the
  accounting context or calls `startExperiment()` itself; the coordinator
  does all of that inside the transaction and `experimentStartTimeNs_`
  comes from the frozen snapshot. Stop is `requestStop(false)` on the
  coordinator (issue #372): the backend's worker flushes, writes experiment
  info / accounting / acquisition provenance, closes the file and publishes
  the terminal `ExperimentStatus`; the window only renders. A flush or
  metadata failure is latched by the coordinator as an unresolved fault so
  the next Start is blocked until acknowledged. Start blocks with an explicit
  acknowledgement when `CaptureService::activeDeliveryMode()` is
  `LatestFrame`: "Switch to Every Frame" (default; routes through
  `ConnectTab::setDeliveryMode`, i.e. the same setConfig + persist path as
  the combo, without restarting the running capture), "Continue with Latest
  Frame" (explicit override), or Cancel (abort). When multi-image is enabled
  and realtime mode is `async_batch`, the coordinator's Start switches
  processing to `inline` for the run and its finalization restores it; the
  window only shows the explanation dialog.
  The constructor records the application identity
  (`MIB_STUDIO_QT_VERSION_FULL`, OS/arch) in the coordinator so every frozen
  run snapshot carries it.
- `onUpdateStats` — timer tick, split in three (issue #363):
  `sampleStats()` pulls `CaptureStats`, count-only
  `ProcessingService::getBufferedFrameCounts()` and autofocus state and
  schedules the round-robin flush (skipped while a stop is finalizing);
  `renderStats(data)` writes the sidebar panel and the compact status
  metrics; `refreshDiagnostics(data)` fills the Diagnostics dialog when it
  is open. Rendering never touches the alert model or the run state, so a
  stats tick cannot erase an error. It must not copy full `ProcessedFrame`
  buffers on the 500 ms UI timer.
- `onTabChanged(index)` — starts/stops the realtime loop when entering or
  leaving the experiment-related tabs (ExperimentController state).
- `onNoCamerasFound` — shows a friendly dialog when
  `DeviceInitManager` reports empty discovery.
- Startup restores the persisted native core through
  [[ProcessingCoreDialog]] before capture begins. A restore/pin failure is
  visible in the status bar and experiment start remains blocked.

- EGrabber JavaScript camera scripts are skipped during `onTabChanged` when
  a MindVision camera is selected.

## Window geometry and the hardware panel (issues #358, #359)

- **One geometry restoration path.** The constructor ends with
  `restoreWindowGeometry()`: `Window/LayoutVersion` + `Window/Rect`
  (+ `Window/Maximized`) are resolved through the pure
  `frontend::geometry::resolveWindowGeometry()` ([[System-Utilities]]
  `WindowGeometryPolicy`) against the actual screens'
  `availableGeometry()` — a rect from a removed/larger monitor, an old layout
  version or garbage falls back to the centered default (≤ 1280x800) on the
  best matching screen. `main.cpp` no longer resizes unconditionally. The
  first `showEvent` validates the *decorated* frame (`ensureWindowFitsScreen`,
  clamps size/position without going below the minimum size hint) and hooks
  `QScreen::availableGeometryChanged` with a coalesced 250 ms adjustment;
  `closeEvent` saves only when the close is accepted. Tests and the screenshot
  tour inject the desktop with `setAvailableGeometryOverrideForTests()` so the
  800x600 offscreen screen never drives decisions.
- **Single-owner sidebar.** `mainSplitter_` owns the hardware panel's width;
  `MainWindow` owns the preference (`Sidebar/LayoutVersion`, `Sidebar/Visible`,
  `Sidebar/PreferredWidth`, migrated once from the legacy
  `Sidebar/Collapsed`/`ExpandedWidth`, sanitized + clamped 200–1000) and
  commands the splitter. `setHardwarePanelVisible(false)` captures the actual
  expanded width as the preference, hides the child and lets the workspace
  take the space; `true` shows it and allocates
  `geometry::fitSidebarWidth(preferred, contents, handle)` — compact when
  the window is narrow, hidden-for-space (with a status message and the
  intent remembered) when not even the compact panel leaves the 640 px
  workspace. `splitterMoved` records user drags (debounced persist); the
  remembered width is never a hard minimum; nothing ever resizes the outer
  window. The stable reopen affordance is the checkable
  `hardwarePanelAct` (Settings menu, `Ctrl+Shift+H`) mirrored by the
  `hardwarePanelBtn` tool button in the main tab bar's left corner.
- **Bounded status text.** `statusLabel_` is an `ElidingLabel` (stretch 1):
  its minimum size is independent of the text, the full string is in the
  tooltip. Guards: `frontend.window_geometry_policy` (pure),
  `frontend.ui_layout` (1024x768 / 1366x768 / 1920x1080 matrix, every tab,
  long strings, 50 sidebar cycles, rapid toggles, oversized legacy width,
  removed-monitor restore).

## Run state, alerts and metrics (issue #363)

The status bar used to be one string that every path overwrote (a 500 ms
stats tick could erase a save error). Three separate surfaces now exist,
created in `setupStatusSurfaces()`:

- **Run state** — `frontend::RunStatusModel` (`runStatusModel()`) projected
  by the `RunStatusWidget` in the Experiment tab-bar corner (glyph + text,
  accessible name "Run state: …"; never color alone). Every run is an
  *operation*: `onStartExperiment` does `runOperationId_ =
  beginOperation(Starting, file)` and moves to `Running`; a phase change
  with a stale id is rejected, so a late completion cannot overwrite a newer
  run. `latchFailure(op, reason)` keeps the state **Failed – recovery
  required** even when the later `Complete` arrives; only a new operation
  clears it. Camera-only lifecycle uses `setIdlePhase(Idle/CameraRunning)`,
  which is a no-op while a failure is latched.
- **Alerts** — `frontend::UiAlertModel` (`alertModel()`) shown by the
  `AlertBanner` above the tab widget (wrapping summary, ×count, "+N more",
  bounded Details list, **Acknowledge**). Keys used by the window:
  `save.fatal` (Critical, fatal save callback), `save.flush`,
  `save.metadata`, `run.accounting` (stop-time failures; also latch the run
  failure and report the coordinator fault), `camera.start` (resolved by a
  successful start), `processing.core` (restore/pin failure at startup),
  `config.conflict` ([[ConfigTabs]] `documentStateChanged`). Acknowledging
  hides; only the owner `resolve()`s. Metrics ticks never add/remove alerts.
- **Metrics** — `statusLabel_` carries one bounded compact line
  ("Camera N fps · Valid x/s · Invalid y/s · Algo z µs · Run t s · buffered
  n", `compactStatusText()`); verbose transport counters, delivery-mode
  confirmation, timestamp descriptor, core identity/sha256 and process memory
  live in the non-modal **Diagnostics…** dialog (status-bar `diagnosticsBtn`,
  Help ▸ Diagnostics…, `showDiagnostics()` slot, dialog objectName
  `diagnosticsDialog`, text `diagnosticsText`). Long values never change the
  window's required width. Since issue #370 the dialog also lists every
  memory owner from `AppBackend::memoryBudgetSnapshot()` (current / peak /
  budget MB, items, evictions, "unknown" for vendor memory, "OVER" when a
  declared budget is exceeded) and the preview's presentation counters
  (`PlaybackPanel::displayFramesPresented/Skipped`, labelled as
  presentation-only, never acquisition/processing/persistence loss).
- **Stop is a status stream.** `onStopExperiment` calls
  `ExperimentCoordinator::requestStop(false)` and sets `Stopping`;
  `onExperimentStatus(status)` (queued to the GUI thread from the
  coordinator's callback, registered in the constructor and cleared first
  thing in the destructor) renders `Stopping` as `Saving`, and on
  `terminal` raises `save.flush` / `save.metadata` / `run.accounting` from
  `status.faultCode` and `status.completion`, latches the failure, sets
  `Complete` and only then shows the deferred dialogs. A fatal save error
  reaches the coordinator first (it finalizes the run as `Failed`); the
  window's fatal callback marks `Saving` and shows the critical dialog. A
  Stop issued by another client (the bridge) or by shutdown is rendered the
  same way. `stopInProgress_` disables Start/Stop and the compact text;
  `flushInProgress_` mirrors `status.flushing` for the camera operation
  guard. `closeEvent` calls `experiment().shutdown()` (bounded) when a run
  or finalization is active. Guards: `frontend.run_status_model`
  (pure), `frontend.run_status_ui` (25 ticks do not touch a raised alert,
  aggregation ×100, acknowledge ≠ resolve, latched Failed survives Complete
  and ticks, bounded width, keyboard-reachable banner buttons, Diagnostics
  dialog adds no alerts).

## Composition

- `connectTab_`, `overviewTab_`, `experimentTabs_` (QTabWidget with child
  tabs), `playbackPanel_`, `sidebarWidget_`, `updater_`, `initManager_`,
  `runStatusModel_`/`alertModel_`/`alertBanner_`/`runStatusWidget_` (#363).
- The nested Experiment pages wire the Monitoring tune panel's
  `applyRequested` to `AppConfigWatcher::onApplyProcessingDraft` and its
  `processingDraftApplied` result back to `onApplyResult` (issue #364, one
  acknowledged apply/persist path); `configFileChanged` refreshes the
  panel's baseline with the watcher's document fingerprint. The panel's
  `tuneStateChanged` drives the `tune.conflict` / `tune.apply` alerts.

## Menu bar

- **File:** Open Data Folder (`Documents/MIB_Studio_Qt`), Open Logs Folder
  (`%LOCALAPPDATA%/MIB_Studio_Qt/logs`), Exit.
- **Settings:** Processing / Monitoring / Pixel-to-Micron / Syringe Pump
  settings, **Processing Core…** ([[ProcessingCoreDialog]]), Boot Service
  Toggles (added in code), and **Profiles…** (navigates to Experiment ▸
  Preview, which hosts the config/profiles editor).
- **Help:** About, **Software Updates…** (opens [[System-Utilities]]'s
  `SoftwareUpdatesDialog` — channel + version selection; replaced the old
  "Check for Updates…"), **Diagnostics…** (#363), Documentation and Report a
  Problem (open the GitHub repo/issues). The Software Updates action is
  disabled when `auto_update` is disabled at boot.

## Boot disable GUI

- Settings menu includes **Boot Service Toggles...** for selecting services to
  disable on next launch.
- Selection is persisted in `QSettings` at `Startup/DisabledServices`.
- `main.cpp` applies this persisted CSV to `MIB_DISABLED_SERVICES` before
  backend initialization (unless the env var is already explicitly set).
- `auto_update` is honored in `MainWindow` startup: updater construction and
  quiet update checks are skipped when disabled.

## Gotchas

- **The window never touches `Hdf5Service` for a run.** Flush, the
  stop-time remainder, metadata, accounting, provenance and `closeFile()`
  live in [[../architecture/ExperimentCoordinator]] (the 2026-09-08 bench
  defects, IntentionallyPartial labelling of clean runs and a modal holding
  the file open for 108 s, were both caused by window-owned finalization).
- **No modal before the terminal status.** Dialogs in `onExperimentStatus`
  run only when `status.terminal` is true, i.e. after the coordinator closed
  the file.
- `closeEvent` stops experiment services, then stops the capture service
  before the window destructs. Mis-ordering causes the stale `StreamModule`
  stats seen in `docs/howto/safe-start-stop-egrabber.md`. This and the
  `applyCameraScriptFromFile`/`applyMindVisionConfigFromFile` internal stops
  are the only camera stops that do not go through `CameraController`; both
  are documented lifecycle transactions guarded by the experiment check
  above, not user commands.
- The corner **Start Live View / Stop Camera** buttons are pure
  presentations of `cameraController()->startAction()/stopAction()`
  (object names `startCameraBtn`/`stopCameraBtn`); do not attach separate
  guard logic to them.
- The destructor stops `statsTimer_` before `delete ui` to prevent timer
  callbacks from accessing backend_ on a partially-destroyed widget tree.
- `experimentServicesActive_` must stay in sync with
  `ExperimentController::State` or buttons wedge.
- The status-bar `Core: <version> · contract <n>` label is authoritative for
  the selected engine. Experiment metadata receives that core identity on
  stop; activation itself is rejected while an operation is active.
- Never write run-lifecycle or error text into `statusLabel_` directly: use
  `runStatusModel_->setPhase/latchFailure` and `alertModel_->raise` (#363).
  The compact metrics line is regenerated on every tick and would overwrite
  it.
- `deliveryModeLabel_` is a permanent status-bar badge ("▶ EVERY FRAME ·
  sequence preserved" / "⏩ LATEST FRAME · drops stale frames"), so the
  acquisition mode is visible on every tab. `updateDeliveryModeBadge()` shows
  the **backend-confirmed** `CaptureService::activeDeliveryMode()` and
  appends " (requested)" while `deliveryModeConfirmed` is false; it refreshes
  on every `onUpdateStats` tick and on combo/config changes.
  `CaptureService` clears the confirmed flag when the capture loop releases
  the camera, so between runs the badge tracks the requested config mode
  (with the "(requested)" suffix) — the ConnectTab combo is the
  requested-mode source of truth.
- The coordinator's status callback captures `this` and posts a queued
  lambda; the destructor clears the callback before any member dies, and
  `AppBackend` (constructed before the window in `main()`) outlives it.
