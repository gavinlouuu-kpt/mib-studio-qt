# OverviewTab

> Live camera display with ROI overlay and JS configuration. Shows the raw
> Mono8 frame stream, lets the user drag an ROI rectangle, and hosts the
> EGrabber GenICam configuration JS editor.

**Source:** `src/frontend/tabs/OverviewTab.cpp`,
`include/frontend/tabs/OverviewTab.h`
**Related:** `src/frontend/utils/SimpleImageCanvas.cpp`, [[../services/DotGridService]] (Wafer Grid),
`include/frontend/utils/SimpleImageCanvas.h`,
[[../services/ProcessingService]] (ROI propagation, realtime snapshot),
[[../architecture/AppBackend]] (recording ROI)

## Display tick (`onTick`, up to 50 Hz)

A `QTimer` fires `onTick` at the configured display rate. Each tick:

1. `backend_.playback().fetchLatest(scratchFrame_)` — fetches into a
   member `scratchFrame_` (vector capacity reused across ticks; no heap
   allocation after the first frame of each resolution).
2. **In-place pixel update**: if `frameImage_` already has the correct
   size and format, `memcpy` rows into `frameImage_.bits()` and call
   `frameImage_.detach()` to invalidate the cacheKey (so `SimpleImageCanvas`
   rescales on the next paintEvent). No QImage heap allocation.
3. **Geometry change**: if size/format changed, constructs a fresh QImage
   and `img.copy()` for ownership — one allocation, happens only when the
   camera resolution changes.
4. Calls `canvas_->update()` to schedule a repaint.

## `SimpleImageCanvas` scale cache

`paintEvent` caches the `Qt::SmoothTransformation` rescale result in
`scaledImgCache_`. The cache is invalidated when `image_->cacheKey()`
changes (new frame arrived) or the draw area size changes. ROI drag and
resize events fire many paintEvents without changing the image — these
reuse the cached scaled QImage at zero rescale cost.

## ROI overlay

The canvas draws a semi-transparent red rectangle at the current ROI
position. ROI drag events emit `roiPositionChanged(QPointF)`, which
`MainWindow` connects to `ProcessingService::setRealtimeRoi` and
`AppBackend`'s recording thread.

### Camera window in the profile and recording capacity (#833)

The eGrabber rectangle is the camera window (cROI). When the active profile
has `camera.window`, `initializeRoiFromConfig` shows it instead of the
script's lines (the profile wins after the Experiment script);
`reloadCameraWindow()` redraws it when the watcher reports a new window and
emits nothing (`roiChanged` would also move the processing ROI in
[[MainWindow]]). An operator edit still writes the script and, through
`MainWindow`, `AppConfigWatcher::writeBackCameraWindow` into an existing
`camera.window`. `capacityPanel()` (`frontend/widgets/RecordingCapacityPanel`,
under the image) shows the window, requested and maximum fps, save stream,
buffer (frames, size, seconds), destination rate and the four capacity gates
as text (never colour alone); MainWindow feeds it once per second from
`ExperimentCoordinator::recordingCapacity()` with the rectangle as the
planned window, so resizing it updates the buffer at once.

## Wafer Grid (dot-grid localization, Overview only)

The **Wafer Grid** toolbar button (`overviewWaferGridBtn`, after the ROI
size spin boxes) flips `DotGridService::Config::enabled` on
[[../services/DotGridService]]. This is the only place localization is shown:
the Experiment Preview page has no toggle.

- **Runs only while the Overview is on screen.** The constructor and
  `hideEvent` call `backend_.dotGrid().setPaused(true)`; `showEvent` calls
  `setPaused(false)` (which wakes the service to decode the newest frame at
  once). Switching to the Experiment tab, any other tab, or minimising the
  window therefore stops decoding even with Wafer Grid on.
- `onTick` → `updateDotGridOverlay()` (only while visible) follows the
  service's `isEnabled()` (config.json can switch it too, but only when the
  file's `dot_grid.enabled` value changes, so the toggle survives unrelated
  reloads) and, when `DotGridService::poseSequence()` moved, copies the
  latest `Pose` into `DotGridOverlay`: detected dots, image-centre cross,
  pose text with design name + chip, or the failure reason.
  `SimpleImageCanvas::paintEvent` draws dots and cross over the frame and
  the text box last, on top of the ROI rectangle. No decoding on the GUI
  thread.
