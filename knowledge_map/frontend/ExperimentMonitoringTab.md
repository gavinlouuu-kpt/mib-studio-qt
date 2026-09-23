# ExperimentMonitoringTab

> Live charts while capture is running: histograms (area, deformability,
> brightness) + scatter plots (deformability-vs-area, etc.).

**Source:** `src/frontend/tabs/ExperimentMonitoringTab.cpp`,
`include/frontend/tabs/ExperimentMonitoringTab.h`,
`include/frontend/tabs/MonitoringDensity.h` (KDE kernel + colour ramp),
`include/frontend/tabs/MonitoringRoiCrop.h`
**Related:** [[../services/ProcessingService]] (monitoring rings),
[[../frontend/System-Utilities]] (`ZoomableChartView`),
[[Dialogs]] (`MonitoringSettingsDialog`)

## Responsibility

- Read `ProcessingService::getMonitoringValidFrames()` /
  `getMonitoringInvalidFrames()` on a timer (ring buffer of 1000 frames
  each).
- Render via `QtCharts`: `QScatterSeries`, `QHistogramSeries`,
  `QBarSeries`, etc. `frontend::ZoomableChartView` adds scroll/zoom.
- Live totals: valid count, invalid count, algo FPS, valid FPS.
- `showEvent` / `hideEvent` pause rendering when the tab isn't visible **and**
  gate the backend accumulation: `showEvent` calls
  `ProcessingService::setMonitoringActive(true)`, `hideEvent` calls
  `setMonitoringActive(false)`. Monitoring rings only fill while this tab is
  visible — when hidden, the realtime loop skips the per-object frame copies
  entirely (see [[../services/ProcessingService]] "Monitoring rings").
- **Top-row trigger controls:**
  - `sortTriggerBtn` — single manual pulse; calls
    `backend_.trigger().onTargetGroupResult(services::TargetGroupSignal{.isTargetGroup=true})`.
  - `triggerDurationSpin` — pulse width in µs
    (`TriggerService::setPulseDurationUs`).
  - `periodicTriggerBtn` (checkable) + `periodicTriggerIntervalSpin` —
    periodic test pulses. When armed, `periodicTriggerTimer_`
    (`QTimer`) fires `onTargetGroupResult(services::TargetGroupSignal{.isTargetGroup=true})`
    every N ms. The
    interval spinbox is disabled while armed; the timer is disarmed in
    `hideEvent`. Intended for oscilloscope/sorter bring-up without
    needing live target-group classifications. See
    [[../services/TriggerService]].

## Scatter density (KDE) colouring

The **Density (KDE)** checkbox in the top row (`kdeToggleCheck`) colours every
valid point of the Deformability-vs-Area scatter by its normalised local
population density, the pseudocolour dot plot used in flow / deformability
cytometry, so an operator can see where the population sits even when
markers overlap.

- **Kernel** — `include/frontend/tabs/MonitoringDensity.h` (Qt-free,
  header-only, same pattern as `MonitoringRoiCrop.h`): Gaussian KDE evaluated
  at every sample with a **per-axis Silverman bandwidth** (`sigma_axis ·
  n^(-1/6)` × user factor), normalised so the densest sample is 1. Per-axis
  is not optional: area spans hundreds of µm² while deformability spans
  0..1; one isotropic bandwidth in raw units merges populations that differ
  only in deformability (the old, never-called `computeKDE` grid did exactly
  that and used a 1-D normalisation — it was removed). `densityRampColor(t)`
  is the sequential single-hue ramp (blue, light → dark; the light end still
  clears 2:1 on the white chart). Guard: `frontend.monitoring_density`
  (invariants, per-axis separation with an isotropic control, degenerate
  and non-finite input, order invariance, ratio-gated quadratic cost).
- **Periodic, off the GUI thread** — `kdeTimer_` (default 2 s,
  `kKdeIntervalMs*`) calls `requestKdeUpdate()`, which snapshots the rolling
  buffer into value types and runs the kernel via `QtConcurrent::run` +
  `QFutureWatcher<KdeResult>` (the frontend's established async idiom, see
  [[HdfReviewTab]]). One job at a time; an unchanged buffer (fingerprint =
  count, first/last frame index, factor, µm conversion) is skipped; the
  destructor drains an in-flight job; a result arriving after the toggle
  went off is dropped. 1000 points cost ~3 ms on a desktop core.
- **Cheap refresh** — `updateScatterplot` (every 500 ms) only records the
  frame index behind each series point (`scatterPointFrames_`,
  `targetPointFrames_`) and re-applies the last density map through
  `QXYSeries::setPointsConfiguration` (per-point `Color`, quantised 65-entry
  LUT). Points that arrived after the last estimate wear the sparse end of
  the ramp until the next tick. While on, the target-group series switches
  to a **rectangle marker** so it stays identifiable without its blue; off
  restores circles and clears all per-point configuration — the chart looks
  exactly as before.
- **Settings** — `MonitoringSettingsDialog` exposes *KDE bandwidth factor*
  (0.2–5, default 1) and *KDE update interval* (500–60000 ms); the toggle,
  factor and interval persist in `QSettings` under `Monitoring/Kde*` with a
  version guard (`Monitoring/KdeVersion`), like `Preview/*`. Test hooks:
  `kdeToggle()`, `requestKdeUpdate()`, `kdeJobInFlight()`,
  `kdeTimerActive()`, `kdeGeneration()`, `scatterSeriesForTests()`,
  `injectMonitoringFramesForTests()`. Guard: `frontend.monitoring_kde_density`.

