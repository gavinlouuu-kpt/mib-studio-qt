# Measure HDF5 compression headroom on the rig

PR 0 of the [lossless compression epic](../exec-plans/active/2026-09-25-hdf5-lossless-compression.md)
([ADR 0006](../decisions/0006-hdf5-lossless-compression.md)). These runs set the
default number of compression threads and confirm the per-chunk time budget.
They need the rig PC and change nothing in the app.

## What you need

- The rig PC with a Release build (for the capability test) and the installed
  app (for the live run).
- Python 3.10+ with `pip install numpy h5py tifffile`. The commands below say
  `python`; on a machine with the Windows `py` launcher, `py` works too. The
  rig PC has no launcher, so use the miniconda `python.exe` there.
- The mock frames: `python scripts/provision-assets.py --asset 512x96stream-mock-frames --count 1000`.
  Alternatively, pass `--h5 <a real recording>` to use real frames.
- A current Conan install. If `conanfile.py` changed since `build-ninja` was
  configured (zlib became a direct requirement on 2026-09-26), re-run
  `conan install` first. An old install still configures, but the
  capability test then fails to compile with `C1083: Cannot open include
  file: 'zlib.h'`.

Record every number in the plan's decision log, together with the CPU model,
core count and the drive letter of the recording volume.

## 1. HDF5 build capability (Windows Conan build)

```powershell
ctest --test-dir build-ninja -R recording.hdf5_direct_chunk_capability -V
```

This uses the `windows-ninja` preset. For the Visual Studio `build` tree,
use `--test-dir build --build-config Release`. The test must pass. Copy the
`HDF5_CAPABILITY ... threadsafe=N` and `HDF5_S1` lines into the decision log.
`threadsafe=0` means HdfReviewTab, HdfExportService and the live writer share
a non-threadsafe library. The rig showed exactly that on 2026-09-26, and it
is tracked as TD-17.

## 2. Idle benchmark, including the recording drive

Close the app, then run:

```powershell
python scripts/bench_hdf5_compression.py --threads 1,2,4 --chunk-frames 10,50 `
    --write-dir D:\bench --spikes --json rig-idle.json
```

Use a folder on your recording volume for `--write-dir` (the rig records to
`D:`). The script creates the folder if needed.

**Check that the folder is not NTFS-compressed.** On the rig, the `D:\` root
has "compress contents" set, so any new top-level folder inherits it. Windows
then compresses every write synchronously, and a 1000 fps recording overflows
its write queue within seconds while the disk sits idle (TD-19). Both scripts
print a `WARNING` when their target folder is compressed. Clear it on your
own bench folder with `compact /u /s:D:\bench`, or use a subfolder of
`D:\data`, which is uncompressed. Read the level-1, chunk-10 rows:
threads × MB/s gives the ceiling; the `1000fps` and `5000fps` columns show
whether each thread count keeps up.

## 3. Headroom during a live run (the number that sets `threads`)

### 3a. Scripted, mock camera (run this first)

```powershell
python scripts\run_compression_headroom_e2e.py --runner build-ninja\Release\mib_backend_tests.exe `
    --duration 300 --threads 1,2,3 --work-dir D:\bench\e2e --json rig-headroom.json
```

- **`--work-dir`:** put it on the recording volume, so the soaks write where
  real recordings go. Without it they write to `build\compression-e2e` on
  the system drive.
- **`--threads`:** also run the value `clamp(logical CPUs / 2, 1, 4)` gives
  on the host (4 on the rig).

For each save mode (experiment, then recording), the script runs:

- a baseline soak: the mock camera at 1000 fps on the 512x96 frames, through
  the full production pipeline, `ExperimentCoordinator` /
  `startFrameRecording`, `HdfWriteQueue` and HDF5, using
  `mib_backend_tests mock_experiment_soak_run`;
- the gzip ratio of the frames that run actually stored;
- one soak per pool size T, with `bench_hdf5_compression.py --seconds`
  compressing on T below-normal threads for the whole run.

**Pass for T:** both conditions hold.

- **Headroom:** gzip MB/s sustained during the run is at least 1.3 × the
  rate the baseline actually wrote. The worst case, every frame non-empty
  (fps × 512 × 96 = 49 MB/s), is reported beside it.
- **No harm:** the completion state equals the baseline's, capture fps is
  within 1 %, and the loss terms (ring overwrites, persistence failed or
  pending, processing drops, camera discards) grow by at most 1 % of
  admitted frames.

The smallest passing T becomes the `threads = auto` default on the rig.
About 45 minutes for both modes.

The mock camera is a software timer thread, so under heavy CPU load its
frame rate dips slightly, which a hardware-timed camera would not do. That
makes the capture-fps check conservative.

### 3b. Manual, real camera (confirmation)

1. Start the app and run the reference load: a 1000 fps experiment with
   processing on, using the real camera and the bundled 1000 fps preset.
2. Once it is steady, run the benchmark as a load in a second terminal:

   ```powershell
   py scripts\bench_hdf5_compression.py --levels 1 --threads <T from 3a> --chunk-frames 10 `
       --seconds 300 --json rig-under-load.json
   ```

3. Stop the experiment. Compare its run accounting (completion state, drop and
   overwrite counters, `persistence*` terms) with an identical run without the
   benchmark, using the same pass rules as 3a.

## 4. Reader compatibility (spike S2 by hand)

```powershell
python scripts/bench_hdf5_compression.py --levels 1 --threads 1 --chunk-frames 10 --repeat 1 `
    --emit-mixed mixed.h5
```

Every third chunk in `mixed.h5` is stored raw. Open it in HDFView and in MATLAB
(`h5read('mixed.h5','/recorded_frames/images')`) and check that frames display
and match the source. h5py was already verified byte-identical by `--spikes`.

## Results so far

| Where | HDF5 | Result |
|---|---|---|
| Cloud VM, apt build (ctest) | 1.10.10, threadsafe=1 | capability pass; S1 growth 0.00 % |
| Cloud VM, h5py 3.14 | 1.14.6 | S1 +0.65 %, S2 byte-identical |
| Cloud VM, h5py 3.16 | 2.0.0 | S1 +0.65 %, S2 byte-identical |
| Cloud container, 4 vCPU (step 3a, 8 × 300 s soaks) | 1.10.10 | all runs complete, zero loss. gzip-1 during the run: 47/93/127 MB/s on 1/2/3 threads, against the measured demand of 1.7 (experiment) and 19.2 (recording) MB/s. Smallest passing pool is 1; 2 covers the worst case. [Evidence](../evidence/2026-09-26-compression-headroom-container/README.md) |
| Rig PC, i9-13900 24 cores / 32 logical, recordings on a `D:` SATA HDD (2026-09-26) | Conan 1.14.6, threadsafe=0 (TD-17) | Capability test passes, S1 growth 0.00 %. Idle gzip-1: 84/125/253 MB/s on 1/2/4 threads; to the HDD: 60/125/247 MB/s. Step 3a: 12 soaks complete with zero loss. gzip-1 during the run: 35/68/99/129 MB/s on 1/2/3/4 threads, and every T passes in both modes, so `auto = clamp(hw/2, 1, 4)` = 4 is confirmed. Recording on the HDD, T = 4: 135 MB/s, PASS. The soak's 16–18 busy cores are OpenCV's ConcRT pool (TD-18). NTFS-compressed folders break 1000 fps recording (TD-19). h5dump 1.14.6 reads the mixed file byte-identically. HDFView and MATLAB: not available on the rig. 3b (real camera): skipped. [Evidence](../evidence/2026-09-26-compression-headroom-rig/README.md) |
