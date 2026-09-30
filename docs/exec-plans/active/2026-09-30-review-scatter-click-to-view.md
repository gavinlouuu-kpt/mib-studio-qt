# Review scatter: zoom/pan and click a point to view its frame

Status: active

Date: 2026-09-30. Scope: the Qt Review tab only (`HdfReviewTab`, Charts
view). Companion notes: `knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/frontend/System-Utilities.md` (`ZoomableChartView`),
`knowledge_map/frontend/ExperimentMonitoringTab.md`.

## Goal

In the Review tab's Charts view the deformability-vs-area scatter can be
zoomed and panned like the Monitoring scatter, and a single left click on a
point highlights that cell, selects its row in the Valid Frames table and
opens `FrameViewerDialog` on its image, with the same overlay / ROI /
multi-image-series behaviour as double-clicking the table row. Dragging
pans and never opens a frame. Exports, file reload and the Monitoring tab
behave exactly as before.

## Current state (what the spec builds on)

- The review scatter is one `QScatterSeries` in a plain `QChartView`
  (`src/frontend/tabs/HdfReviewTab.cpp:378-413`); no zoom or pan. The chart
  also carries the isoelastic `QLineSeries` curves and the stored KDE
  contour lines; right-click is `Qt::ActionsContextMenu` with
  `computeCoreAction_` ("Compute core contour from full run").
- `generateScatterPlot` (`:2428`) clears the series and appends one point per
  frame with `validation.isValid`, so **point k is not frame k**.
- Images open only from thumbnails (double-click) and table rows
  (double-click) via `showFrameViewer(int)` (`:1600`), which reads
  `isShowingValid_` to pick the dataset. `onTabChanged` sets
  `isShowingValid_ = (index == 0)`, so on the Charts tab (index 2) it is
  **false** and the tab would read `invalidFrames_`.
- `FrameViewerDialog` is modal (`exec()`, `:1723`) with prev/next that call
  `setSelectedFrame` on the tab.
- `ZoomableChartView` (`src/frontend/widgets/ZoomableChartView.cpp`) is used
  by the Monitoring tab: wheel zoom around the cursor (Ctrl = X only, Shift =
  Y only, axis-label regions zoom one axis), **left press starts a pan
  immediately**, double-click resets to the ranges given by
  `setDefaultRange`.
- Export (`renderChartSnapshots`, `:1986`; `chartToPixmap`, `:2628`)
  re-runs `generateScatterPlot` (with *another file's* data during batch
  export), resizes the view to a fixed size, grabs it and restores the size.

## Behaviour

### Zoom and pan

- The scatter view becomes a `ZoomableChartView`. Wheel, modifier and
  axis-region semantics are the Monitoring tab's.
- **Click vs drag:** a left press that moves less than
  `QApplication::startDragDistance()` before release is a click; once the
  pointer passes that distance the gesture is a pan for the rest of the
  press and cannot become a click. The closed-hand cursor appears when the
  pan starts, not on press (this is the one visible change on the Monitoring
  tab).
- **Reset zoom:** double-click on empty plot space (neither click hit a
  point), and a new **Reset zoom** entry in the scatter's right-click menu
  next to "Compute core contour from full run".
- Middle-drag also pans (no existing binding; no click ambiguity).

### Click on a point

- Hit test: the nearest scatter point to the release position, measured in
  **screen pixels** (tolerance `max(markerSize, 8 px)`), considering only
  points whose data value lies inside the visible axis ranges. Ties go to the
  lowest frame index. Only `scatterSeries_` is hit-tested: isoelastic
  curves, KDE contours and the highlight series are not.
- Hit → highlight the point (one-point series drawn on top), call
  `setSelectedFrame(frame, /*valid=*/true)` so the Valid Frames table row and
  thumbnail follow, then open the viewer (see "Viewer open/close").
- Click on empty plot space → clear the highlight. Clicks on axes, legend or
  outside `plotArea()` do nothing. Modifier keys do not change click
  behaviour (no multi-select).
- Prev/next inside the viewer move the highlight to the frame shown; if that
  frame has no point (failed validation) the highlight is hidden. The view
  does not auto-pan to follow.
- Cursor: pointing hand over a point, closed hand while panning, arrow
  otherwise. The hover test uses the **same** nearest-point function as the
  click (not `QScatterSeries::hovered`, whose marker-geometry test would
  disagree with the click tolerance).
- Optional, same PR if cheap: tooltip on hover with frame #, area (µm²),
  deformability.
- Recording files have no metrics: the scatter is empty; clicks and hover do
  nothing.

