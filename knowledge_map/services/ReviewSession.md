# ReviewSession

> YOFO Review's review implementation (ADR 0014, plan
> `docs/exec-plans/active/2026-10-01-standalone-review-app.md`), also used by
> the Qt tab for the recorded factor. MIB Studio's Tauri shell keeps its own
> facade review commands until #512 (ADR 0014 amendment, decision A), which
> moves them here slice by slice (plan
> `docs/exec-plans/active/2026-10-18-one-review-implementation.md`); since S5
> the facade's loaded review file **is** a `ReviewSession` (see "The facade's
> session" below). Qt-free,
> owns the open HDF5 file through its **own** read-only `Hdf5Service`,
> serves bounded reads with overlays composed in the backend, and will own
> the review jobs (PR 1b).

**Source:** `src/backend/review/ReviewSession.cpp`,
`include/backend/review/ReviewSession.h`, `include/backend/review/ReviewTypes.h`,
`src/backend/review/OverlayCompose.cpp` (+ header),
`src/backend/review/Reanalysis.cpp` + `include/backend/review/Reanalysis.h`
(the reanalysis engine, #512 S2),
`src/backend/review/ExportEngine.cpp` + `include/backend/review/ExportEngine.h`
(the export engine, #512 S3)
**Library:** `mib_review_core` (static; `src/backend/CMakeLists.txt`) on
`mib_processing` only — no `mib_backend`, so YOFO Review links it without
cameras, serial, SQLite, curl or Sentry. `mib_backend` links it for the
facade's review engines (reanalysis since #512 S2, export since S3). The
unused second CSV writer `review::writeMetricsCsv` (`ReviewExport.cpp`) was
deleted in S3; every metrics CSV comes from [[HdfExportService]].
**Tests:** `tests/review/review_session_test.cpp` (`review.session`, labels
`review;recording;backend;hdf5;safety`), `tests/review/review_overlay_kernel_test.cpp`
(`review.overlay_kernel`: one overlay kernel, #512 S1),
`tests/review/review_reanalysis_test.cpp` (`review.reanalysis`: the
reanalysis engine without an `AppBackend`, #512 S2),
`tests/review/review_export_engine_test.cpp` (`review.export_engine`: the
export engine without an `AppBackend`, #512 S3)
**Consumers:** `crates/mib-bridge/src/review_shim.cpp` (`ReviewBridge`, [[../architecture/Rust-Bridge]]),
`BackendFacade` (`review_`, the facade's review source, #512 S5;
`tests/backend/facade_review_session_test.cpp`, `backend.facade_review_session`),
the Qt [[../frontend/HdfReviewTab]] (`recordedPixelToMicron`, TD-17)
**Related:** [[Hdf5Service]], [[HdfExportService]], [[BatchMaskSources]],
[[../data-model/HDF5-Storage]], [[../frontend/YofoReview]]

## Responsibility

- `open(path, &error)` / `close()` / `isOpen()` / `filePath()`: one reader
  per session, never `AppBackend::hdf5()` (the experiment writer's handle),
  so a file can be reviewed while an experiment runs and the Tauri Raw
  Frames view reads the **file**, not the live FrameStore. `open()` is all
  or nothing (#512 S5): a metadata read that throws after the reader opened
  (an allocation failure, an OpenCV exception from a malformed dataset such
  as an oversized background) closes the reader again and returns false with
  "Failed to read <path>: …"; it never throws for a bad file and never leaves
  a reader open that a host was not told about. The reopen after
  `saveCoreRecordJson` follows the same rule.
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
- `framesPage(valid, offset, count, frames, total)` (#512 S5): the same page
  as the metadata-only `ProcessedFrame`s read at open (no pixels), for hosts
  with their own row type (the facade's `MonitoringObjectRow`); same bounds
  and totals, false only when closed, and overflow-safe for a huge `count`
  (`metricsPage` still computes `offset + count`).
- `validSet()` → `ValidSet` (#512 S5): the open file's path, file type,
  `metadataReadable` and valid rows in **one snapshot under one lock**, so a
  concurrent open of another file can never pair one file's identity with
  the other's rows. `metadataReadable` is false when an experiment file's
  `/valid_frames/metadata` exists but could not be read at open, or its
  presence could not be determined (`metadataDatasetPresent`); an absent
  dataset is an empty, readable set. The facade's charts answer "Cannot read
  valid-frame metrics" for it, as their own reader did before S5.
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
  [[HdfExportService]] through the export engine (`runExportFile` /
  `runExportBatch`, cancel applied at each progress report, since #512 S3)
  with **each source's own recorded** factor (the
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

## Charts JSON and saved overlay (`ReviewCharts.h`, #512 S4)

**Source:** `src/backend/review/ReviewCharts.cpp` +
`include/backend/review/ReviewCharts.h` (same library). **Test:**
`tests/review/review_charts_json_test.cpp` (`review.charts_json`: byte for
byte against the pre-slice facade code, kept in the test as the oracle).

The builders behind the backend bridge's `fetch_review_charts_json` and
`render_review_overlay` (MIB Studio, YOFO Studio), moved verbatim out of
`src/backend/app/ReviewCharts.cpp`, which keeps the policy: the loaded file
(its own reader, "Charts require an experiment HDF file"), the **live** factor
and ring-ratio range (D3 / D4 keep them for the facade's JSON commands), and
the facade's ROI style:

- `reviewChartsJson(rows, source, factor, ringMin, ringMax)`:
  `recording::makeReviewChartData`, the 128 × 128 density (conserves every
  finite point), the exact 0.5-wide histogram, the bundled isoelastic curves,
  counters as decimal strings. It throws what `makeReviewChartData` throws (an
  empty or reversed ring range); the facade turns any throw into
  `reviewChartsErrorJson(message)` (`{"valid":false,"error":…}`).
- `renderSavedOverlayPng(requestJson, RoiStyle)`: an independent reader, the
  64 MiB source-frame limit, image + saved mask + classification from the same
  file, `recording::renderProcessingOverlay`, PNG; throws with the bridge's
  messages. `RoiStyle::Facade` is the facade's yellow 1 px rectangle in source
  pixels (still the facade's, until D2's switch); `RoiStyle::Review` draws
  `drawRoiOverlay`'s red 3 px scaled rectangle, so its pixels equal
  `fetchImage` with the ROI (Qt / YOFO).

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

## Export engine (`ExportEngine.h`, #512 S3)

The engine behind the backend bridge's `review_export_json` /
`review_export_status_json` (MIB Studio, YOFO Studio) and ReviewJobs' Export
Metrics / Export All / Batch export, moved verbatim out of
`BackendFacade::submitReviewExportJson`, which now keeps only the policy
(live factor and chart ring range as the defaults, the loaded file as the
default source, one export at a time, the `Export` operation, the snapshot):

- `parseExportRequest(json, defaults, out, error)`: the contract request
  (`output_root`, 1-256 `source_paths`, `explicit_destination` (single source
  only; an empty root becomes "."), `format` metrics_csv / images / all /
  charts, `frames`, `series`, `conversion_factor`, `keep_partial_on_failure`,
  `isoelastic_overlays`) with the facade's messages; `defaults` carries the
  host's factor and chart range; the source is left to the host.
- `runExportBatch(request, sources, cancel, onProgress, options)`: one
  [[HdfExportService]] run per source on the caller's thread; continue after a
  failed source, stop on a cancelled one or on a cancel seen before a source
  starts; per-source outcomes, the last result, `cancelled` / `failed` and the
  batch message. `options.prepare` lets the host adjust or skip each source's
  request (ReviewJobs' batch: recorded factor, recording files → images,
  Batch Metrics refusing recording files, `<base>_metrics_N.csv` names
  resolved just before each run). `runExportFile` is one run.
- Two cancel models, both kept byte for byte: `ExportCancelPolling::Direct`
  (the facade; the service polls the operation's flag at every check) and
  `AtProgress` (ReviewJobs; the flag is copied into the service's token at
  each progress report, so a cancel lands one report later).
- `export{Validating,Progress,Terminal,StartFailed}StatusJson`: the contract's
  status snapshots (phase strings and the per-file `results` array read by
  `desktop/src/exportControls.tsx`). A single-source batch cancelled before it
  started reports `"error":""`, as the facade always has.

## The facade's session (#512 S5)

`BackendFacade` holds one `std::shared_ptr<ReviewSession> review_` (never
reassigned; shared so the review bridge can attach to it, S10). MIB Studio's
and YOFO Studio's review commands read it: `load_recording` →
`open(path)` (replaces the open file, also when the open fails),
`close_review` → `close()`, `fetch_review_metadata` → `metadata()` field for
field, `fetch_review_metrics_page` → `framesPage` mapped to
`MonitoringObjectRow`, `fetch_review_image` → `readRaw` (Mono8, channel 0),
`fetch_run_accounting("review")` → the accounting read at open,
`fetch_review_charts_json` → `validSet()` (one snapshot; no second reader),
and `review_export_json`'s default source → `filePath()`. The load event's
counts come from the same metadata. Nothing in the facade holds review state
of its own any more, and none of it touches `AppBackend::hdf5()` (the
experiment writer's handle): a load beside an open writer handle is refused
only by the guards, and the writer is never closed or replaced by load or
close.

The guards stay the facade's policy around `open()` / `close()`, inside the
coordinator's idle transaction: "Experiment must be idle before loading review
data", "Stop raw recording before loading review data" and the close
equivalents. **Decision D1 (a):** the facade tells AppBackend whether its
session is open (`AppBackend::setReviewSourceOpen`, set in the same
transaction from `isOpen()` by a scope guard that also runs when an exception
leaves the transaction, so the flag equals `isOpen()` on every exit; cleared
by close, a failed load, shutdown and the destructor). While it is, experiment readiness fails `storage.hdf5` with the
text a file in the shared handle produced ("an HDF5 file is already open",
"finish or close the current file first"), Start is refused with "stop raw
recording and close the existing HDF5 file before starting an experiment"
and raw recording with "Close the existing HDF5 file before starting raw
recording; its writer will not be replaced": HDF5 is built without thread
safety (#722), so a review reader must not run beside the 5 kHz writer. See
[[../architecture/ExperimentCoordinator]]. Shutdown closes the session after
the export / reanalysis jobs are joined.

Proof: `backend.facade_review_session` keeps the pre-S5 facade code as its
oracle (load result and events, metadata, pages, images, accounting, charts,
the export's default source on experiment, legacy, recording, empty-set,
unreadable-metadata and non-HDF5 fixtures, failed loads included), the writer
handle untouched, the D1 gate equal to the shared handle's; `review.session`
covers `framesPage`, `validSet` and the all-or-nothing open;
`backend.facade_review_session` also loads a file whose background read
throws (the flag equals `isOpen()`, the load fails closed) and loads files on
one thread while charting on another; `backend.experiment_readiness`
(section 5b) and `backend.facade_boundary` the D1 refusals and the guards
with a live camera.

## Threading

Every public call takes one mutex (`Hdf5Service` is not thread-safe).
Jobs (PR 1b) open their own readers on `filePath()` and never block a read.
The facade's session is read on the bridge's calling threads, as the shared
handle was before S5 (now serialized by this mutex); HDF5 itself is still not
thread-safe across readers (#722), which is why D1 keeps experiments out.

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
