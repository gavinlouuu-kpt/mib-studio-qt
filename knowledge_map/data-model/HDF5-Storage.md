# HDF5 Storage

> Schema and patterns for experiment files. Implementation is behind a
> PIMPL in [[../services/Hdf5Service]].

**Source:** `src/backend/recording/Hdf5Service.cpp`
**Related:** [[../services/Hdf5Service]], [[../services/ProcessingService]]
(`ProcessedFrame`, `ProcessingConfig`), [[../frontend/HdfReviewTab]]

## File layout (conceptual)

- `/experiment_info` — root attributes:
  `startTimeNs`, `endTimeNs`, `totalValidFrames`, `totalInvalidFrames`,
  serialized `ProcessingConfig`, ROI, optional `background` image,
  `config_json` (raw JSON string), plus processing-core provenance:
  `processing_core_version`, `processing_contract_version`,
  `processing_engine_abi_version`, `processing_core_sha256`,
  `processing_manifest_sha256`, `processing_release_tag`,
  `processing_core_source`, `processing_core_build_id`, and
  `processing_runtime_fingerprint`.
- `/valid_frames/` — per-field datasets (images, masks, metrics).
- `/invalid_frames/` — same shape; populated only at
  `invalidFrameSamplingRate` sampling.
- `/series_images` — 4D `(N, seriesCount, H, W)` for multi-image mode.
- `/recorded_frames/` — used by frame-recording mode (images + basic
  metadata only; no contour metrics).
- `/recording_info` — raw-recording totals/config plus the same nine
  processing-core provenance attributes as `/experiment_info`.
