# HdfReviewTab

> Post-experiment review. Opens a saved HDF5 file and lets the user
> browse valid/invalid frames with their metrics.

**Source:** `src/frontend/tabs/HdfReviewTab.cpp`,
`include/frontend/tabs/HdfReviewTab.h`
**Related:** [[../services/Hdf5Service]], [[../data-model/HDF5-Storage]],
`models/HdfMetricsModel.cpp`

## Responsibility

- Open HDF5 file via `Hdf5Service::loadFile`.
- Lazily read metadata (not images) with
  `readValidMetadata` / `readInvalidMetadata`.
- On scroll/selection, fetch image payloads by index using
  `readImageByIndex` / `readImagesRange` (hyperslab reads — bounded memory).
- Display metrics in a `QTableView` backed by `HdfMetricsModel`.
- Optional charts: scatter + histograms over the saved dataset; click a
  scatter point to view the cell in the docked frame pane (see "Scatter
  interaction and frame pane").
- **Bounded file row** (issue #358): the `.ui` file row is now two rows
  (`fileRowLayout`: Select/Close/Export Metrics/Export All + a native
  **More…** `QToolButton` menu holding Batch Metrics, Batch Export All,
  Export Charts and Regenerate masks — the original buttons stay as hidden
  enable-state owners mirrored by `updateSecondaryActionState()`;
  `fileInfoRowLayout`: overlay combo, legend, ROI check, elided path
  (`ElidingLabel`, `setFilePathText`) and status). The thumbnail scroll
  areas no longer carry a 400 px minimum width.
- **Exports run as one asynchronous job at a time** (issue #344): every
  export button builds a `backend::recording::HdfExportRequest` on the GUI
  thread (series-range prompt, chart snapshots rendered into `cv::Mat`s via
  `renderChartSnapshots`), then `beginExportJob` runs
  [[../services/HdfExportService]] through `QtConcurrent::run` +
  `QFutureWatcher` with a cancellable `QProgressDialog`; the worker callable
  owns request/token/service and never captures a widget. Progress is
  re-dispatched via `QPointer` + queued invocation; export buttons are
  re-enabled only from the finished handler. A second export request while
  one runs is refused. Cancelled/failed jobs report "partial output was
  discarded" (or the retained `.partial-*` path); output folders are
  published only on success. Batch exports chain per file
  (`continueBatchExport`) using a separate reader for each file — the live
  `hdfReader_` / frame vectors are never moved out and the current file's
  charts are restored with `updateCharts()` afterwards. The destructor
  cancels a running job and waits (bounded by one frame).
- Metrics exports default to `<loaded-h5-basename>_metrics.csv` and suffix
  `_2`, `_3`, etc. before the save dialog opens when files already exist.
- `Export All` writes into a source-specific folder under the selected root
  (for example `<root>/<loaded-h5-basename>/metrics.csv` plus TIFF/chart
  outputs) instead of writing `metrics.csv` directly in a shared export root.
- Batch Metrics and Batch Export All let the user select multiple `.h5` /
  `.hdf5` files. Batch exports reserve output names/folders within the run,
  continue after per-file failures, and show a final success/failure summary.
- Metrics, Export All, and their batch flows remember their last successful
  output directory via `QSettings`.
- `Export All` now prompts how to export multi-image series frames:
  all frames, a custom 1-based range (for example `9-15`), or skip.

## Scalability

- Virtualised — the tab never loads all images at once.
- Supports files > 2 GB; see task
  `knowledge_map/task/review_2gb_scalability.md`.
- Close File button releases the HDF5 handle cleanly (recent crash fix —
  see `git log` entry
  "fix: add Close File button and fix dangling pointer crash in review tab").

## Recording-mode files

When a file has `/recording_info` present, [[../services/Hdf5Service]]'s
`isRecordingFile()` returns true and the tab branches into a single-view
layout:

- The "Invalid Frames" tab is hidden and "Valid Frames" is relabelled to
  "Frames"; all recorded frames populate `validFrames_`.
- Metadata comes from `readRecordingMetadata` (only `index` + `timestampNs`
  populated); status text uses `readRecordingInfo`.
- `readRecordingInfo` also exposes persisted multi-image flags
  (`multi_image_enabled`, `multi_image_count`). When enabled (`count > 1`),
  `showFrameViewer` synthesizes a per-frame series window by reading a bounded
  range from `/recorded_frames/images`, so FrameViewer's series prev/next
  controls are available in recording review mode.
- Dataset reads go through `imagesPath(bool)` / `masksPath(bool)` helpers
  that route to `/recorded_frames/images` (and return `""` for masks,
  since recording files have none).
- Disabled in recording mode: overlay combo, ROI overlay, Export Metrics
  (no per-frame metrics), Export Charts (no metrics to chart). Export All
  still writes the raw TIFF images. Regenerate Masks remains enabled and
  feeds `/recorded_frames/images` into `BatchMaskDialog`, then reloads the
  standard remasked HDF5 output.
- `clearDisplay()` restores default tab labels/visibility so loading an
  experiment file after a recording file works correctly.

## Regenerate masks button

Toolbar action **"Regenerate masks…"** opens [[Dialogs|BatchMaskDialog]]
(`include/frontend/dialogs/BatchMaskDialog.h`). The dialog drives
[[../services/ProcessingService]]'s `processBatch` API on either the
currently loaded HDF5 file's image dataset (`/valid_frames/images` for
standard experiment files, `/recorded_frames/images` for recording files)
with start/count, the whole HDF5 file, an AVI file, or a folder of
TIFF/PNG/JPEG images. Whole-file mode processes all recording frames for
recording files and both valid + invalid datasets for standard review files.
HDF5-sourced runs preserve source frame indices and store timestamps
normalised to the first regenerated image. Output is saved to a new HDF5
file via [[../services/BatchMaskSources]] `saveMasksToHdf5` and the tab
reloads from that file. If the user does not select a background frame,
the dialog can synthesize one tile-by-tile from the least-changing source
frames. The currently active
`ProcessingConfig`, ROI, and background image (from
`processing().getProcessingConfig()` / `getRealtimeRoi()` /
`getRealtimeBackgroundGray()`) are used as inputs.

## Stored KDE core contours

On open, `readStoredKdeRecords()` reads `/analysis @kde_core_json` and
`/monitoring @kde_live_json` through the retained reader and parses them with
`backend/processing/KdeCoreRecord.h`; `generateScatterPlot` redraws them via
`drawStoredKdeContours()` as `QLineSeries` on the scatter: full-run solid
blue, live (provisional) dashed orange, one legend entry per record
("Core 90% (full run)" / "Core 90% (live, provisional)"). An unreadable
record is logged and skipped; `clearDisplay()` removes the previous file's
contours. Test hooks: `loadHdfFileForTests`, `storedKdeContourSeriesForTests`.
**Compute core contour from full run** (scatter right-click,
`computeCoreAction`, enabled for experiment files with valid cells): the
valid cells (µm² with the same factor as the scatter, see TD-17) go to
`computeFullRunCoreRecord` (`KdeCoreRecord.h`: Silverman factor 1, core share
from `Monitoring/KdeCoreFraction`, fixed-seed subsample above 5000 cells,
grid over the padded data range) on `QtConcurrent`. On completion an existing
full-run record needs confirmation to be replaced; saving closes the review
reader, writes `/analysis @kde_core_json` through
`Hdf5Service::openFileForUpdate`, and reopens the reader. When the file
cannot be written (read-only, export running) the contour is still drawn and
the status says it was not saved. A result for a file that is no longer open
is dropped; the destructor drains the job. Guard: `frontend.hdf_review_core`
(compute, save, decline/confirm overwrite, read-only).

## Scatter interaction and frame pane (issue #467)

The Charts view is a splitter: scatter | (frame pane over histogram), sizes
persisted as `Review/ChartsSplitter` / `Review/ChartsRightSplitter` (written
once, in the destructor). The pane sits beside the plot and never covers it.

- **Scatter view** is a `ZoomableChartView` ([[System-Utilities]]): wheel
  zoom, left/middle-drag pan, single click selects, double-click resets only
  on empty space (`setResetOnDoubleClick(false)` + `onScatterDoubleClicked`),
  "Reset zoom" in the right-click menu after "Compute core contour…".
  `generateScatterPlot` registers the data extent with `setDefaultRange`.
- **Point ≠ frame.** `generateScatterPlot` skips `!validation.isValid`, so it
  builds `scatterPoints_` (µm², deformability, frame), `scatterPointToFrame_`
  and `frameToScatterPoint_` (−1 when a frame has no point). It sets
  `scatterShowsLiveFile_` only when drawing `validFrames_`; batch-export
  snapshots draw other files, and clicks then select nothing.
- **Dataset is explicit.** `setSelectedFrame(int, bool valid)` and
  `showFrameViewer(int, bool valid)`: `isShowingValid_` is false on the
  Charts tab. `onTableSelectionChanged` uses the sending table, not the tab.
  `loadFrameForDisplay(int, bool)` is the one image/mask/series read shared
  by the modal viewer and the pane.
- **Hit test** (`include/frontend/utils/ScatterHitTest.h`, Qt-free): visible
  points only, pixel distance ≤ max(marker, 8 px), ties → lowest frame index.
  `tests/fixtures/review_scatter_hits.json` is the shared contract with the
  React shell (#470; `desktop/src/review/charts/scatterHitTest.ts` passes
  it in vitest). Hover uses the same function (cursor + tooltip).
- **Selection** (`setSelectedFrame(frame, true)`) moves the one-point
  `scatterHighlight_` (kept last in the chart's series by
  `raiseScatterHighlight()`, hidden for frames without a point) and refreshes
  the pane. Highlight and pane show the **last valid cell chosen**
  (`highlightFrame_`); an invalid-set selection changes neither, and pane
  prev/next continue from that cell. The pane reads a frame only while the
  Charts tab is visible and no modal viewer covers it (`refreshFramePane`
  marks it stale behind the modal and catches up when it closes, so the
  modal's prev/next read each frame once). `setSelectedFrame` guards against
  re-entry through the table's `selectionChanged`. Pane prev/next and ←/→
  walk `validFrames_` with wrap; "Open in window…" opens the modal viewer on
  that cell. Pane title and hover tooltip name the **recorded**
  `ProcessedFrame::index` (what the viewer and the metrics CSV show), with
  the 1-based valid-set position second. Close/reload clears maps,
  highlight and pane.
- **Exports**: `renderChartSnapshots` (Export All, Export Charts, batch)
  saves the axes, the user-zoomed flag and the highlight
  (`saveScatterView`), draws full extent without the highlight, then
  restores (`restoreScatterView`, re-arming `markUserZoomed`); batch restores
  once after the final `updateCharts()`. Export Charts writes those same
  snapshots as TIFFs. While an export redraws the scatter, click, hover and
  double-click are all ignored.
- **Cost:** the scatter is filled with `QXYSeries::replace()`. On Qt 6.4
  `append()` (per point and the `QList` overload) emits `pointAdded` per
  point and rebuilds the series geometry each time — O(n²): a 20 000-cell
  file did not finish opening in two minutes; with `replace()` it opens in
  ~1.6 s. At 20 000 points the hit rule costs 0.14–0.18 ms per call, hover
  (hit + tooltip) 1.2–3.2 ms median, a pan step's axis/geometry update
  ~8–9 ms, but its repaint ~200–260 ms: Qt Charts draws one item per marker
  on the CPU (TD-18). Linux container, system Qt 6.4, scatter ~800 px wide.
  Gates: hit rule < 2 ms, hover < 5 ms, pan update < 60 ms, pan with
  repaint < 1.5 s (catches O(n²)-class regressions only).
- **Scaling (measured 2026-10-01, `MIB_REVIEW_SCATTER_CELLS=<n>` on
  `frontend.hdf_review_scatter`, Linux container, system Qt 6.4):**

  | valid cells | open (metadata + scatter) | RSS delta | hover | pan repaint |
  |---|---|---|---|---|
  | 20 000 | 1.5 s | +38 MB | 2 ms | 250 ms |
  | 100 000 | 31–36 s | +208 MB | 11 ms | 1.3 s |
  | 300 000 | > 3 min (watchdog) | — | — | — |

  Memory is linear (~2 KB per cell, mostly the per-marker graphics item).
  Open time is **quadratic inside Qt Charts**: `ScatterChartItem::createPoints`
  adds each marker with `QGraphicsItemGroup::addToGroup`, which recomputes
  the group's bounding rect per add (gdb samples during the 100 k load).
  Our own `replace()` call is one rebuild; the maps and hit test are linear.
  Practical ceiling today: tens of thousands of cells on the Charts tab;
  the Valid/Invalid tabs are unaffected (virtualised). The fix is to spread
  the points over several `QScatterSeries` of ~2 000 (PR 3b's eight density
  level series do this for free when KDE is on) or OpenGL series (TD-18).
- **Layout:** the scatter keeps ≥ 420 px and starts with 60 % of the width;
  the embedded viewer hides its overlay / ROI / zoom in-out / export
  controls (the tab's toolbar and Export All cover those) so its one
  control row — Prev, Next, Fit to Window, Open in window… — fits a 400 px
  pane.
- **Chart export colours** (pre-existing bug fixed with #467):
  `renderChartSnapshots` read the grabbed `Format_RGB32` image (B,G,R,A in
  memory) as RGBA, so every exported chart TIFF had red and blue swapped —
  blue points came out orange. Now `COLOR_BGRA2BGR`. Caught by
  `integration.review_scatter_e2e`: the real `MainWindow` records a run on
  the real 512x96 cells, opens it in Review and drives the scatter (click,
  zoom, pan, prev/next, open in window, export), writing a screenshot of
  every state to `MIB_REVIEW_E2E_OUT`; it skips (77) without the asset.
- Test hooks: `scatterViewForTests`, `scatterHighlightForTests`,
  `framePaneForTests`, `framePaneFrameForTests`, `scatterPointToFrameForTests`,
  `scatterPointViewPosForTests`, `setFrameViewerSinkForTests` (replaces the
  modal `exec()`), `stepScatterSelectionForTests`, `renderChartSnapshotsForTests`.
  Guard: `frontend.hdf_review_scatter`.

## Run accounting (issue #367)

`accountingSummary()` appends the recorded completion state and the
empty / rejected / processing-failed / store-loss / persisted / persistence-
failed counts (from `Hdf5Service::readRunAccounting`) to the status text for
both experiment and recording files; legacy files show "accounting: not
recorded (legacy file)" rather than implying completeness.

## Shared implementation (ADR 0008)

The React shells reproduce this tab over [[../services/ReviewSession]]
(`mib_review_core`): overlay composition is `OverlayCompose.cpp`, a port of
`OverlayRenderer.cpp` (same colours and contour rules — keep them in step),
and the session reads the **recorded** pixel-to-micron factor (TD-17); this
tab still uses the live backend factor until PR 8 of the plan switches it.
Likewise the React histogram takes its range from the file's recorded
ring-ratio thresholds where this tab uses the live processing config.

## Gotchas

- Multi-image series (4D dataset) need `readSeriesImagesByIndex` — not the
  flat `readImageByIndex`.
- During `Export All`, the series prompt applies one range selection across
  every valid multi-image record so exports stay consistent.
- Series export filename padding is computed as an `int`; Qt 6 returns
  `qsizetype` from `QString::size()`, so cast before using `std::max` on
  MSVC.
- Chart snapshots stored in the file are 2D/3D — use `readChartSnapshot`
  to display them.
- See tasks `review_hdf_thumbnail_spacer_crash.md` and
  `fix_hdfreviewtab_linker_error.md` for historical fixes.
