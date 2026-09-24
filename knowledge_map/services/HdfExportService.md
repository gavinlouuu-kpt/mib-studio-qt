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
  `frame_` prefix, no metrics/charts).
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
  atomically at commit, an existing folder is refused.
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
