# Compression headroom e2e, rig PC (ADR 0006, PR 0 steps 1–4)

Captured 2026-09-26 on the Windows rig PC, branch
`claude/hdf5-compression-impact-bc7zvq` at `3eb6e61`, `windows-ninja` Release
build (Conan HDF5 1.14.6). The app was not running.

| Rig fact | Value |
|---|---|
| CPU | 13th Gen Intel Core i9-13900 (8 P-cores + 16 E-cores): 24 cores, 32 logical processors |
| RAM | 32 GB (31.7 GiB usable) |
| Recording volume | `D:` ("Data"), WD20EZBX 2 TB SATA **HDD**. The app's recent recordings are in `D:\data`. |
| System volume | `C:`, Samsung MZVL21T0HDLU 1 TB NVMe SSD |
| OS / Python | Windows 11 10.0.26200; Python 3.13.11 (miniconda; the `py` launcher is not installed), h5py 3.16.0 / HDF5 2.0.0, zlib 1.3.1 |

The commands were:

```powershell
ctest --test-dir build-ninja -R recording.hdf5_direct_chunk_capability -V
python scripts\bench_hdf5_compression.py --threads 1,2,4 --chunk-frames 10,50 --write-dir D:\bench --spikes --json rig-idle.json
python scripts\run_compression_headroom_e2e.py --runner build-ninja\Release\mib_backend_tests.exe --duration 300 --threads 1,2,3 --json rig-headroom.json
python scripts\run_compression_headroom_e2e.py --runner build-ninja\Release\mib_backend_tests.exe --duration 300 --threads 4 --json rig-headroom-t4.json
python scripts\bench_hdf5_compression.py --levels 1 --threads 1 --chunk-frames 10 --repeat 1 --emit-mixed mixed.h5
```

- **Soaks:** `mib_backend_tests mock_experiment_soak_run`, with the mock camera
  at 1000 fps on the 1,000 `512x96stream-mock-frames` TIFFs (looped), full
  frame ROI, the app's default `ProcessingConfig` (`--gates default`) and a
  median background. Soak files went to the script's default work dir
  (`build\compression-e2e` on the `C:` NVMe), not to the `D:` HDD.
- **Load:** `bench_hdf5_compression.py --seconds 308` with gzip-1 on 10-frame
  chunks, T threads, at `BELOW_NORMAL_PRIORITY_CLASS`.
- **Raw numbers:** `rig-idle.json` (step 2), `rig-headroom.json` (step 3a,
  T = 1, 2, 3) and `rig-headroom-t4.json` (step 3a, T = 4, with its own
  baselines). Local paths are replaced by repo-relative paths and
  `<work-dir>`.

## Step 1: Conan HDF5 capability

```
HDF5_CAPABILITY version=1.14.6 threadsafe=0 deflate_encode=1 deflate_decode=1 zlib=1.3.1
HDF5_S1 raw_bytes=19660800 direct_bytes=15200776 tail_bytes=15200776 growth_pct=0.00 ratio=1.29
```

The test passes: mixed, raw and edge chunks read back byte-identical through
`Hdf5Service`, and S1 growth is 0.00 %. The library is **not threadsafe**, so
TD-17 is filed in the [tech-debt tracker](../../exec-plans/tech-debt-tracker.md).

## Step 2: idle benchmark (recording drive `D:`)

gzip-1, C = 10, ratio 1.64. The process runs at below-normal priority.

| Threads | Compress MB/s | fps capacity | Write to `D:` MB/s (`H5Dwrite_chunk`) | Inflate ms/chunk |
|---|---|---|---|---|
| 1 | 84 | 1,715 | 63 | 2.2 |
| 2 | 127 | 2,581 | 129 | 2.1 |
| 4 | 256 | 5,215 | 242 | 2.0 |

- An earlier attempt of the same command, aborted because `D:\bench` did not
  exist yet, measured 56 / 126 / 256 MB/s. So one thread varies between 56 and
  84 MB/s, most likely depending on whether Windows puts it on a P-core or an
  E-core. From 2 threads on, the results repeat.
- C = 50: 58 / 128 / 253 MB/s, inflate 11 ms per chunk. Level 4 is 35 / 78 /
  151 MB/s and level 6 is 14 / 30 / 60 MB/s, both at ratio 1.65. This confirms
  D1 (level 1) and D2 (C = 10).
- **S1** (h5py / HDF5 2.0.0): tail rewrites grow the file by 0.65 %, PASS.
  **S2**: mixed chunks read back byte-identical, PASS.

## Step 3a: baselines (no compression load)

