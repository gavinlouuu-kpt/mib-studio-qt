# PZ7035 result records

> Decoding the records the PZ7035 FPGA (PL) writes to the PS DDR result
> ring: FRAME per frame, RESULT per cell, plus EVENT / COUNTERS / PREVIEW /
> DESCRIPTOR / IMAGE (#447 E3, plan W3.B1).

**Source:** `include/backend/pz/PzRecords.h`, `src/backend/pz/PzRecords.cpp`
(in `mib_processing`, Qt-free)
**ABI bundle:** `third_party/pz7035-abi/` (pz7035-imx426 `abi/`, pinned by
`PROVENANCE.json`; `scripts/vendor_pz7035_abi.py --from <repo>` refreshes it,
`--check` verifies it; `.gitattributes` marks it `-text` so a Windows checkout
keeps the exact bytes the sha256 check expects)
**Tests:** `processing.pz_records`, `processing.pz_unet_cells_host`,
`scripts.pz7035_abi_vendor`
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
- A RESULT may carry zero payload words. Encoder and decoder both skip the
  payload copy then (copying into an empty vector's null `data()` is undefined
  behaviour; caught by UBSan). `processing.pz_records` round-trips one.

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
- `processing.pz_unet_cells_host` checks that the PL vectors' payloads,
  passed through encode, decode and the profile decoder, equal the host
  Contract 3 science on the same frames.

## Not yet

- The device mapping (`/dev/mem` or UIO onto the ring and registers).
- Ingest into accounting, monitoring and recording on the PZ7035 line
  (W3.B2).
