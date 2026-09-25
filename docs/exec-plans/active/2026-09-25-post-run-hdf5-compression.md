# Post-run lossless HDF5 compression

Status: active

**Goal:** After a recording or experiment file is closed cleanly, the app can
rewrite its image datasets with lossless gzip (deflate) compression in the
background, verify the result byte-for-byte, and atomically replace the
original. The live save path is unchanged, so compression can never cause a
`write queue overflow (disk too slow)` abort. Compressed files open in every
standard HDF5 reader (HDFView, h5py, MATLAB) without plugins. The feature is
opt-in and off by default.

**Architecture:** A Qt-free `backend::recording::Hdf5Compactor` (single file →
compacted temp file → verify → atomic replace) driven by a
`CompactionScheduler` owned by `AppBackend`. The scheduler queues closed files
and runs one job at a time, only while no recording or experiment is active.
It surfaces progress through `BackendFacade` operation events
(`BackendOperationKind::Compaction`, additive). Qt and Tauri shells show status
only.

## Evidence (2026-09-25)

Measured on 1,000 real frames from `512x96stream-mock-frames` (repeated to
4,000), 50-frame chunks, 2.1 GHz Xeon cloud VM. Scripts were run ad hoc; PR 1
checks in a reproducible version (see Progress).

| Codec | Ratio | 1 thread | 4 threads (zlib, direct) | Decompress 50-frame chunk |
|---|---|---|---|---|
| none | 1.00 | ~1900 MB/s | – | – |
| gzip-1 | 1.64 | 45–47 MB/s | 150 MB/s | ~26 ms |
| gzip-4 | 1.65 | 32 MB/s | 125 MB/s | ~25 ms |
| gzip-6 | 1.64 | 14 MB/s | 54 MB/s | ~26 ms |

- Live data rate is 49 MB/s at 1000 fps (512x96 u8) and ~246 MB/s at
  5000 fps. Inline gzip on the single writer thread cannot keep up, which is
  why compression happens after the run.
- The frames have a bright, noisy background (mean 147, std 36), so the lossless
  ratio stays about 1.6x (~39 % smaller) whatever the level. Levels above 1
  cost time and gain nothing.
- Sizing: the 2026-09-08 soak run (5 min at 1000 fps, 36,592 frames, ~1.8 GB)
  compacts in ~12 s of CPU with 4 threads at gzip-1. A worst case with no empty
  frames filtered (300k frames, ~14.7 GB) is disk-bound at a few minutes.

## Global constraints

- **The live path is untouched.** No filter is set on any dataset created
  during a run, and nothing in this plan changes `HdfWriteQueue`, batch sizes or
  the interval flush.
- **Lossless and verified.** The original is replaced only after every image
  dataset reads back byte-identical and all attributes and non-image datasets
  match. Any mismatch deletes the temp file and keeps the original.
- **Crash-safe.** Output goes to `<file>.h5.compacting` in the same directory.
  The swap is a single same-volume rename. A crash or kill at any point leaves
  the original readable, plus at most a stale temp file that is removed on the
  next start.
- **No HDF5 work concurrent with a run.** A job starts only when no recording
  or experiment is active. Starting a run cancels the job cooperatively
  (checked between chunks) and requeues the file. See D6.
- **Standard readers only.** Deflate filter (`H5Z_FILTER_DEFLATE`), no shuffle
  (no benefit for u8), no third-party filter plugins.
- **Opt-in.** Default off. Enabling it never changes files already on disk
  unless the user queues them explicitly (PR 4).
- Bridge contract changes are additive (ADR 0004). The facade enum is
  append-only and ABI parity tests stay green.
- Vault notes update in the same commit as the code they describe;
  `python3 scripts/check_docs.py` passes.

## Acceptance criteria

- [ ] With compaction on, a clean recording and a clean experiment file each
      end up ~1.6x smaller, and every frame is byte-identical to the uncompressed
      run (checked by test on `512x96stream-mock-frames` and, where `HF_TOKEN`
      is available, the `z-adjustment-50v` corpus).
- [ ] Compacted files open and read in h5py without `hdf5plugin`, and in
      HdfReviewTab, HdfExportService and BatchMaskSources unchanged.
- [ ] Killing the process mid-job leaves the original file intact and
      loadable. The next start deletes the stale `.compacting` file and requeues
      the job.
- [ ] Starting a recording or experiment during a job cancels it within one
      chunk (≤ ~100 ms at 8-frame chunks). The new run shows no queue overflow
      in the rig soak (1000 fps, 5 min, compaction on).
- [ ] Files that did not close cleanly (final flush or close failed, or
      `completion_state == failed`) are never compacted.
- [ ] Insufficient free space (< 0.8x source size on the volume) skips the job
      with a logged, user-visible reason.
- [ ] Random-access review (`readImageByIndex`) on a compacted file stays under
      10 ms per frame once the chunk is cached (D5).
