## 2026-11-05 — Tauri Experiment start/stop and run status at Qt parity (#866, PR D)

The Tauri classic view's Experiment start/stop flow and run status follow the Qt app (MainWindow,
RunStatusModel, StatisticsPanel) for epic #866 rows MON-003, 004, 008, 009, 010, 012, 013, 014, 022,
023, 025, 028, 029, 095, 098, 100, 102 and 104. Frontend only: no bridge or backend change.

- **Start order and wording** (`desktop/src/experimentStart.ts`, `areas/monitoring/startReadiness.ts`):
  blocking readiness gates (all but `storage.output`) are explained in a modal before the save dialog
  opens, then the discard-ring question, the file dialog (`HDF5 Files (*.h5 *.hdf5)` and `All Files`, no
  default name on the desktop), the multi-image inline-mode note (Qt's condition: a series of more than
  one image while the realtime mode is async batch, read from `image_processing.multi_image` and
  `realtime_processing.mode` of `fetch_processing_config_json`; #877 review), readiness for the
  destination and Start. Every gate is "Blocked — id: reason → remediation" with the Warn advisories after it. A
  refused Start is worded by its typed `start_outcome` (ABI 40, already in the reply): StaleReadiness
  warning, StorageFailed/ProvenanceFailed critical, Busy/AlreadyActive/InvalidRequest information,
  NotReady re-explained from fresh gates. This also closes SHELL-058.
- **Standing readiness** (`useStartReadiness`, `ReadinessAdvisories`): while the Experiment stage is
  shown and no run is active, `fetch_experiment_readiness("")` is read every 2 s (one at a time) and the
  blocking and Warn gates (camera.rate near its limit, a short buffer, the E-LUT warning) are listed
  with reason and remediation before Start is pressed. Warn gates of a started run are all shown in the
  start notice, not only `storage.persistent`.
- **Run state** (`areas/monitoring/runStatus.ts`, `RunStatusBadge`): Qt's glyph + text widget (Idle,
  Camera running, Starting, Running, Stopping, Saving, Complete, Failed – recovery required) in the
  Experiment toolbar, derived from the backend's experiment status (closes SHELL-059). The same phase
  gives the run lock: Overview and Review stage tabs, Review's Select HDF File and File ▸ Open Recording
  are shut for the whole run (#585); Stop Camera is refused during a run, a save and raw recording
  (reasons as Qt); Raw Record is disabled while the experiment is not idle or a Review file is open
  (#538). Stop and save progress text ("Stopping experiment…", "Saving experiment data…", "Experiment
  saved: N frames persisted") shows in the Experiment tab and, while saving, in the status bar.
- **Run accounting** (`runFinish.ts`): a finished run that is neither Complete nor IntentionallyPartial
  (and not the fatal save error, which keeps its recovery panel) opens Qt's "Experiment Accounting"
  modal with empty / processed / rejected / failed / store-loss / persisted counts, only for a run this
  client watched, and the Qt remediation line shows under the outcome notice.
- **Side panel and status bar** (`statsReadouts.ts`, `BackgroundThumbnail`): Camera group Status
  Running/Stopped, FPS, Data Rate; Flush Status "Flushing…" in orange; Runtime hh:mm:ss; background
  thumbnail ("Background: WxH", pulled from `fetch_background_packet` when the config is re-read with a
  background, never per frame); the status bar is Qt's one elided line. Algo Time (µs) with the
  stale-after-1-s cue and a backend capture rate are wired in the readouts, but the bridge's processing
  stats carry no `algo_avg_us` / capture telemetry yet: the row stays hidden and the camera FPS and
  data rate remain the rates measured from received frames (labelled so in the tooltip).
- **Clear Buffer** asks Yes/No first and exposes `onMonitoringCleared(listener)` for later PRs (KDE
  state, selection).
- **Swallowed failures surfaced**: a refused `monitoring_set_active`, snapshot and trigger polls that
  fail three times in a row (`pollHealth.ts`) and a run-accounting read that fails now show a notice in
  the Monitoring panel / Experiment tab and in the log; the stale "lands with UI-3 (#268)" sidebar
  tooltip is fixed.

`transport/dialogs.ts` gains `message` (info/warning/error modal; `window.alert` in a browser). See
[[architecture/Desktop-Shell]] and `docs/howto/desktop-area-ownership.md`.

**Review fixes (#877 non-author review, 2026-11-06).** The MON-008 note read a top-level `multi_image`
that the config document never has, so it never showed; it now reads the document's real shape and
Qt's exact condition. The run lock (MON-013) is enforced at the stage setter, not only on the stage
tabs: `useRunLockedNavigation` wraps the shell's `setTab`, so no route (the context bar's Calibration
segment, the workflow's next step, a notice's "place the window" link) opens Overview or Review during a
run; the context bar's segment for a shut stage is disabled with the lock reason.
