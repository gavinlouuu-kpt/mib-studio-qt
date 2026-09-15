Title: Repeated GUI exports slow down and crash — bounded, transactional, cancellable exporters

Date: 2026-08-24 (plan) / 2026-09-07 (implementation)

Context
- Issue #344: running the standalone PySide HDF5 Export Tool (or the native
  HDF Review export) repeatedly in one process got slower each time and
  eventually froze/crashed. Root causes found in source: whole-dataset
  materialization (`dataset[:]`) per export, PySide worker/`QThread`
  teardown that blocked the GUI in `wait()` and never used the
  `deleteLater` chain, unbounded `_2`, `_3`, … name probing, no
  cancellation primitive, no partial-output policy, and a native batch path
  that swapped the live reader/model out of the tab.
- Execution plan: `docs/exec-plans/active/2026-08-24-exporter-stability.md`.

Changes
- Python engine `scripts/hdf_export_engine.py` (no Qt): frozen `ExportJob`
  / `ExportProgress` / `ExportResult`, `run_export_job(job, cancel_event,
  on_progress, image_writer)`, one-frame-at-a-time streaming by integer
  index (series as `dataset[i, s]`), `threading.Event` cancellation checked
  before open/metrics/every image/every series frame/commit, same-parent
  `.<name>.partial-<job>` staging + rename-on-success, single-listing
  `next_available_name` (`max suffix + 1`). `export_hdf5.py` is now a CLI
  adapter over the engine (`unique_path` delegates to the engine; the old
  `read_hdf5_images` is kept only for `reanalyse_hdf5.py`).
- `scripts/export_worker.py`: `ExportWorker(job, cancel_event, runner)`
  with a no-argument `run` slot, `progress`/`result`/`finished` signals, a
  worker exception becomes a `FAILED` result.
- `scripts/hdf5_export_app.py`: `ActiveExport` record; wiring
  `thread.started→worker.run`, `worker.finished→thread.quit/deleteLater`,
  `thread.finished→thread.deleteLater/on_export_thread_finished`; no GUI
  `wait()`; single-flight; cancel button idempotent; `closeEvent` cancels,
  ignores the close, and completes it after `thread.finished`; job-id
  logging; monotonic phase-labelled progress.
- Native `backend::recording::HdfExportService` (Qt-free, own reader,
  transactional, cancellable) — see [[../services/HdfExportService]];
  `Hdf5Service::globalOpenObjectCountForDiagnostics()`.
- [[../frontend/HdfReviewTab]]: all export buttons run one job at a time
  through `QtConcurrent::run` + `QFutureWatcher` with a cancellable
  `QProgressDialog`; charts are snapshotted on the GUI thread into
  `cv::Mat`s before the job; batch exports are chained per file with a
  separate reader (the live `hdfReader_` / frame vectors are never moved
  out); destructor cancels and waits (bounded by one frame).
- Packaging: `hdf_export_engine` added to both PyInstaller specs.

Tests / evidence
- `scripts/test_export_hdf5_streaming.py` (regression: a dataset that
  raises on slice/`__array__` passes through the engine and fails through
  the legacy reader; real-fixture round-trip + manifests; cancel per phase;
  write failure; retained partial; unwritable root; 2000-name lookup with
  one `scandir` and zero `exists()` calls).
- `scripts/test_hdf5_export_app_lifecycle.py` (offscreen PySide: 10
  exports with reconciled destroyed counts and cleared weakrefs; second
  start refused; cancel in every phase; close during export deferred;
  worker exception; Qt thread warnings fail the test; watchdog).
- `tests/recording/hdf_export_service_test.cpp` (+ TSan).
- `scripts/exporter_soak.py` + `recording.hdf_export_soak`: 50-round
  reports in `docs/evidence/2026-09-07-exporter-soak/` — stable timing,
  RSS plateau, constant HDF5 object count, identical manifests, source
  hash unchanged.

Open
- Windows packaged-executable and native Windows validation;
  `.github/workflows/exporter-soak.yml` runs the harness on schedule.
