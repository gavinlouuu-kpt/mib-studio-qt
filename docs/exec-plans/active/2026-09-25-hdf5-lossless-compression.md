# Epic: lossless HDF5 compression (live with raw fallback, finished post-run)

Status: active

ADR: [0006 — Lossless HDF5 compression](../../decisions/0006-hdf5-lossless-compression.md)

**Goal:** Recording and experiment files come out of a run gzip-compressed
(~1.6x smaller on 512x96 frames) without ever costing a frame. Image chunks
are compressed live on a worker pool. Any chunk that cannot be compressed in
time is stored raw in the same dataset. An idle-time finish pass later
compresses those raw chunks. Files stay standard HDF5 that any reader opens
without plugins.

**Architecture:**

```
recording/experiment thread ─► HdfWriteQueue (3 slots, unchanged) ─► HDF5 writer thread
                                                                        │
                           ChunkAssembler (fixed C frames/chunk) ◄──────┤
                                    │ full chunks                       │
                                    ▼                                   │
                           CompressionPool (N zlib workers,             │
                           below-normal priority, per-chunk deadline)   │
                                    │ compressed | late | failed        │
                                    ▼                                   │
                           H5Dwrite_chunk(mask 0 = deflate, 1 = raw) ◄──┘  (only this thread calls HDF5)

idle ─► CompactionScheduler ─► Hdf5Compactor: copy compressed chunks as-is, compress raw
        chunks, write temp → verify → atomic replace
```

**Tech stack:** C++17, HDF5 C API (≥ 1.10.5: `H5Dwrite_chunk`,
`H5Dread_chunk`, `H5Dget_chunk_info`), zlib `compress2`/`uncompress`, CTest
bare-`main()` tests, pybind11 wheel, cxx bridge + Tauri (additive only).

## Evidence (2026-09-25)

Real `512x96stream-mock-frames` frames (1,000, repeated to 4,000), 2.1 GHz
Xeon VM. PR 0 re-runs this on the rig.

| Codec | Ratio | 1 thread | 4 threads | Decompress 2.4 MB chunk |
|---|---|---|---|---|
| none | 1.00 | ~1900 MB/s | – | – |
| gzip-1 | 1.64 | 45–47 MB/s | 150 MB/s | ~26 ms |
| gzip-4 | 1.65 | 32 MB/s | 125 MB/s | ~25 ms |
| gzip-6 | 1.64 | 14 MB/s | 54 MB/s | ~26 ms |
| Blosc-LZ4 + bitshuffle (plugin, rejected) | 1.56 | ~590 MB/s | – | – |

- Demand: 49 MB/s at 1000 fps and ~246 MB/s at 5000 fps (512x96 u8,
  before empty-frame filtering).