| Mode | Admitted | Empty | Stored | Wrote | Stored-frame gzip-1 ratio | Completion | Loss | Stop | Soak CPU |
|---|---|---|---|---|---|---|---|---|---|
| experiment | 300,694 | 182,815 | 5,095 records (3,610 processed + sampled invalid) | 1.7 MB/s (504 MB) | 1.57 | complete | 0 | 32 ms | 16.3 cores |
| recording | 300,604 | 182,777 | 117,827 frames | 19.3 MB/s (5.80 GB) | 1.63 | complete | 0 | 15 ms | 17.6 cores |

The T = 4 run re-measured both baselines and got the same results: 1.68 and
19.28 MB/s, complete, zero loss.

## Step 3a: with a T-thread gzip-1 load for the whole run

| Mode | T | gzip MB/s during run | Needed (1.3 × wrote) | Covers worst case ×1.3 (64 MB/s)? | Capture fps Δ | Extra loss | Algo fps min | Completion | Result |
|---|---|---|---|---|---|---|---|---|---|
| experiment | 1 | 34.9 | 2.2 | no | 0.00 % | 0 | 345 | complete | PASS |
| experiment | 2 | 68.1 | 2.2 | yes | 0.01 % | 0 | 348 | complete | PASS |
| experiment | 3 | 99.4 | 2.2 | yes | 0.01 % | 0 | 326 | complete | PASS |
| experiment | 4 | 129.3 | 2.2 | yes | 0.02 % | 0 | 324 | complete | PASS |
| recording | 1 | 34.8 | 25.1 | no | 0.00 % | 0 | 345 | complete | PASS |
| recording | 2 | 68.1 | 25.1 | yes | 0.00 % | 0 | 347 | complete | PASS |
| recording | 3 | 99.3 | 25.1 | yes | 0.00 % | 0 | 324 | complete | PASS |
| recording | 4 | 129.5 | 25.1 | yes | 0.04 % | 0 | 318 | complete | PASS |

The baselines' minimum algo fps was about 345–349. Mean algo fps stayed at
about 392 in every run.

Summary lines as printed:

```
# --threads 1,2,3
experiment: baseline complete, wrote 1.68 MB/s, stored-frame ratio 1.565, smallest passing pool = 1
recording: baseline complete, wrote 19.28 MB/s, stored-frame ratio 1.628, smallest passing pool = 1
# --threads 4 (only T = 4 was tried, so "smallest" is 4)
experiment: baseline complete, wrote 1.68 MB/s, stored-frame ratio 1.565, smallest passing pool = 4
recording: baseline complete, wrote 19.28 MB/s, stored-frame ratio 1.628, smallest passing pool = 4
```

## Step 3b: real camera

Skipped. Nobody was at the rig to run the camera during this session.

## Step 4: readers

`--emit-mixed` wrote `mixed.h5`: 1000 × 96 × 512 u8, C = 10, 100 chunks, 34
of them raw (filter mask 0x1). h5py 3.16 / HDF5 2.0.0 reads the whole dataset
with no plugin.

| Reader | Result |
|---|---|
| HDFView | not available (not installed on the rig) |
| MATLAB | not available (not installed; only the NI "Matlab Interface" LabVIEW add-on is present) |

## Reading

- **Every pool size from 1 to 4 passes in both modes, with zero loss in 12
  soaks.** The smallest passing pool is 1, and 2 is the smallest that also
  covers the 49 MB/s worst case ×1.3.
- **`clamp(hw_concurrency / 2, 1, 4)` gives 4 on this rig, and 4 passes:**
  130 MB/s, which is 2.6× the worst case, with capture within 0.04 %. So the D6
  rule stands.
- **Throughput per pool thread during a run is lower than on the container:**
  about 33 MB/s per thread (35 / 68 / 99 / 129), against 47 in the container
  and 56–84 idle on the rig. It scales linearly to 4 threads. The soak itself
  keeps 16–18 cores busy (the container's used 1.7–2.1 at the same algo fps),
  so the below-normal pool threads most likely run on E-cores.
- **Algo fps minimum dips at T ≥ 3** (from about 347 to 318–326). The mean is
  unchanged and no frame was dropped. On the container, T = 3 already cost
  capture rate; on the rig capture stays within 0.04 % up to T = 4.
- **Side finding:** the headless soak uses about 16–18 of 32 logical CPUs on
  the rig, against about 2 of 4 on the container, at the same frame and algo
  rates. Something in the pipeline sizes itself to `hardware_concurrency` and
  keeps those threads busy (it could be thread-pool spinning). It was not
  investigated here. It matters for PR 5's rig soak, because it is the load the
  compression pool competes with.
- The mock camera is a software timer, so the capture-fps check is
  conservative relative to a hardware-timed camera.
