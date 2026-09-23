# HdfExportService

> Qt-free, bounded, cancellable export of one HDF5 experiment/recording
> file to CSV + TIFF (issue #344). One job = one immutable request, one
> private read-only reader, streamed frames, transactional output.

**Source:** `src/backend/recording/HdfExportService.cpp`,
`include/backend/recording/HdfExportService.h`
**Tests:** `tests/recording/hdf_export_service_test.cpp`
(`recording.hdf_export_service`; `recording.hdf_export_soak` = 50 rounds,
`performance` label), TSan lane
**Related:** [[Hdf5Service]], [[../frontend/HdfReviewTab]],
[[../data-model/HDF5-Storage]]; Python twin `scripts/hdf_export_engine.py`

## Responsibility

- `run(request, cancelToken, onProgress)` executes synchronously on the
  calling thread and **never throws for job failures**: the
  `HdfExportResult` carries `Completed / Cancelled / Failed`, counts
  (valid/invalid rows, images, series, charts), warnings, `finalPath` (only
  when completed) and `retainedPartialPath` (only when a partial output was
  deliberately kept).
- Opens its **own** `Hdf5Service` read-only per job; streams every image
  through `readImageByIndex` / `readSeriesImagesByIndex` (one frame
  resident at a time); handles experiment files (`/valid_frames`,
  `/invalid_frames`, series) and recording files (`/recorded_frames`,
  `frame_` prefix, no metrics/charts in All mode; explicit MetricsCsv requests
  export frame metadata through the same CSV writer).
- Output is **transactional**: writes into
  `.<final-name>.partial-<job-id>` (file for `MetricsCsv`, directory for
  `Images`/`All`) next to the destination and publishes it with a rename
  only after success. Cancel/failure removes the partial output; if removal
  fails or `keepPartialOnFailure` is set, it stays under the `.partial-`
  name with an `export-failure.json` manifest. A normal-looking export can
  never be partial.
- Cancellation (`HdfExportCancelToken`, shared atomic) is polled before
  every artifact, image, series frame and the commit.
- Generated names: `nextAvailableName()` lists the parent once and picks
  `<base>` or `<base>_<max suffix + 1>` — cost does not grow with the number
  of previous exports. An explicit destination is honoured; an existing
  *file* destination (already confirmed by a save dialog) is replaced
  atomically at commit, an existing folder is refused. A destination equivalent
  to the source recording (including symlinks/hardlinks) is always rejected.
- Charts are not rendered here: the caller passes `supplementalImages`
  (name → BGR `cv::Mat`) captured on its own thread; the job writes them for
  `All` exports.
- CSV format/columns are identical to the historical `HdfReviewTab` writer
  (`Frame Type … Bright Q4`, fixed 3/2-decimal formatting).

## Threading

Stateless apart from the optional test image-writer seam; safe to run on a
thread-pool thread (the Qt shell uses `QtConcurrent::run` + a
`QFutureWatcher`). Progress callbacks fire on the worker thread; the caller
re-dispatches to its UI thread.

## Gotchas

- `Hdf5Service::globalOpenObjectCountForDiagnostics()` /
  `openObjectCountForDiagnostics()` wrap `H5Fget_obj_count`; the stress
  test asserts the global count returns to baseline after every job.
- The bridge work in #276 should consume this API rather than re-implement
  export logic in the UI layer.

## Tauri facade binding (2026-09-23)

`BackendFacade::submitReviewExportJson` starts one owned export worker over
this service. Busy submissions are rejected; completed workers are reaped
before replacement. `fetchReviewExportStatusJson` retains the latest job's
progress/terminal snapshot so event loss or view navigation does not hide
its outcome. Operation IDs/counters are decimal strings at the JSON seam.
`requestOperationCancel` shares the service's atomic cancellation token;
shutdown joins the worker before teardown and publishes terminal operation
status only after service cleanup. Terminal results include published final
path, retained partial path, errors, warnings and image/metrics counts.

Tauri commands `review_export_json` / `review_export_status_json` back the
TypeScript `bridge.reviewExport` / `reviewExportStatus` helpers. The legacy
`review_export_csv` command now uses the same engine with an explicit CSV
destination. No second exporter remains in the facade. Chart image payloads
and series subrange selection are not yet exposed in this bridge request;
All uses the service's default full-series selection without UI charts.

`backend.export_bridge_facade` guards repeated cancellation/reopen/export,
busy rejection, source hash immutability, output-parent faults, and retained
terminal recovery under a watchdog. Rust review contract tests verify CSV
round-trip and retained status. Run the backend stress/TSan lane before cutover.


Valid-only and invalid-only experiment recordings need not contain both metadata
datasets: export checks presence and skips the absent side without swallowing
read errors. An explicit output equivalent to the source (including existing
hard/symbolic links) is refused. React owns status polling across navigation,
keeps cancel available in a persistent status panel, and distinguishes accepted
cancellation from authoritative cancelled/completed/failed results and retained
partial output. Native facade tests exercise repeated cancel/reopen/export cycles.

### Facade batch chain

`submitReviewExportJson` accepts optional `source_paths` (1–256 nonempty paths).
One owned worker runs the existing service serially, with one cancellation ID
and independently opened readers. `explicit_destination` is rejected for batch
requests; generated names are resolved per job. Status retains each attempted
source's terminal state and final/partial path. A failed source does not suppress
later files; any failure prevents aggregate success. Cancellation retains already
published outputs and stops the remaining chain. The active Review source is
unchanged. Regression coverage includes a failed middle source, duplicate source
names and source immutability, in addition to repeated cancel/reopen cycles.

### Shared full-data Review charts

`ReviewChartData` extracts Qt's area calibration (`area * pixelToMicron²`),
valid-object filtering, 0.5-wide edge-clamped ring-ratio histogram and isoelastic
reference curves into a Qt-free helper. Both Qt Review and Tauri snapshots use
it. The repository's existing isoelastic table is embedded at build time, so
installed chart behavior does not depend on a developer working directory.
`fetchReviewChartsJson` uses all valid metrics from an independent source reader;
its 128×128 density cells conserve every finite point (not a sampled page), and
histogram bins retain exact full-file counts. Counters are decimal strings.

Tauri `all` and `charts` jobs render 1200×1200 TIFFs using that same helper inside
the transactional export worker. Chart-only jobs export no source-frame images;
failed chart writes never publish a successful output. Calibration/ring limits
are explicit snapshots of current backend settings, matching Qt, not claimed to
be recovered recording calibration. Fixed isoelastic reference conditions are
labelled rather than automatically assumed to match the experiment.