- gzip-1 on one core (today's single writer thread) is just below 1000 fps.
  Two pool threads give ~1900 fps of capacity; bursts above that fall back to
  raw.

## Design

### D1. Codec

Deflate level 1, no shuffle (no gain on u8). The level is configurable from 1
to 9. No third-party filters.

### D2. Chunk geometry (fixed at dataset creation)

- 3D image and mask datasets: `C = clamp(floor(chunk_target / frameBytes),
  1, 64)` frames per chunk, with `chunk_target` defaulting to 512 KiB. At
  512x96 that gives C = 10 (480 KiB).
- 4D series datasets `(N, S, H, W)`: the same rule over `S·H·W` bytes.
- Recording batches become chunk-aligned: `FLUSH_BATCH = C · ceil(50 / C)`,
  which is 50 at 512x96.
- This replaces today's `min(100, first batch)` rule. It applies in every
  mode, including `off`.

### D3. Live write path (per append call, on the HDF5 writer thread)

1. `ChunkAssembler` (one per image dataset) copies frames into the open chunk
   buffer and emits full chunks.
2. Each full chunk goes to `CompressionPool` with a deadline of `now + budget`.
3. The writer extends the dataset extent, then for each chunk in index order
   waits until the chunk is done or its deadline passes:
   - Compressed and smaller than raw: `H5Dwrite_chunk(mask = 0)`.
   - Otherwise: raw bytes with `mask = 0x1` (deflate skipped).
   - Late results are discarded. The worker checks a per-task cancel flag.
4. **Partial tail:** frames in the not-yet-full chunk are written immediately
   as a padded raw chunk (`mask = 0x1`), so they are durable when the append
   returns, as they are today. When the chunk fills, it is rewritten.
   Spike S1 checks that HDF5 reuses the freed space; if not, fallback T2
   applies (below).
5. `append*()` returns only after every chunk and the tail are written.
   `persistenceCommitted` and the #367 accounting keep their exact meaning.

Fallback T2 (if S1 fails): carry the tail in memory and write it only at the
next full chunk, at the interval-flush deadline, or at stop. This means at most
C−1 frames per dataset are held, bounded by the flush interval.

### D4. Budget and raw-fallback policy

A chunk is written raw, and the reason counted, when any of these holds:

| Reason | Trigger |
|---|---|
| `pressure` | `HdfWriteQueue::depth() >= pressure_depth` (default 2) at append start. All chunks in that append go raw. |
| `budget` | Not compressed by its deadline. `budget = clamp(0.75 × EWMA(inter-append interval) − writeTime, 2 ms, 200 ms)`. |
| `pool_full` | In-flight tasks ≥ 2 × threads. |
| `incompressible` | Compressed size ≥ raw size. |
| `zlib_error` | `compress2` returned an error. Logged once per run and never fatal. |

The writer's per-append time therefore stays within about 75% of the arrival
interval, and the queue-overflow behaviour is exactly what it is with
compression off. Compression can make a file larger but never costs a frame.

### D5. Thread ownership

Only the HDF5 writer thread calls HDF5 for a live file. Pool workers see
plain byte buffers only. On datasets written with `H5Dwrite_chunk`, nothing
calls `H5Dwrite`, so no chunk is ever in the HDF5 chunk cache and the cache
and direct writes cannot conflict. The finish pass runs only while no run is
active.

### D6. Pool

`threads` defaults to `auto = clamp(hw_concurrency / 2, 1, 4)`: 2 on a
4-core host (PR 0 container e2e, 2026-09-26) and 4 on the rig PC (32 logical
CPUs; confirmed by the PR 0 rig e2e, 2026-09-26). Workers run at below-normal OS priority (Windows
`THREAD_PRIORITY_BELOW_NORMAL`, POSIX `nice +5` or `SCHED_BATCH`), so capture
and processing threads win under contention. The pool is created at run start
and joined at run end, never detached.

### D7. File attributes (`storage_schema_version` = 1, on `/`)

`storage_compression_mode`, `storage_compression_level`,
`storage_chunk_frames`, `storage_chunks_compressed`, `storage_chunks_raw`,
`storage_chunks_raw_{pressure,budget,pool_full,incompressible,zlib_error}`,
`storage_bytes_logical`, `storage_bytes_stored`, `storage_finished_at_ns`
(written by the finish pass).

- They are written with the other finalization metadata.
- A file without them is legacy uncompressed. Readers never require them.
- The requested compression settings also go into `RunConfigurationSnapshot`
  (#369) so the frozen run config records them.

### D8. Finish pass (`Hdf5Compactor` + `CompactionScheduler`)

- **Eligible when:** the file closed cleanly, `completion_state` is not
  `failed`, `storage_finished_at_ns` is absent, and the raw fraction
  (`chunks_raw / total`) is above `finish_raw_threshold_pct` (default 5).
  Legacy uncompressed files qualify only when queued explicitly (CLI or UI).
- **Algorithm:** create `<file>.h5.compacting` next to the original, then:
  - `H5Ocopy` all non-image objects and attributes.
  - Image datasets are recreated with D1/D2. Deflate-filtered chunks are
    copied as-is (`H5Dget_chunk_info` + `H5Dread_chunk` → `H5Dwrite_chunk`).
    Raw chunks are compressed on the pool.
  - Legacy unfiltered datasets are re-chunked from `H5Dread` hyperslabs.
  - Set `storage_*`, close, verify (dataset list, shapes, dtypes, attributes,
    image bytes chunk by chunk), fsync.
  - Replace the original. On Windows this is `MoveFileExW(REPLACE_EXISTING |
    WRITE_THROUGH)`; if the file is locked, the original is kept and the job
    retries once.
- **Scheduling:** runs only when idle. Run start cancels the job within one
  chunk and requeues it. The queue persists as `compaction-queue.json` in the
  app data directory. Stale temp files are deleted at startup.
  `AppBackend::shutdown()` cancels and joins the pass. A `RecordingLoad` of a
  file being compacted cancels and requeues that job.
- **HDF5 exclusivity (TD-17):** the Windows Conan HDF5 is not threadsafe
  (rig, 2026-09-26). "No run active" does not mean "no other HDF5 caller":
  HdfReviewTab and HdfExportService can have files open on their own threads
  while the pass runs. Before PR 4 lands, one of these must hold:
  - TD-17 is closed (a threadsafe build or one process-wide HDF5 lock); or
  - the pass holds the same HDF5 access gate as review and export, and
    starts or continues only while no other HDF5 user is active.

  A test runs the pass while a review read loop runs on another thread.
- **Space check:** free space must be at least `stored × 1.05 + raw bytes`,
  otherwise the job is skipped with a user-visible reason.
- **CLI:** `mib_h5_compact [--level N] [--dry-run] <file.h5>...` covers
  legacy archives and benchmarking.

### D9. Readers

`Hdf5Service` image read paths (`readImageByIndex`, `readImagesRange`, series
readers) open datasets with a dataset-access chunk cache
(`H5Pset_chunk_cache`: 32 MiB, 12421 slots). At C = 10 a random frame costs
one ~480 KiB inflate (about 5 ms); neighbouring frames are then cache hits.
Nothing else changes for HdfReviewTab, HdfExportService, BatchMaskSources or
the Python wheel.

### D10. Configuration

App config `storage.compression.*`:

| Key | Values | Default |
|---|---|---|
| `mode` | `off` \| `live` \| `live_and_finish` | `off` (flipped in PR 5) |
| `level` | 1–9 | 1 |
| `threads` | `auto` or 1–8 | `auto` |
| `chunk_target_kib` | 64–4096 | 512 |
| `pressure_depth` | 1–3 | 2 |
| `finish_raw_threshold_pct` | 0–100 | 5 |

- The env override `MIB_HDF5_COMPRESSION=off` is a field kill switch.
- The mode is sampled at run start and frozen for the run.

## Global constraints

- **No frame is ever lost to compression.** With any mode on, the overflow,
  failure and #367 accounting behaviour is identical to mode `off`. Compression
  outcomes are never fatal.
- **Lossless.** Every stored chunk (compressed or raw) inflates to the exact
  input bytes. Tests compare bytes, not images.
- **Standard HDF5.** Deflate filter only. Mixed compressed and raw chunks rely
  only on core HDF5 filter-mask semantics.
- **The original file is never modified in place** by the finish pass.
  Temp → verify → atomic replace.
- **One HDF5 thread per live file** (D5). No new cross-thread HDF5 use.
- **Default `off` until the PR 5 rig soak passes.** With mode `off`, the only
  behaviour change is the fixed chunk geometry (PR 1).
- Bridge changes are additive (ADR 0004). Qt-free public headers. Vault notes
  are updated in the same commit, and `python3 scripts/check_docs.py` passes.

## Acceptance criteria (epic)

- [ ] **Correctness:** recording and experiment files (images, masks, metadata,
      series) read back byte-identical with mode `live` vs `off`. This covers
      all-compressed, all-raw (forced), mixed and edge-chunk cases. #367
      accounting snapshots are identical.
- [ ] **Burst safety:** a mock camera at 5000 fps for 60 s and the rig at
      ~4500 fps for 60 s, both in mode `live`: zero queue overflows, run
      `complete`, raw fallback counted.
- [ ] **Rig steady state:** a 1000 fps, 5-min experiment with processing on in
      mode `live`: zero overflows; capture and processing drop counters within
      ±1 % of the `off` baseline; ≥ 95 % of chunks compressed; stored bytes
      ≤ 0.65 × logical.
- [ ] **Stop latency:** stop-to-closed grows by ≤ 100 ms vs `off`.
- [ ] **Crash:** after a kill -9 mid-run, every frame committed before the last
      interval flush reads back identical (after `h5clear` where needed, as
      today).
- [ ] **Finish pass:** it brings a burst file's raw fraction to 0, verified and
      atomically replaced. A kill mid-pass leaves the original intact and the
      temp is cleaned at the next start. It yields to a run start within one
      chunk. The soak file (~1.8 GB) finishes in < 60 s on the rig.
- [ ] **Readers:** h5py without `hdf5plugin`, HDFView, HdfReviewTab,
      HdfExportService, BatchMaskSources and the Python wheel read compressed
      and mixed files. `readImageByIndex` random access ≤ 10 ms/frame.

## Decision log

- 2026-09-25: Compress live with a per-chunk raw fallback, then finish
  post-run. This replaces the earlier post-run-only draft of this plan (same
  file, renamed). Rationale in ADR 0006.
- 2026-09-25: Deflate-1 (D1). Fixed ~512 KiB chunks (D2). A partial tail is
  written raw and rewritten when full, pending S1 (D3). A 75 % time budget
  plus queue-pressure fallback (D4).
- 2026-09-25: Early S1/S2 run on the cloud VM (h5py 3.16 / HDF5 2.0.0, 1,000
  real frames, C = 10, gzip-1 via `write_direct_chunk`):
  - Ratio at 10-frame chunks is 1.64x (49.15 → 29.96 MB), the same as with
    50-frame chunks.
  - Nine padded raw tail rewrites per chunk before the compressed write grow
    the file by only +0.6 % (S1 pass, so D3.4 stands).
  - A file with every third chunk raw reads back byte-identical in plain h5py
    (S2 pass).
  - PR 0 still repeats both on HDF5 1.14.6 (Conan) and 1.10.x (apt,
    manylinux), plus HDFView and MATLAB.
- 2026-09-26 (PR 0, cloud VM, 4 × 2.1 GHz Xeon, idle): these are the numbers
  PR 1 and PR 2 build on until the rig numbers replace them.
  - gzip-1 at C = 10 scales linearly with threads: 56 / 114 / 222 MB/s
    compress on 1 / 2 / 4 threads (≈ 1,140 / 2,320 / 4,510 fps at 512x96).
    End to end with `H5Dwrite_chunk` to local disk: 57 / 101 / 209 MB/s.
  - Inflating one C = 10 chunk takes 2.9 ms, against 15.5 ms at C = 50. D9's
    ≤ 10 ms random-access target holds at C = 10, which confirms D2.
  - Levels 4 and 6 again give no ratio gain (1.65 vs 1.64) at 1.6x and 3.5x
    the CPU, which confirms D1.
  - `recording.hdf5_direct_chunk_capability` (C++, the app's own
    `Hdf5Service` reader) passes on apt HDF5 1.10.10: deflate encode and
    decode are available, the library is threadsafe (`threadsafe=1`), mixed,
    raw and edge chunks read back byte-identical, and S1 growth is 0.00 %.
  - S1 and S2 via the benchmark script: HDF5 1.14.6 (h5py 3.14) +0.65 %, pass;
    HDF5 2.0.0 (h5py 3.16) +0.65 %, pass.
  - **S1 passes on 1.10.10, 1.14.6 and 2.0.0, so D3.4 stands and T2 is not
    needed.**
- 2026-09-26: **zlib can no longer be absent.**
  - `conanfile.py` requires `zlib/[>=1.2.11 <2]`, the hdf5 recipe's own
    range. `conan graph info` for `linux-gcc13` and `windows-msvc194-ninja`
    shows one host zlib, 1.3.2, the same package HDF5, OpenCV and Qt already
    use, so the rig's team remote needs no new binaries.
  - `find_package(ZLIB REQUIRED)` in `cmake/MIBDependencies.cmake` fails
    configure without zlib. This was checked with
    `CMAKE_DISABLE_FIND_PACKAGE_ZLIB`.
  - apt `zlib1g-dev` and the manylinux `zlib-devel` image package are
    declared.
  - The capability test is now always built, not skipped.
  - Still open (rig only): the Conan 1.14.6 capability line (threadsafe
    status), headroom under a live 1000 fps run (sets `threads`), and
    HDFView/MATLAB reads of `--emit-mixed`. The procedure is in
    [hdf5-compression-measurement](../../howto/hdf5-compression-measurement.md).
- 2026-09-26 (PR 0 step 3a, cloud container, 4 vCPU): full headroom e2e, run
  as `scripts/run_compression_headroom_e2e.py`. Each soak is 300 s at
  1000 fps through the production save path, with the app's default gates.
  Evidence: [container headroom](../../evidence/2026-09-26-compression-headroom-container/README.md).
  - **Every run ended `complete` with zero loss:** 8 soaks, about 2.4 M
    frames admitted.
  - **Measured demand is far below the 49 MB/s worst case:** experiment
    1.7 MB/s, recording 19.2 MB/s (39 % of frames non-empty).
  - **Stored-frame gzip-1 ratio:** 1.57 for experiment files, 1.63 for
    recordings.
  - **gzip-1 throughput during a live run:** 47 / 93 / 127 MB/s on 1 / 2 / 3
    threads. The smallest passing pool is **1** in both modes, and **2** also
    covers the worst case ×1.3 with capture within 0.26 %.
  - **3 threads on 4 vCPU harmed the run:** the mock camera lost 0.9–1.1 % of
    its frame rate (experiment mode failed the 1 % rule), and algorithm
    throughput dipped. No frame was lost.
  - **Decision:** the provisional D6 default becomes
    `auto = clamp(hw_concurrency / 2, 1, 4)`, which is 2 on a 4-core host.
    The rig run confirms or replaces it.
  - The 75 % budget (D4) is not stressed at these rates: one thread
    compresses a 50-frame batch in about 50 ms at 47 MB/s, while batches
    arrive every ~130 ms in recording mode.
  - **Side finding, TD-16:** experiment files store a full frame and mask per
    object record. That is about 1 record per frame with the default gates,
    but 5.56 with wide-open gates.
- 2026-09-26 (PR 0, rig): the rig PC is an Intel Core i9-13900 (8 P + 16 E
  cores, 32 logical) with 32 GB RAM. Recordings go to `D:`, a SATA HDD. The
  build is `windows-ninja` Release with Conan HDF5 1.14.6, at commit `3eb6e61`.
  Evidence: [rig headroom](../../evidence/2026-09-26-compression-headroom-rig/README.md).
  - **Capability:** the test passes.
    `HDF5_CAPABILITY version=1.14.6 threadsafe=0 deflate_encode=1 deflate_decode=1 zlib=1.3.1`
    and `HDF5_S1 ... growth_pct=0.00`. The Conan build is **not
    threadsafe**, so TD-17 is filed. D5 still holds: the epic does not widen
    the sharing.
  - **Idle (step 2):** gzip-1 at C = 10 runs at 84 / 127 / 256 MB/s on
    1 / 2 / 4 threads, ratio 1.64. One thread varies from 56 to 84 between
    attempts (P-core vs E-core). `H5Dwrite_chunk` to the `D:` HDD runs at
    63 / 129 / 242 MB/s. Inflating a chunk takes 2.0–2.2 ms at C = 10 and
    11 ms at C = 50. S1 +0.65 % and S2 pass (h5py 3.16 / HDF5 2.0.0).
  - **Headroom (step 3a):** 12 soaks of 300 s at 1000 fps, all `complete`
    with zero loss.
    - Demand: experiment 1.7 MB/s (stored-frame ratio 1.57), recording
      19.3 MB/s (1.63). This is the same as the container.
    - gzip-1 during the run: 35 / 68 / 99 / 129 MB/s on 1 / 2 / 3 / 4
      threads. **Every T passes in both modes**, with capture Δ ≤ 0.04 %.
    - Per-thread throughput under load (about 33 MB/s) is below the
      container's 47, because the soak keeps 16–18 of 32 logical CPUs busy.
    - Algo fps minimum dips from about 347 to 318–326 at T ≥ 3; the mean is
      unchanged.
  - **Readers (step 4):** HDFView and MATLAB are not installed on the rig, so
    neither is checked. h5py reads the mixed file (34/100 chunks raw).
    Step 3b (real camera) was skipped, because nobody was at the rig.
  - **Decision: D6 confirmed.** `auto = clamp(hw_concurrency / 2, 1, 4)`
    gives 4 on the rig, which passes at 2.6× the 49 MB/s worst case. 2
    threads (68 MB/s) would just cover the worst case ×1.3.
  - **Side finding:** the headless soak uses about 16–18 cores on a 32-thread
    host, against about 2 on 4 vCPU, at the same algo rate. This needs a look
    before the PR 5 rig soak, because it is the load the compression pool
    competes with.
  - Build note: an old `build-ninja` Conan output configures but fails with
    `C1083 'zlib.h'` until `conan install` is re-run. The rig resolved zlib
    1.3.1, not 1.3.2, from its cache (the package HDF5 links).
- 2026-09-26 (PR 0, rig follow-up): closing the gaps from the rig run.
  - **The HDD recording volume passes.** Recording mode with
    `--work-dir D:\bench\e2e`: baseline complete, 5.80 GB, zero loss;
    T = 4 at 135 MB/s, capture Δ 0.04 %, PASS.
  - **NTFS compression breaks recording (TD-19).** The rig's `D:\` root has
    NTFS "compress contents" set, and new top-level folders inherit it. A
    1000 fps recording into such a folder fails within about 13 s with
    `write queue overflow (disk too slow)`, while the disk is at most 35 %
    busy.
    - This is not an HDF5 compression issue: the files were uncompressed
      HDF5.
    - `D:\data` is unaffected.
    - Both measurement scripts now detect and warn about it.
  - **The soak's CPU is OpenCV's Concurrency Runtime pool (TD-18).** The Conan
    OpenCV 4.12.0 imports `CONCRT140.dll`: about 32 threads at 45–50 %, about
    41 ms of CPU per processed frame against 4.4 ms in the container.
    `OPENCV_FOR_THREADS_NUM` has no effect. The fix needs
    `cv::setNumThreads` in product code, so it is out of PR 0's scope. It
    is not needed for D6, which already passes at 4 threads.
  - **Readers:** `h5dump` 1.14.6 reads the mixed file byte-identically. That
    is a second standard reader besides h5py. HDFView and MATLAB are still
    not installed on the rig.
  - **D8 is amended:** the finish pass is a new HDF5 thread next to review
    and export. With `threadsafe=0` (TD-17), PR 4 needs process-wide HDF5
    exclusivity or a threadsafe build.
  - **Scripts:** the e2e's per-T PASS now requires `completion=complete`,
    and the benchmark creates `--write-dir`.

## Phases (one PR each)

### PR 0: measure and de-risk (no product behaviour change)

- [x] Check in `scripts/bench_hdf5_compression.py` (codec × level × threads ×
  chunk frames; ratio, compress/decompress MB/s, `--write-dir`, `--spikes`,
  `--emit-mixed`). [x] Run it on the rig PC (2026-09-26).
- [x] `recording.hdf5_direct_chunk_capability` (C++ guard, runs on every
  ctest lane, including Windows): prints `HDF5_CAPABILITY` (version,
  threadsafe, deflate) and `HDF5_S1`, and asserts mixed-chunk byte identity
  through `Hdf5Service::readImageByIndex`.
- [x] Headroom under a live 1000 fps run, scripted
  (`run_compression_headroom_e2e.py`, `mock_experiment_soak_run`), done in
  the cloud container. [x] Repeat on the rig, 3a (scripted): done
  2026-09-26, every T from 1 to 4 passes and `auto` stays
  `clamp(hw/2, 1, 4)`. [ ] 3b uses the real camera; it is optional and was
  skipped because nobody was at the rig. Sets `threads = auto` and validates
  the 75 % budget.
- [x] **S1** (h5py prototype and the C++ guard above): a
  partial chunk written raw and rewritten 10×, then compressed. Final file size
  must be within 5 % of an h5repack of the same data. Pass → D3.4; fail → T2.
- [x] **S2** in h5py (no plugin). [ ] HDFView and MATLAB on the rig
  (`--emit-mixed`): neither is installed on the rig PC (2026-09-26), so this
  needs a machine that has them.
- [x] apt 1.10.10: threadsafe. [x] Conan 1.14.6 (rig, 2026-09-26):
  `threadsafe=0`, so TD-17 is filed. [ ] manylinux 1.10.5 (the wheel does not write
  live files, so this is informational). Record `H5is_library_threadsafe()`
  for each build. Add a tech-debt row if review/export threads share a
  non-threadsafe library with the writer.
- Exit: decision-log entries with the rig numbers, S1/S2 outcomes and the
  default thread count.

### PR 1: fixed chunk geometry + chunk-aligned batches + reader cache (still uncompressed)

- `Hdf5Service`: D2 chunk rule for `writeImageDataset`,
  `writeSeriesImageDataset` and the recording path; `chunkFrames()` accessor.
- `AppBackend` recording: `FLUSH_BATCH` derived from C.
- D9 chunk cache on read paths, with a perf test of 1,000 random reads.
- Tests: chunk shape independent of first-batch size (1, 7, 50, 120 frames);
  existing recording and experiment tests unchanged.
- Vault: [HDF5-Storage](../../../knowledge_map/data-model/HDF5-Storage.md),
  [Hdf5Service](../../../knowledge_map/services/Hdf5Service.md), task note,
  Recent-Work.

### PR 2: compressing chunk writer (core, behind `mode`, default `off`)

- New `include/backend/recording/ChunkWriter.h` (`ChunkAssembler`,
  `CompressionPool`, `ChunkBudget`, `ChunkWriteStats`) in `mib_processing`.
  Link `ZLIB::ZLIB` to `mib_processing`. zlib itself is already guaranteed
  since 2026-09-26: a direct Conan requirement, apt `zlib1g-dev`, manylinux
  `zlib-devel`, and `find_package(ZLIB REQUIRED)`. See
  [Build](../../../knowledge_map/build-and-run/Build.md) and
  [Dependencies](../../../knowledge_map/build-and-run/Dependencies.md).
- `HdfWriteQueue::depth()` (lock-protected read) for D4 pressure.
- `Hdf5Service` image append paths route through `ChunkWriter` when the
  mode is not `off`. Datasets are created with `H5Pset_deflate`, and
  `H5Dwrite_chunk` handles full and tail chunks.
- Tests:
  - Byte-exact round trip: compressed, forced-raw (budget 0), mixed, edge
    chunk, C = 1, series.
  - Each raw reason is injected: pressure, budget, pool full, incompressible
    (random bytes), zlib error (via a seam).
  - Accounting unchanged vs `off`.
  - A deterministic "slow codec" seam proves no overflow under a slow pool.
  - Kill mid-run readback.
  - The S1 space test.
- Vault: new `knowledge_map/services/ChunkWriter.md` (+ `_MOC.md`, README,
  Agent-Onboarding), [Threading-Model](../../../knowledge_map/architecture/Threading-Model.md),
  HDF5 notes, task note, Recent-Work.

### PR 3: integration and telemetry

- Config keys (D10) + env kill switch; mode frozen at run start (recording
  thread and `ExperimentCoordinator`); settings recorded in the run snapshot.
- D7 `storage_*` attributes at finalization, plus the
  `Hdf5Service::readStorageInfo()` reader.
- Live stats (chunks compressed/raw, ratio) exposed through `AppBackend` and
  `BackendFacade` status. Bridge contract and ABI bump, cxx and Rust contract
  test, generated TS.
- Qt: settings controls and a status-bar readout ("gzip · 1.63x · 98 %
  compressed"). HdfReviewTab file info shows `storage_*`.
- Vault: [AppBackend](../../../knowledge_map/architecture/AppBackend.md),
  [ExperimentCoordinator](../../../knowledge_map/architecture/ExperimentCoordinator.md),
  facade/bridge notes, HdfReviewTab, Recent-Work.

### PR 4: finish pass

- `Hdf5Compactor` (D8) + `CompactionScheduler` owned by `AppBackend`, run
  only in mode `live_and_finish`, plus the `mib_h5_compact` CLI (legacy files
  too).
- **Precondition (TD-17):** HDF5 exclusivity per D8. Either TD-17 is closed
  first, or the pass shares one HDF5 access gate with HdfReviewTab and
  HdfExportService.
- `BackendOperationKind::Compaction` (append-only) with Started / Progress /
  Completed / Failed / Cancelled.
- Tests: chunk copy is byte-preserving; raw-only compression; legacy re-chunk;
  verify-failure keeps the original; cancel on run start; kill mid-pass;
  insufficient space; Windows locked-target retry; queue persistence across
  restart; the pass running against a concurrent review read loop.
- Vault: new `knowledge_map/services/Hdf5Compactor.md` (+ MOC links),
  AppBackend shutdown order, Recent-Work.

### PR 5: UI, docs, rig soak, default flip

- Qt and Tauri: finish-pass progress, a "Compress existing files…" action,
  and a completion toast with space saved.
- User manual section and screenshot tour (`python3 scripts/check_screenshots.py`).
- Rig soak against every acceptance criterion; results recorded here.
- Only if every criterion passes: default `mode = live_and_finish`, the ADR
  status note, and this plan moved to `completed/`.

## Risks

- **CPU contention with processing (YOLO, contour metrics).** Mitigated by
  below-normal priority, the budget/pressure fallback and PR 0 sizing. Worst
  case is larger files, never lost frames.
- **Free space fragmented by tail rewrites.** De-risked by S1, with T2 as the
  fallback. The finish pass rewrites compactly regardless.
- **zlib in `mib_processing` changes the wheel and plugin build.** The
  availability part is closed: zlib is a declared, configure-checked dependency
  in every environment (2026-09-26). PR 2 still checks the signed-core ABI.
- **Existing concurrent HDF5 use by review and export threads.** The live
  path does not widen it (D5). The Windows Conan build is `threadsafe=0`
  (PR 0 rig, TD-17). The **finish pass does widen it**: it is a new HDF5
  caller on its own thread, and it runs while review or export may be
  active. PR 4 is therefore gated on D8's HDF5-exclusivity rule.
- **Windows AV or indexers locking the file during replace.** The soft retry
  keeps the original. Observe it in the PR 5 soak.
- **Readers on very old HDF5 (< 1.8).** Filter masks are core since 1.8, so
  this is accepted.

## Progress

- [x] 2026-09-25: Impact analysis, codec benchmark, ADR 0006, this epic spec,
      early S1/S2 on HDF5 2.0.0 (pass).
- [ ] PR 0: rig measurements, S1/S2 spikes, threadsafe audit
  - [x] 2026-09-26: benchmark script, C++ capability guard, measurement howto,
        VM numbers, and S1/S2 on HDF5 1.10.10, 1.14.6 and 2.0.0
  - [x] 2026-09-26: headroom e2e in the container. 1 thread passes, 2 cover
        the worst case, and 3 harm a 4-vCPU host. Zero loss in 8 soaks.
  - [x] 2026-09-26, rig: Conan capability line (1.14.6, `threadsafe=0`,
        TD-17), idle benchmark, and headroom under load. Every T from 1 to 4
        passes with zero loss, and D6 `clamp(hw/2, 1, 4)` (4 on the rig) is
        confirmed. See the
        [rig handover](2026-09-26-hdf5-compression-rig-handover.md) §8.
  - [ ] rig, not done: HDFView/MATLAB are not installed on the rig, and the
        optional 3b real-camera check was skipped because nobody was at the rig
- [ ] PR 1: fixed chunk geometry, chunk-aligned batches, reader chunk cache
- [ ] PR 2: compressing chunk writer + raw fallback (default off)
- [ ] PR 3: config, run snapshot, storage attributes, telemetry, bridge, Qt status
- [ ] PR 4: finish pass (compactor, scheduler, CLI)
- [ ] PR 5: UI, manual, rig soak, default flip
