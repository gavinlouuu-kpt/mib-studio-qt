# ReviewSession

> YOFO Review's review implementation (ADR 0014, plan
> `docs/exec-plans/active/2026-10-01-standalone-review-app.md`), also used by
> the Qt tab for the recorded factor. MIB Studio's Tauri shell keeps its own
> facade review stack until #512 (ADR 0014 amendment, decision A), which moves
> it here slice by slice (plan
> `docs/exec-plans/active/2026-10-18-one-review-implementation.md`). Qt-free,
> owns the open HDF5 file through its **own** read-only `Hdf5Service`,
> serves bounded reads with overlays composed in the backend, and will own
> the review jobs (PR 1b).

**Source:** `src/backend/review/ReviewSession.cpp`,
`include/backend/review/ReviewSession.h`, `include/backend/review/ReviewTypes.h`,
`src/backend/review/OverlayCompose.cpp` (+ header),
`src/backend/review/Reanalysis.cpp` + `include/backend/review/Reanalysis.h`
(the reanalysis engine, #512 S2)
**Library:** `mib_review_core` (static; `src/backend/CMakeLists.txt`) on
`mib_processing` only — no `mib_backend`, so YOFO Review links it without
cameras, serial, SQLite, curl or Sentry. `mib_backend` links it for the
facade's review engines (the reanalysis engine since #512 S2) and
`ReviewExport.cpp`, whose `review::writeMetricsCsv` no longer has a caller
(the facade exports through [[HdfExportService]]; #512 S3 deletes it).
**Tests:** `tests/review/review_session_test.cpp` (`review.session`, labels
`review;recording;backend;hdf5;safety`), `tests/review/review_overlay_kernel_test.cpp`
(`review.overlay_kernel`: one overlay kernel, #512 S1),
`tests/review/review_reanalysis_test.cpp` (`review.reanalysis`: the
reanalysis engine without an `AppBackend`, #512 S2)
**Consumers:** `crates/mib-bridge/src/review_shim.cpp` (`ReviewBridge`, [[../architecture/Rust-Bridge]]),
the Qt [[../frontend/HdfReviewTab]] (`recordedPixelToMicron`, TD-17)
**Related:** [[Hdf5Service]], [[HdfExportService]], [[BatchMaskSources]],
[[../data-model/HDF5-Storage]], [[../frontend/YofoReview]]

## Responsibility

- `open(path, &error)` / `close()` / `isOpen()` / `filePath()`: one reader
  per session, never `AppBackend::hdf5()` (the experiment writer's handle),
  so a file can be reviewed while an experiment runs and the Tauri Raw
  Frames view reads the **file**, not the live FrameStore.
- `metadata()` → `ReviewMetadata`: counts, ROI, dataset infos (five paths),
  core identity, background flag, **series** (`/valid_frames/series_images`
  4D or the recording multi-image window), **run accounting** (#367,
  `hasAccounting` false for legacy files), stored **KDE records** (analysis +
  live JSON) and the **pixel-to-micron factor** the file was recorded with
  (`/run_provenance @run_snapshot_json` → `pixel_to_micron`; TD-17). Files
  without it use `setFallbackPixelToMicron()` (the host's live/preferred
  factor; `fallbackPixelToMicron()` reads it back); `pixelToMicronFromFile`
  says which applied. The static `recordedPixelToMicron(reader)` is that
  read (0 when absent), shared with the Qt tab and the batch job.
- `metricsPage(valid, offset, count)` → `MetricRow` with **every**
  FilterResult column the Qt `HdfMetricsModel` shows plus `areaUm2`.
- `fetchImage(dataset, index, OverlayMode, roiOverlay)` → packed Mono8 or
  RGB `ReviewImage`: the overlay (contours / outer-inner / mask / filtered
  mask) and the red 3 px ROI rectangle are composed here by `OverlayCompose`,
  so every shell shows the same pixels. Since #512 S1 the overlay is the one
  shared kernel, `recording::renderProcessingOverlay`
  (`backend/recording/ProcessingOverlay.h`, `mib_processing`), which the Qt
  `OverlayRenderer` and the facade's `render_review_overlay` also call;
  `composeOverlay` passes `OverlayPassthrough::KeepMono`, so an undrawn Mono8
  original stays Mono8 (Qt and the facade get RGB). Mask datasets and
  recording files ignore the mode.
- `seriesInfo(index)` / `fetchSeriesImage(index, k, …)`: experiment 4D
  series via `readSeriesImagesByIndex`, recording window via
  `readImagesRange(index, multiImageCount)` (as the Qt tab).
- `thumbnails(valid, offset, count, size, …)` → one packed strip of
  letterboxed `size`×`size` tiles (a 200-thumbnail page is one IPC pull).
- `scatter()` → columnar valid-set arrays (`validation.isValid` rows only,
  valid-set order, µm² with the effective factor, target flags,
  `validPosition` ↔ `frameIndex`, ring ratio — the Charts histogram input).
- `metadata()` also carries `hasRecordedConfig` and the recorded
  `ringRatioMin/Max` (`readRecordedProcessingConfig`; the
  `ProcessingConfig` defaults when absent): the histogram's x range.
- `accountingSummary()`: the Qt status-line text.
- `saveCoreRecordJson(json, overwrite)`: refuses an existing record unless
  `overwrite`; closes the reader, writes `/analysis @kde_core_json` through
  `openFileForUpdate`, reopens and refreshes metadata.
- `loadFrameForDisplay(valid, index)`: the full `ProcessedFrame` (image,
  mask, series) the Qt viewer loads — shared by viewer, pane and jobs.

## Jobs (`ReviewJobs`, PR 1b)

`include/backend/review/ReviewJobs.h` / `src/backend/review/ReviewJobs.cpp`,
same library. One job at a time (a second start is refused), each a tracked
operation (ADR 0004 semantics: Started, bounded Progress, exactly one
terminal Completed / Failed / Cancelled through the `ReviewJobSink`), each
opening its own reader so session reads never block:

- `startExportMetrics(path)` / `startExportAll(root, series, charts)` /
  `startBatchExport(sources, root, metricsOnly, series)`: run
  [[HdfExportService]] with **each source's own recorded** factor (the
  fallback for files without one; 2026-10-04 — before, every source used the
  open file's factor); chart snapshots arrive
  encoded (PNG/TIFF) from the shell and are written beside the images; batch
  continues after per-file failures and reports them in the terminal
  message ("exported N of M file(s); failed: …"); batch metrics names
  follow the service's `<base>_metrics_N.csv` rule; recording files export
  images only, and Batch Metrics refuses them per file ("<name>: recording
  files do not contain metrics", the Qt wording) instead of attempting them.
- `startExportCharts({outputDir, charts})` (PR 4, contract kind
  `ExportCharts` = 6): the shell's snapshots decoded and written as files
  into an existing directory, all or nothing (temporary
  `.<stem>.partial.<ext>` names, renamed once every one wrote; any failure
  or cancel removes them). Names must be bare file names ending
  .tif/.tiff/.png (`validChartName`, mirrored by the Tauri staging command).
- `startRegenerateMasks(request)`: current valid / invalid range, whole file,
  AVI or folder → `ProcessingService::processBatch` (bundled kernel) with the
  recorded config, ROI and background by default (optional median
  synthetic background) → `batch_masks::saveMasksToHdf5`; HDF5 sources keep
  recorded indices and normalise timestamps to the first image.
- `startComputeCore(fraction)`: `computeFullRunCoreRecord` over the scatter
  (recorded factor, fixed seed); `computedCoreJson()` hands the record to the
  shell, which saves it with `saveCoreRecordJson`. `cell_count` is the
  record's **in-core** count (as the Qt tab saves it; `population_count` is
  the estimated size) — until 2026-10-04 the job overwrote it with every
  valid cell. The terminal message is the Qt status ("Core 90%: N of M
  cells, K loop(s)").
- `startDensity({bandwidthFactor, coreFraction, levels, wantCoreRecord})`:
  per-point density → `levelForDensity` (same bucketing as the Monitoring
  tab); above 5000 cells a fixed-seed 5000-cell subsample → 256×128 grid →
  bilinear interpolation (`densityAtPoints`, within one level of the direct
  path for ≥ 95 % of a 4000-point population); a computed full-run record
  only when the file has none. This is the scatter plan's PR 3a "review
  density" in the review core instead of `MonitoringDensityService`.
- Results belong to their file: `density()` and `computedCoreJson()` return
  nothing once the session holds another file (each result carries the
  path it was computed from; fixed in PR 4 — before, a new file could be
  handed the previous file's levels as ready). The density result is
  cleared when its job starts running, not when a start is refused.

Guard: `review.jobs` (`tests/review/review_jobs_test.cpp`).

## Reanalysis engine (`Reanalysis.h`, #512 S2)

The engine behind the backend bridge's `review_reanalysis_json` and
`fetch_review_reanalysis_preview` (MIB Studio, YOFO Studio), moved verbatim
out of `BackendFacade` (`src/backend/app/ReviewReanalysis.cpp` now only
delegates). Not yet used by YOFO Review's Regenerate masks (#512 S8):

- `parseReanalysisRequest(json, defaults, out, error)`: the contract request
  (source kind hdf / folder / avi, dataset `all` or one images dataset,
  start / count, `max_frames` 1..1 000 000, `max_input_mib` 1..16 384, ROI,
  `image_processing`, background index / dataset / clear, synthetic
  background) with the facade's messages; `defaults` is the config used when
  the request carries none (the facade passes the live config); an existing
  output is refused.
- `runReanalysis(request, kernel, cancel, onProgress)`: load (HDF "all" in
  timestamp order, then index, dataset, row), budgets, overrides, quiet-tile
  synthetic background, `processBatch`, source index / timestamp / track ids
  restored, `.<name>.partial-<job>` beside the output, no-replace publish
  (`renameat2` / `MoveFileExW` / hard link), partial removed (or reported).
  Runs on the caller's thread; the host owns single flight, the operation and
  the status snapshot.
- `loadReanalysisPreview(json, image, index)`: one Mono8 source frame through
  an independent reader.
- `reanalysis{Loading,Progress,Terminal,StartFailed}StatusJson`: the
  contract's status snapshots, byte for byte as the facade wrote them.

## Threading

Every public call takes one mutex (`Hdf5Service` is not thread-safe).
Jobs (PR 1b) open their own readers on `filePath()` and never block a read.

## Gotchas

- Dataset paths: `/valid_frames/images|masks`, `/invalid_frames/images|masks`,
  `/recorded_frames/images`; recording files route the "valid" set to the
  recorded images and have no masks.
- `ReviewDataset` / `OverlayMode` values are contract-pinned
  (`bridge-contract.json` `review_image_datasets`, `overlay_modes`);
  append only.
- Multi-channel images are reduced with `cv::extractChannel(…, 0)` like the
  facade did; RGB only appears after an overlay / ROI draw.
- A thumbnail page with `size == 0` or `> 1024` is refused; the bridge
  further caps `size ≤ 512`, `count ≤ 1000`.

Issue #570: `hasAccounting` remains true for schema-bearing files whose required
attributes cannot be read. Their snapshot has `readError`, Unknown completion
and an explanatory `completionReason`; `accountingSummary()` shows that reason
instead of partial counts. Legacy files still have `hasAccounting == false`.
