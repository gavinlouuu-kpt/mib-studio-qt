# ReviewSession

> YOFO Review's review implementation (ADR 0014, plan
> `docs/exec-plans/active/2026-10-01-standalone-review-app.md`), also used by
> the Qt tab for the recorded factor. MIB Studio's Tauri shell keeps its own
> facade review commands until #512 (ADR 0014 amendment, decision A), which
> moves them here slice by slice (plan
> `docs/exec-plans/active/2026-10-18-one-review-implementation.md`); since S5
> the facade's loaded review file **is** a `ReviewSession` (see "The facade's
> session" below), and since S7 the facade's review jobs run on `ReviewJobs`
> lanes (see "Jobs"). Qt-free,
> owns the open HDF5 file through its **own** read-only `Hdf5Service`,
> serves bounded reads with overlays composed in the backend; the review jobs
> beside it are `ReviewJobs`.

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
export engine without an `AppBackend`, #512 S3),
`tests/review/review_run_identity_test.cpp` (`review.run_identity`: run
identity and the identity probe, #717 B1a; labels
`review;recording;backend;hdf5;concurrency;safety`)
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
  `saveCoreRecordJson` follows the same rule. A throw inside the reader's own
  open right after `H5Fopen` is caught the same way ([[Hdf5Service]] releases
  the handle). `isOpen()` **fails closed**: it is true while the session has
  a file open *or* its reader still holds any handle
  (`Hdf5Service::holdsFileHandle()`), so a handle that somehow outlived a
  failed open keeps the facade's D1 flag set (Start refused) until `close()`
  or the next `open()`, which always close the reader first. Nothing is
  served from such a handle (reads check the session's own state).
  `ReviewSessionTestAccess::reader(session)` (friend, test only) reaches the
  reader for fault injection.
- `metadata()` → `ReviewMetadata`: counts, ROI, dataset infos (five paths),
  core identity, background flag, **series** (`/valid_frames/series_images`
  4D or the recording multi-image window), **run accounting** (#367,
  `hasAccounting` false for legacy files), stored **KDE records** (analysis +
  live JSON) and the **pixel-to-micron factor** the file was recorded with
  (`/run_provenance @run_snapshot_json` → `pixel_to_micron`; TD-17). Files
  without it use `setFallbackPixelToMicron()` (the host's live/preferred
  factor; `fallbackPixelToMicron()` reads it back); `pixelToMicronFromFile`
  says which applied. The static `recordedPixelToMicron(reader)` is that
  read (0 when absent), shared with the Qt tab, the batch job and the
  reanalysis engine. A file without a run snapshot factor falls back to
  `/experiment_info @pixel_to_micron`, the factor a regenerated or
  reanalysed file was validated at (#758), so such a file reopens at it
  instead of the fallback.
- `metadata().runIdentity` → `RunIdentity` (#717 B1a, review ABI 2): what
  the file says about its own run, read at open by the static
  `readRunIdentity(reader)` from run snapshot v3 (#712, parsed with
  `recording::parseRunProvenance`) and `/experiment_annotation`
  (`parseExperimentAnnotation`): `hasSnapshot`, `snapshotVersion` (1 for a
  snapshot without `schema_version`), `runId`, `declared`
  (`RunStartIdentity`: `experimentId`, `runIndex`, `sampleRef`,
  `selectedOperator`, `calibrationRecordId`), `annotation` (fixed members +
  free key/values sorted by key) and `annotationSchemaVersion` (read on its
  own, presence-aware: missing is absent, a recorded 0 stays 0). Absent is
  `std::nullopt` / `false`, **never** 0 or ""; a legacy file reads every
  member absent. `snapshotUnreadable` / `annotationUnreadable`: the attribute
  exists but is not a JSON object, which is not the same as legacy.
- `probeRuns(paths, out, &error)` (#717 B1a, R1 Open's experiment library):
  for each path a short-lived reader of its own opens the file, reads only
  header attributes into a `RunProbe` (the `RunIdentity`, `startTimeNs` /
  `endTimeNs` with 0 read as not recorded, `totalValid` / `totalInvalid`
  from `/experiment_info` or `totalValid` from `/recording_info`
  `total_recorded_frames`, the accounting `completion`, the processing
  `coreVersion`) and closes it: no images, no metadata rows. Each count and
  time is read on its own with `Hdf5Service::readScalarUintAttribute`
  (#746 review), not through `readExperimentInfo` / `readRecordingInfo`,
  which default a missing count to 0: a missing or unreadable count is
  absent, a recorded 0 stays 0, a negative signed value is unreadable. A
  recorded 0 time stays "not recorded" (the regenerate path writes 0 when it
  has no wall clock). A missing path ("No such file"), a directory
  ("Not a file"), an empty path, a non-HDF5 or locked file ("Not an HDF5
  file, or it cannot be opened …") or a throwing read is a per-file error
  (`ok` false, every other member absent), never a failure of the call.
  More than `kReviewProbeMaxPaths` (256, review contract
  `review_probe.max_paths`) is refused as a whole. **Serialization (#722):**
  each file is probed under the session's own mutex, the one every read,
  open and close takes, so its HDF5 calls never interleave with the
  session's; the open file, its metadata and its pixels are left as they
  were, also when the probe reads the open file itself (a second read-only
  handle, as the export jobs open). Jobs read outside that mutex, so the
  bridge calls `ReviewJobs::probeRuns`, which holds the lanes' mutex (where a start claims its lane)
  for the whole probe (no job starts) and refuses while one runs ("A review
  job is running; …").
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

## Jobs (`ReviewJobs`, PR 1b; lanes since #512 S7)

`include/backend/review/ReviewJobs.h` / `src/backend/review/ReviewJobs.cpp`,
same library. Each job is a tracked operation (ADR 0004 semantics: Started,
bounded Progress, exactly one terminal Completed / Failed / Cancelled), each
opening its own reader so session reads never block.

**Lanes (decision D7 as amended 2026-10-19).** Every job runs on one of two
lanes, single flight per lane: **export** (Export Metrics / All / Batch /
Charts, `review_export_json`) and **processing** (regenerate masks, core
contour, density, `review_reanalysis_json`); `laneOf(kind)`. A start on a
busy lane is refused. Whether the two lanes run at the same time is the
host's policy, `ReviewJobsOptions`, with one named preset per host:

| Preset | Host | Lanes at once | Operation ids, Started, cancel | Lane free |
|---|---|---|---|---|
| `forReviewBridge()` (also the default) | YOFO Review's bridge (`review_shim.cpp`) | **no**: one job of any kind ("Another review job is still running") | ReviewJobs' own ids from 1, Started through the sink, `cancel(id)` | before the terminal event (`busy()` is false when it arrives) |
| `forFacade(host)` | MIB Studio's `BackendFacade` | yes: one export and one processing job, as the facade ran before #512 | the host's (`ReviewJobHost::beginOperation` = `BackendFacade::beginOperation`: kinds `Export` / `Reanalysis`, `cancel_operation`, shutdown cancellation); Progress and terminal reach `reportOperationProgress` / `finishOperation` | after the terminal event (a start from inside it is refused, as the facade's flags were cleared after `finishOperation`) |

YOFO Review does **not** gain the second lane until #722: HDF5 is built
without thread safety and `Hdf5Service` has no process-wide lock, so an export
beside a regenerate would call into the library from two threads at once.
MIB Studio's existing export-while-reanalysing is listed in #722 as at risk.
The refusal is the job's own message on its own lane ("An export is already
active", "Reanalysis already active" for the JSON jobs) and "Another review
job is still running" otherwise. Neither host lock is held while ReviewJobs
calls the host or the sink, so either may call back in (a start, a cancel, a
status read, even `shutdown()`). A worker is never joined by itself: one whose
own event starts its lane again or calls `shutdown()` is detached, and it
holds a reference to the runner state (`impl_` is a `shared_ptr`; job bodies
reach the runner only through it), so the sink it is executing, its lane and
the mutex stay alive until it ends even if the ReviewJobs object is destroyed
meanwhile (#742 review). The host's own objects (session, kernel, the
callback's targets) are the host's to keep. **Callback contract (#742 review
round 2):** callbacks (the sink, the host's `beginOperation`) may call
`shutdown()` or `cancel()`, but must not destroy this ReviewJobs or its host.
Destroying the owner from inside its own callback is out of contract. Hosts
deliver events to their consumers asynchronously: both bridges' sinks only
convert and enqueue, and the shell drains the queue. `shutdown()` sets every
lane's flag and joins both lanes. Starts stay possible afterwards (as before).
A start that a `shutdown()` overtook between its claim and its worker does
not run its job. That covers a shutdown from the job's own Started event or
the host's `beginOperation`, and one on another thread. The start itself
reports the one Cancelled terminal. Whether `shutdown()` waits for such starts
depends on where it is called (#742 review round 3):
- **From outside the runner** (the owner's teardown: a thread that is neither
  a lane worker nor inside a ReviewJobs callback), it waits until every start
  claimed before it has returned, with its Cancelled reported. So no start is
  inside ReviewJobs, the host or the sink when it returns, and
  `BackendFacade::shutdown()` can tear down after it.
- **From a lane worker or from inside a callback** (the host's
  `beginOperation`, the sink), it does not wait. A thread-local marker per
  runner tells the two cases apart. Round 1 waited here and deadlocked: under
  the bridge's policy a start joins the previous worker while that worker may
  still be calling `shutdown()` from its terminal event.
- **Why the outside wait cannot deadlock:** a start in progress runs only
  ReviewJobs' code and those callbacks, and joins its lane's previous worker.
  A shutdown from either is the inside case, which returns, so the worker ends
  and the join completes. ReviewJobs' own jobs carry "Review jobs shut down
before the job started". The JSON jobs carry their engine's outcome for a run
cancelled before its first step, with that status snapshot
(`exportBatchCancelledBeforeStart`, `reanalysisCancelledBeforeStart`), so the
facade's events and status match a run cancelled on entry. A start holds its
own reference to the runner state throughout. A worker that cannot be created gets its one Failed
terminal and frees the lane (before S7 the lane stayed busy).

**The JSON jobs (the facade's, #512 S7).** `startExportJson(ExportBatchRequest)`
and `startReanalysisJson(ReanalysisRequest, kernel)` are the facade's
`review_export_json` / `review_reanalysis_json` workers moved here verbatim:
`runExportBatch` with the service polling the flag (`Direct`) and
`runReanalysis` on the facade's live processing service; Progress without a
message; terminal message = the final path (export completed) or the
engine's error; no "a set flag turns Failed into Cancelled" rule (the
engine's outcome stands). The lane holds each job's status snapshot,
`exportStatusJson()` / `reanalysisStatusJson()` (the S2 / S3 formatters,
written before each event: validating / loading once the id exists and
before the worker starts, then progress, then terminal; start failed; `{"state":"idle"}`
before the first). `ReviewJobKind::Export` (7) and `Reanalysis` (8) are not in
`review-contract.json` (S12 proposes them with these values); the review
bridge never starts them. The two export cancel models stay as they were:
`Direct` for the JSON export, `AtProgress` for ReviewJobs' own exports.

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
- `startRegenerateMasks(request)`: current valid / invalid range (clamped
  to the set), whole file, AVI or folder, run on the reanalysis engine
  (`runReanalysis`, below; #512 S8) with YOFO's D6 options. Timestamps are
  rebased to the first selected frame and clamped at 0. The optional
  synthetic background is the median of the first 32 frames. The whole
  file is valid rows then invalid rows. The job has no frame or byte budget.
  The recorded config (read here, its target group from `@config_json`), ROI,
  background and pixel-to-micron factor (#758: the file's recorded one, the
  session's `fallbackPixelToMicron()` when it records none) are used by
  default. Otherwise the request's config and ROI apply, the background is
  cleared and objects are validated at the default 0.4886 µm/px, as for AVI
  and folder sources. The factor goes to the engine per request; the bundled
  kernel's own factor is never set. An existing output is refused at start
  ("Output already exists; choose a new file"). An output created mid-run
  fails the job and stays untouched; the partial file is removed. Terminal
  messages: "<path> (N images: V valid, I invalid; px→µm F recorded | default
  | fallback: the file records none)", "nothing written" on cancel, or the
  engine's error.
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

Guard: `review.jobs` (`tests/review/review_jobs_test.cpp`);
`review.jobs_lanes` (`tests/review/review_jobs_lanes_test.cpp`, #512 S7):
both presets through job bodies supplied by `ReviewJobsTestAccess`, lanes ×
cancel × shutdown with handshakes, racing starts, a cancel racing a job's
end, 200 mixed cycles per policy, one terminal per operation, the release
order, a start from inside the terminal event, and `shutdown()` from inside
it with ReviewJobs destroyed before the callback returns. It also covers
`shutdown()` from inside Started under both presets, for an own job and both
JSON jobs (each one Cancelled terminal from the start, matching the engine run
cancelled on entry byte for byte, body never run). And it covers `shutdown()`
from a terminal event while another thread's start joins that worker; this
deadlocked before round 2. Finally, it covers `shutdown()` from outside while a
start is held inside its Started event, under both presets: the shutdown must
be waiting, and must return only after the start's Cancelled. Lane release is
waited for with `ReviewJobsTestAccess::waitIdle` (the release's own
condition). The facade's side by `backend.facade_review_jobs`
(`BackendFacadeTestAccess::reviewJobs` reaches its lanes).

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
delegates). Since #512 S8 it is also YOFO Review's Regenerate masks
(`ReviewJobs::startRegenerateMasks`), with the D6 options on the request.
Parsing never sets them, so the facade keeps the defaults:

- `timestamps`: `Preserve` (facade) | `Rebase` (minus the first selected
  frame's, clamped at 0; YOFO, the Qt BatchMaskDialog rule).
- `syntheticBackgroundKind`: `QuietTile` (`batch_masks::buildSyntheticBackground`;
  facade, Qt) | `MedianOf32` (per-pixel upper median of the first ≤ 32
  selected frames; YOFO, moved from ReviewJobs).
- `frameOrder` (HDF `all` only): `Timestamp` (facade, Qt) | `Datasets` (rows
  as stored, valid then invalid; YOFO's whole file). D6 named two options.
  The third keeps YOFO's 2026-10-04 accepted "timestamp order" difference.
- `convertColorToGray` (HDF only): off, frames with more than one channel are
  refused ("Source frames must be grayscale"; facade). On, they become Mono8
  as `batch_masks::loadFromHdf5` converts them (BGR → gray; YOFO, whose
  pre-S8 reader did this; #735 review).
- `ReanalysisOutcome::valid` counts the valid records (YOFO's message).
- **Pixel-to-micron factor (#758).** `useRecordedPixelToMicron` (default on):
  an HDF source's recorded factor (`ReviewSession::recordedPixelToMicron`)
  wins; `pixelToMicron` (default 0.4886) applies when the source records
  none, to folder / AVI sources and with the flag off. Not in the JSON
  request: the facade sets its live factor as the fallback
  (`ReviewReanalysis.cpp`), YOFO's job the session fallback (recorded
  settings) or the default with the flag off (default settings). The engine
  passes the factor and the channel band detected from the run's own
  background to `processBatch` as `BatchAnalysisInputs`
  (`ProcessingService::offlineAnalysisInputs`): the kernel's live factor and
  band are never read or set, so a reanalysis beside a live run (the facade's
  kernel is the live service) or after another source cannot pick up the
  wrong one. `ReanalysisOutcome::pixelToMicron` / `pixelToMicronRecorded`
  report what was used; the output records it (`/experiment_info
  @pixel_to_micron`) with the applied config document (`@config_json`). Before
  #758 YOFO Review validated at 0.4886 and MIB Studio / the Qt dialog at the
  live factor, whatever the source recorded (pending release notice in
  `docs/release-notes/README.md`).

The functions:

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
  the status snapshot (since #512 S7 ReviewJobs' processing lane, for the
  facade and for YOFO's regenerate).
- `loadReanalysisPreview(json, image, index)`: one Mono8 source frame through
  an independent reader.
- `reanalysis{Loading,Progress,Terminal,StartFailed}StatusJson`: the
  contract's status snapshots, byte for byte as the facade wrote them, plus
  one key: the terminal snapshot's `valid_objects` (of `objects`, the valid
  records; #763 F18). Only the terminal snapshot carries `objects`; it has
  no `phase` / `completed` / `total`, which is why MIB Studio's status line
  read "Reanalysis: completed /" (`reanalysisStatusLine` in
  `desktop/src/reanalysisControls.tsx` now words each state).

## Export engine (`ExportEngine.h`, #512 S3)

The engine behind the backend bridge's `review_export_json` /
`review_export_status_json` (MIB Studio, YOFO Studio) and ReviewJobs' Export
Metrics / Export All / Batch export, moved verbatim out of
`BackendFacade::submitReviewExportJson`, which now keeps only the policy
(live factor and chart ring range as the defaults, the loaded file as the
default source, the `Export` operation; since #512 S7 the single flight and
the snapshot are ReviewJobs' export lane, `startExportJson`):

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
`ReviewJobs` owns one worker thread per lane (at most two; one at a time
under the review bridge's policy), joined by the next start on the lane or
by `shutdown()`; its lane state, results and status snapshots sit under one
mutex that is never held while the host or the sink is called.
The identity probe (#717 B1a) takes the session's mutex per file and,
through `ReviewJobs::probeRuns`, holds the lanes' mutex for the whole call
(lock order: lanes, then session; nothing takes them the other way round)
and refuses while either lane is claimed, so it never runs beside a session
read or a job. `review.run_identity` probes from one thread while another
reads frames and rows.
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
