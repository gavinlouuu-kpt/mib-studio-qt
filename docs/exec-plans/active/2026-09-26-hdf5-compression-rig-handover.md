# Handover: HDF5 compression epic, PR 0 rig measurements

Status: active

Date: 2026-09-26. Author: the cloud agent session that wrote the epic spec and
PR 0. The reader is whoever runs the measurements on the Windows rig PC and
records the results. Companion documents:

- the epic plan
  [`2026-09-25-hdf5-lossless-compression.md`](2026-09-25-hdf5-lossless-compression.md)
  (design D1–D10, acceptance criteria, decision log, progress);
- ADR [`0006-hdf5-lossless-compression.md`](../../decisions/0006-hdf5-lossless-compression.md);
- the detailed procedure
  [`hdf5-compression-measurement.md`](../../howto/hdf5-compression-measurement.md);
- the task note `knowledge_map/task/2026-09-26-hdf5-compression-pr0.md`.

## 1. Where the work is

| Item | Value |
|---|---|
| Repository | `gavinlouuu-kpt/mib-studio-qt` |
| Branch | `claude/hdf5-compression-impact-bc7zvq` (pushed; no PR opened yet) |
| Base | `develop` at `2fe0282` |
| Commits (oldest first) | `dc5bf2c` first plan draft · `fa2c83c` epic spec + ADR 0006 · `566d05a` PR 0: capability test, benchmark script, measurement howto · this handover |
| Product behaviour change | **None.** Nothing in `src/` or `include/` changed. The branch adds one test, one script and documentation. |

## 2. What is on the branch

| Deliverable | Path | Purpose on the rig |
|---|---|---|
| Capability test `recording.hdf5_direct_chunk_capability` | `tests/recording/hdf5_direct_chunk_capability_test.cpp`, registered in `tests/CMakeLists.txt` (only when zlib is found) | Prints the Windows Conan HDF5 version and whether it is **threadsafe**. Proves mixed compressed/raw chunks read back byte-identical through `Hdf5Service`. |
| Benchmark script | `scripts/bench_hdf5_compression.py` | Compression throughput and ratio on the rig CPU and recording drive, idle and during a live run. |
| Measurement procedure | `docs/howto/hdf5-compression-measurement.md` | The long form of §4 below. |
| Epic spec + ADR | plan and ADR above | What the numbers feed into. |

Already verified off-rig on a 4-core cloud VM, 2026-09-26:

- gzip-1 at 10-frame chunks runs at 56 / 114 / 222 MB/s on 1 / 2 / 4
  threads, ratio 1.64. Inflating one chunk takes 2.9 ms.
- apt HDF5 1.10.10 is threadsafe and the capability test passes.
- Spike S1 (tail-chunk rewrite file growth) passes on HDF5 1.10.10, 1.14.6
  and 2.0.0. Spike S2 (mixed chunks readable in h5py) passes.

## 3. Before you start

1. Get the branch.

   ```powershell
   git fetch origin claude/hdf5-compression-impact-bc7zvq
   git switch claude/hdf5-compression-impact-bc7zvq
   ```

2. Build the tests with the usual fast loop (VS 2022 x64 developer shell).

   ```powershell
   cmake --preset windows-ninja
   cmake --build --preset windows-ninja-build
   ```

   Look in the configure output for
   `recording.hdf5_direct_chunk_capability skipped: zlib not found`. If it
   appears, write that down: it is itself a result (PR 2 must add zlib as a
   direct Conan requirement), and step 1 below cannot run.

3. Install the Python side and fetch the frames.

   ```powershell
   py -m pip install numpy h5py tifffile
   py scripts\provision-assets.py --asset 512x96stream-mock-frames --count 1000
   ```

4. Note the rig facts: CPU model and core count (`Get-CimInstance Win32_Processor`),
   RAM, the recording volume and its type (NVMe, SATA SSD, HDD or network),
   and the app build under test.

## 4. The four measurements

### Step 1: Conan HDF5 capability (about 1 minute)

```powershell
ctest --test-dir build-ninja -R recording.hdf5_direct_chunk_capability -V
```

- **Pass:** the test passes.
- **Record:** the `HDF5_CAPABILITY ...` and `HDF5_S1 ...` lines verbatim.
- **Decision it drives:** `threadsafe=0` → add a tech-debt row to
  `docs/exec-plans/tech-debt-tracker.md`, because review/export threads and
  the live writer share a non-threadsafe library today. The epic itself does
  not widen that sharing (design D5). `threadsafe=1` → no action.

### Step 2: idle benchmark on the recording drive (about 5–10 minutes)

