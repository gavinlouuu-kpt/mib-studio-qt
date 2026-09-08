# Hdf5Service

> Batched HDF5 read/write for experiment frames + metadata, plus
> frame-recording mode and review-time scalable (hyperslab) reads.
> HDF5 headers are hidden behind a PIMPL.

**Source:** `src/backend/recording/Hdf5Service.cpp`,
`include/backend/recording/Hdf5Service.h`
**Related:** [[ProcessingService]], [[../data-model/HDF5-Storage]],
[[../frontend/HdfReviewTab]], [[../architecture/AppBackend]]

**Build target:** compiled into `mib_processing`, the Qt-free static library
`mib_backend` links publicly (`src/backend/CMakeLists.txt`). `Hdf5Service`
itself has no `CrashReporter` (or Qt) dependency — see "Performance telemetry
hook" below.

## Performance telemetry hook

`setHdf5PerformanceTraceHook(PerformanceTraceFn)` (free function, same
header) is an optional sink for HDF5 I/O performance events (`hdf5.close_file`,
`hdf5.append_frames`). It defaults to a no-op so `Hdf5Service` stays Qt-free;
`src/frontend/core/main.cpp` wires it to
`CrashReporter::capturePerformanceTransaction` during startup
(`installCrashReporter`), right after `CrashReporter::init`. This is the only
place `Hdf5Service` and `CrashReporter` are connected, and only in the real
desktop app — tests and portable consumers may leave it unset.

## Async write decoupling — `HdfWriteQueue`

`include/backend/recording/HdfWriteQueue.h` is a header-only, bounded (3-slot)
single-writer queue used by **both** experiment flush ([[ProcessingService]]
`flushBufferedFrames`) and frame recording ([[../architecture/AppBackend]]
`startFrameRecording`) so slow HDF5 writes never stall the producer
(capture/collection). A dedicated writer thread drains the FIFO and calls an
injected `writeFn` (`appendFrames` / `appendRecordingFrames`). A failed write
**or** a `submit` when all 3 slots are in flight (disk too slow) is a fatal,
latched error: the writer stops, a one-shot `onError` fires, and further
`submit`s are rejected. The written/flushed counters advance only on a confirmed
successful write. `flushAndStop()` drains + joins for a clean stop. `Hdf5Service`
has no internal lock, so callers must ensure only one writer touches the open
file at a time — the queue's single writer thread provides that, and stop paths
drain the queue before any direct write.

`HdfWriteQueue`'s ctor parameter is `slotCount` (not `slots`, which Qt defines as
a macro). Unit-tested by `tests/backend/hdf_write_queue_test.cpp`.

## Responsibility

- Opens one HDF5 file at a time (`openFile`, `loadFile`, `closeFile`).
- Writes valid/invalid frames as bulk (`saveFrames`) or incrementally
  (`initializeDatasets` + `appendFrames`).
- Stores experiment metadata: start/end time (ns), totals, `ProcessingConfig`,
  ROI, optional background image, and the exact `ProcessingCoreIdentity` used;
  plus raw config JSON via `writeConfigJson`.
- Append hot paths (`appendFrames`, `appendRecordingFrames`) flush via
  `maybeIntervalFlush()`: an `H5Fflush(H5F_SCOPE_GLOBAL)` at most once per
  `MIB_HDF5_FLUSH_INTERVAL_MS` (default 5000 ms). This keeps the recorder
  thread off synchronous I/O on every batch, so it cannot fall behind the
  `frameStore_` ring buffer and drop frames. One-shot finalization writes
  (`writeExperimentInfo`, `writeConfigJson`, `writeRecordingInfo`) flush
  unconditionally. There is **no** `.recovery.h5` sidecar copy.
- Writable files are created with HDF5 strong-close semantics and
  `closeFile()` performs an explicit final global flush before `H5Fclose`.
  This protects the superblock/EOA state after recording stop and logs final
  flush status, close timing, and the open-object count. A crash mid-recording
  can lose up to one flush interval — the accepted tradeoff for throughput.
- Review reads: `readValidFrames`, `readInvalidFrames`, plus scalable
  `readImageByIndex` / `readImagesRange` using hyperslabs.
- Metadata-only reads (`readValidMetadata` / `readInvalidMetadata`) skip
  image payloads — used by [[../frontend/HdfReviewTab]] for lazy virtualization.
- Chart snapshots: `saveChartSnapshot` / `readChartSnapshot` (2D/3D images
  without batch dim).
- **Multi-image series** (`multi_image_enabled` in ProcessingConfig):
  `getSeriesImageInfo`, `readSeriesImagesByIndex` — 4D `(N, seriesCount, H, W)`.
- **Frame recording mode** (raw frames, no contours):
  `initializeRecordingDatasets`, `appendRecordingFrames`, `writeRecordingInfo`
  with `RecordingFrameMeta` (index, timestampNs, width, height).
  Matching readers: `isRecordingFile()` (probes `/recording_info`),
  `readRecordingMetadata(frames)` (fills only index + timestampNs on each
  `ProcessedFrame`), `readRecordingInfo(start, end, total, filtered, ...)`.
  Recording info now persists `multi_image_enabled` (`uint8`) and
  `multi_image_count` (`uint64`) alongside start/end/totals so review mode can
  reconstruct multi-image windows from `/recorded_frames/images`.
  Used by [[../frontend/HdfReviewTab]] to present recording files.

## Threading

Blocking I/O on whichever thread calls it. In practice:
- Experiment flush: called from `ProcessingService::flushBufferedFrames`.
- Review reads: called from Qt main thread via [[../frontend/HdfReviewTab]].
- Frame recording writes: called from `AppBackend`'s recording thread.