### Viewer open/close

- Open only on a left **release** that stayed under the drag threshold, never
  on press.
- Sequence: set the highlight and table selection first, reset the view's
  pan state and cursor, then open through a queued call
  (`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`), never `exec()`
  inside a mouse handler.
- One flag "viewer open or pending"; further opens are dropped while set. Two
  rapid clicks produce one viewer.
- The double-click reset never fires under an opening viewer: it resets only
  when neither click of the pair hit a point.
- Pan state is cleared on viewer open, `leaveEvent` and focus loss, so a
  release swallowed by the modal dialog cannot leave a "sticky pan".
- On close: zoom preserved, highlight stays on the last frame viewed,
  cursor re-evaluated for the current pointer position; closing (X, Esc,
  button) never registers as a chart click.

### Zoom state across the tab's other flows

- File load / reload / close: zoom resets to the data extent and the
  highlight is cleared. `generateScatterPlot` records its computed axis
  ranges with `setDefaultRange` so the reset target is the data extent, not
  the 0–1000 / 0–1 fallback.
- Chart export, single and batch: snapshots are full data extent with no
  highlight. Save the four axis bounds and the highlight before
  `renderChartSnapshots`, hide the highlight for the grab, restore both after
  (batch already calls `updateCharts()` afterwards; restore after that).
