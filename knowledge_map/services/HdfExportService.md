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
