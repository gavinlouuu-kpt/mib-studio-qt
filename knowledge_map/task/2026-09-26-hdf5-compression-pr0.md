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
  It is registered only when `find_package(ZLIB)` succeeds. It:
  - prints `HDF5_CAPABILITY` (version, threadsafe, deflate encode/decode,
    zlib) and `HDF5_S1` lines;
  - asserts that compressed, raw (filter mask 0x1) and padded edge chunks read
    back byte-identical through `H5Dread` and `Hdf5Service::readImageByIndex`;
  - asserts that 9 raw tail rewrites per chunk grow the file by < 5 %.
- `docs/howto/hdf5-compression-measurement.md`: the rig procedure, covering
  the capability line, the idle benchmark, headroom under a live 1000 fps run
  (sets `threads`), and the reader check.

## Results (cloud VM, 4 × 2.1 GHz Xeon)

- gzip-1 at 10-frame chunks: 56 / 114 / 222 MB/s on 1 / 2 / 4 threads,
  ratio 1.64. Inflating one chunk takes 2.9 ms (15.5 ms at 50-frame chunks).
- apt HDF5 1.10.10: threadsafe=1, capability test pass, S1 growth 0.00 %.
- h5py HDF5 1.14.6 and 2.0.0: S1 +0.65 %, S2 byte-identical.

## Gotchas

- `H5Dwrite_chunk` needs HDF5 ≥ 1.10.3 and `H5Dget_chunk_info_by_coord`
  needs ≥ 1.10.5. The mask assertions are behind `H5_VERSION_GE(1, 10, 5)`,
  which is also the manylinux floor.
- Raw and edge chunks are stored at the full chunk size (zero-padded), so a
  raw chunk's stored size equals `C × H × W`.
- HDF5 2.x changes `H5Dread_chunk`'s signature. The test does not use it, but
  the PR 4 finish pass must guard it by version.

## Open (rig)

Handover with steps, pass criteria and the results table:
`docs/exec-plans/active/2026-09-26-hdf5-compression-rig-handover.md`.

The Conan 1.14.6 `HDF5_CAPABILITY` line (threadsafe status), headroom under
load, and HDFView/MATLAB reads of `--emit-mixed`.
