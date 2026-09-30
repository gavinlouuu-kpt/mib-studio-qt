# Review scatter: zoom/pan, click a point to view its frame, KDE colour and contour

Status: active

Date: 2026-09-30. Scope: the Qt Review tab only (`HdfReviewTab`, Charts
view). Companion notes: `knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/frontend/System-Utilities.md` (`ZoomableChartView`),
`knowledge_map/frontend/ExperimentMonitoringTab.md`,
`knowledge_map/task/2026-09-23-monitoring-kde-density.md`.
Interactive prototype of the interactions (browser mock, not the widget):
https://claude.ai/artifact/RvgErR7JqnFtYFzQVfNYbz

## Goal

In the Review tab's Charts view the deformability-vs-area scatter is
coloured by KDE density with the core contour drawn over it (the Monitoring
tab's look), can be zoomed and panned like the Monitoring scatter, and a
single left click on a point highlights that cell, selects its row in the
Valid Frames table and shows its image in a **docked frame pane beside the
scatter**, so the image never covers the plot. Dragging pans and never
selects. Exports, file reload and the Monitoring tab behave as before.

## Current state (what the spec builds on)

- The review scatter is one `QScatterSeries` in a plain `QChartView`
  (`src/frontend/tabs/HdfReviewTab.cpp:378-413`); no zoom or pan, no density
  colouring. The chart also carries the isoelastic `QLineSeries` curves and
  the stored KDE contour lines; right-click is `Qt::ActionsContextMenu` with
  `computeCoreAction_` ("Compute core contour from full run").
- `generateScatterPlot` (`:2428`) clears the series and appends one point per
  frame with `validation.isValid`, so **point k is not frame k**.
- Images open only from thumbnails (double-click) and table rows
  (double-click) via `showFrameViewer(int)` (`:1600`), which reads
  `isShowingValid_` to pick the dataset. `onTabChanged` sets
  `isShowingValid_ = (index == 0)`, so on the Charts tab (index 2) it is
  **false** and the tab would read `invalidFrames_`.
- `FrameViewerDialog` (`include/frontend/dialogs/FrameViewerDialog.h`) is a
  `QDialog` opened modally (`exec()`, `:1723`) with `setFrame`, overlay mode,
  ROI overlay, zoom, series prev/next and `requestPreviousFrame` /
  `requestNextFrame` signals that the tab answers by reading the neighbour
  frame and calling `setSelectedFrame`.
- `ZoomableChartView` (`src/frontend/widgets/ZoomableChartView.cpp`) is used
  by the Monitoring tab: wheel zoom around the cursor (Ctrl = X only, Shift =
  Y only, axis-label regions zoom one axis), **left press starts a pan
  immediately**, double-click resets to the ranges given by
  `setDefaultRange`.
- KDE: the kernel, Silverman bandwidth, sample and grid evaluation, core
  level, iso-contours and the colour ramp are Qt-free in
  `include/backend/processing/MonitoringDensity.h`
  (`normalizedDensity`, `gaussianKdeGrid`, `coreLevel`, `isoContours`,
  `densityRampColor`). The Monitoring tab colours points by routing them
  into `kKdeLevels` (8) level `QScatterSeries` (`kdeLevelColor`,
  `ExperimentMonitoringTab.cpp:1695-1715`, legend markers hidden) because
  per-point `setPointsConfiguration` cost ~220 ms per refresh. The Review
  tab already reads `/analysis @kde_core_json` and `/monitoring
  @kde_live_json` and draws them (`drawStoredKdeContours`, `:2259`), and
  computes a full-run record on demand (`computeFullRunCoreRecord`, fixed
  seed subsample above 5000 cells, core share from
  `Monitoring/KdeCoreFraction`, `:2325-2345`).
- Export (`renderChartSnapshots`, `:1986`; `chartToPixmap`, `:2628`)
  re-runs `generateScatterPlot` (with *another file's* data during batch
  export), resizes the view to a fixed size, grabs it and restores the size.

## Behaviour

### Charts view layout

- `chartsLayout` becomes a horizontal `QSplitter`: scatter (left, stretch
  3) | right column (stretch 2) holding the **frame pane** (top) and the
  histogram (bottom). The splitter position is persisted in `QSettings`
  (`Review/ChartsSplitter`).
- The frame pane shows the selected cell exactly as `FrameViewerDialog`
  does (image with the current overlay mode and ROI overlay, series
  prev/next, zoom, frame index / area / deformability) plus **Previous /
  Next** buttons and an **Open in window** button that opens today's modal
  viewer on the same frame for a large view. Empty state: "Click a point on
  the scatter to view the cell".
- The pane is a child widget of the tab: it never overlaps the scatter, is
  cleared on file close/reload, and is destroyed with the tab. Thumbnail
  and table double-click keep opening the modal viewer as today.

### KDE colour and contour

- **Density colouring on by default** (`Review/KdeEnabled`, checkbox
  "Density (KDE)" in the Charts view; persisted, versioned like the
  Monitoring keys). Bandwidth: Silverman per axis × the shared
  `Monitoring/KdeBandwidthFactor`. Points are routed into `kKdeLevels`
  level series coloured by `kdeLevelColor` (the Monitoring idiom, legend
  markers hidden); off restores the single blue series.
- **Cost bound.** Review files can hold 10⁵ valid cells; the O(n²)
  at-sample evaluation Monitoring uses (capped at 1000 points) does not
  scale. Rule: n ≤ 5000 → `normalizedDensity` at the samples; n > 5000 →
  `gaussianKdeGrid` (256 × 128 over the padded data range) from the same
  fixed-seed 5000-cell subsample `computeFullRunCoreRecord` uses, then each
  point's density is bilinearly interpolated from the grid and normalised by
  the grid maximum. The two paths must agree within 5 % (level assignment
  within one level) on the 4000-cell fixture; test both.
- **One KDE job per file load** on `QtConcurrent` (same idiom and
  file-changed guard as the full-run core job): it returns the per-point
  density levels and, when the file has **no stored full-run record**, a
  freshly computed `KdeCoreRecord`. The contour is drawn as "Core 90 %
  (computed, unsaved)" dashed grey-blue; the context-menu action becomes
  **Save core contour to file** (same confirm/overwrite/read-only
  behaviour as today's compute action) and the label reverts to
  "Compute…" when nothing is computed. A stored full-run record is drawn as
  today and not recomputed; the stored live record stays dashed orange.
- While the job runs the scatter shows the plain blue series; the status
  text says "Estimating density (n cells)…". Toggling the checkbox re-routes
  the cached densities without recomputing; a bandwidth change in
  Monitoring Settings invalidates the cache on the next load.
- Chart export and batch export render the scatter as displayed (density
  colours and contours), at full extent, without the highlight.
- Recording files (no metrics): no density, no contour, nothing to click.

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
  point), and a new **Reset zoom** entry in the scatter's right-click menu.
- Middle-drag also pans (no existing binding; no click ambiguity).

### Click on a point

- Hit test: the nearest scatter point to the release position, measured in
  **screen pixels** (tolerance `max(markerSize, 8 px)`), considering only
  points whose data value lies inside the visible axis ranges. Ties go to the
  lowest frame index. The test runs on the point ↔ frame maps and the
  frames' data values, **independent of which level series a point sits
  in**; isoelastic curves, KDE contours and the highlight series are never
  hit.
- Hit → highlight the point (one-point series drawn on top of every level
  series), `setSelectedFrame(frame, /*valid=*/true)` so the Valid Frames
  table row and thumbnail follow, and `framePane_->setFrame(...)` with the
  image, mask and series read on demand (the same reads `showFrameViewer`
  does). Reads are synchronous single-frame hyperslab reads as today.
- Click on empty plot space, axes or legend → no change (selection is only
  cleared by file change). Modifier keys do not change click behaviour.
- Previous / Next in the pane (and ←/→ while the pane has focus) step
  through `validFrames_` in frame order; the highlight follows; on a frame
  without a point (failed validation) the highlight is hidden and the pane
  says so. The view does not auto-pan to follow.
- Cursor: pointing hand over a point, closed hand while panning, arrow
  otherwise. Hover uses the **same** nearest-point function as the click
  (not `QScatterSeries::hovered`). Tooltip on hover: frame #, area (µm²),
  deformability, density level when KDE is on.
- Clicks and hover are ignored while an export job is running (the export
  redraws the scatter with other files' data) and while the KDE job for
  the current file is still running they work but the density in the
  tooltip reads "pending".

### Zoom state across the tab's other flows

- File load / reload / close: zoom resets to the data extent, highlight and
  pane cleared, KDE cache dropped. `generateScatterPlot` records its
  computed axis ranges with `setDefaultRange` so the reset target is the
  data extent, not the 0–1000 / 0–1 fallback.
- Chart export, single and batch: save the four axis bounds and the
  highlight before `renderChartSnapshots`, hide the highlight for the grab,
  restore both after (batch already calls `updateCharts()` afterwards;
  restore after that). The pane is untouched by export.

## Non-goals

- React/Tauri shell: its review screen has no chart yet ("Chart rendering
  lands with UI-3 (#268)"); follow up after #268.
- TD-17 (µm² factor from the recorded calibration): unaffected, mapping is
  by index. The density is estimated in the same µm² the scatter shows, so
  it follows TD-17's fix automatically.
- Clicking points on the live Monitoring scatter.
- A density heat-map layer (Monitoring decided against it; same reasons).

## Decision log

- 2026-09-30: **Single click selects and shows the frame** (user request);
  the rest of the tab keeps click = select, double-click = open.
- 2026-09-30: **Left button carries both click and pan**, disambiguated by
  `startDragDistance()`. Right-drag was considered and rejected: right-click
  already owns the context menu (and Qt opens it on press on X11, on release
  on Windows), the Monitoring tab shares the widget and pans with left-drag,
  and trackpads make right-drag awkward. Middle-drag added as an unambiguous
  second pan binding.
- 2026-09-30: **Docked frame pane instead of the modal viewer** (user: the
  image must not block the scatter while looking at the scatter). A
  non-modal floating `FrameViewerDialog` was considered; it still covers the
  plot on a single monitor until moved, and needs the same close-on-file-
  change handling. A child widget in a splitter can never cover the plot,
  needs no "viewer pending" gating, dies with the tab, and keeps the modal
  dialog available through "Open in window". The earlier decision to keep the
  modal viewer as the click target is superseded.
- 2026-09-30: **The pane reuses `FrameViewerDialog`'s rendering.** First
  try: embed one instance as a child (`setWindowFlags(Qt::Widget)`,
  `reject()` neutralised, dialog buttons hidden). If that fights QDialog too
  much, extract the image/overlay/zoom/series core into a `FrameImageView`
  widget used by both the dialog and the pane; do not write a second
  overlay renderer.
- 2026-09-30: **KDE colour and contour on the review scatter** (user). Reuse
  the backend kernel and the Monitoring level-series rendering; grid +
  interpolation above 5000 cells to bound the cost; auto-compute the
  full-run contour when the file has none but never write the file without
  the explicit save action.
- 2026-09-30: **Double-click reset stays**, restricted to pairs where neither
  click hit a point, plus a menu entry. With the pane there is no modal race;
  the rule only prevents a double-click on a point from zooming out.
- 2026-09-30: **Hit-test in the view, not via `QScatterSeries::clicked`**:
  the series signal gives coordinates, not a frame, would arrive from
  whichever level series holds the point, and its marker-geometry test would
  not match a pixel tolerance.

## Implementation plan

Three PRs; the first is shared infrastructure with no user-visible feature.

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
   press is pending.
3. `mouseReleaseEvent`: if `pressPending_` (never crossed the threshold) →
   `pressPending_ = false`, emit `plotClicked(QPointF viewPos, Qt::MouseButton)`
   only when the position is inside `chart()->plotArea()`. If `isPanning_`
   → end the pan as today.
4. `mouseDoubleClickEvent`: emit `plotDoubleClicked(QPointF)`; reset zoom
   only if `resetOnDoubleClick_` (default true) is set. The Review tab sets
   it false and decides itself.
5. `leaveEvent` / `focusOutEvent`: clear `pressPending_` and `isPanning_`,
   restore the arrow cursor. Public `cancelGesture()`.
6. `QAction* resetZoomAction()` owned by the view (text "Reset zoom"), for
   context menus.
7. Test (`frontend.zoomable_chart_view`, offscreen, `QTest` mouse events on a
   shown, fixed-size view after `settle()`): sub-threshold press/release →
   one `plotClicked`, axes unchanged; press, move past threshold, release →
   axes shifted, no `plotClicked`; wheel → range shrinks around cursor;
   double-click with default flag → default ranges restored; with flag off →
   `plotDoubleClicked` emitted, ranges unchanged; `leaveEvent` mid-drag → next
   move without button does not pan. Register in `tests/CMakeLists.txt`
   with `LABELS "frontend;utility"`, TIMEOUT 30.
8. Vault: `System-Utilities.md` (signals, threshold, `cancelGesture`,
   `resetZoomAction`); `ExperimentMonitoringTab.md` one line: cursor changes
   after the threshold, otherwise unchanged. Existing
   `frontend.monitoring_kde_*` tests must still pass.

### PR 2 — Review tab: zoomable scatter, click-to-view, docked frame pane

Files: `src/frontend/tabs/HdfReviewTab.cpp`,
`include/frontend/tabs/HdfReviewTab.h`, `resources/ui/HdfReviewTab.ui`,
`src/frontend/dialogs/FrameViewerDialog.cpp` (+ header),
new `tests/frontend/hdf_review_scatter_test.cpp` (fixture helpers from
`hdf_review_core_test.cpp`), `tests/CMakeLists.txt`,
`knowledge_map/frontend/HdfReviewTab.md`, `knowledge_map/frontend/Dialogs.md`,
`docs/manual/review-and-postprocess.md`,
`knowledge_map/current-state/Recent-Work.md`, this plan.

1. **Dataset parameter.** Change `showFrameViewer(int)` and
   `setSelectedFrame(int)` to take `bool valid`; existing callers pass
   `isShowingValid_`. The viewer's prev/next lambdas already carry
   `navState->isValidSet`; pass it through. Factor the "read image, mask and
   series for frame i of dataset d" block (used three times in
   `showFrameViewer`) into `loadFrameForDisplay(int, bool) ->
   ProcessedFrame` and use it for the pane too.
2. **Point ↔ frame maps.** In `generateScatterPlot`, fill
   `std::vector<int> scatterPointToFrame_` and
   `std::vector<int> frameToScatterPoint_` (−1 when the frame has no
   point). Clear both in `clearDisplay()`. Record the computed axis ranges
   with `scatterPlotView_->setDefaultRange(...)` for both axes.
3. **Layout.** Replace `chartsLayout` in the `.ui` with a `QSplitter`
   (scatter | vertical splitter: pane, histogram); restore/save its state in
   `QSettings`. Frame pane: embedded `FrameViewerDialog` per the decision
   log (`Qt::Widget` flags, `reject()` overridden to no-op, its own
   prev/next wired to the tab's `stepSelectedFrame(±1)`), plus "Open in
   window" → `showFrameViewer(selected, true)`. Empty-state label when no
   frame is selected.
4. **View swap.** Construct `scatterPlotView_` as `ZoomableChartView`
   (`resetOnDoubleClick_ = false`), keep the context-menu policy and add
   `resetZoomAction()` after `computeCoreAction_`.
5. **Highlight series.** `scatterHighlight_`: `QScatterSeries`, one point,
   larger marker + contrasting border, legend marker hidden, added **after**
   every other series so it draws on top; re-add after any code path that
   appends series later (`drawStoredKdeContours`, `loadIsoelasticCurves`,
   PR 3's level series). `setScatterHighlight(std::optional<int> frame)`
   uses `frameToScatterPoint_`; hide when −1. Cleared in `clearDisplay()`.
6. **Hit test.** `std::optional<int> scatterPointAt(QPointF viewPos) const`:
   pixel positions from the axis ranges and `plotArea()` (linear map, no
   per-point `mapToPosition`), skip points outside the visible ranges,
   nearest within `max(markerSize, 8)` px, ties → lowest frame index. O(n)
   per call; see the performance gate in step 9.
7. **Slots.** `onScatterClicked(QPointF, Qt::MouseButton)`: ignore unless
   left; ignore while an export job runs; hit → `selectScatterFrame(frame)`
   = `setScatterHighlight` + `setSelectedFrame(frame, true)` +
   `framePane_->setFrame(loadFrameForDisplay(frame, true))`; miss → nothing.
   `stepSelectedFrame(int delta)`: wraps over `validFrames_`, calls
   `selectScatterFrame`. `onScatterDoubleClicked`: if `!scatterPointAt(pos)`
   → `scatterPlotView_->resetZoom()`. `onScatterHover(QPointF)`: cursor +
   `QToolTip`.
8. **Export.** Around `renderChartSnapshots` (single and batch): capture
   axis min/max and the highlight; hide the highlight before the grab;
   after the snapshot (and after the batch `updateCharts()`) restore ranges
   and highlight. Test that the snapshot has no highlight colour and the
   axes come back.
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
    - click at point k → pane shows frame_k of the **valid** set (test hook
      `framePaneFrameForTests()` returns the index and dataset), table row
      selected, highlight visible at point k, pane visible beside the chart
      (geometries do not intersect);
    - press–move–release past the threshold → axes shift, pane unchanged;
    - wheel-zoom, then click → correct point at the new scale; a point that
      is now outside the visible range is not hit;
    - click on point then double-click → no zoom reset; double-click on empty
      space → default ranges;
    - Previous/Next → pane and highlight follow; a frame without a point →
      highlight hidden;
    - close file → highlight gone, pane empty, ranges default;
      recording-file fixture → click does nothing;
    - export charts while zoomed and highlighted → snapshot has no highlight
      colour, ranges restored afterwards.
    Register with `LABELS "frontend;safety"`, TIMEOUT 60.
11. **Docs.** `HdfReviewTab.md`: new "Scatter interaction and frame pane"
    section (maps, highlight, hit test, pane lifecycle, export
    save/restore, test hooks). `Dialogs.md`: `FrameViewerDialog` embeddable
    mode. Manual: zoom/pan/click paragraph and a Charts-view screenshot
    with the pane; update the `screenshot_tour` harness and run
    `scripts/check_screenshots.py`. `Recent-Work.md` dated entry. Run
    `python3 scripts/check_docs.py`.

### PR 3 — Review scatter: KDE density colour and auto-computed contour

Files: `src/frontend/tabs/HdfReviewTab.cpp` (+ header, `.ui`),
`include/backend/processing/MonitoringDensity.h` (grid interpolation
helper), `tests/backend/monitoring_density_test.cpp`,
`tests/frontend/hdf_review_scatter_test.cpp`,
`knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/task/2026-09-23-monitoring-kde-density.md`,
`docs/manual/review-and-postprocess.md`, `Recent-Work.md`, this plan.

1. **Backend helper.** `densityAtPointsFromGrid(const DensityGrid&, const
   std::vector<DensityPoint>&) -> std::vector<double>` (bilinear, clamped to
   the grid, normalised by the grid maximum) next to `gaussianKdeGrid`.
   Backend test: on a 4000-point two-cluster set, grid path vs
   `normalizedDensity` agree within 5 % RMS and the 8-level assignment
   differs by at most one level for ≥ 99 % of points; degenerate input
   (all points equal, non-finite) yields zeros without throwing.
2. **Review KDE job.** `startReviewKdeJob()` after `generateScatterPlot`
   on load: copies the µm² points, the bandwidth factor and core fraction,
   runs on `QtConcurrent` (`kdeWatcher_`, file-changed guard, drained in the
   destructor like `coreWatcher_`), returns `{levels per point,
   optional<KdeCoreRecord>}` (record only when no stored full-run record).
   `onReviewKdeFinished`: cache levels, `routeScatterByLevel()`, draw the
   computed contour, update the menu action label, status text.
3. **Level series.** Create `kKdeLevels` level series once (Monitoring
   idiom: colour `kdeLevelColor`, legend markers hidden, hidden until KDE is
   on). `routeScatterByLevel()` moves points between the plain series and
   the level series from the cache without touching the maps
   (`scatterPointToFrame_` is by point, not by series). Re-add the highlight
   series last. Checkbox "Density (KDE)" in the Charts view →
   `Review/KdeEnabled` (+ `Review/KdeVersion`).
4. **Contour and save action.** Draw the computed record with
   `drawStoredKdeContours`'s series style but dashed grey-blue and legend
   text "Core 90 % (computed, unsaved)". Rename `computeCoreAction_`
   dynamically: "Save core contour to file" when a computed record exists
   (skips the compute, goes straight to the confirm/write path), "Compute
   core contour from full run" otherwise. Stored full-run record present →
   no recompute, label as today.
5. **Export.** Snapshots include level colours and both contours (they are
   chart series; nothing extra), highlight hidden as PR 2.
6. **Tests** (extend `frontend.hdf_review_scatter`): two-cluster fixture →
   cluster centres land in higher levels than the outliers; toggling the
   checkbox off restores one plain series with all points and on re-routes
   without a new job (watcher stays null); file without full-run record →
   computed contour series present with the "unsaved" name and the action
   reads "Save…"; file with stored record → no computed contour, action
   unchanged; click hit-test still resolves point k → frame_k with KDE on
   (points spread over level series); recording file → no job started.
   Performance: the job on the 20 000-cell fixture finishes under the
   ratio gate (grid path), and the GUI thread is never blocked by it
   (5 ms-tick p99 under 60 ms during the job, as the KDE e2e measures).
7. **Docs.** `HdfReviewTab.md` "Density colouring" section; the KDE task
   note gets a "Review tab" paragraph (grid path threshold, unsaved
   contour); manual paragraph + the Charts screenshot from PR 2 regenerated;
   `Recent-Work.md`.

## Acceptance criteria

- [ ] Wheel zoom / left- or middle-drag pan on the review scatter; drag never
      selects; Monitoring scatter behaviour unchanged except the cursor
      timing.
- [ ] Single left click within tolerance of a visible point highlights it,
      selects the Valid Frames row and shows the cell in the docked pane
      **beside** the scatter (never covering it), with the Charts tab
      active and the **valid** dataset; "Open in window" opens the modal
      viewer on that frame.
- [ ] Previous/Next in the pane move highlight and image; frames without a
      point hide the highlight.
- [ ] Double-click on empty space and "Reset zoom" restore the data extent;
      a double-click whose first click hit a point does not reset.
- [ ] File load/close resets zoom, highlight and pane; exports (single and
      batch) are full-extent without highlight and restore the user's zoom.
- [ ] Scatter is coloured by KDE density by default with the Monitoring
      ramp; toggle persists; contour drawn from the stored record or
      auto-computed (unsaved) and saveable from the context menu; files
      with > 5000 cells use the grid path and stay within the performance
      gate; the GUI thread is not blocked by the estimate.
- [ ] Recording files: no density, no contour, no click/hover effect.
- [ ] Hit test and pan meet the performance gate at 20 k points, or the
      documented fallback is in place.
- [ ] `frontend.zoomable_chart_view`, `frontend.hdf_review_scatter` and the
      backend density test pass on the Linux backend lane; existing
      `frontend.hdf_review_core` and `frontend.monitoring_kde_*` unchanged.
- [ ] Vault notes, manual, screenshot harness and `Recent-Work.md` updated;
      `check_docs.py` and `check_screenshots.py` clean.

## Progress

- [ ] PR 1 — `ZoomableChartView` click/drag disambiguation + test
- [ ] PR 2 — Review tab zoomable scatter, click-to-view, docked frame pane,
      export save/restore
- [ ] PR 3 — KDE density colouring, auto-computed contour, save action