## Processing-core provenance

`writeExperimentInfo(..., processingCore)` and
`writeRecordingInfo(..., processingCore)` write the selected core identity as
attributes on `/experiment_info` and `/recording_info`, respectively:

- `processing_core_version`, `processing_contract_version`,
  `processing_engine_abi_version`
- `processing_core_sha256`, `processing_manifest_sha256`,
  `processing_release_tag`
- `processing_core_source`, `processing_core_build_id`,
  `processing_runtime_fingerprint`

Callers hold a `ProcessingService::CoreOperationLease` and pass its identity,
so a core cannot be swapped between processing/empty classification and final
metadata. Offline mask regeneration uses the identity captured by
`processBatch`. `readProcessingCoreIdentity` reads either group (with scalar,
bounded variable/fixed-string handling) and returns `false` for a legacy file
that predates provenance. The writer records the bundled identity when no
explicit identity is supplied, preserving deterministic metadata for older
call sites.

## Run accounting (issue #367)

`writeRunAccounting(RecordingAccountingSnapshot)` / `readRunAccounting(...)`
persist and read the versioned `accounting_*` attributes described in
[[../data-model/HDF5-Storage]] on whichever info group exists. Producers:
`AppBackend` raw recording (after `writeRecordingInfo`) and
`MainWindow::onStopExperiment` (after `writeExperimentInfo`, from
`ProcessingService::experimentAccountingSnapshot()`). Consumer:
[[../frontend/HdfReviewTab]] status text. Types live in
`include/backend/recording/RecordingAccounting.h` (Qt-free, header-only).

## Open-object diagnostics (issue #344)

`globalOpenObjectCountForDiagnostics()` (static) and
`openObjectCountForDiagnostics()` wrap `H5Fget_obj_count` so tests and
debug logging can prove HDF5 handles return to baseline after repeated
jobs ([[HdfExportService]] stress test). HDF5 ids never leave this class.

## Run configuration snapshot (issue #369)

`writeRunSnapshotJson(runJson, readinessJson)` / `readRunSnapshotJson(...)`
store the frozen `RunConfigurationSnapshot` and the readiness evaluation it
was started from as variable-length UTF-8 string attributes
(`run_snapshot_json`, `readiness_json`, `run_snapshot_schema_version` = 1)
on the `/run_provenance` group. [[../architecture/ExperimentCoordinator]]
writes them immediately after `initializeDatasets()` and before the run may
enter Running; a failure rolls the Start back and removes the file.

## Acquisition provenance (issue #368)

`writeAcquisitionProvenance(descriptor, telemetry)` /
`readAcquisitionProvenance(...)` persist the session `TimestampDescriptor` and
the per-metric telemetry with validity (see [[../data-model/HDF5-Storage]]).
Written by `AppBackend` raw recording and `MainWindow::onStopExperiment` after
the run info; legacy files return `false` with an Unsupported descriptor.

## Gotchas

- `openFile(path)` creates the destination's parent directory tree
  (`std::filesystem::create_directories`) before `H5Fcreate`. HDF5 cannot
  create intermediate directories, so without this a save to a folder that
  does not exist yet (e.g. a freshly chosen path on a second/external/network
  drive) fails — this was the root cause of "can save on one drive but not
  the other." Open failures now log an actionable message (drive available?
  writable? valid name? free space?) instead of a generic error. Regression
  guard: `tests/integration/e2e_storage_destinations_test.cpp`
  (`integration.e2e_storage_destinations`) saves to fresh/nested/long paths.
  Note: paths exceeding the Windows `MAX_PATH` (260) limit still fail unless
  long-path support is enabled at the OS level.
- `writeConfigJson` must follow `writeExperimentInfo`.
- **Append paths validate batch dimensions against the dataset extent**
  (`appendImageDataset`, `appendSeriesImageDataset`): the extent is fixed by
  the first-ever batch, so a mid-recording frame-size change fails loudly
  with a precise log message (surfaced via the fatal-save-error sink) instead
  of a cryptic `H5Dwrite` error. The series path also rejects a series image
  whose dims differ from the dataset's — its row-copy scratch buffer is sized
  from the dataset dims and a larger image would overflow the heap.
- `writeImageDataset` computes per-frame byte size in `size_t` (an `int`
  product would overflow for pathological frame sizes and undersize the
  staging buffer).
- `loadFile(path)` opens the primary file only — there is no recovery-sidecar
  fallback. A corrupt/unfinalized primary fails to load (try
  `h5clear --increment`).
- Scalable reads (`readImageByIndex`, `readImagesRange`) support both 3D
  `(N,H,W)` and 4D `(N,H,W,C)` datasets — use them for large files instead
  of `readValidFrames` to avoid OOM. See task
  `knowledge_map/task/review_2gb_scalability.md`.
- PIMPL means you can't see HDF5 types in headers — look at
  `src/backend/recording/Hdf5Service.cpp` for dataset paths and dtypes.
- The optional `H5Pset_file_locking` call is compiled only for HDF5 1.10.7+.
  This keeps the processing-only manylinux build compatible with the 1.10.5
  development package while preserving locking on newer desktop libraries.
- If a primary `.h5` opens only after `h5clear --increment`, check recorder
  logs for final flush/close errors and whether the app or host was killed
  before `stopFrameRecording()` returned. With interval flushing, a kill
  between flushes can leave up to `MIB_HDF5_FLUSH_INTERVAL_MS` of frames
  unflushed.
