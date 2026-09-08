# ExperimentMonitoringTab

> Live charts while capture is running: histograms (area, deformability,
> brightness) + scatter plots (deformability-vs-area, etc.).

**Source:** `src/frontend/tabs/ExperimentMonitoringTab.cpp`,
`include/frontend/tabs/ExperimentMonitoringTab.h`
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

- Histograms are computed client-side from the monitoring rings — not
  persisted.
- `loadCurrentConfig()` refreshes the histogram ring-ratio defaults as well
  as the tune panel baseline, so config reloads keep the visible chart
  range aligned with the saved thresholds. Never write tune values to the
  backend from this tab; go through the apply request.
- Chart snapshots to HDF5 use
  [[../services/Hdf5Service]]::`saveChartSnapshot` (used by experiment-save
  to preserve the final view).
- See tasks `knowledge_map/task/ui-status-stats.md` and
  `fps_mbs_zero.md` for common metric-display issues.
