# Compression headroom e2e, cloud container (ADR 0006, PR 0 step 3a)

Captured 2026-09-26 on the Linux backend-only environment: a 4-vCPU Intel
Xeon cloud VM, apt HDF5 1.10.10, Release build. The command was:

```bash
python3 scripts/run_compression_headroom_e2e.py --duration 300 --threads 1,2,3 --json headroom-report.json
```

- **Soaks:** `mib_backend_tests mock_experiment_soak_run`, with the mock camera
  at 1000 fps on the 1,000 `512x96stream-mock-frames` TIFFs (looped), full
  frame ROI, the app's default `ProcessingConfig` (`--gates default`) and a
  median background.
- **Load:** `bench_hdf5_compression.py --seconds 308` with gzip-1 on 10-frame
  chunks, T threads, at nice +5.
- **Raw numbers:** `headroom-report.json`.

## Baselines (no compression load)

| Mode | Admitted | Empty | Stored | Wrote | Stored-frame gzip-1 ratio | Completion | Loss | Stop |
|---|---|---|---|---|---|---|---|---|
| experiment | 299,194 | 181,926 | 5,066 records (3,588 processed + sampled invalid) | 1.7 MB/s (501 MB) | 1.57 | complete | 0 | 30 ms |
| recording | 299,264 | 181,981 | 117,283 frames | 19.2 MB/s (5.77 GB) | 1.63 | complete | 0 | 19 ms |

The worst case at 1000 fps, with every frame non-empty and stored, is
49.2 MB/s. Pass needs gzip throughput ≥ 1.3 × the measured write rate.

## With a T-thread gzip-1 load for the whole run

| Mode | T | gzip MB/s during run | Needed (1.3 × wrote) | Covers worst case ×1.3 (64 MB/s)? | Capture fps Δ | Extra loss | Completion | Result |
|---|---|---|---|---|---|---|---|---|
| experiment | 1 | 47.3 | 2.2 | no | 0.13 % | 0 | complete | PASS |
| experiment | 2 | 93.2 | 2.2 | yes | 0.13 % | 0 | complete | PASS |
| experiment | 3 | 129.2 | 2.2 | yes | 1.12 % | 0 | complete | FAIL (capture Δ > 1 %) |
| recording | 1 | 46.9 | 25.0 | no | 0.16 % | 0 | complete | PASS |
| recording | 2 | 92.7 | 25.0 | yes | 0.26 % | 0 | complete | PASS |
| recording | 3 | 125.0 | 25.0 | yes | 0.91 % | 0 | complete | PASS |

## Reading

- **Smallest passing pool: 1 thread in both modes.** One thread gives about
  2x the demand of a 1000 fps recording on this data and about 28x that of an
  experiment.
- **2 threads is the smallest pool that also covers the worst case,** with
  30 % margin and no measurable harm (capture within 0.26 %, zero loss).
- **3 threads on a 4-vCPU box starts to cost** mock-camera rate (0.9–1.1 %)
  and algorithm throughput (minimum 1 s algo fps fell from ~310 to ~240–275).
  No frame was lost in any run.
- The mock camera is a software timer, so the capture-fps check is
  conservative relative to a hardware-timed camera.
- **Provisional default:** `threads = auto` = 2 on a 4-core host, which is
  `clamp(hw_concurrency / 2, 1, 4)`. The rig run confirms or replaces it.
- **Side finding (TD-16):** experiment files write one full frame and mask per
  object record. In memory these are shared, not cloned (see the 2026-09-07
  memory-budget evidence, scenario A), but on disk each record is a separate
  copy. With the default gates it is about 1 record per processed frame. With
  wide-open gates it was 5.56, a 20 s smoke run at about 160 MB/s.