Close the app first.

```powershell
py scripts\bench_hdf5_compression.py --threads 1,2,4 --chunk-frames 10,50 `
    --write-dir <recording volume>\bench --spikes --json rig-idle.json
```

- **Pass:** S1 and S2 print PASS (exit code 0).
- **Record:** the level-1, chunk-10 rows (compress MB/s and write-to-disk
  MB/s per thread count); keep `rig-idle.json`.
- **Decision it drives:** the rig ceiling. It is compared with the VM
  (56 MB/s per thread).

### Step 3: headroom during a live run (about 30 minutes; the key number)

1. Run a **baseline** 1000 fps, 5-minute experiment with processing on. Use
   the reference soak setup: mock camera on the 512x96 frames, or the real
   camera with the bundled 1000 fps preset. Save the run accounting: the log
   line `ExperimentCoordinator: run N accounting: completion=... persisted=x/y
   failed=z`, plus capture drop/overwrite counters from the status panel or
   log.
2. Run the **same experiment again**. Once it is steady, start the benchmark
   in a second terminal. It lowers its own priority, as the planned pool
   will.

   ```powershell
   py scripts\bench_hdf5_compression.py --levels 1 --threads 1,2,3 --chunk-frames 10 `
       --repeat 8 --json rig-under-load.json
   ```

3. Stop the run and record its accounting the same way.

- **Pass for a thread count:** compress MB/s under load ≥ **64 MB/s** (1.3 ×
  the 49 MB/s demand at 1000 fps), **and** the second run has the same
  completion state as the baseline, with drop/overwrite counters within
  **±1 %**.
- **Record:** MB/s per thread count under load, and both runs' accounting.
- **Decision it drives:** the smallest passing thread count becomes the rig
  default for `threads = auto` (design D6). If no thread count passes, stop
  and report: the plan's budget (D4) and default mode need revisiting before
  PR 2.

### Step 4: reader compatibility (about 5 minutes)

```powershell
py scripts\bench_hdf5_compression.py --levels 1 --threads 1 --chunk-frames 10 --repeat 1 `
    --emit-mixed mixed.h5
```

Open `mixed.h5` (dataset `/recorded_frames/images`; every third chunk is
stored raw):

- in **HDFView**: frames display, no filter errors;
- in **MATLAB**: `x = h5read('mixed.h5','/recorded_frames/images'); size(x)`
  returns 512×96×1000 (MATLAB reverses dimensions) with no error.

- **Pass:** both open without errors or plugins.
- **Decision it drives:** confirms the "standard readers only" constraint
  (design D1). If either fails, record the error message and stop before PR 2.

## 5. Where to record results

1. Fill in this table (edit this file) and commit it on the branch.
2. Copy the numbers into the plan's **Decision log** as a dated
   `2026-MM-DD (PR 0, rig)` entry.
3. Tick the rig items under **PR 0** and **Progress** in the plan.
4. Update the "Results so far" table in the howto.

| Measurement | Result | Pass? |
|---|---|---|
| Rig CPU / cores / RAM / recording drive | | – |
| Step 1: `HDF5_CAPABILITY` line | | |
| Step 1: `HDF5_S1` line | | |
| Step 2: level 1, C = 10 compress MB/s @ 1 / 2 / 4 threads | | – |
| Step 2: write-to-disk MB/s @ 1 / 2 / 4 threads | | – |
| Step 2: S1 / S2 | | |
| Step 3: baseline run accounting (completion, persisted/admitted, drops) | | – |
| Step 3: under-load MB/s @ 1 / 2 / 3 threads | | |
| Step 3: with-benchmark run accounting | | |
| Step 3: chosen default `threads` | | – |
| Step 4: HDFView | | |
| Step 4: MATLAB | | |

Do not commit `rig-*.json`, `mixed.h5`, the `bench\` directory or build
trees. Attach the JSON files to the PR instead.

## 6. What happens next

- **With every step passing:** open the PR for this branch (spec + PR 0),
  then start PR 1: fixed chunk geometry, chunk-aligned batches, and the
  reader chunk cache. PR 1 changes chunk shape only and adds no compression,
  so it does not depend on the thread count. PR 2 (the compressing writer)
  uses the Step 3 thread count as its default.
- **If Step 3 fails at every thread count**, or Step 4 fails: record the
  failure, set this handover to `Status: blocked` with the reason, and
  revisit D4/D6 (or D1 for Step 4) in the plan before any product code lands.
- **If zlib was not found (§3, step 2):** PR 2 adds `zlib` as a direct
  requirement in `conanfile.py`. Record it in the decision log.