- Test: `frontend.dot_grid_overview` (tab switches pause/resume, overlay,
  toggle, no toggle on the Preview page).

**Tauri (React) twin (CONNECT-054, bridge ABI 45).** The Overview stage of
`desktop/src/areas/connect/OverviewTab.tsx` has the same toggle:
`WaferGridToggle` (`WaferGridControls.tsx`, inside a `<ViewerGate>`, hidden
when `caps.frame_buffer` is false as on the PZ7035 Align) calls
`set_dot_grid_enabled`; a refusal shows "Dot-grid localization is not
available: <reason>" under the toolbar (`WaferGridNotice`, `role="alert"`) and in
the log instead of Qt's dialog. `useWaferGrid.ts` mirrors `showEvent` /
`hideEvent`: `set_dot_grid_paused(false)` while the Overview stage is shown in a
visible window (`document.visibilitychange`) with no experiment running,
`set_dot_grid_paused(true)` on any stage change, a hidden window, an experiment
or unmount (the bridge starts paused). While enabled it polls
`fetch_dot_grid_pose(since_sequence)` every 250 ms and copies the pose only when
the sequence moved (the reply to the current sequence has no dot list); one read
on arrival learns whether the service is already on. The overlay is a ref
(`waferGridOverlayRef`, connect state) that `areas/preview/usePreview.ts`
`draw()` calls last on the live canvas after the frame and the camera-window
box: orange dot circles, cyan centre cross, then the text box (`waferGrid.ts`:
the same three lines as Qt, or "Wafer grid: <reason> (<n> dots)", or "Wafer grid:
waiting for a frame"). The marks are sized in display pixels (Qt: dot radius
max(3, 6 x scale), 14 px cross arms) and divided by the canvas's display
scale, so they keep their size in Fit: Window; they are drawn only when the
pose was decoded from a frame of the canvas's size. Tests:
`waferGrid.test.ts` (text, reducer, pause rule, geometry, draw order),
`useWaferGrid.test.tsx` (pause / resume, polling, refused toggle),
`ViewerGate.test.tsx`; backend `backend.dot_grid_facade`, Rust
`dot_grid_commands_decode_the_mock_camera_view`.

Operator workflow, registry coverage and failure messages: [Acquire & record — Wafer Grid](../../docs/manual/acquire-and-record.md#wafer-grid--locate-the-chip).

## Gotchas

- `scratchFrame_` persists between ticks; do not move from it.
- `frameImage_.detach()` is required after the in-place `memcpy` —
  without it Qt's implicit sharing will not change the cacheKey, and
  `SimpleImageCanvas` will not rescale even though the pixels changed.
- The `SimpleImageCanvas` cache holds a reference to `scaledImgCache_`
  which can be a large allocation (full canvas resolution). It is freed
  when the canvas is destroyed or on the first resize.
- The destructor explicitly stops `timer_` before `delete ui` — if the
  50fps timer fires during widget destruction, `onTick()` accesses
  `backend_.playback()` on a potentially-freed backend (use-after-free).

## MindVision mode and ROI

`refreshCameraMode()` loads the selected MindVision JSON experiment ROI and
hides the eGrabber script editor, leaving the full view available for selection.
The editable ROI defaults to 512x96. Drag positions stay in sensor coordinates;
SimpleImageCanvas transforms the rectangle and drag direction for ISP mirroring.
Sensor bounds come from the capture capability snapshot, not 1920x1080 constants.
Save errors restore the previous ROI and remain visible until a successful edit.
MainWindow synchronizes the four ROI fields into ConfigTabs without discarding
unrelated JSON edits. Hardware-cropped experiment frames use local processing
coordinates, so the camera offset is not applied twice.

The display timer is capped at 50 fps; the status identifies the 400 Hz trigger
separately from measured capture FPS. Hidden tabs do not fetch/copy frames.
The raw script editor and eGrabber alignment rules remain provider-specific.