- Clicks and hover are ignored while an export job is running (the export
  redraws the scatter with other files' data) and while a viewer is
  open/pending.

## Non-goals

- React/Tauri shell: its review screen has no chart yet ("Chart rendering
  lands with UI-3 (#268)"); follow up after #268.
- TD-17 (µm² factor from the recorded calibration): unaffected, mapping is
  by index.
- Clicking points on the live Monitoring scatter.
- A reusable non-modal viewer that swaps frames on each click. Decision
  below; separate issue.

## Decision log

- 2026-09-30: **Single click opens the viewer** (user request); the rest of
  the tab keeps click = select, double-click = open.
- 2026-09-30: **Left button carries both click and pan**, disambiguated by
  `startDragDistance()`. Right-drag was considered and rejected: right-click
  already owns the context menu (and Qt opens it on press on X11, on release
  on Windows), the Monitoring tab shares the widget and pans with left-drag,
  and trackpads make right-drag awkward. Middle-drag added as an unambiguous
  second pan binding.
- 2026-09-30: **Keep the modal `FrameViewerDialog`.** Modality blocks file
  close/reload/export/regenerate while a frame is open, which removes a class
  of lifecycle bugs. A persistent non-modal viewer that follows scatter
  clicks is the better exploration tool but roughly doubles the lifecycle
  and test surface (close on file change, block during export and
  Regenerate masks, destroy with the tab); it is a follow-up issue.
- 2026-09-30: **Double-click reset stays**, restricted to pairs where neither
  click hit a point, plus a menu entry. Delaying every click by the
  double-click interval to disambiguate was rejected as sluggish.
- 2026-09-30: **Hit-test in the view, not via `QScatterSeries::clicked`**:
  the series signal gives coordinates, not a frame, and its marker-geometry
  test would not match a pixel tolerance.

## Implementation plan

Two PRs; the first is shared infrastructure with no user-visible feature.

### PR 1 — `ZoomableChartView`: click/drag disambiguation

Files: `include/frontend/widgets/ZoomableChartView.h`,
`src/frontend/widgets/ZoomableChartView.cpp`, new
`tests/frontend/zoomable_chart_view_test.cpp`, `tests/CMakeLists.txt`,
`knowledge_map/frontend/System-Utilities.md`,
`knowledge_map/frontend/ExperimentMonitoringTab.md`.

1. `mousePressEvent` (left or middle): record `pressPos_`, set
   `pressPending_ = true`; do not change the cursor or `isPanning_` yet.
2. `mouseMoveEvent`: if `pressPending_` and
   `(pos - pressPos_).manhattanLength() >= QApplication::startDragDistance()`
   → `pressPending_ = false`, `isPanning_ = true`, closed-hand cursor, then
   the existing pan code. Emit `hoverMoved(QPointF)` when not panning and no
   press is pending (the Review tab uses it for cursor/tooltip).
3. `mouseReleaseEvent`: if `pressPending_` (never crossed the threshold) →
   `pressPending_ = false`, emit `plotClicked(QPointF viewPos, Qt::MouseButton)`
   only when the position is inside `chart()->plotArea()`. If `isPanning_`
   → end the pan as today.
4. `mouseDoubleClickEvent`: emit `plotDoubleClicked(QPointF)`; reset zoom
   only if a new `resetOnDoubleClick_` (default true) is set. The Review tab
   sets it false and decides itself (see PR 2).
5. `leaveEvent` / `focusOutEvent`: clear `pressPending_` and `isPanning_`,
   restore the arrow cursor. Add `cancelGesture()` public for the Review tab
   to call before opening the viewer.
6. Add `resetZoom` menu-friendly `QAction* resetZoomAction()` owned by the
   view (text "Reset zoom"), so both tabs can add it to a context menu.
7. Test (`frontend.zoomable_chart_view`, offscreen, `QTest` mouse events on a
   shown, fixed-size view after `settle()`): sub-threshold press/release →
   one `plotClicked`, axes unchanged; press, move past threshold, release →
   axes shifted, no `plotClicked`; wheel → range shrinks around cursor;
   double-click with default flag → default ranges restored; with flag off →
   `plotDoubleClicked` emitted, ranges unchanged; `leaveEvent` mid-drag → next
   move without button does not pan. Register in `tests/CMakeLists.txt`
   after the other `frontend.*` tests with `LABELS "frontend;utility"`,
   TIMEOUT 30.
8. Vault: `System-Utilities.md` (signals, threshold, `cancelGesture`,
   `resetZoomAction`); `ExperimentMonitoringTab.md` one line: cursor changes
   after the threshold, otherwise unchanged. Existing
   `frontend.monitoring_kde_*` tests must still pass.

### PR 2 — Review tab: zoomable scatter, click-to-view

Files: `src/frontend/tabs/HdfReviewTab.cpp`,
`include/frontend/tabs/HdfReviewTab.h`,
`tests/frontend/hdf_review_core_test.cpp` (or a new
`hdf_review_scatter_test.cpp` with the same fixture helpers),
`tests/CMakeLists.txt`, `knowledge_map/frontend/HdfReviewTab.md`,
`docs/manual/review-and-postprocess.md`,
`knowledge_map/current-state/Recent-Work.md`, this plan.

1. **Dataset parameter.** Change `showFrameViewer(int)` and
   `setSelectedFrame(int)` to take `bool valid`; existing callers pass
   `isShowingValid_`. The viewer's prev/next lambdas already carry
   `navState->isValidSet`; pass it through. Nothing on the Charts tab reads
   `isShowingValid_` afterwards.
2. **Point ↔ frame maps.** In `generateScatterPlot`, fill
   `std::vector<int> scatterPointToFrame_` (point k → index into
   `validFrames_`) and `std::vector<int> frameToScatterPoint_` (−1 when the
   frame has no point). Clear both in `clearDisplay()`. Also record the
   computed axis ranges with `scatterPlotView_->setDefaultRange(...)` for
   both axes (or the fallback ranges when there are no points).
3. **View swap.** Construct `scatterPlotView_` as `ZoomableChartView`
   (`resetOnDoubleClick_ = false`), keep the context-menu policy and add
   `scatterPlotView_->resetZoomAction()` after `computeCoreAction_`.
4. **Highlight series.** `scatterHighlight_`: `QScatterSeries`, one point,
   larger marker + contrasting border, legend marker hidden (same idiom as
   the KDE contour series), added **after** every other series so it draws
   on top; re-add after `drawStoredKdeContours` / `loadIsoelasticCurves` if
   they append series later. `setScatterHighlight(std::optional<int> frame)`
   uses `frameToScatterPoint_`; hide when −1. Cleared in `clearDisplay()`.
5. **Hit test.** `std::optional<int> scatterPointAt(QPointF viewPos) const`:
   compute pixel positions from the axis ranges and `plotArea()` (linear
   map; avoid per-point `mapToPosition`), skip points outside the visible
   ranges, nearest within `max(markerSize, 8)` px, ties → lowest frame index.
   O(n) per call; see the performance gate in step 9.
6. **Slots.** `onScatterClicked(QPointF, Qt::MouseButton)`: ignore unless
   left; ignore while `exportWatcher_`/export job or `viewerPending_`; hit →
   `setScatterHighlight`, `setSelectedFrame(frame, true)`,
   `viewerPending_ = true`, `scatterPlotView_->cancelGesture()`, queued
   `openScatterFrame(frame)`; miss → clear highlight.
   `onScatterDoubleClicked`: if `!scatterPointAt(pos)` and no viewer pending
   → `scatterPlotView_->resetZoom()`. `onScatterHover(QPointF)`: cursor
   pointing hand / arrow; optional `QToolTip::showText` with frame #, area,
   deformability.
7. **Viewer.** `openScatterFrame(int frame)` calls
   `showFrameViewer(frame, true)`; the dialog's prev/next path additionally
   calls `setScatterHighlight(navState->currentIndex)` when
   `navState->isValidSet`. After `exec()` returns: `viewerPending_ = false`,
   re-evaluate the cursor from `QCursor::pos()`. Test hook:
   `setFrameViewerSinkForTests(std::function<void(int frame, bool valid)>)`;
   when set, `showFrameViewer` records the request instead of constructing
   the dialog (same style as `setOverwriteAnswerForTests`).
8. **Export.** In `onExportCharts` / `beginExportJob` paths that call
   `renderChartSnapshots`: capture `scatterXAxis_`/`scatterYAxis_` min/max
   and the current highlight; hide the highlight before the grab; after the
   snapshot (and after the batch `updateCharts()`), restore ranges and
   highlight. Assert in the test that the snapshot has no highlight colour
   and the axes come back.
9. **Performance gate.** Extend the population fixture to N = 20 000 valid
   cells (parameterised). Measure (a) `scatterPointAt` per call and (b) a
   synthetic 30-step pan; gate ratio-based per `docs/howto/writing-tests.md`
   (hit test ≤ 1 ms at 20 k on the CI runner scale; pan step median under
   ~60 ms). If (b) fails, the fallback within this PR is to suppress the
   hover hit-test above a point threshold and to throttle pan repaints to one
   axis update per event-loop turn; if the series rebuild itself is the cost,
   record it against TD-16 rather than widening the PR.
10. **Tests** (`frontend.hdf_review_scatter`, offscreen; show the tab, fix
    its size, switch to the Charts tab, `settle()` before synthesizing
    events; fixture: valid frames where some have `validation.isValid =
    false` so point k ≠ frame k):
    - click at point k → sink receives (frame_k, valid = true), table row
      selected, highlight visible at point k;
    - press–move–release past the threshold → axes shift, sink not called;
    - wheel-zoom, then click → correct point at the new scale; a point that
      is now outside the visible range is not hit;
    - two clicks before the queued open runs → one sink call;
    - click on point then double-click → no zoom reset; double-click on empty
      space → default ranges;
    - close file → highlight gone, ranges default; recording-file fixture →
      click does nothing;
    - export charts while zoomed and highlighted → snapshot pixel check has no
      highlight colour, ranges restored afterwards;
    - simulated prev/next to a frame without a point → highlight hidden.
    Register in `tests/CMakeLists.txt` with `LABELS "frontend;safety"`,
    TIMEOUT 60.
11. **Docs.** `knowledge_map/frontend/HdfReviewTab.md`: new "Scatter
    interaction" section (maps, highlight, hit test, viewer gating, export
    save/restore, test hooks). `docs/manual/review-and-postprocess.md`:
    zoom/pan/click paragraph; if the Charts screenshot changes, update the
    `screenshot_tour` harness and run `scripts/check_screenshots.py`.
    `Recent-Work.md` dated entry. Run `python3 scripts/check_docs.py`.

## Acceptance criteria

- [ ] Wheel zoom / left- or middle-drag pan on the review scatter; drag never
      opens a frame; Monitoring scatter behaviour unchanged except the
      cursor timing.
- [ ] Single left click within tolerance of a visible point opens the viewer
      on the right frame of the **valid** set with the Charts tab active,
      highlights the point and selects the table row; empty-space click
      clears the highlight; axes/legend clicks do nothing.
- [ ] Two rapid clicks open one viewer; closing leaves zoom intact and no
      sticky pan.
- [ ] Double-click on empty space and "Reset zoom" restore the data extent;
      a double-click whose first click hit a point does not reset.
- [ ] File load/close resets zoom and highlight; exports (single and batch)
      are full-extent without highlight and restore the user's zoom.
- [ ] Recording files: no click/hover effect.
- [ ] Hit test and pan meet the performance gate at 20 k points, or the
      documented fallback is in place.
- [ ] `frontend.zoomable_chart_view` and `frontend.hdf_review_scatter` pass
      on the Linux backend lane; existing `frontend.hdf_review_core` and
      `frontend.monitoring_kde_*` unchanged.
- [ ] Vault notes, manual and `Recent-Work.md` updated; `check_docs.py` and
      (if screenshots changed) `check_screenshots.py` clean.

## Progress

- [ ] PR 1 — `ZoomableChartView` click/drag disambiguation + test
- [ ] PR 2 — Review tab zoomable scatter, click-to-view, export save/restore
- [ ] Follow-up issue filed: persistent non-modal viewer following scatter
      clicks