- **Run accounting (issue #367, `accounting_schema_version` = 1)** — written
  by `Hdf5Service::writeRunAccounting` as `accounting_*` attributes on
  `/experiment_info` or `/recording_info`: `admitted_frames`, `empty_frames`,
  `processed_frames`, `scientifically_rejected_frames`,
  `processing_failed_frames`, `store_overwritten_frames`,
  `store_not_committed_frames`, `store_malformed_frames`,
  `cancelled_by_policy_frames`, `pending_at_stop_frames`,
  `persistence_{admitted,committed,failed,pending_at_stop,cancelled_by_policy}_frames`,
  `objects_detected`, `has_index_range` / `first_frame_index` /
  `last_frame_index`, `sequence_gaps` / `sequence_gap_frames`,
  `session_generation`, `policy_allows_drops`, `fatal_error` /
  `fatal_message`, `completion_state` (`complete` | `intentionallyPartial` |
  `incompleteLoss` | `failed`), `completion_reason`, `reconciled`. The stored
  form is always the *reconciled* snapshot, so a file whose required
  accounting does not reconcile is stored as `failed`, never `complete`.
  `readRunAccounting` returns `false` (completion `unknown`) for files that
  predate the schema; legacy `total_recorded_frames` /
  `total_filtered_empty_frames` are never reinterpreted.
- **Acquisition time/telemetry provenance (issue #368,
  `timestamp_schema_version` = 1)** — `Hdf5Service::writeAcquisitionProvenance`
  stores `timestamp_clock_domain`, `timestamp_ticks_per_second`,
  `timestamp_native_ticks_per_second`, `timestamp_semantic`,
  `timestamp_validity`, `timestamp_counter_bits`, `timestamp_session_generation`,
  `timestamp_host_receipt_domain` (what the per-frame `hostTimestampUs` column
  means) and, per metric, `telemetry_<name>_value` / `_validity` /
  `_sample_host_time_us` for frames delivered, capture frame/data rate, SDK
  queue depth, input buffers, underruns, transport loss, intentional
  discards, frame age and publish latency. The per-frame `timestampNs`
  column name is historical: its unit/domain is whatever the descriptor says.
  Files without the schema read back as `unknown`/`unsupported`
  (`camera::common::legacyTimestampInterpretation()` documents what old
  values held per producer); nothing is rewritten.
- Chart snapshot datasets — 2D/3D `cv::Mat` saved via
  `saveChartSnapshot(path, image)`.

- **Run configuration snapshot (issue #369, `run_snapshot_schema_version`
  = 1)** — `Hdf5Service::writeRunSnapshotJson` stores the frozen
  `RunConfigurationSnapshot` (`run_snapshot_json`, stable key order: camera
  requested/effective/simulated/fallback, delivery mode, timestamp
  descriptor, ROI, frame geometry, processing core + pin, config version /
  sha, `config.json` sha, profile, pixel-to-micron, background
  generation/sha, trigger binding, output path, application identity) and
  the readiness evaluation (`readiness_json`, per-gate status/reason) as
  attributes on `/run_provenance`. Written at Start, before any frame;
  `readRunSnapshotJson` returns false (never a fabricated snapshot) for
  older files. See [[../architecture/ExperimentCoordinator]].

## Write paths

- **Batch save** (rarely used for experiments, useful for tests):
  `saveFrames(valid, invalid)`.
- **Incremental** (primary path):
  `initializeDatasets()` →
  `appendFrames(valid, invalid)` repeatedly →
  `writeExperimentInfo(...)` →
  optional `writeConfigJson(...)`.
  Called from `ProcessingService::flushBufferedFrames`.
  `writeExperimentInfo` receives the exact core identity used by the live or
  offline operation; `readProcessingCoreIdentity` returns it to review/tooling
  callers and reports absence for legacy files.
  Append steps flush on a time interval (`maybeIntervalFlush`); one-shot
  finalization writes (`writeExperimentInfo`, `writeConfigJson`) flush
  unconditionally.
- **Recording mode**: `initializeRecordingDatasets()` →
  `appendRecordingFrames(images, metadata)` →
  `writeRecordingInfo(..., processingCore)`. The recorder holds one operation
  lease from start through final metadata so the identity describes the exact
  core used for every empty-frame decision.
  Recording appends also flush on the same time interval — no per-append
  full-file copy. Finalization performs a global HDF5 flush and strong
  `H5Fclose`, so the superblock EOA is updated before `stopFrameRecording()`
  returns.
- **Interval flush**: append paths call `H5Fflush` at most once per
  `MIB_HDF5_FLUSH_INTERVAL_MS` (default 5000 ms) so the recorder thread stays
  off synchronous I/O on every batch. A crash loses at most one interval's
  worth of buffered frames; there is no recovery sidecar.

## Read paths (scalable)

- Metadata only: `readValidMetadata`, `readInvalidMetadata`. Use these to
  populate `HdfMetricsModel` without loading image bytes.
- Recording mode: `isRecordingFile()` probes `/recording_info`;
  `readRecordingMetadata` fills minimal `ProcessedFrame`s (index +
  timestampNs only); `readRecordingInfo` reads the core attributes
  (`start_time_ns`, `end_time_ns`, `total_recorded_frames`,
  `total_filtered_empty_frames`) plus optional multi-image attributes
  (`multi_image_enabled`, `multi_image_count`). [[../frontend/HdfReviewTab]]
  uses these to present recording files in a single-tab view and build
  recording-series windows in FrameViewer when multi-image was enabled.
- Single image (bounded memory): `readImageByIndex(datasetPath, idx, cv::Mat&)`.
- Small batch (e.g. thumbnails): `readImagesRange(datasetPath, start, count, vec)`.
- Dataset shape discovery: `getDatasetInfo(path, count, H, W, channels)`;
  `getSeriesImageInfo(count, seriesCount, H, W)`.

## Gotchas

- `writeConfigJson` **must** be called after `writeExperimentInfo` — see
  header docstring.
- There is no recovery sidecar: if the primary file fails to open,
  `Hdf5Service::loadFile` fails directly. A crash/power-loss mid-recording can
  lose up to one flush interval and may leave the primary `.h5` needing
  `h5clear --increment` (or unrecoverable) — the accepted tradeoff for
  real-time recorder throughput.
- A stale EOA/superblock that can be repaired with `h5clear --increment`
  points at an interrupted or failed final flush/close. Recorder logs include
  final flush status, close timing, and the HDF5 object count before close to
  make that diagnosable.
- Don't use `readValidFrames` (full load) on files > 1 GB; prefer the
  scalable hyperslab APIs. See task
  `knowledge_map/task/review_2gb_scalability.md`.
- 3D `(N,H,W)` and 4D `(N,H,W,C)` datasets both supported by
  `readImageByIndex` — channels auto-detected.
