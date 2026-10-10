# Stored records: byte layout (authoritative)

What `pzrec read RUN ...` writes to stdout and what the PL drain writes to the SSD. Written from the ABI (`abi/pz_mib_abi.json`, `docs/platform/FRAME_STORE.md`), the record builders
(`src/imx426/pz_platform_model.py`, co-simulated byte-exact with the RTL) and **verified on real records** read from the board after the S2 slot (2026-10-10):
`/mnt/hdd/developer-data/IMX426/s2-records/` (run 18 = 27,119 records, run 19 = 10,892; PL pl-results14g, U-Net profile 2 version 3). The reference decoder is
`tools/pzrec/pzrec_decode.py`; it implements exactly this document, and `tests/test_pzrec_decode.py` pins it to the reference files.

All multi-byte fields are little-endian, fixed width, naturally aligned.

## The stream

`pzrec read` output = the run's records back to back, nothing else: no header, no trailer, no separators. Record size = the run's `rec_sectors` x 512 B (run table field `rec_sectors`
= 116 -> **59,392 B** for 512 x 96 Mono8 + mask; always a multiple of 512). A stream of `written` records is `written x 59,392` bytes; anything else is truncation (a cut by SIGTERM or a
closed pipe can end mid-record, `pzrec` reports the exact byte count on stderr). Record k of a run starts at byte `k x rec_bytes`; `--from/--count` select whole records.
Run-level metadata (start time, filter, tag, counters, tick rate) is **not** in the records: it is the run table entry (`pzrec runs`).

## One stored record (59,392 B)

```
byte 0       record set area, 4096 B: wire records back to back (8-byte aligned), then zero padding
byte 4096    MONO8 block   512 x 96 = 49,152 B   (IMAGE record #1 points here)
byte 53248   MASK1 block   96 x 64   =  6,144 B   (IMAGE record #2 points here)
byte 59392   end
```

The block offsets are constants of the configuration (set area 4096 = `STORE_SET_BYTES`); the decoder takes them from the IMAGE records, not from this table. There is no INT8 block in
these runs (STORE_MODE 0x601 has no INT8 bit).

### The record set (bytes 0..4095), in the order the PL writes it

`FRAME`, `IMAGE` (MONO8), `IMAGE` (MASK1), `RESULT` x result_count, then `EVENT` x n if the profile issues any (none in the S2 runs). Measured: a frame with two cells has
64 + 56 + 56 + 112 + 112 = 400 bytes of records; bytes 400..4095 are zero. If the whole set does not fit in 4096 B only the FRAME is kept and FRAME.flags carries RESULTS_OVERFLOW.

Every wire record starts with the 16-byte header:

| Offset | Field | Meaning |
|---|---|---|
| 0 | magic u32 | `0x524D5A50` (bytes `50 5A 4D 52`) |
| 4 | type u8 | 1 FRAME, 2 RESULT, 3 EVENT, 7 IMAGE (enum `record_type` in the ABI JSON) |
| 5 | version u8 | 1 for all types today |
| 6 | length u16 | whole wire record incl. this header, multiple of 8, <= 4096 |
| 8 | sequence u32 | **ordinal of the wire record inside this set: 0, 1, 2, ...** (so 0 for the FRAME). It is *not* the per-seen-frame store sequence the design note (`SSD_RECORDING_DESIGN.md` section 5.1) intended; the PL numbers each set from 0. Use `frame_id` for ordering. |
| 12 | crc32 u32 | zlib/IEEE CRC32 over the whole wire record with this field read as zero |

**CRC coverage:** each wire record has its own CRC. **The MONO8 and MASK1 blocks are not covered by any CRC** (nor are the zero padding and the tail). Their integrity rests on the
SATA link (CRC per frame) and on the record being structurally consistent; a converter that needs more must add its own check.

#### FRAME (type 1, 64 B)

| Offset | Field | Notes |
|---|---|---|
| 16 | run_id u64 | = the run table id (Studio writes it to the bridge RUN_ID before ARM; pz7035 issue #49) |
| 24 | frame_id u64 | physical sensor frame counter, ascending within a run; gaps = frames not stored |
| 32 | timestamp u64 | ticks at SOF; rate = the run table's `tick_hz` (100,000,000 on pl-results14g: consecutive 5 kHz frames are 19,996 or 19,998 ticks apart in the reference files) |
| 40 | epoch u32 | configuration epoch (5 in run 18, 7 in run 19: it changes with every Run entry) |
| 44 | flags u32 | bits: 0 EMPTY, 1 PARTIAL, 2 INVALID, 3 RESULTS_TRUNCATED, 4 RESULTS_OVERFLOW, 5 PREVIEW_AVAILABLE, 6 PREVIEW_DROPPED, 7 FIRST_OF_EPOCH, **8 STORED (always set in stored records)**, 9 STORE_OVERWRITTEN |
| 48 | result_count u16 | RESULT records of this frame (the set holds them all unless RESULTS_OVERFLOW) |
| 50 | result_limit u16 | 64 (MAX_RESULTS_PER_FRAME) |
| 52 | science_profile u16 | 1 CLASSICAL_CONTRACT1, 2 UNET_W8A8_C4_SINGLE |
| 54 | profile_version u16 | 3 for the U-Net profile of the S2 runs |
| 56 | width u16, 58 height u16 | 512, 96 |
| 60 | pixel_format u8 | 1 MONO8 |
| 61 | **drops_before u8** | passing records the drain dropped since the previous stored record, saturating at 255 (see below) |
| 62 | reserved u8[2] | 0 |

**drops_before.** The drain patches this byte on the way to the SSD and recomputes the FRAME CRC, so the CRC covers it. Record 0 of a run has 0. It counts passing records the drain dropped
between two stored records. `frame_id` is the **sensor's** counter, so a frame-id gap can hold more than drain drops: frames lost before the ring (acquisition `FRAMES_LOST`, e.g. the Align to Run
flood), and with filter `valid` / `any` frames the filter skipped. Hence the rule a reader may always apply is `drops_before <= min(frame_id gap, 255)`. Equality `drops_before == min(gap, 255)` holds
only when every gap frame was a drain drop, which the run table proves for a filter-ALL run when `seen == last_frame_id - first_frame_id + 1` (`pzrec_decode.drops_exact(run)`); then the decoder is asked
for `exact_drops=True`. Run 18 satisfies it: seen 152,672 = span 152,672; over its 27,119 records the gaps total 125,553 = the run's `dropped` and `drops_before` sums to 118,703 because of saturation at 255.
Records dropped *before the first* and *after the last* stored record are only in the run table counters.

