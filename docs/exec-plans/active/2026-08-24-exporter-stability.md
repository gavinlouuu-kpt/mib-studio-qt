# Repeated-export performance and stability

Status: active

## Goal

Make the standalone PySide HDF5 Export Tool and the native Qt HDF Review exporter safe to run repeatedly in one process. Equivalent jobs must use bounded memory, preserve output semantics, destroy workers deterministically, support cancellation and close-during-export, leave no apparently complete partial output, and pass repeat-run soak gates without modifying the source HDF5 file.

GitHub tracking:

- [Issue #344](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/344)
- [Related exporter path work #218](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/218)
- [Backend review/export job work #276](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/276)

## Scope and boundaries

In scope:

- Stream valid, invalid, and series image datasets instead of materializing whole datasets.
- Introduce immutable export-job/result/progress contracts.
- Correct PySide `QObject`/`QThread` lifecycle, single-flight behavior, cancellation, and shutdown.
- Write through same-parent temporary destinations and commit only complete exports.
- Bound generated-name lookup cost.
- Add a Qt-free native C++ export service that the current Qt shell can use and #276 can later expose through the backend bridge.
- Add regression, round-trip, fault-injection, lifecycle, sanitizer, packaging, and repeated-run soak coverage.

Out of scope:

- Changing CSV/JSON schemas, TIFF names/content, valid/invalid semantics, series numbering, or processing-contract behavior.
- Rewriting the HDF5 scientific schema.
- Implementing React/Tauri UI work from #276 in this issue.
- Loading all metadata into a new frontend cache. Paged review/metadata queries remain owned by #276 unless profiling proves metadata is part of this crash.

## Architectural decisions

- 2026-08-24: The Python CLI and PySide GUI will share one bounded-memory export engine. `export_hdf5.py` remains a CLI adapter; `export_worker.py` remains a Qt adapter.
- 2026-08-24: Images are read by integer hyperslab/index, with one frame (or a small explicit chunk) resident at a time. Series images are read one series frame at a time.
- 2026-08-24: Every export is an immutable job with a job ID, cancellation token, progress events, result, and explicit terminal state.
- 2026-08-24: Output is transactional. Metrics-only jobs use a same-directory temporary file; image/all jobs use a same-parent temporary directory. The final generated path is published only after success.
- 2026-08-24: Normal GUI completion never blocks the UI thread with `QThread.wait()` or another naked wait/join.
- 2026-08-24: The native implementation will live behind a Qt-free `HdfExportService` in the backend/processing layer, with a thin Qt asynchronous adapter. This prevents #344 from creating exporter logic that #276 must later replace.
- 2026-08-24: Fast PR checks use small repeat counts; the full 50-round soak is a scheduled/manual release gate. Both run the same harness and emit a machine-readable report.
- 2026-08-24: Source HDF5 files are opened read-only and SHA-256 checked before/after fault, cancellation, and soak tests.

## Delivery slices and dependencies

```text
PR 1: Python bounded-memory engine + transactional output
   ├──> PR 2: PySide lifecycle / cancellation / close safety
   └──> PR 3: Native Qt-free C++ export service + Qt adapter
PR 2 + PR 3 ──> PR 4: cross-surface soak, CI, packaging, final docs
```

#344 is not blocked by #276. PR 3 creates the reusable native seam that #276 should consume.

## PR 1 — Python bounded-memory export engine

### Objective

Remove whole-image-dataset materialization and establish one testable export engine used by CLI and GUI.

### Preferred file changes

- New `scripts/hdf_export_engine.py`
- Refactor `scripts/export_hdf5.py`
- Refactor path helpers currently in `scripts/export_hdf5.py`
- New `scripts/test_export_hdf5_streaming.py`
- New `scripts/test_export_hdf5_faults.py`
- Extend `scripts/test_export_hdf5_paths.py` only for path-specific cases
- Update `tests/CMakeLists.txt`
- Update `docs/howto/hdf5-export-app.md`
- Add `knowledge_map/task/2026-08-24-exporter-stability.md`
- Update `knowledge_map/current-state/Recent-Work.md`

### Contracts

Add frozen dataclasses/enums equivalent to:

- `ExportJob`
  - `job_id`
  - source HDF5 path
  - output root
  - format (`csv`, `json`, `images`, `all`)
  - frame selection (`valid`, `invalid`, `both`)
  - pixel-to-micron factor
  - series selection/range
  - cleanup policy
- `ExportPhase`
  - validating, metadata, valid images, invalid images, series images, committing, cleanup
- `ExportProgress`
  - job ID, phase, completed units, total units, current output
- `ExportResult`
  - terminal state, final path, counts, duration, warnings/error
- `ExportCancelled`
- cancellation uses `threading.Event`

Core entry point:

```python
run_export_job(job, *, cancel_event, on_progress) -> ExportResult
```

The engine contains no PySide imports.

### Streaming changes

- Replace GUI/CLI use of `read_hdf5_images(...)->dataset[:]` with indexed iteration.
- Never use `dataset[:]`, `np.array(dataset)`, or an equivalent whole-dataset conversion for image payloads.
- Read valid and invalid groups sequentially.
- Release the frame conversion/encoder temporary before reading the next frame.
- Iterate series as `dataset[record_index, series_index]`; do not materialize one whole 4-D record if avoidable.
- Derive total work from dataset shapes so progress is monotonic.
- Keep the HDF5 file open once per job.
- Preserve existing TIFF preparation, names, CSV/JSON schemas, and source-derived destination policy.
- Leave metadata behavior unchanged initially unless the regression profile identifies it as material; paged metadata remains coordinated with #276.

### Transactional output

- CSV/JSON-only: write `.name.partial-<job-id>.csv|json` in the final parent, flush/close, then `os.replace`/same-filesystem rename.
- Images/All: create `.<name>.partial-<job-id>/`, write everything there, then rename to the final source-derived directory.
- Select the final path from one parent-directory listing rather than `_2`, `_3`, ... `exists()` calls.
- Treat path selection as advisory: atomic create/rename handles concurrent exporters and retries on collision.
- On cancellation/failure, remove the partial destination. If cleanup fails, retain the `.partial-*` name and write a compact failure manifest; never expose it under a normal completed name.

### Regression-first tests

The first implementation commit must include a test that fails against current `develop`:

- fake image dataset raises on slice/Ellipsis/whole-array conversion;
- current exporter fails because it requests the complete dataset;
- refactored engine passes by requesting integer indices only.

Additional tests:

- Real temporary HDF5 fixture with valid, invalid, metadata, and series datasets.
- CSV/TIFF counts, names, pixels, and hashes match current behavior.
- Source file hash is unchanged.
- Cancellation after N valid frames, during invalid frames, and during series frames.
- Injected `cv2.imwrite` failure and unwritable/file-as-parent destination.
- Partial destination is removed or remains visibly `.partial-*` with failure metadata.
- Existing output-root validation and dotted/spaced basenames remain green.
- Directory with thousands of old generated names is resolved with one listing and bounded calls.

### PR 1 acceptance

- [ ] Regression test demonstrated failing before the implementation and passing after it.
- [ ] Image peak memory is independent of the number of frames at the engine level.
- [ ] CLI `csv`, `json`, `images`, and `all` outputs remain compatible.
- [ ] Cancellation/faults cannot publish a normal-looking partial output.
- [ ] Source HDF5 is byte-identical after every test.
- [ ] Docs/vault and execution-plan progress are updated in the same PR.

## PR 2 — PySide worker/thread lifecycle and shutdown

### Objective

Make repeated GUI jobs deterministic: one active job, no UI-thread wait, safe cancellation, and no destruction of a running `QThread`.

### Preferred file changes

- `scripts/export_worker.py`
- `scripts/hdf5_export_app.py`
- New `scripts/test_hdf5_export_app_lifecycle.py`
- New minimal `scripts/requirements-export-test.txt` if CI should avoid the full general-purpose requirements set
- `scripts/hdf5_export.spec`
- Windows/macOS build scripts if module inclusion changes
- New/updated `.github/workflows/exporter-ci.yml`
- Documentation/vault updates

### Worker lifecycle

- Construct `ExportWorker` with an immutable `ExportJob`, shared cancellation event, and injected/default engine runner.
- Worker slot takes no mutable widget parameters.
- Emit progress/result separately from a no-argument terminal `finished` signal.
- Wire:

```text
thread.started -> worker.run
worker.finished -> thread.quit
worker.finished -> worker.deleteLater
thread.finished -> thread.deleteLater
thread.finished -> window cleanup handler
```

- Do not call `thread.wait()` during normal completion.
- Keep strong references in one `ActiveExport` record until `thread.finished`.
- Clear references and re-enable controls only in the thread-finished handler.
- Never call `terminate()`.
- Include `job_id` in status/log records.

### Window behavior

- Enforce one active export per window; a second start is rejected/disabled.
- Cancel button sets the shared `threading.Event` and remains disabled after the first request.
- `closeEvent`:
  - closes immediately when idle;
  - while active, ignores the close, records `close_pending`, requests cancellation, and keeps the window alive;
  - after `thread.finished`, schedules the final close on the GUI event loop.
- A worker exception must become a structured failed result and still execute the same teardown path.
- UI progress must remain monotonic and identify the active phase.

### Lifecycle tests

Run with `QT_QPA_PLATFORM=offscreen` using real PySide6:

- 10 consecutive small exports through the actual window/worker path in PR CI.
- Count worker/thread `destroyed` signals; starts, finishes, and destructions reconcile.
- Install a Qt message handler and fail on `QThread: Destroyed while thread is still running` or related lifecycle warnings.
- Attempt a second export while one is active.
- Cancel in each engine phase using an injected slow/test runner.
- Close the window in each phase; assert deferred close and eventual clean destruction.
- Worker raises an exception; controls recover and thread is destroyed.
- Weak references to prior worker/thread wrappers clear after event-loop drain.

### Packaging checks

- Add `hdf_export_engine` and any new modules to PyInstaller hidden imports where required.
- Build the Windows executable and run a headless/smoke invocation against a fixture.
- Verify the packaged executable completes two consecutive exports and exits cleanly.

### PR 2 acceptance

- [ ] No UI-thread blocking wait on normal completion.
- [ ] Exactly one active worker/thread per window.
- [ ] Close-during-export is cancellation-first and crash-free.
- [ ] All terminal paths destroy worker and thread deterministically.
- [ ] Packaged exporter includes and runs the new engine.
- [ ] Thread/lifecycle tests have a watchdog or bounded event-loop timeout.

## PR 3 — Native Qt-free C++ export service and Qt adapter

### Objective

Replace the mutable, synchronous `HdfReviewTab` export orchestration with a reusable bounded/cancellable backend service while preserving the Qt workflow.

### Preferred file changes

- New `include/backend/recording/HdfExportService.h`
- New `src/backend/recording/HdfExportService.cpp`
- `src/backend/CMakeLists.txt` (`mib_processing`, because the service is Qt-free and reuses HDF5/OpenCV)
- `include/backend/recording/Hdf5Service.h`
- `src/backend/recording/Hdf5Service.cpp`
- `include/frontend/tabs/HdfReviewTab.h`
- `src/frontend/tabs/HdfReviewTab.cpp`
- `src/frontend/utils/HdfReviewExportPaths.cpp`
- Possibly a thin `frontend/system/HdfReviewExportController` if needed for queued progress/cancellation
- New `tests/recording/hdf_export_roundtrip_test.cpp`
- New `tests/recording/hdf_export_fault_injection_test.cpp`
- New `tests/recording/hdf_export_stress_test.cpp`
- Optional focused frontend controller test
- `tests/CMakeLists.txt`
- Matching service/frontend/data-flow vault notes

### Backend API

Define Qt-free request/progress/result types:

- `HdfExportRequest`
  - source path, output root, format, frame selection, conversion factor, series range
  - optional named supplemental images for chart TIFFs
- `HdfExportProgress`
  - phase, completed, total, current path
- `HdfExportResult`
  - status, final path, exported/filtered/failed counts, warnings/error
- cancellation token backed by shared `std::atomic_bool` (C++17)
- progress callback

Core call:

```cpp
HdfExportResult HdfExportService::run(
    const HdfExportRequest&,
    const HdfExportCancelToken&,
    HdfExportProgressFn);
```

The service:

- opens a separate read-only `Hdf5Service` per job;
- streams images through existing `readImageByIndex` and series readers;
- does not mutate `HdfReviewTab::hdfReader_`, `validFrames_`, or `invalidFrames_`;
- uses same-parent temporary files/directories and commit-on-success;
- preserves metrics/image/chart names and content;
- checks cancellation before each frame/artifact;
- uses spdlog with job ID/phase;
- is directly reusable by #276.

### Qt adapter

- Prompt for destination and series range on the GUI thread before creating the request.
- Snapshot Qt charts on the GUI thread into owned `cv::Mat` supplemental images before launching the job; no QPixmap/QWidget access occurs in the worker.
- Run the backend call asynchronously using a thin controller or `QtConcurrent::run` + `QFutureWatcher`.
- Worker callable captures request/token/service state only, never raw `this`/QWidget pointers.
- Convert progress callbacks to queued UI updates using `QPointer`/queued invocation.
- Add `exportInProgress`, disable conflicting actions, expose cancellation, and restore controls only on completion.
- Eliminate the batch-export pattern that moves `hdfReader_` and frame vectors out of the live tab.
- Avoid nested `QCoreApplication::processEvents()` for export orchestration. Chart rendering may explicitly process layout only before the job and must not admit a second export.
- Main-window close should request cancellation and defer final destruction until the exporter reports idle; no naked `waitForFinished()`.

### HDF5 diagnostics

Add a small diagnostics API such as `openObjectCountForDiagnostics()` that wraps `H5Fget_obj_count` for tests/logging. Record before/after each native export in debug/test builds. Do not expose HDF5 IDs outside `Hdf5Service`.

### Native tests

- Round-trip: generate HDF5 fixture, export, reopen/verify CSV, TIFF dimensions/pixels, series, charts, and source hash.
- Fault injection: file-as-parent, read-only/unwritable destination, writer failure seam, cancellation at every phase, partial cleanup.
- Stress: repeated jobs with `mib::test::Watchdog`, constant HDF5 open-object count, output equivalence, no stale reader state.
- Sanitizers: backend/recording labels so ASan+UBSan and TSan exercise the service.
- Frontend test where available: single-flight, queued progress, cancellation, and tab/window destruction without a callback-after-free.

### PR 3 acceptance

- [ ] Native export logic is Qt-free and reusable by #276.
- [ ] `HdfReviewTab` no longer mutates its active reader/model state to batch export another file.
- [ ] Native exports run off the GUI thread with cancellation/progress.
- [ ] HDF5 object counts return to baseline after every job.
- [ ] Round-trip, fault-injection, stress, ASan/UBSan, and TSan checks pass.
- [ ] No processing/HDF5 schema or scientific-output change.

## PR 4 — Soak gates, CI, packaging, and closure

### Objective

Prove the fix under repeated use and make regression visible in CI/release evidence.

### Harnesses

Add a machine-readable soak harness for each surface, sharing the same fixture and manifest rules where practical:

- Python/PySide: `scripts/exporter_soak.py`
- Native: `tests/performance/hdf_export_soak_test.cpp` or a small driver invoked by the Python orchestrator

Fixture includes:

- valid and invalid metadata/images;
- multi-image series;
- deterministic pixel patterns derived from frame index;
- enough frames to exercise allocation without making PR CI excessively slow.

Each round records:

- duration by phase and total;
- baseline, peak, and post-job RSS;
- active/created/destroyed worker/thread counts;
- HDF5 open-object count for native jobs;
- output counts and SHA-256 manifest;
- source SHA-256 before/after;
- terminal result and cleanup state.

### Test tiers

- PR fast lane: 5-10 cycles, small fixture, lifecycle/fault checks.
- Scheduled/manual soak: 50 cycles in one process for PySide and native implementations.
- Packaging smoke: two consecutive jobs using the built standalone executable.

Suggested workflow: `.github/workflows/exporter-soak.yml`, manual + scheduled, with JSON report/log artifacts. When MLflow credentials are present, publish duration/RSS summaries to the repository's configured MLflow service; never require secrets for ordinary PR checks.

### Robust gates

After warm-up:

- Round 50 duration <= 1.25x median rounds 2-5.
- Idle RSS plateaus: compare medians rather than one sample; final-window median must remain within the agreed relative/absolute allowance of the early post-warm-up median.
- RSS slope over the steady-state rounds must not show per-job linear growth.
- A large-frame-count fixture must not increase peak RSS in proportion to total dataset bytes; peak should scale with one frame/chunk plus bounded encoder/runtime overhead.
- Created/finished/destroyed lifecycle counts reconcile.
- HDF5 open-object count is constant.
- Output manifests are identical for completed jobs.
- Source hash never changes.
- Every threaded harness has a watchdog and exits with a distinct stuck-test code.

Thresholds should be calibrated once from Windows and Linux evidence and recorded in this decision log; do not weaken them silently to make CI green.

### Final documentation

- Update `docs/howto/hdf5-export-app.md` with cancellation, partial-output, and memory behavior.
- Update `knowledge_map/frontend/HdfReviewTab.md` and `knowledge_map/services/Hdf5Service.md`.
- Update architecture/threading/data-flow notes if the native asynchronous job seam is introduced.
- Append `knowledge_map/current-state/Recent-Work.md`.
- Run docs and screenshot checks if visible UI text/layout changed.
- Set this plan `Status: completed` and move it to `docs/exec-plans/completed/` only after the 50-round evidence is attached to #344.

## Verification commands

Python core and GUI:

```bash
python3 -m unittest -v \
  scripts/test_export_hdf5_paths.py \
  scripts/test_export_hdf5_streaming.py \
  scripts/test_export_hdf5_faults.py
QT_QPA_PLATFORM=offscreen python3 scripts/test_hdf5_export_app_lifecycle.py
```

Backend/native:

```bash
cmake --preset linux-backend-only
cmake --build --preset linux-backend-only-build
ctest --preset linux-backend-only-test -R 'hdf_export|export_hdf5' --output-on-failure
ctest --preset linux-backend-only-test --output-on-failure
```

Sanitizers:

```bash
cmake --preset linux-backend-only -DMIB_SANITIZER=thread
cmake --build --preset linux-backend-only-build
ctest --test-dir build/linux-backend -L 'backend|recording' -LE 'integration|performance' --output-on-failure

cmake --preset linux-backend-only -DMIB_SANITIZER=address+undefined
cmake --build --preset linux-backend-only-build
ctest --test-dir build/linux-backend -L 'backend|recording' -LE 'integration|performance' --output-on-failure
```

Docs/package/soak:

```bash
python3 scripts/check_docs.py
python3 scripts/check_screenshots.py   # when visible UI/screens change
cd scripts && pyinstaller hdf5_export.spec --clean
python3 scripts/exporter_soak.py --cycles 50 --report exporter-soak.json
```

Windows validation must include the packaged exporter and native Qt app because this is the reported production platform.

## Risks and mitigations

- Windows/network rename semantics: keep temporary and final paths under the same parent; handle collision at commit; retain visible `.partial-*` only if cleanup fails.
- PyInstaller omissions: explicit hidden import plus built-executable repeated-run smoke.
- Qt deletion ordering: terminal signal and thread-finished cleanup are separate; tests observe `destroyed` signals and Qt warnings.
- Timing-test flakiness: use warm-up, medians, slopes, and ratios; full 50-cycle gate runs outside the sanitizer lane.
- Behavior drift: compare output manifests/hashes and keep schema/name tests from #218.
- Scope overlap with #276: native service is bridge-neutral; no Tauri command/event/UI implementation lands in #344.

## Progress

- [x] PR 1 — Python bounded-memory engine and transactional output
      (`scripts/hdf_export_engine.py`, `scripts/test_export_hdf5_streaming.py`; 2026-09-07)
- [x] PR 2 — PySide lifecycle, cancellation, close safety
      (`scripts/export_worker.py`, `scripts/hdf5_export_app.py`,
      `scripts/test_hdf5_export_app_lifecycle.py`; PyInstaller hidden import added —
      packaged-executable smoke on Windows still pending)
- [x] PR 3 — Qt-free native export service and asynchronous Qt adapter
      (`HdfExportService`, `HdfReviewTab` rewiring, `tests/recording/hdf_export_service_test.cpp`, TSan)
- [x] PR 4 — 50-round soak harnesses (`scripts/exporter_soak.py`, `recording.hdf_export_soak`,
      `.github/workflows/exporter-soak.yml`) and evidence in `docs/evidence/2026-09-07-exporter-soak/`
- [ ] Attach soak reports and implementation PR links to #344 (Linux evidence captured; Windows evidence pending)
- [ ] Move this plan to `completed/` and close #344

Delivered on branch `claude/host-sdk-reliability-qt-ui-g03ubd` as part of the
Host SDK reliability release (#371). Decision log addition (2026-09-07): an
image *write* failure fails the job (partial output discarded) while an
unsupported frame shape is skipped with a warning, matching the historical
exporter; an explicit metrics *file* destination confirmed by the save dialog
is replaced atomically at commit, an existing folder is never merged into.
