# Review scatter: zoom/pan, click a point to view its frame, KDE colour and contour

Status: active

Tracking issue: #465 (sub-issues #466–#470).

Date: 2026-09-30. Scope: the Review tab Charts view in **both shells** —
Qt (`HdfReviewTab`) and React + Tauri (`desktop/`, UI-3 #268 / UI-4 #269).
Density science and review data plumbing live in the backend behind
`BackendFacade` so both shells consume one implementation (decoupling plan
[`2026-07-15-qt-decoupling-and-tauri-migration.md`](2026-07-15-qt-decoupling-and-tauri-migration.md),
principle 3). Companion notes: `knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/frontend/System-Utilities.md` (`ZoomableChartView`),
`knowledge_map/frontend/ExperimentMonitoringTab.md`,
`knowledge_map/task/2026-09-23-monitoring-kde-density.md`,
ADR [0004](../../decisions/0004-bridge-contract-and-operation-state.md)
(bridge contract governance).
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
The density levels, the core contour and the scatter data come from one
backend job exposed over the bridge, so the React Charts view has the same
behaviour with no science in TypeScript.

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
- `MonitoringDensityService` (`include/backend/services/MonitoringDensityService.h`)
  already runs the kernel off the GUI thread for the live Monitoring scatter:
  `MonitoringDensityInput` is `frameIndices` + µm² `points` +
  `pixelToMicron`; `MonitoringDensityResult` carries per-frame normalised
  `density`, `coreLevel`, `contours` and the grid range. It is not exposed
  through `BackendFacade` and has no review (one-shot, file-fed) mode.
- React + Tauri shell: review is paged over the bridge —
  `fetch_review_metadata`, `fetch_review_metrics_page(valid, offset, count)`
  (`MonitoringRow`: frame_index, area, deformability, ring_ratio, …) and
  `fetch_review_frame_packet(dataset, index)` (image/mask bytes through the
  frame-pull transport; datasets `ValidImage`/`ValidMask`/… in
  `crates/mib-bridge/contract/bridge-contract.json`). The Charts tab is a
  placeholder ("Chart rendering lands with UI-3 (#268)"); no chart library in
  `desktop/package.json`; nothing about KDE crosses the bridge. Contract is
  `abi_version` 14; changes are additive and checked by
  `crates/mib-bridge/tests/contract.rs`, `static_assert`s in `shim.cpp` and
  `scripts/gen_bridge_contract.py --check`.

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
- **One backend density job per file load** (`BackendFacade` review
  density operation, run by `MonitoringDensityService` in a one-shot review
  mode on its low-priority thread; file-changed guard by review session
  id): it returns the per-frame density levels and, when the file has **no
  stored full-run record**, a freshly computed `KdeCoreRecord`. Shells
  never run the kernel themselves. The contour is drawn as "Core 90 %
  (computed, unsaved)" dashed grey-blue; the context-menu action becomes
  **Save core contour to file** (same confirm/overwrite/read-only
  behaviour as today's compute action) and the label reverts to
  "Compute…" when nothing is computed. A stored full-run record is drawn as
  today and not recomputed; the stored live record stays dashed orange.
- While the job runs the scatter shows the plain blue series; the status
  text says "Estimating density (n cells)…". Toggling the checkbox re-routes
  the cached densities without recomputing. Bandwidth factor and core
  fraction are the density service's current `MonitoringDensitySettings`
  (each shell pushes its Monitoring settings there); a change takes effect
  on the next file load.
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

## Tauri parity

The split between what is shared and what each shell owns:

| Concern | Qt shell | React + Tauri shell | Shared source |
|---|---|---|---|
| Scatter data (frame index, µm² area, deformability, target flag) | `fetchReviewScatter` via facade | `fetch_review_scatter` | Facade reads `/valid_frames` metadata once per file |
| Point ↔ frame maps | built from the facade array | built from the bridge array | Same array, same order (valid set, `isValid` only) |
| Density levels, core contour, grid path > 5000 cells | consumes facade result | `fetch_review_density` + `ReviewDensity` operation event | `MonitoringDensityService` review mode |
| Stored full-run / live KDE records | facade read | `fetch_review_kde_records` | `Hdf5Service::readKdeAnalysisJson` / `readKdeLiveJson` |
| Save computed contour | facade command (confirm in shell) | `review_save_core_contour(overwrite)` | Facade → `Hdf5Service::openFileForUpdate` |
| Frame image / mask for the pane | `loadFrameForDisplay` (reads via facade in a later step; `hdfReader_` today) | `fetch_review_frame_packet` (exists) | `Hdf5Service` hyperslab reads |
| Click-vs-drag, wheel zoom, pan, double-click reset, pixel hit test, hover | `ZoomableChartView` + tab slots | pointer events on a `<canvas>` | **Rules** in this plan (thresholds, tolerance, tie-break) + a shared fixture of expected hits |
| Docked frame pane, Prev/Next, "Open in window" | splitter + embedded `FrameViewerDialog` | CSS grid panel + overlay component | Behaviour spec only |
| Density colour ramp | `densityRampColor` | ramp stops emitted into `bridgeContract.ts` by the generator | `MonitoringDensity.h` is the source of the stops |
| Chart snapshot export | `renderChartSnapshots` (Qt widget grab) | out of scope here (follow-up: shell PNG or backend rasteriser) | — |
| Preferences (KDE on/off, splitter) | `QSettings` `Review/*` | Tauri store | Shell-local by design |

Bridge additions (one additive contract bump, `abi_version` 14 → 15 at
time of writing; take the next free number when the PR lands):

- `fetch_review_scatter() -> BridgeReviewScatter { valid, session_id, frame_index[], area_um2[], deformability[], target_group[], pixel_to_micron }`
  — columnar, valid set only, `isValid` rows only, in `validFrames_`
  order. 10⁵ cells ≈ 2.5 MB columnar; if IPC time exceeds the review
  budget, move it onto the binary frame-packet transport (packet kind
  appended to the contract).
- `fetch_review_density() -> BridgeReviewDensity { valid, session_id, ready, levels: u8[] (parallel to the scatter), level_count, bandwidth_factor, core_fraction, computed_record_json }`
  — `ready = false` while the job runs; `computed_record_json` empty when a
  stored full-run record exists.
- `fetch_review_kde_records() -> { analysis_json, live_json }` (empty when
  absent or unreadable; reason in `BackendError`).
- `review_save_core_contour(overwrite: bool) -> CommandResult` — refuses
  with a distinct code when a record exists and `overwrite = false`, when
  the file is read-only, or while an export job runs.
- `operation_kinds`: append `ReviewDensity`; completion/failure through the
  existing `OperationStatus` event, polled by `poll_events`.
- `density_ramp` table appended to the contract JSON (8 RGB stops) so the
  generated `bridgeContract.ts` carries the same colours.

## Non-goals

- Chart snapshot export from the Tauri shell (the Qt export keeps using
  its widget grab); separate follow-up once the React chart exists.
- A backend chart rasteriser.
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
- 2026-09-30: **Density job and review scatter data live in the backend,
  not in `HdfReviewTab`** (user: how does this work with React + Tauri?).
  A Qt-only `QtConcurrent` job would have to be rewritten in the Tauri
  shell and the two could drift on bandwidth, levels and core fraction.
  `MonitoringDensityService` already has the kernel, the low-priority
  thread and a result type carrying per-frame density and contours; a
  one-shot review mode plus facade/bridge commands costs about the same as
  the Qt job. Gestures, the pane and preferences stay shell code; their
  rules are specified here and exercised by a shared hit-test fixture.
- 2026-09-30: **Bridge payload is columnar and valid-set only**, fetched
  once per file, rather than rebuilding the scatter from paged
  `fetch_review_metrics_page` calls (10⁵ cells would be hundreds of page
  round trips, and paging order is not a contract for point indices).
- 2026-09-30: **Double-click reset stays**, restricted to pairs where neither
  click hit a point, plus a menu entry. With the pane there is no modal race;
  the rule only prevents a double-click on a point from zooming out.
- 2026-09-30: **Hit-test in the view, not via `QScatterSeries::clicked`**:
  the series signal gives coordinates, not a frame, would arrive from
  whichever level series holds the point, and its marker-geometry test would
  not match a pixel tolerance.

## Implementation plan

Five PRs. PR 1 and PR 3a are infrastructure with no user-visible feature;
PR 2 and PR 3b are the Qt shell; PR 4 is the React + Tauri shell. Order:
PR 1 → PR 2 (Qt interaction ships without density); PR 3a can run in
parallel with PR 2; PR 3b needs PR 2 + PR 3a; PR 4 needs PR 3a and the
chart groundwork of #268.

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
   PR 3b's level series). `setScatterHighlight(std::optional<int> frame)`
   uses `frameToScatterPoint_`; hide when −1. Cleared in `clearDisplay()`.
6. **Hit test.** `std::optional<int> scatterPointAt(QPointF viewPos) const`:
   pixel positions from the axis ranges and `plotArea()` (linear map, no
   per-point `mapToPosition`), skip points outside the visible ranges,
   nearest within `max(markerSize, 8)` px, ties → lowest frame index. O(n)
   per call; see the performance gate in step 9. Put the pure part (ranges,
   plot rect, points, position → point index) in a Qt-free function and
   add the shared fixture `tests/fixtures/review_scatter_hits.json`; PR 4's
   TypeScript hit test runs against the same file.
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

### PR 3a — Backend review density + bridge commands (no UI)

Files: `include/backend/processing/MonitoringDensity.h`,
`include/backend/services/MonitoringDensityService.h` +
`src/backend/services/MonitoringDensityService.cpp`,
`include/backend/app/BackendFacade.h` + `src/backend/app/BackendFacade.cpp`,
`crates/mib-bridge/contract/bridge-contract.json`,
`crates/mib-bridge/src/{lib.rs,shim.h,shim.cpp}`,
`crates/mib-bridge/tests/contract.rs`, `desktop/src-tauri/src/lib.rs`
(command registration), `desktop/src/bridge.ts`, generated
`desktop/src/bridgeContract.ts`, `tests/backend/monitoring_density_test.cpp`,
new `tests/backend/review_density_test.cpp`, `tests/CMakeLists.txt`,
`knowledge_map/services/` note for the density service,
`knowledge_map/task/2026-09-23-monitoring-kde-density.md`,
`docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`
(parity matrix row), this plan.

1. **Grid interpolation helper.** `densityAtPointsFromGrid(const
   DensityGrid&, const std::vector<DensityPoint>&) -> std::vector<double>`
   (bilinear, clamped to the grid, normalised by the grid maximum) next to
   `gaussianKdeGrid`. Backend test: on a 4000-point two-cluster set, grid
   path vs `normalizedDensity` agree within 5 % RMS and the 8-level
   assignment differs by at most one level for ≥ 99 % of points; degenerate
   input (all points equal, non-finite) yields zeros without throwing.
   `levelForDensity(double, int levels)` moves here from the Monitoring tab
   so both shells and the service share the bucketing.
2. **Review mode in `MonitoringDensityService`.** `requestReviewEstimate(
   ReviewDensityRequest{sessionId, frameIndices, points, pixelToMicron,
   bandwidthFactor, coreFraction, wantCoreRecord})`: one-shot, runs on the
   service's existing lowest-priority thread between live ticks (never
   concurrently with a live estimate; live Monitoring keeps priority while
   an experiment is Active), n ≤ 5000 → at samples, else fixed-seed 5000
   subsample → grid → interpolate. Result: `ReviewDensityResult{sessionId,
   levels (u8, parallel to input), levelCount, record (optional
   KdeCoreRecord via computeFullRunCoreRecord's parameters)}`. A request
   for a newer session supersedes an older pending one. Service test:
   supersede, cancel on shutdown, parity with the helper, no effect on the
   live path's `MonitoringDensityStats` budget.
3. **Facade.** On review load (`loadRecording` of an experiment file) the
   facade builds the columnar valid-set scatter from `readValidMetadata`
   (same filter and order as `HdfReviewTab::generateScatterPlot`, µm² with
   the facade's pixel-to-micron factor), reads the stored KDE records, and
   always submits the review density request (levels are always
   computed; `wantCoreRecord` is false when a stored full-run record
   exists), taking bandwidth factor and core fraction from the service's
   current `MonitoringDensitySettings`. New facade
   methods: `reviewScatter()`, `reviewDensity()`, `reviewKdeRecords()`,
   `saveReviewCoreContour(bool overwrite)` (closes the review reader, writes
   through `Hdf5Service::openFileForUpdate`, reopens; refuses while an
   export job runs), plus a `ReviewDensity` operation reported through the
   existing operation-state machinery (ADR 0004). Recording files: no
   scatter, no job.
4. **Bridge contract bump** (additive, next free `abi_version`): commands,
   structs, `operation_kinds.ReviewDensity`, `density_ramp` table exactly as
   listed under "Tauri parity". Regenerate `bridgeContract.ts`
   (`scripts/gen_bridge_contract.py`), extend `shim.cpp` `static_assert`s.
5. **Tests.** `backend.review_density` (facade on the population fixture:
   scatter array order equals the valid-set `isValid` order; levels
   parallel; stored-record file → no computed record; recording file → no
   job; save/overwrite/read-only/export-running refusals). Bridge
   `contract.rs`: `fetch_review_scatter` / `fetch_review_density` round
   trip on a fixture file, `ready` goes false → true, event observed via
   `poll_events`. Linux backend lane + `bridge-ci.yml`; TSan lane for the
   service change (it touches a thread).
6. **Docs.** Service note (review mode, supersede rule, priority vs live);
   KDE task note paragraph; decoupling plan parity matrix: Review row notes
   "scatter/density contract landed"; `Recent-Work.md`.

### PR 3b — Qt Review scatter: KDE colour and contour from the facade

Files: `src/frontend/tabs/HdfReviewTab.cpp` (+ header, `.ui`),
`tests/frontend/hdf_review_scatter_test.cpp`,
`knowledge_map/frontend/HdfReviewTab.md`,
`docs/manual/review-and-postprocess.md`, `Recent-Work.md`, this plan.

1. **Scatter from the facade.** `generateScatterPlot` builds the maps and
   points from `backend_.facade().reviewScatter()` (identical order to
   today's loop, now asserted by the PR 3a test). Batch export keeps its
   per-file reader path for snapshots.
2. **Levels.** Poll `reviewDensity()` on the `ReviewDensity` operation
   event (queued to the GUI thread); on `ready`, cache the levels and
   `routeScatterByLevel()`. Create `kKdeLevels` level series once
   (Monitoring idiom: `kdeLevelColor`, legend markers hidden, hidden until
   KDE is on). Routing moves points between the plain series and the level
   series without touching the maps. Re-add the highlight series last.
   Checkbox "Density (KDE)" → `Review/KdeEnabled` (+ `Review/KdeVersion`);
   toggling re-routes from the cache, never re-requests.
3. **Contour and save action.** Draw the computed record with
   `drawStoredKdeContours`'s series style but dashed grey-blue, legend
   "Core 90 % (computed, unsaved)". `computeCoreAction_` reads "Save core
   contour to file" when a computed record exists (confirm dialog →
   `saveReviewCoreContour(overwrite)`), "Compute core contour from full run"
   otherwise; the tab's own `QtConcurrent` compute path is removed in
   favour of the facade job (`computeFullRunCoreForTests` hook re-pointed).
4. **Export.** Snapshots include level colours and both contours;
   highlight hidden as PR 2.
5. **Tests** (extend `frontend.hdf_review_scatter`): two-cluster fixture →
   cluster centres in higher levels than outliers; toggle off → one plain
   series with all points, on → re-routed with no new request; file without
   full-run record → "unsaved" contour series and "Save…" action; stored
   record → no computed contour; click hit test still resolves point k →
   frame_k with KDE on; recording file → nothing. GUI 5 ms-tick p99 under
   60 ms while the 20 000-cell job runs. Existing `frontend.hdf_review_core`
   updated for the moved compute path, same assertions.
6. **Docs.** `HdfReviewTab.md` "Density colouring" section; manual
   paragraph; Charts screenshot regenerated; `Recent-Work.md`.

### PR 4 — React + Tauri Charts view (tracked under UI-3 #268 / UI-4 #269)

Files: `desktop/src/App.tsx` (Charts tab), new
`desktop/src/review/ReviewScatter.tsx`, `desktop/src/review/scatterGestures.ts`,
`desktop/src/review/scatterHitTest.ts`, `desktop/src/review/FramePane.tsx`,
their `*.test.ts`, `desktop/src/bridge.ts` (typed wrappers from PR 3a),
shared fixture `tests/fixtures/review_scatter_hits.json`,
`knowledge_map/frontend/` React review note, decoupling plan parity row,
`Recent-Work.md`, this plan.

1. **Data.** On review load: `fetch_review_scatter()` once; build maps in
   TS. Subscribe to `ReviewDensity` operation events from `poll_events`;
   on completion `fetch_review_density()` and `fetch_review_kde_records()`.
   Session id guards against a stale response after reload.
2. **Rendering.** One `<canvas>` (not SVG: 10⁵ points), device-pixel-ratio
   aware; points drawn per level with the `density_ramp` stops from
   `bridgeContract.ts` (plain colour while `ready = false` or KDE off);
   contours as polylines from the record JSON (stored full-run solid blue,
   live dashed orange, computed dashed grey-blue); highlight drawn last.
   Isoelastic curves: add them to the contract's static data or a bridge
   read in this PR if the React view needs parity; otherwise note as a gap
   in the parity matrix.
3. **Gestures** (`scatterGestures.ts`, pure state machine over pointer
   events, unit-tested without a DOM): same rules as the Qt view —
   drag threshold 10 CSS px (the web has no `startDragDistance`; 10 matches
   Qt's default), left/middle pan, wheel zoom at cursor with Ctrl/Shift and
   axis-region rules, double-click reset only when neither click hit,
   right-click menu with "Reset zoom" / "Save core contour to file"
   (`review_save_core_contour`, overwrite confirm in-app since
   `window.confirm` is not used in the shell), pan cleared on
   `pointerleave`/`blur`/`pointercancel`.
4. **Hit test** (`scatterHitTest.ts`): same algorithm — visible points
   only, pixel distance ≤ max(marker, 8), ties → lowest frame index. The
   shared fixture `tests/fixtures/review_scatter_hits.json` (axis ranges,
   plot rect, points, click positions → expected frame or none) is
   consumed by both a vitest test and a C++ case in
   `frontend.hdf_review_scatter`, so the two shells cannot drift.
5. **Frame pane.** CSS grid column beside the canvas (stacks under it
   below ~900 px width); image via `fetchReviewImage(ValidImage, idx)` and
   mask via `ValidMask` through the existing `framePullScheduler`
   ("review" slot, latest-wins); Prev/Next and ←/→; "Open in window" =
   in-app overlay component, not a native window.
6. **Preferences.** KDE toggle and column split in the Tauri store.
7. **Tests.** vitest: gesture state machine (click vs drag, double-click
   rule, leave/blur), hit-test fixture, level colouring falls back to plain
   while not ready, stale-session response ignored. Desktop Xvfb smoke
   (`desktop-ci.yml`): load the fixture file, click a known point, assert
   the pane requested the expected frame index.
8. **Docs.** React review note; decoupling plan parity row → "Review
   charts: scatter, density, click-to-view"; chart export noted as the
   remaining gap.

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
- [ ] Density levels, computed contour and scatter data come from the
      backend review density job via `BackendFacade`; neither shell runs the
      kernel; bridge contract bump is additive and `contract.rs`,
      `gen_bridge_contract.py --check` and the `shim.cpp` asserts pass.
- [ ] React Charts view: same click/drag/zoom/reset/pane behaviour; the
      shared hit-test fixture passes in vitest and in the C++ test; density
      colours match the contract ramp.
- [ ] Recording files: no density, no contour, no click/hover effect (both
      shells).
- [ ] Hit test and pan meet the performance gate at 20 k points, or the
      documented fallback is in place.
- [ ] `frontend.zoomable_chart_view`, `frontend.hdf_review_scatter`,
      `backend.review_density`, the backend density test and the bridge
      contract tests pass on their lanes; TSan clean for the service change;
      existing `frontend.hdf_review_core` and `frontend.monitoring_kde_*`
      unchanged in what they assert.
- [ ] Vault notes, manual, screenshot harness, decoupling-plan parity row and
      `Recent-Work.md` updated; `check_docs.py` and `check_screenshots.py`
      clean.

## Progress

- [ ] PR 1 (#466) — `ZoomableChartView` click/drag disambiguation + test
- [ ] PR 2 (#467) — Qt Review tab zoomable scatter, click-to-view, docked frame
      pane, export save/restore
- [ ] PR 3a (#468) — Backend review density mode, facade methods, bridge contract
      bump + contract tests (no UI)
- [ ] PR 3b (#469) — Qt KDE colouring, computed contour and save action from the
      facade
- [ ] PR 4 (#470) — React + Tauri Charts view (#268 / #269): canvas scatter,
      shared gesture/hit-test rules, frame pane
- [ ] Follow-up issue: chart snapshot export from the Tauri shell
