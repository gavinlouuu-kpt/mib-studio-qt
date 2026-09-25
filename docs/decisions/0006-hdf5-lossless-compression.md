# ADR 0006 — Lossless HDF5 compression: live with raw fallback, finished post-run

- Status: accepted
- Date: 2026-09-25
- Plan: [2026-09-25-hdf5-lossless-compression](../exec-plans/active/2026-09-25-hdf5-lossless-compression.md)

## Context

Recording and experiment files are written uncompressed: image datasets are
chunked but unfiltered. At 512x96 u8 the camera produces 49 MB/s at 1000 fps
and ~246 MB/s at 5000 fps. The live save path is one HDF5 writer thread behind
a 3-slot `HdfWriteQueue`, and a full queue is a fatal "write queue overflow
(disk too slow)" that stops the run. That is deliberate (no silent data loss,
issue #367).

Measured on the real `512x96stream-mock-frames` frames (2026-09-25, 2.1 GHz
Xeon VM):

- gzip-1 compresses 1.64x at ~47 MB/s on one core and ~150 MB/s on four.
  gzip-4 and gzip-6 give the same ratio at 1.4–3.4x the CPU. The noisy
  background (mean 147, std 36) caps the lossless ratio.
- Blosc-LZ4 with bitshuffle runs at ~590 MB/s (1.56x) but is an external
  filter plugin that every reader would need.

So a filter set on today's datasets would put gzip on the single writer thread
and abort runs at ~1000 fps. Compressing only after the run is safe, but it
writes every byte twice, needs ~0.6x the file size in free space, and leaves
large files for a while after Stop.

## Decision

1. **Codec: deflate (gzip) level 1, no shuffle.** It is built into every HDF5
   and every reader (HDFView, h5py, MATLAB), so no plugins ship. The level is
   configurable. Third-party filters are out of scope.
2. **Compress live, off the HDF5 thread.** Image datasets are created with the
   deflate filter and a fixed chunk geometry. Chunks are compressed with zlib
   on a bounded, below-normal-priority worker pool. The single HDF5 writer
   thread stores them with `H5Dwrite_chunk` (HDF5 ≥ 1.10.3; our floor is
   1.10.5). Only the writer thread calls HDF5.
3. **Never let compression cost frames: per-chunk raw fallback.** A chunk
   whose compression misses its time budget, meets queue pressure, or fails in
   zlib is written uncompressed into the same dataset, with the deflate bit set
   in its chunk filter mask. Standard readers handle mixed chunks
   transparently. The overflow and accounting guarantees are unchanged:
   compression can make a file larger, never lose a frame.
4. **Finish post-run.** When the app is idle, a compaction pass rewrites a file
   whose raw fraction exceeds a threshold. Compressed chunks are copied
   byte-for-byte (`H5Dread_chunk` → `H5Dwrite_chunk`), only raw chunks are
   compressed, and the result goes to a temp file. It is verified
   byte-identical, then atomically replaces the original. The pass never edits
   the only copy in place, and it yields to any starting run.
5. **Opt-in until the rig soak passes.** The mode is `off | live |
   live_and_finish`, default `off`. It flips to `live_and_finish` after the
   rig acceptance criteria in the plan are met.

## Consequences

- The writer path gains a chunk assembler, a compression pool and a budget
  policy. The HDF5 thread-ownership rule (one thread calls HDF5 per file)
  becomes explicit and is tested.
- The chunk shape is fixed when the dataset is created (target ~512 KiB), not
  derived from the first batch. Recording batches are chunk-aligned.
- `mib_processing` (home of `Hdf5Service`) links zlib directly, which affects
  the portable core and the Python wheel build.
- Files carry `storage_*` attributes recording mode, level, and chunk counts
  (compressed/raw). A file without them is uncompressed.
- Compressed or mixed files are standard HDF5: the only reader-side change is
  a larger chunk cache for random access. Nothing else consumes them
  differently.
- Frame processing and capture compete with the pool for CPU during a run.
  The pool size is chosen from rig measurements, and the raw fallback absorbs
  bursts.