- [ ] The 2026-09-08 soak file (~1.8 GB) compacts in under 60 s on the rig PC.

## Decision log

- 2026-09-25: **Compress after the run, not inline.** gzip-1 on one core
  (~47 MB/s) is already short of 1000 fps (49 MB/s), and the 3-slot
  `HdfWriteQueue` turns any shortfall into a fatal abort within seconds.
  Post-run compaction takes compression cost off the live save path entirely.
- 2026-09-25: **D1 codec = deflate level 1.** Built into every HDF5 build and
  reader, so no plugins need shipping. Levels 4 and 6 give the same ratio at
  1.4–3.4x the CPU. Level stays configurable (1–9) for users who want it.
  Blosc/LZ4/Zstd were rejected: they are faster but need a plugin everywhere
  the file goes.
- 2026-09-25: **D2 parallel compression via direct chunk write.** Worker
  threads compress chunks with zlib `compress2` (the exact stream the deflate
  filter stores). One HDF5 thread writes them with `H5Dwrite_chunk` and the
  filter mask set to 0. Only that thread calls HDF5. PR 1 ships the simple
  single-threaded filter path first; PR 2 swaps in the parallel path behind the
  same interface and must produce identical output.
- 2026-09-25: **D3 scope = image datasets only.** `/recorded_frames/images`,
  `/valid_frames/*` and `/invalid_frames/*` image and mask datasets, and
  `/series_images`. Everything else (metadata compounds, attributes, groups,
  chart snapshots, `/run_provenance`) is copied with `H5Ocopy` unchanged.
- 2026-09-25: **D4 fixed output chunk = 8 frames** (~393 KB at 512x96),
  independent of the first-batch-dependent live chunking
  (`min(100, first batch)`). Series datasets use 1 series per chunk. PR 1
  re-measures the ratio at 8 frames; if it drops by more than 0.05, revisit.
- 2026-09-25: **D5 readers get a chunk cache.** Hdf5Service read paths open
  image datasets with a dataset-access `H5Pset_chunk_cache`
  (`rdcc_nbytes` = 32 MiB, `rdcc_nslots` = 12421), so scrubbing does not
  decompress the same chunk once per frame. This also helps uncompressed files.
- 2026-09-25: **D6 idle-only scheduling instead of a threadsafe HDF5 build.**
  A `--enable-threadsafe` build takes a global lock, so a compactor would still
  stall the live writer's `H5Dwrite`. The scheduler runs only while idle and
  yields on `startFrameRecording` / `ExperimentCoordinator::start`.
  PR 1 records `H5is_library_threadsafe()` for the Conan (Windows), apt (Linux)
  and manylinux builds in the task note. Concurrent HDF5 use by review/export
  threads already exists and is tracked separately (see Risks).
- 2026-09-25: **D7 marker attributes on `/`**:
  `storage_compaction_schema_version` = 1, `storage_compression` =
  `"deflate-<level>"`, `storage_original_bytes`, `storage_compacted_bytes`,
  `storage_compacted_at_ns`. They are written into the temp file before
  verification, so a replaced file always carries them and a file without them
  is uncompressed. The scheduler skips files that already carry the marker.
