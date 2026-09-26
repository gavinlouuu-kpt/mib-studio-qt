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
| Commits (oldest first) | `dc5bf2c` first plan draft · `fa2c83c` epic spec + ADR 0006 · `566d05a` PR 0: capability test, benchmark script, measurement howto · `7b2f9be` this handover · `38c50b9` zlib made a required dependency · `c04fa12` scripted headroom e2e (soak tool + orchestrator) · "docs: compression headroom e2e results" (container evidence, the commit after `c04fa12`) |
| Product behaviour change | **None.** Nothing in `src/` or `include/` changed. The branch adds one test, one script, documentation, and a direct zlib dependency that resolves to the package HDF5 already uses. |

## 2. What is on the branch

| Deliverable | Path | Purpose on the rig |
|---|---|---|
| Capability test `recording.hdf5_direct_chunk_capability` | `tests/recording/hdf5_direct_chunk_capability_test.cpp`, registered in `tests/CMakeLists.txt` (always built; zlib is a required dependency) | Prints the Windows Conan HDF5 version and whether it is **threadsafe**. Proves mixed compressed/raw chunks read back byte-identical through `Hdf5Service`. |
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

   zlib is a direct Conan requirement (`zlib/[>=1.2.11 <2]`, which resolves
   to the zlib package HDF5 already uses: 1.3.2 fresh, 1.3.1 in the rig's
   cache), so `conan install` brings no new binaries. `find_package(ZLIB
   REQUIRED)` stops the configure if it is missing. **Re-run `conan install`
   first** if `build-ninja` predates the zlib requirement. The old output
   still configures, because `ZLIBConfig.cmake` exists as an HDF5
   dependency, but its include dirs are empty, and the capability test fails
   with `C1083: Cannot open include file: 'zlib.h'` (rig, 2026-09-26).

3. Install the Python side and fetch the frames.

   ```powershell
   py -m pip install numpy h5py tifffile
   py scripts\provision-assets.py --asset 512x96stream-mock-frames --count 1000
   ```

   The rig PC has no `py` launcher. Use the miniconda `python` wherever this
   handover says `py`.

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
  the live writer share a non-threadsafe library today. The live path does
  not widen that sharing (design D5), but the PR 4 finish pass would. That is
  now gated in plan D8. `threadsafe=1` → no action. (Rig result:
  `threadsafe=0`, TD-17.)

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

### Step 3: headroom during a live run (about 45 minutes scripted, plus an optional real-camera check)

**3a. Scripted, mock camera.** This is the same run the cloud container did
(§7), now on rig hardware.

```powershell
py scripts\run_compression_headroom_e2e.py --runner build-ninja\Release\mib_backend_tests.exe `
    --duration 300 --threads 1,2,3 --work-dir <recording volume>\bench\e2e --json rig-headroom.json
