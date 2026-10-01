# Review scatter: zoom/pan and click a point to view the cell

Request: in Review, look at the deformability-vs-area scatter and click a
point to see that cell's image, without the image covering the plot while
looking at it; zoom and drag must keep working. Issues #466 (PR 1) and
#467 (PR 2) of #465; plan
[`2026-09-30-review-scatter-click-to-view`](../../docs/exec-plans/active/2026-09-30-review-scatter-click-to-view.md).
Notes: [[../frontend/HdfReviewTab]] ("Scatter interaction and frame pane"),
[[../frontend/System-Utilities]] (`ZoomableChartView`),
[[../frontend/Dialogs]] (`FrameViewerDialog` embedded mode).

## What changed

- `ZoomableChartView`: a press is a click until it travels
  `startDragDistance()`, then a pan; `plotClicked` / `plotDoubleClicked` /
  `hoverMoved`; optional double-click reset; `resetZoomAction()`;
  `cancelGesture()`; a leave with no button held drops a stale pan.
- `HdfReviewTab`: Charts view is a splitter scatter | (frame pane over
  histogram). Click → highlight + Valid Frames row + pane. Point ↔ frame
  maps; explicit dataset in `setSelectedFrame` / `showFrameViewer`;
  `loadFrameForDisplay` shared by the modal viewer and the pane; exports
  save/restore the view; `onTableSelectionChanged` keys on the sending table.
- `FrameViewerDialog::setEmbedded`: child-widget mode for the pane.
- `include/frontend/utils/ScatterHitTest.h`: Qt-free hit rule; fixture
  `tests/fixtures/review_scatter_hits.json` shared with the React shell.

## Traps met

- `isShowingValid_ == (index == 0)` is false on the Charts tab; everything
  selected from the scatter must name the valid dataset explicitly, and the
  table-selection slot must not infer the table from the tab.
- Batch export redraws the scatter with other files; the maps are flagged
  (`scatterShowsLiveFile_`) so a click cannot open the wrong file's frame.
- Qt 6.4 `QXYSeries::append(QList)` emits `pointAdded` per point → one
  geometry rebuild per point → O(n²). The 20 000-cell gate hung in
  `ScatterChartItem::setBrush` under `handlePointAdded`; `replace()` fixed
  it (≈1.6 s load). Lead recorded on TD-16 (Monitoring scatter stall).
- No QtTest in the tree: `tests/support/qt_mouse.h` sends mouse/wheel events
  to the view's viewport; `QSignalSpy` replaced by lambda counters.

## Review pass (2026-10-01)

`/code-review` on the branch found nine items; fixed: a lost release with
the pointer still over the chart could start a button-less pan (now a move
with the button up ends the gesture); double-click lacked the export guards
and the export restore dropped the user-zoomed flag; an invalid-set
selection cleared the pane but left the highlight (both now follow the last
valid cell); the modal viewer's prev/next read each frame twice (pane is
stale behind the modal, catches up on close) and `setSelectedFrame`
re-entered itself via `selectionChanged`; Export Charts duplicated
`renderChartSnapshots`; tooltip and pane title numbered cells by valid-set
position while the viewer and CSV use `ProcessedFrame::index`; splitter
state was written to disk per drag pixel; a redundant coordinate vector.
Not taken: rendering snapshots on an offscreen chart (the plan's
save/restore is deliberate; revisit if a third redraw path appears), and
"disable the camera in `ui_layout`" (the race was in the test's wait, and
the controller's status update can only arrive through the event loop,
which the fixed assertion no longer runs).

## End-to-end with the real app (2026-10-01)

`integration.review_scatter_e2e` (`tests/frontend/review_scatter_e2e_test.cpp`)
boots the real `MainWindow` on the mock camera with the public 512x96 cell
stream (`python scripts/provision-assets.py --asset 512x96stream-mock-frames
--count 1000`), relaxes the acceptance criteria, records an 8 s experiment
through `ExperimentCoordinator` (~3700 valid cells), opens the file in
Review and drives the Charts scatter with synthesized input, saving a
screenshot per state (`MIB_REVIEW_E2E_OUT`). Looking at the screenshots
found two things the widget tests had not: the embedded viewer's control
row was clipped at a 400 px pane (its Export button is now hidden there,
Export All covers it), and **every exported chart TIFF had red and blue
swapped** — `renderChartSnapshots` converted the grabbed `Format_RGB32`
(B,G,R,A in memory) as RGBA; pre-existing, fixed with `COLOR_BGRA2BGR`, and
the e2e now asserts the snapshot's point colour. Also: with the asset
provisioned, `integration.monitoring_kde_e2e` passes here; its earlier
failure was the missing frames (synthetic fallback), not a regression.

## Verification (Linux container, Ubuntu 24.04, system Qt 6.4.2, `linux-system-release`)

- `frontend.zoomable_chart_view`, `frontend.hdf_review_scatter`,
  `frontend.hdf_review_core`, `frontend.monitoring_kde_density` and the other
  runner-hosted frontend tests pass.
- `frontend.device_discovery` ("camera probe entered after the initial
  delay") fails in this container **with and without** these changes
  (checked by stashing the change and rebuilding); not caused here.
  `integration.monitoring_kde_e2e` failed for the same reason at first; with
  the asset it then failed its capture gate at an identical 200 fps because
  the gate compared raw frame counts over phases of unequal wall time (the
  KDE-off phase overruns while the GUI stalls, TD-16: 9.1 s vs 7.9 s). The
  capture and ring-fill gates now compare rates (`PhaseMetrics::seconds`).
- 20 000 cells: see the numbers in [[../frontend/HdfReviewTab]] ("Cost");
  the scaling table there (20 k / 100 k / 300 k) answers "is there a file
  size limit": memory is linear, opening is quadratic inside Qt Charts
  (`addToGroup` per marker), practical ceiling tens of thousands of cells
  on the Charts tab until TD-18.
- `frontend.ui_layout` became flaky (5 of 14 runs failed, 0 of 10 before):
  its long-status check raced the camera controller's late "Camera
  running" (the Overview auto-start bypasses the controller), and the heavier
  Charts tab moved the timing into the window. The test now re-applies the
  text and lays out + paints with no other events before asserting; 35/35
  passed afterwards. The race was in the test; the app behaviour is as before.
- Looking at the rendered Charts view (`MIB_REVIEW_SCATTER_OUT=<dir>` saves
  screenshots from the test) showed the embedded viewer's long control row
  squeezing the scatter to ~200 px; fixed (scatter ≥ 420 px, 60 % start,
  compact embedded controls). The same look corrected the pan numbers: at a
  real width the repaint dominates (TD-18).
- Not verified here: Windows / Conan Qt 6.7.3, the real app by hand.