- 2026-09-25: **D8 the swap stays in place.** The compacted file replaces the
  original path, so saved paths, SQLite rows and review history stay valid. On
  Windows the replace uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING |
  MOVEFILE_WRITE_THROUGH)`. If the target is open elsewhere (sharing
  violation), the job fails softly, the original stays, and the job retries
  once after a short delay.
- 2026-09-25: **D9 the pending queue persists** as `compaction-queue.json` in
  the app data directory, so a job cancelled by shutdown or a new run resumes
  on the next idle period or app start.

## Phases (one PR each)

### PR 1: `Hdf5Compactor` core (backend, Qt-free) + CLI

- `include/backend/recording/Hdf5Compactor.h`,
  `src/backend/recording/Hdf5Compactor.cpp`: API
  `CompactionResult compact(const CompactionRequest&, const CancelToken&,
  ProgressFn)`. The request holds path, level, chunk frames and free-space
  factor. The result holds the outcome enum (`Compacted`, `AlreadyCompacted`,
  `NotEligible`, `InsufficientSpace`, `Cancelled`, `VerifyFailed`,
  `ReplaceFailed`, `IoError`), bytes before and after, and a message.
- Steps: eligibility (opens read-only; no marker; completion state not
  `failed`) → space check → create temp → `H5Ocopy` non-image objects → image
  datasets recreated with deflate + D4 chunking, copied chunk-by-chunk →
  root attributes and marker → close → verify (reopen both, compare dataset
  lists, shapes, dtypes, attributes, and image bytes chunk by chunk) → fsync →
  replace → log `hdf5.compact` via `tracePerformance`.
- Stale `*.h5.compacting` cleanup helper (called by the scheduler at startup).
- `mib_h5_compact` CLI (`tools/`) for existing files and rig benchmarking:
  `mib_h5_compact [--level N] [--dry-run] <file.h5>...`.
- `scripts/bench_hdf5_compression.py`: the codec and thread benchmark above,
  checked in so the numbers can be re-run on the rig.
- Tests (CTest, `tests/recording/`): round-trip byte equality for recording
  files, experiment files and multi-image series files; attribute and
  provenance preservation (`readRunAccounting`, `readRunSnapshotJson`,
  `readProcessingCoreIdentity` identical before and after); marker idempotency;
  cancellation leaves the original and no temp; simulated verify failure keeps
  the original; not eligible when `completion_state == failed`; insufficient
  space (injected free-space provider).
- Vault: new `knowledge_map/services/Hdf5Compactor.md` (+ `_MOC.md`, README,
  Agent-Onboarding links); [HDF5-Storage](../../../knowledge_map/data-model/HDF5-Storage.md) gains the marker attributes and
  compacted-file layout; task note; Recent-Work entry.

### PR 2: parallel compression (D2) + reader chunk cache (D5)

- Compress on a bounded worker pool (`min(4, hardware_concurrency - 1)`)
  feeding the single HDF5 writer thread with `H5Dwrite_chunk`. Output must be
  byte-identical to the PR 1 filter path, checked by a test that decompresses
  both and compares the stored chunk bytes.
- Link zlib directly (already a transitive Conan/apt dependency of HDF5):
  `conanfile.py` / `env/apt-packages.txt` updates if needed, plus the
  [Build](../../../knowledge_map/build-and-run/Build.md) and [Dependencies](../../../knowledge_map/build-and-run/Dependencies.md) notes.
- Dataset-access chunk cache in `readImageByIndex`, `readImagesRange` and the
  series readers. Perf test: 1,000 random-index reads on a compacted file.
- Meets the "1.8 GB in < 60 s on the rig" criterion; results go in the task note.

### PR 3: scheduling and integration (AppBackend + facade)

- `CompactionScheduler` owned by `AppBackend`. It enqueues after
  `AppBackend::startFrameRecording`'s thread closes the file and after
  `ExperimentCoordinator` finalization (both only when the final flush and
  close succeeded). It persists the queue (D9), cleans stale temps and resumes
  at startup, and yields to runs (D6).
- `AppBackend::shutdown()`: cancel the job (≤ one chunk), join the worker, and
  keep the queue file. The order is after recording and experiment stop and
  before the HDF5 atexit concerns in [Hdf5Service](../../../knowledge_map/services/Hdf5Service.md).
- `BackendFacade`: `BackendOperationKind::Compaction` (append-only) with
  Started / Progress / Completed / Failed / Cancelled events. A `RecordingLoad`
  of a path being compacted cancels and requeues that job before loading.
- Config: `storage.post_run_compression.enabled` (default false) and
  `.level` (default 1) in the app config, plus the ConnectTab/settings plumbing
  (`AppConfigWatcher`, settings dialog).
- Bridge: ABI bump, contract JSON, cxx and Rust contract test, and generated
  TS (ADR 0004).
- Vault: [AppBackend](../../../knowledge_map/architecture/AppBackend.md), [Threading-Model](../../../knowledge_map/architecture/Threading-Model.md), [ExperimentCoordinator](../../../knowledge_map/architecture/ExperimentCoordinator.md),
  facade notes, Recent-Work.

### PR 4: UI

- Qt: a settings checkbox and level, and a status-bar indicator ("Compressing
  run_0042.h5 — 63 %", then "saved 1.1 GB"). HdfReviewTab shows the
  `storage_compression` attribute in file info. A "Compress existing files…"
  action queues selected files.
- Tauri: the same status from the operation events.
- Manual: user-manual section + screenshot tour update
  (`python3 scripts/check_screenshots.py`).

## Risks

- **Existing concurrent HDF5 use.** HdfExportService (thread pool),
  HdfReviewTab and the live writer can already call a non-threadsafe HDF5 from
  different threads. This plan adds no new overlap (D6), but PR 1 checks the
  threadsafe status of each build and files a tech-debt row if any is
  non-threadsafe.
- **Antivirus or indexers holding the file on Windows** can block the replace.
  This is handled by the soft retry (D8). Observe it on the rig.
- **Disk usage peaks at ~1.6x the source during a job.** Handled by the space
  check.
- **Rig CPU differs from the benchmark VM.** Re-run the PR 1 benchmark script
  on the rig before choosing the default thread count.

## Progress

- [x] 2026-09-25: Impact analysis and codec benchmark; decision to compress
      after the run (this plan).
- [ ] PR 1: `Hdf5Compactor` core + CLI + benchmark script + tests
- [ ] PR 2: parallel direct-chunk compression + reader chunk cache
- [ ] PR 3: scheduler, AppBackend/facade/bridge integration, config
- [ ] PR 4: Qt/Tauri UI, manual, screenshots
- [ ] Rig soak with compaction on (1000 fps, 5 min); numbers recorded here
