# OverviewTab

> Live camera display with ROI overlay and JS configuration. Shows the raw
> Mono8 frame stream, lets the user drag an ROI rectangle, and hosts the
> EGrabber GenICam configuration JS editor.

**Source:** `src/frontend/tabs/OverviewTab.cpp`,
`include/frontend/tabs/OverviewTab.h`
**Related:** `src/frontend/utils/SimpleImageCanvas.cpp`,
`include/frontend/utils/SimpleImageCanvas.h`,
[[../services/ProcessingService]] (ROI propagation, realtime snapshot),
[[../architecture/AppBackend]] (recording ROI)

## Display tick (`onTick`, ~20 Hz)

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
