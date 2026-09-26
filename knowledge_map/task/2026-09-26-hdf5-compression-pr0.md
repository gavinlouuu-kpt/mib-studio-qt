# HDF5 lossless compression — PR 0 (measure and de-risk)

Epic plan: `docs/exec-plans/active/2026-09-25-hdf5-lossless-compression.md`.
Decision: `docs/decisions/0006-hdf5-lossless-compression.md`. Storage note:
[[../data-model/HDF5-Storage]]. No product behaviour change in this PR.

## What landed

- `scripts/bench_hdf5_compression.py`: benchmarks real frames (TIFF dir or an
  existing `.h5`) across gzip level × pool threads × chunk frames. It reports
  ratio, compress MB/s, fps capacity against 1000/5000 fps, and inflate
  ms/chunk. Options:
  - `--write-dir`: end-to-end `H5Dwrite_chunk` writes to the recording drive.
  - `--spikes`: S1/S2 on the local h5py.
  - `--emit-mixed`: a mixed raw/compressed file for HDFView and MATLAB.
  
  The process runs at below-normal priority, like the planned pool.
- `tests/recording/hdf5_direct_chunk_capability_test.cpp`
  (`recording.hdf5_direct_chunk_capability`, labels recording/backend/hdf5).
  It is always built, because zlib is required (see below). It:
  - prints `HDF5_CAPABILITY` (version, threadsafe, deflate encode/decode,
    zlib) and `HDF5_S1` lines;
  - asserts that compressed, raw (filter mask 0x1) and padded edge chunks read
    back byte-identical through `H5Dread` and `Hdf5Service::readImageByIndex`;
  - asserts that 9 raw tail rewrites per chunk grow the file by < 5 %.
- `docs/howto/hdf5-compression-measurement.md`: the rig procedure, covering
  the capability line, the idle benchmark, headroom under a live 1000 fps run
  (sets `threads`), and the reader check.

- **zlib is now a required dependency.** It is a direct Conan requirement
  with the hdf5 recipe's range, which resolves to the same 1.3.2 package on
  the Linux and Windows profiles. `find_package(ZLIB REQUIRED)` is in
  `cmake/MIBDependencies.cmake`, and the package is declared as apt
  `zlib1g-dev`, manylinux `zlib-devel` and macOS SDK `libz`. See
  [[../build-and-run/Dependencies]].

## Results (cloud VM, 4 × 2.1 GHz Xeon)

- gzip-1 at 10-frame chunks: 56 / 114 / 222 MB/s on 1 / 2 / 4 threads,
  ratio 1.64. Inflating one chunk takes 2.9 ms (15.5 ms at 50-frame chunks).
- apt HDF5 1.10.10: threadsafe=1, capability test pass, S1 growth 0.00 %.
- h5py HDF5 1.14.6 and 2.0.0: S1 +0.65 %, S2 byte-identical.

## Headroom e2e in the container (step 3a)

- **`mock_experiment_soak_run`** (`tests/tools/`, a `mib_backend_tests`
  entry) runs a headless experiment or recording through the production save
  path. It prints the stored #367 accounting, stop time, file size and
  process CPU as JSON.
- **`scripts/run_compression_headroom_e2e.py`** runs, per mode, a baseline
  soak, measures the stored-frame ratio, then runs one soak per pool size
  with `bench_hdf5_compression.py --seconds` as the load.
- **Result** (8 × 300 s at 1000 fps, 4 vCPU): every run complete with zero
  loss.
  - Demand is 1.7 MB/s (experiment) and 19.2 MB/s (recording).
  - gzip-1 during the run: 47 / 93 / 127 MB/s on 1 / 2 / 3 threads.
  - The smallest passing pool is 1. Two threads cover the worst case, and 3
    harm a 4-vCPU host by −1.1 % capture.
  - D6 default is now `clamp(hw/2, 1, 4)`.
  - Evidence: `docs/evidence/2026-09-26-compression-headroom-container/`.
- **TD-16:** experiment files write one full frame and mask per object
  record (5.56 per frame with wide-open gates).

## Gotchas

- `H5Dwrite_chunk` needs HDF5 ≥ 1.10.3 and `H5Dget_chunk_info_by_coord`
  needs ≥ 1.10.5. The mask assertions are behind `H5_VERSION_GE(1, 10, 5)`,
  which is also the manylinux floor.
- Raw and edge chunks are stored at the full chunk size (zero-padded), so a
  raw chunk's stored size equals `C × H × W`.
- `mib_backend_tests` lands in `build/linux-backend/Release/` on the
  backend-only preset (not the build root); the e2e script searches the
  usual locations or takes `--runner`.
- HDF5 2.x changes `H5Dread_chunk`'s signature. The test does not use it, but
  the PR 4 finish pass must guard it by version.

## Open (rig)

Handover with steps, pass criteria and the results table:
`docs/exec-plans/active/2026-09-26-hdf5-compression-rig-handover.md`.

The Conan 1.14.6 `HDF5_CAPABILITY` line (threadsafe status), headroom under
load, and HDFView/MATLAB reads of `--emit-mixed`.