```

The `windows-ninja` runner is in `build-ninja\Release\`. Without
`--work-dir`, the soaks write to `build\compression-e2e` on the system drive,
not the recording volume. The first rig run on 2026-09-26 did that. The
recording-mode check on `D:` is in §8.

For each save mode (experiment, recording) the script runs:

- a baseline 5-minute 1000 fps soak through the full production save path;
- the gzip ratio of the frames that soak actually stored;
- one soak per pool size T, with the benchmark compressing on T
  below-normal threads for the whole run.

It prints PASS/FAIL per T and a summary line per mode with the smallest
passing pool.

**3b. Real camera (optional confirmation).** Repeat with the real camera and
the bundled 1000 fps preset in the app. First run without load, then with
`py scripts\bench_hdf5_compression.py --levels 1 --threads <T> --chunk-frames 10 --seconds 300`
in a second terminal. Compare the two runs' accounting: the log line
`ExperimentCoordinator: run N accounting: completion=... persisted=x/y failed=z`
and the drop counters.

- **Pass for T** (what the script checks):
  - **Headroom:** compress MB/s during the run is at least 1.3 × the rate the
    baseline actually wrote. The 49 MB/s worst case (every frame non-empty)
    is reported too.
  - **No harm:** same completion state, capture fps within 1 %, and loss
    terms grow by at most 1 % of admitted frames.
- **Record:** the summary lines and `rig-headroom.json`.
- **Decision it drives:** the smallest passing T becomes the rig default for
  `threads = auto` (design D6). If no T passes, stop and report: the plan's
  budget (D4) and default mode need revisiting before PR 2.

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
Results from 2026-09-26, commit `3eb6e61`. Details are in §8 and the
[rig evidence](../../evidence/2026-09-26-compression-headroom-rig/README.md).

| Measurement | Result | Pass? |
|---|---|---|
| Rig CPU / cores / RAM / recording drive | Intel Core i9-13900 (8 P + 16 E), 24 cores / 32 logical; 32 GB; recordings on `D:`, a WD20EZBX 2 TB SATA HDD (system `C:` is NVMe) | – |
| Step 1: `HDF5_CAPABILITY` line | `HDF5_CAPABILITY version=1.14.6 threadsafe=0 deflate_encode=1 deflate_decode=1 zlib=1.3.1` (not threadsafe, so TD-17 is filed) | pass |
| Step 1: `HDF5_S1` line | `HDF5_S1 raw_bytes=19660800 direct_bytes=15200776 tail_bytes=15200776 growth_pct=0.00 ratio=1.29` | pass |
| Step 2: level 1, C = 10 compress MB/s @ 1 / 2 / 4 threads | 84 / 125 / 253, ratio 1.64. One thread varies from 56 to 84 between attempts (P-core vs E-core). | – |
| Step 2: write-to-disk MB/s @ 1 / 2 / 4 threads | 60 / 125 / 247 to the uncompressed `D:\bench`. The first run, in an NTFS-compressed folder, gave 63 / 129 / 242. | – |
| Step 2: S1 / S2 | S1 +0.65 % PASS / S2 PASS (h5py 3.16, HDF5 2.0.0) | pass |
| Step 3: baseline run accounting (completion, persisted/admitted, drops) | experiment: complete, 5,095/5,095 persisted of 300,694 admitted, loss 0, 1.7 MB/s. recording: complete, 117,827/117,827 persisted of 300,604 admitted, loss 0, 19.3 MB/s. | – |
| Step 3: under-load MB/s @ 1 / 2 / 3 threads | 35 / 68 / 99 in both modes; 129 at 4 threads (NVMe work dir). Recording on the `D:` HDD, T = 4: 135, complete, zero loss. | pass (all T) |
| Step 3: with-benchmark run accounting | all 8 loaded runs complete with extra loss 0; capture Δ ≤ 0.04 % | pass |
| Step 3: chosen default `threads` | `auto = clamp(hw_concurrency / 2, 1, 4)`, which gives 4 on the rig; 4 passes in both modes. The smallest passing pool is 1. | – |
| Step 3b: real camera | skipped (nobody at the rig) | – |
| Step 4: HDFView | not available. It is not installed, and the portable 3.4.1 build fails to start with "Failed to launch JVM" even without a file (2026-09-27). The MSI was not tried. | – |
| Step 4: MATLAB | not available (not installed) | – |
| Step 4 (extra): `h5dump` 1.14.6 | reads `mixed.h5` with no errors; its binary dump is byte-identical to h5py's read | pass |

Do not commit `mixed.h5`, the `bench\` directory, build trees or any `.h5`
file. The `rig-*.json` reports go into the rig evidence directory, with local
absolute paths replaced by placeholders.

## 6. What happens next

- **With every step passing:** open the PR for this branch (spec + PR 0),
  then start PR 1: fixed chunk geometry, chunk-aligned batches, and the
  reader chunk cache. PR 1 changes chunk shape only and adds no compression,
  so it does not depend on the thread count. PR 2 (the compressing writer)
  uses the Step 3 thread count as its default.
- **If Step 3 fails at every thread count**, or Step 4 fails: record the
  failure, set this handover to `Status: blocked` with the reason, and
  revisit D4/D6 (or D1 for Step 4) in the plan before any product code lands.

## 7. Container results (2026-09-26, for comparison)

The same step 3a ran in the Linux cloud container (4 vCPU, apt HDF5 1.10.10),
with eight 300 s soaks at 1000 fps. Full tables are in the
[container evidence](../../evidence/2026-09-26-compression-headroom-container/README.md).

| Mode | Baseline wrote | gzip-1 during run, 1 / 2 / 3 threads | Result per T | Loss |
|---|---|---|---|---|
| experiment | 1.7 MB/s | 47 / 93 / 129 MB/s | PASS / PASS / FAIL (capture −1.12 %) | 0 |
| recording | 19.2 MB/s | 47 / 93 / 125 MB/s | PASS / PASS / PASS | 0 |

- **Provisional pool default:** 2 on a 4-core host
  (`clamp(hw_concurrency / 2, 1, 4)`).
- **What to look for on the rig:** does 2 still pass and cover the 49 MB/s
  worst case with room to spare? Does the rig's larger core count let 3 pass?
  If the rig has more cores, the rule gives more threads, so check that too.

## 8. Rig results (2026-09-26)

Rig PC: Intel Core i9-13900, 24 cores (8 P + 16 E) / 32 logical, 32 GB RAM,
recordings on `D:` (SATA HDD), Conan HDF5 1.14.6, commit `3eb6e61`. Full
tables are in the
[rig evidence](../../evidence/2026-09-26-compression-headroom-rig/README.md).

| Mode | Baseline wrote | gzip-1 during run, 1 / 2 / 3 / 4 threads | Result per T | Loss |
|---|---|---|---|---|
| experiment | 1.7 MB/s | 35 / 68 / 99 / 129 MB/s | PASS / PASS / PASS / PASS | 0 |
| recording | 19.3 MB/s | 35 / 68 / 99 / 130 MB/s | PASS / PASS / PASS / PASS | 0 |

Compared with the container (§7):

- **Demand and ratio are the same:** 1.7 and 19.3 MB/s written; stored-frame
  ratio 1.57 (experiment) and 1.63 (recording).
- **Per-thread gzip during a run is lower:** about 33 MB/s per thread,
  against 47 on the container. Idle on the rig it is 56–84 MB/s. The headless
  soak keeps 16–18 of the rig's 32 logical CPUs busy (about 2 of 4 on the
  container) at the same frame and algo rates. That is OpenCV's spinning
  Concurrency Runtime pool (TD-18, follow-up below). Scaling stays linear up
  to 4 threads.
- **More threads fit:** T = 3 passes on the rig (capture Δ 0.01 %), where it
  failed on the container (−1.12 %). T = 4, the value `clamp(hw/2, 1, 4)`
  gives here, passes with capture within 0.04 %. The only visible cost is the
  algo fps minimum, which drops from about 347 to 318–326 at T ≥ 3; the mean
  stays at 392 and no frame is lost.
- **2 threads still cover the worst case:** 68 MB/s against the 64 MB/s
  needed (49 MB/s × 1.3). The margin is smaller than on the container
  (93 MB/s). 4 threads give 2.6× the worst case.
- **The Conan HDF5 is not threadsafe** (`threadsafe=0`, unlike apt 1.10.10).
  TD-17 is filed. D5 keeps the live path from widening the sharing. The PR 4
  finish pass would widen it, so it is now gated in plan D8.
- **Step 3b (real camera) was skipped**, because nobody was at the rig.
  **Step 4:** HDFView and MATLAB are not installed on the rig ("not
  available", not pass). h5py and `h5dump` 1.14.6 both read the mixed file
  (34 of 100 chunks raw) byte-identically.
- **Setup notes for the next rig run:**
  - A `build-ninja` whose Conan output predates the zlib requirement
    configures fine (`ZLIBConfig.cmake` already exists as a transitive
    dependency of HDF5) but fails to compile the capability test with
    `C1083: Cannot open include file: 'zlib.h'`. Re-running `conan install`
    fixes it.
  - Conan resolved zlib 1.3.1 here, not 1.3.2. It is the package HDF5 itself
    links, so nothing new was downloaded.
  - `bench_hdf5_compression.py --write-dir` did not create the directory
    (fixed the same day).
  - The rig has no `py` launcher; the miniconda `python` was used.
  - Step 3a's soak files go to `build\compression-e2e` (NVMe) unless
    `--work-dir` is given.

**Follow-up the same day** (details in the evidence README):

- **Recording on the `D:` HDD passes.** With `--work-dir D:\bench\e2e`
  (uncompressed): baseline complete, 5.80 GB, zero loss; T = 4 at 135 MB/s,
  capture Δ 0.04 %, PASS.
- **NTFS compression on `D:` breaks 1000 fps recording (TD-19).** The `D:\`
  root has "compress contents" set, so new top-level folders inherit it. In
  such a folder, an uncompressed-HDF5 recording failed after 13 s with
  `write queue overflow (disk too slow)`, while the disk was at most 35 %
  busy.
  - `D:\data` (the current recordings) is uncompressed and fine.
  - The scripts now warn when their target folder is NTFS-compressed.
  - The step 2 numbers above are from the re-run in the uncompressed folder
    and are within 5 % of the first run.
- **The soak's 16–18 cores are OpenCV's ConcRT pool (TD-18).** OpenCV 4.12.0
  uses the MSVC Concurrency Runtime, with one spinning worker per logical
  CPU. `OPENCV_FOR_THREADS_NUM` has no effect.
- **Script fix:** `run_compression_headroom_e2e.py` no longer passes a
  loaded run whose completion is `failed` just because the baseline failed
  too.

**Decision:** keep `threads = auto = clamp(hw_concurrency / 2, 1, 4)` (4 on
the rig). PR 0 is complete apart from the optional real-camera check and the
HDFView/MATLAB reads, which need those tools on a machine that has them. The
next step is PR 1. TD-18 (fewer spinning OpenCV threads) would give the pool
more headroom, but it is not needed for the chosen default.