## Tune panel (issue #364)

The right-hand panel (`tunePanel`, 220–280 px) is a *draft* over the
exposed subset of `ProcessingConfig`, owned by the pure
`ProcessingConfigDraft` model ([[System-Utilities]] Models):

- **Criteria grouped with their enable state.** Under the heading *Cell
  acceptance filters*: checkable group boxes `criterionArea` ("Area (µm²)",
  Minimum/Maximum with the unit suffix), `criterionDeformability`,
  `criterionRingRatio`, `criterionAreaRatio` (Maximum), `criterionBorder`
  and `criterionSingleInner` (enable-only, with a wrapping description).
  Unchecking a group disables its value controls; the configured values
  stay visible. Under *Target group / sorting gate*: `targetGroupBox`
  (area/deformability ranges, accessible names prefixed "Target group")
  with a hint that it selects valid cells for the sort trigger and never
  changes validity. *Multi-image acquisition*: `multiImageBox` + images per
  trigger. Rows are `QFormLayout` with `WrapLongRows`, so labels stack over
  inputs at the compact width; no horizontal scrolling.
- **Fixed footer** (`tuneFooter`, outside the scroll area): `tuneStateLabel`
  ("Applied" / "N unapplied changes" / "Invalid: …" / "Conflict: …" /
  "Applying…" / "Not applied: …" / "Saved, not applied: …"),
  `tuneValidationLabel`, **Apply changes** (`tuneApplyBtn`, enabled only for
  a valid, non-conflicting dirty draft with no apply in flight) and
  **Revert** (`tuneRevertBtn`). Changed rows get a `*` on their label and an
  accessible description naming the applied value.
- **Bindings, not members.** Every field is one `TuneBinding` (widget +
  read/write closures) created by `bindTuneField`; `loadCurrentConfig()`
  populates through `QSignalBlocker` + `tuneLoading_`, so programmatic
  population is never an edit. User edits call `draft_.setField` (a
  re-typed displayed value is not an edit; precision lives in the model)
  and only mark Dirty — the backend and the file are never touched here.
- **Apply is a request.** `onApplyParams` → `draft_.beginApply()` →
  `applyRequested(ApplyProcessingDraftRequest{requestId,
  baselineFingerprint, patch of changed fields})`; [[MainWindow]] routes it
  to `AppConfigWatcher::onApplyProcessingDraft` and the
  `processingDraftApplied(ConfigApplyResult)` back to `onApplyResult`.
  Dirty clears only on `persisted && applied`; a failure keeps the draft
  and names the error; `persisted && !applied` is shown as "Saved, not
  applied"; a stale request id is ignored; with no coordinator connected
  the apply fails explicitly. While applying, controls are read-only,
  edits are ignored and external reloads are parked.
- **External changes.** `loadCurrentConfig(fingerprint)` (wired to the
  watcher's `configFileChanged`) reloads from the runtime config: clean →
  refresh; dirty with only unexposed differences → keep editing; dirty and
  an exposed field differs → **Conflict** (draft retained, Apply blocked,
  alert `tune.conflict` in the main window) until Revert. Revert always
  adopts the latest authoritative config.
- Test hooks: `tuneDraft()`, `tuneFieldWidget(TuneField)`,
  `setTuneFieldForTests`, `tunePanel/tuneScrollArea/tuneFooter`,
  `tuneApplyButton/tuneRevertButton`, `tuneStateText()`. Guards:
  `frontend.config_draft` (pure), `frontend.monitoring_tune` (grouping,
  zero-mutation editing, request content, confirmed/failed/stale/partial
  results, Revert, conflict incl. hidden panel, disabled criteria keep
  values, 220/280 px without overflow, footer fixed at every scroll
  position), `frontend.config_apply` (watcher side).

## Gotchas

- **Crop with the clamped ROI, never the raw one.** Monitoring frames were
  accumulated under the ROI / camera geometry active when they were
  processed; the current `getRealtimeRoi()` may not fit them (ROI edited,
  camera or config switched). Cropping them with an unclamped
  `cv::Rect(roi)` aborted the installed 1.0.7 app seven times
  (`cv::Mat::Mat` ROI assertion, crash review 2026-09-08). The overlay and
  extract paths go through `frontend/tabs/MonitoringRoiCrop.h`
  (`cropToRoi`: ROI-sized frame as is, else `clampRoiToFrame`, empty frame
  → empty crop). Guard: `frontend.monitoring_roi_crop`.
- Histograms are computed client-side from the monitoring rings — not
  persisted.
- **Never run the density estimate on the GUI thread and never hold
  backend state across it.** `requestKdeUpdate()` copies indices and
  (area, deformability) pairs into the lambda; the worker touches no widget,
  no `ProcessedFrame` and no service. Apply results only in
  `onKdeJobFinished()` (GUI thread) and only if the toggle is still on.
- `updateScatterplot` clears and re-appends the series every 500 ms, which
  also drops Qt's per-point configuration — that is why the density colours
  are re-applied from `kdeDensityByIndex_` on every refresh instead of being
  set once.
- `loadCurrentConfig()` refreshes the histogram ring-ratio defaults as well
  as the tune panel baseline, so config reloads keep the visible chart
  range aligned with the saved thresholds. Never write tune values to the
  backend from this tab; go through the apply request.
- Chart snapshots to HDF5 use
  [[../services/Hdf5Service]]::`saveChartSnapshot` (used by experiment-save
  to preserve the final view).
- See tasks `knowledge_map/task/ui-status-stats.md` and
  `fps_mbs_zero.md` for common metric-display issues.
