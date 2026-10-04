# PZ7035 result records

> Decoding the records the PZ7035 FPGA (PL) writes to the PS DDR result
> ring: FRAME per frame, RESULT per cell, plus EVENT / COUNTERS / PREVIEW /
> DESCRIPTOR / IMAGE (#447 E3, plan W3.B1).

**Source:** `include/backend/pz/PzRecords.h`, `src/backend/pz/PzRecords.cpp`
(in `mib_processing`, Qt-free)
**ABI bundle:** `third_party/pz7035-abi/` (pz7035-imx426 `abi/`, pinned by
`PROVENANCE.json`; `scripts/vendor_pz7035_abi.py --from <repo>` refreshes it,
`--check` verifies it)
**Tests:** `processing.pz_records`, `scripts.pz7035_abi_vendor`
(`processing.pz_unet_cells_host`, the comparison with the host Contract 3
science, lives on `develop`, which has Contract 3)
**Related:** [[HDF5-Storage]], [[../services/ProcessingService]]

## Wire format

- Every record has a 16-byte header: magic, type, version, length, sequence
  and CRC-32. Records are little-endian and 8-byte aligned.
- `decodeRecord` applies the bundle's checks in the reference order: TRUNCATED,
  BAD_MAGIC, OVERSIZED, BAD_LENGTH, BAD_CRC, UNKNOWN_TYPE, BAD_VERSION, type
  minimum, then type rules (RESULT_LIMIT, payload bounds, BAD_STATE).
  Trailing bytes inside `length` are additive and ignored.
- Layouts come from the vendored `pz_mib_abi.h`; its `_Static_assert`s pin
  every offset.
- `StreamDecoder` checks one ring's sequence (a duplicate is rejected; a gap
  is counted and the record still delivered), its epoch (STALE_EPOCH; a FRAME
  with FIRST_OF_EPOCH advances it) and the descriptor generation.

## Ring and frames

- `ResultRingReader` drains `[tail, head)` from a `RingMemory`, handling the
  wrap, then releases `tail = head`.
- HEAD and TAIL are free-running byte counters. When `head - tail` exceeds the
  ring, the consumer fell behind: the reader reports an overrun and
  resynchronises at head.
- At 5 kHz the 1 MiB ring fills in about 1 s, so the consumer must drain it
  continuously.
- `FrameAssembler` groups each FRAME with the RESULTs that follow it. A frame
  is complete at `result_count`; an early next FRAME marks it `incomplete`.

## Profile `unet_cells_v2`

`decodeUnetCellsV2` turns a RESULT of science profile 2, version 2 into a
`UnetCell`: plain host-unit values with the profile reason, flags, shape,
brightness, focus metric, counts and centroid. Coordinates are in ROI 1.

- The module does not depend on the processing contracts, so the PZ7035
  application line can take it as is.
- Any other profile, or a short payload, gives `nullopt`: the payload is
  unavailable, never zero.
- On `develop`, `processing.pz_unet_cells_host` checks that the PL vectors'
  payloads, passed through encode, decode and the profile decoder, equal the
  host Contract 3 science on the same frames.

## Execution providers (YOFO S1)

`include/backend/processing/IExecutionProvider.h` is the seam: one
`ProviderFrame` per sensor frame on the provider's thread. It carries the
frame identity, flags, the `UnetCell`s, and their `FilterResult` view
(`filterResultFromUnetCell`). `include/backend/processing/pz/PzExecutionProviders.h`:

- `PzRecordPipeline` — stream checks, then assembly into ProviderFrames,
  with status counters for decode errors, gaps, overruns, incomplete frames,
  orphans and unknown profiles.
- `ReplayExecutionProvider` — replays a record stream at a frame rate. The
  stream can be `pzres capture` files from the board, or encoded vectors.
- `PzDevMemExecutionProvider` (Linux) — the PS path as `pzres` does it.
  - Bridge registers at `0x40101000`; ring at `0x3F000000`, 1 MiB, mapped
    uncached with `O_SYNC` (Linux booted with `mem=1008M`).
  - start: ring base, `tail = head` (HEAD is not reset at ARM), run id, ARM.
  - The reader thread drains every 500 µs, writes TAIL back raw, and drains
    once more after STOP.
  - **Only one reader at a time:** the TAIL register is shared, so stop
    `pzres` first.

Test `processing.pz_execution_provider` covers vector replay, a corrupted
RESULT (counted; its frame incomplete; the gap reported) and a paced
replay's rate. With `MIB_PZ_RING_CAPTURE=<ring.bin>` it replays a board
capture. On 2026-10-04 these captures decoded with 0 errors, 0 sequence or
frame-id gaps and 0 incomplete frames:

| Capture | Frames | Cells |
|---|---|---|
| `results-hw-20261003/retrain-lit-run3` | 100,090 | 100,086 (all cut off) |
| `results-hw-20261003/dark-120s` | 600,157 | 0 |
| `results-hw-20261004/results4-dark` | 100,091 | 0 |

## Not yet

- `ProcessingService::ingestProviderFrame` (accounting, monitoring rows
  without images, identification, recording) and provider selection in
  `AppBackend` (W3.B2).
