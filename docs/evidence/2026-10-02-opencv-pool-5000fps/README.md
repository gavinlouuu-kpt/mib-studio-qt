# OpenCV pool starving realtime processing at 5000 fps (2026-10-01/02)

Rig PC (i9-13900, 32 logical CPUs), Coaxlink Quad CXP-12 + SVS-Vistek
EoSens 2.0MCX12, 448x116 at 5000 fps, profile `0929_YUHUI` (realtime
`inline`, 1 worker, EveryFrame delivery), no background set, empty channel.
Each run: real app, Experiment > Preview, Start Experiment, 30 s recorded
run to `D:\bench`, stop, read the Experiment Accounting result.

## Symptom

Current `develop` (plus the MSVC build fix) processed about 2900 of 5000
frames/s during a run. Every run ended `incompleteLoss` (about 60 000
sequence gaps per 30 s, plus 25–30 store overwrites). The installed v1.1.2
build processed all 5000/s with the run complete.

## Bisect (first-parent `develop`, recorded run)

| Commit | Processed during run | Run |
|---|---|---|
| v1.1.2 (installed) | 5000/s, 154 µs | complete |
| `973463e` (last beta) | 5000/s, 126 µs | complete |
| `aa3ee95d` (#456) | 5000/s, 147 µs | complete |
| `d165f5d3` (#452) | 2779/s, 343 µs | incompleteLoss |
| `aa9aff9a` (#455) | 2751/s, 349 µs | incompleteLoss |
| `develop` + MSVC fix | 2982/s, 320 µs | incompleteLoss |

Idle (no run) numbers do not discriminate: good and bad builds both sat
near 3000/s, because the idle drop-to-latest branch was already affected.

## Root cause

Stage timers in the EveryFrame branch of `realtimeInlineLoop` (used during
experiments) showed every stage slower by the same kind of factor, including
host code #452 barely touched (pre-mask 17 → 110 µs, mask 19 → 57 µs,
analyze 110 → 182 µs). Both builds ran the full pipeline on every frame
(no empty-frame skips). Per-thread CPU during the run:

| Build | Process CPU | Threads > 50 % busy |
|---|---|---|
| `aa3ee95d` (good) | 131 % | 1 |
| `develop` + MSVC fix | 3056 % | 34 |

The Conan OpenCV on Windows parallelises through the MSVC Concurrency
Runtime (one worker per logical CPU, idle workers spin). #452 put an OpenCV
call that reaches `parallel_for` on the per-frame experiment path, so the
pool never went idle and starved the single processing thread. Proof: the
same build with `cv::setNumThreads(0)` processed 5000/s at 141 % CPU, and
the thread count dropped from 106 to 75. A disproved hypothesis along the
way: gating the new per-object Laplacian variance to Contract 2 changed
nothing.

## Fix and verification

`AppBackend::initialize` calls `cv::setNumThreads(0)` before processing
starts (`MIB_OPENCV_THREADS` overrides it); guard `backend.opencv_threads`.

| Fix build | Value |
|---|---|
| During run | 5000/s at 123 µs, run complete |
| Process CPU during run | 128 %, 1 busy thread |
| Idle | about 4980/s at 119 µs (was about 3000/s) |
| `ctest --preset windows-ninja-test` | 140/141 (`frontend.mainwindow_shutdown`, rig-specific TD-20) |
| `ctest --preset windows-ninja-integration-test` | 12/13 (`integration.monitoring_kde_e2e`, rig-specific TD-20) |
| `hardware.camera`, `hardware.discovery_reentry` (`MIB_TEST_CAMERA=1`) | pass (185 real frames), measured before the OpenCV change |