**RESULTS_OVERFLOW.** When the RESULT/EVENT records do not fit in the 4096 B set (about 35 cells; the PL allows 64) the set holds **only the FRAME** (flag RESULTS_OVERFLOW, `result_count` is the true count);
no IMAGE records are written, but the MONO8 and MASK1 blocks are still at their fixed offsets (4096 and 4096 + width x height). The decoder takes them from there and notes `OVERFLOW_FIXED_LAYOUT`; the
results of such a frame are only in the result ring, not on the SSD. A set without IMAGE records and without RESULTS_OVERFLOW is an error.

**Missing mask.** An IMAGE record with flag NO_RESULT (bit 1) means the block was not produced: its bytes are zeros and are *not* an empty mask. The decoder returns `mask = None` for that record.

## Frames in both the ring and the SSD (the M1 cross-check)

Studio plays back the frames of the DDR ring after Stop; the SSD holds the drained records. Compare them **by `frame_id`** (and `epoch`; both are in `StoredRecord` / `rows()`). Which frames are guaranteed to exist in both,
for a **graceful Stop** (not ABORT) of a run longer than the ring, from `fpga/rec/pz_ssd_feeder.v`:

- The ring holds the newest `ring_records` (5000) frames received up to the bridge STOP, which freezes it (a frozen ring is not overwritten).
- The feeder decides frames until the drain's STOP and then drains every passing frame it has already decided, **except** records the ring is about to overwrite: a record with `head - seq >= ring_records - GUARD`
  (GUARD = 256) is dropped. Decisions stop at the drain's STOP, but the ring's `head` keeps advancing until the bridge STOP freezes it, and the drop test uses the live head. So **at least `ring_records - GUARD - Δ` = 4744 - Δ of the newest passing frames are on the SSD and in the ring**, with Δ = the frames received between the two STOPs (a few at 5 kHz), i.e. about 4744. Older ring frames (the oldest 256) are on the SSD only if the drain
  had already taken them. Measured on run 18: the last **4753** records are consecutive frame ids ending at `last_frame_id` (156928), the 100-frame gap before them is the last drop; `consecutive_tail(frame_ids)` finds that point.
- The ring and the SSD can end a few frames apart: frames that arrive between the drain's STOP and the bridge's STOP are in the ring but not on the SSD (Studio runs `pzrec stop` on its own thread and sends the
  bridge STOP right after, so the order is not fixed); likewise the first frames after ARM, before `pzrec start` (about 11 ms, 55 frames at 5 kHz; irrelevant for a long run). In S2 the provider counted 60 and 53
  frames more than the run's `seen`, of which about 55 are the start offset: the end difference was within a few frames. The SSD's last frame is the run table's `last_frame_id`; do not require the ring to end there.
- With filter `valid` / `any` only passing frames are on the SSD; a ring frame that did not pass is legitimately absent.
- After an ABORT the pending frames are dropped: no guarantee.

The guarantee also needs the drain to finish the post-STOP backlog (about 281 MB, the ring's contents) inside `pzrec stop`'s timeout, as it did on run 18 (7.8 s at 38 MB/s); a stop that times out turns into an ABORT.
The 4753 above is a measurement, not a bound: `tests/test_pzrec_decode.py` pins it as a regression check on run 18.

Rule for the cross-check: take the ring frames with `frame_id <= last_frame_id` and within the final consecutive tail of the SSD stream (about 4744 frames for an overloaded ALL run, at least 4744 - Δ), and require
image, mask and results to be byte-equal by `frame_id`. A frame in that range missing on either side is a failure.

## What a consumer must check (the decoder does)

1. Stream length is a multiple of `rec_bytes`.
2. Each wire record: magic, length (multiple of 8, within the set area), CRC, in-set sequence 0, 1, 2, ...
3. The set starts with a FRAME with STORED set; IMAGE blocks lie inside the record, after the set area, do not overlap; frame ids and timestamps of IMAGE/RESULT equal the FRAME's.
4. `result_count` equals the number of RESULT records unless RESULTS_OVERFLOW.
5. Zero padding after the set, zero tail after the last block.
6. Across records: one `run_id` (and the run the stream was read from: `expected_run_id`), `frame_id` and `timestamp` strictly ascending, `drops_before` against the gap (rules above).
