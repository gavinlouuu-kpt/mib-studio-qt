# PreviewPage

> Central workspace widget: live preview canvas on the left, playback scrub
> and overlays above, [[ConfigTabs]] as the inspector below (vertical splitter).

**Source:** `src/frontend/tabs/PreviewPage.cpp`,
`include/frontend/tabs/PreviewPage.h`
**Related:** [[System-Utilities]] (`PlaybackPanel`), [[ConfigTabs]],
[[../data-model/FrameStore]], [[../services/ProcessingService]]
(realtime snapshot)

## Responsibility

- Host the `PlaybackPanel` widget (see [[System-Utilities]]) for live + scrub
  display with ROI drawing and overlay modes (mask, contours, both). The panel
  is the direct, only child of `overlayContainer` (`QVBoxLayout`, zero
  margins): the former centered "▶ Play / ■ Stop" `QStackedLayout` overlay,
  its 300 ms polling timer and its direct `CaptureService::start()/stop()`
  calls were removed in issue #360 — they bypassed the experiment guard and
  covered the image. Camera Start/Stop lives in the main-window chrome
  through [[Controllers]] `CameraController`; the Space-bar toggle in the
  panel emits `captureToggleRequested` for [[MainWindow]] to route there.
- Wire an `AppConfigWatcher` to pick up external JSON config changes
  (see `docs/howto/live-config-reload.md`).
- Bridge UI events (ROI changes, overlay toggles, zoom fit) to
  [[../services/ProcessingService]] (`setRealtimeRoi`,
  `setRealtimeBackgroundGray`) and to
  [[../frontend/System-Utilities]] (`PlaybackPanel`).

## Inspector modes and persistence (issue #362)

- `InspectorMode { Expanded, Compact, Hidden }` with a **stable mode bar**
  (`inspectorModeBar`: Expanded / Compact / Hidden tool buttons bound to
  `inspectorExpandedAct` etc., plus an elided compact summary
  `"<profile> · <state>"` from `ConfigTabs::compactSummary()`) placed
  *outside* the splitter, so the reopen affordance never disappears.
- **Expanded** shows the full `ConfigTabs`; **Compact** shows only its
  bounded header (`ConfigTabs::setCompactMode(true)`, editors hidden, height
  = header size hint); **Hidden** hides the child. Documents, edits,
  `AppConfigWatcher` and the `getConfigTabs()`/`getConfigWatcher()` pointers
  stay alive in every mode.
- The `QSplitter` is the only geometry owner. No more constructor 50/50
  `setSizes`; `applyInspectorLayout()` runs once after the first show and
  (coalesced, 80 ms) on resize/mode change: Expanded allocates
  `preferredRatio × usable` clamped to `[220 px, usable − 140 px image]`;
  when even that cannot fit, the *effective* mode falls back to Compact and
  the preference is untouched. A drag below 220 px is clamped deliberately
  (`onSplitterMoved`), never a sliver. Nothing resizes the outer window.
- Persistence (`Preview/LayoutVersion` = 1, `Preview/InspectorMode`,
  `Preview/InspectorRatio`; `Preview/ShowTable` preserved): saved only from
  user drags/mode changes (debounced, flushed on hide), restored once when
  valid, corrupt/obsolete values fall back to the image-biased default
  (ratio 0.34 ⇒ ≈ 66 % image at 1366x768).
- `setTemporaryInspectorMode(std::optional<InspectorMode>)` is the
  presentation-only workflow hook for #311: a temporary Compact (e.g. while
  a run is active) keeps pending edits and the dirty summary; `nullopt`
  restores the user preference. It changes no acquisition/processing/config
  state. Guard: `frontend.preview_layout`.

## UI controls

Overlay mode, fit mode (FitToWindow / Zoom100), display FPS target, save
buffer to disk (via [[Dialogs]] `BufferSaveDialog`), conversion-factor
spinner (μm/pixel — see [[Dialogs]] `ConversionFactorDialog`).

Preview canvas context menu: "Set Background" (current paused frame),
"Calibrate Background (bounded)" / "Cancel Background Calibration" (issue
#369 — `PlaybackPanel::onCalibrateBackground` starts
`ProcessingService::startBackgroundCalibration` with the default request,
polls `backgroundCalibrationStatus()` every 200 ms and, on `Succeeded`,
adopts the published background for the overlay; every other terminal state
is reported with its counts and leaves the previous background untouched),
"Clear ROI".

## Gotchas

- Display FPS is capped (60 Hz by default) — see task
  `knowledge_map/task/2025-11-17-preview-60hz.md`.
- PreviewPage does not push frames to HDF5; only [[../services/ProcessingService]]
  does, during an active experiment.
- Overlay cell color is per-frame only while following live; in paused/replay
  it is recomputed from the displayed buffered frame (see [[System-Utilities]]
  `PlaybackPanel`).
