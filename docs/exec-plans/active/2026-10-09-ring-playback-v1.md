# #649 v1: Stop, then play back the buffered frames (results13 ring)

Status: active (the reader core is in; provider, bridge and UI follow)

## Goal

[#649](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/649): at 5 kHz the PZ7035 stops, plays back the buffered frames with
their own mask and cell results, and shows how much it holds. v1 is **playback only**: Save clip arrives with the SSD path
(#667), so the control is disabled with a reason until then. Built on the PC parity spec
([2026-10-08-playback-save-parity.md](2026-10-08-playback-save-parity.md)) and the board owner's `docs/FRAME_RING.md` (pz7035 PR #34,
results13, ABI 1.4). The parity spec's finding stands: the PC reviews raw frames and recomputes the overlay on the host; the PZ
overlay is record-based because the PL does the science.

## What the PL gives (FRAME_RING.md, the final reader wording)

- A ring of N store records (59,392 B each for 512 x 96: 4 KiB record set, 49,152 B MONO8, 6,144 B MASK1) in PS DDR **outside
  Linux's RAM**, newest N frames, overwritten oldest first. Registers (0x380 block): `STORE_BASE`, `STORE_RECORDS`, `STORE_MODE`
  (`CONTINUOUS | RING_ONLY`), read-only `STORE_RECORD_BYTES`, `STORE_STATE`, `STORE_HEAD_SEQ` (newest sequence started, -1 before
  the first), `STORE_FINAL_SEQ` (0x3BC: every sequence below it has set, image and mask written and acknowledged).
- **Reader rule.** `lo = max(0, HEAD + 1 - N)`; readable `lo <= seq < FINAL`. Copy the set header first, then the blocks; re-read
  HEAD as HEAD' and keep the copy only if `seq >= max(0, HEAD' + 1 - N)`. Compare the FRAME header before and after as a second
  check (it cannot replace the HEAD check: the new frame's pixels are written before its set replaces the header).
- **Frozen** = `STATE = IDLE` after STOP and `FINAL = HEAD + 1`, never earlier; `DRAINING` lasts until the open frame's record is
  complete (an ~84 ms watchdog bounds it). A coming ring-fault indication means "not frozen, re-arm needed": the reader treats
  `STATE = FAULT` that way today and gets the exact bit from the board owner.
- STOP handling (the board owner's final wording): after STOP the device reaches IDLE by itself in about 2 ms (the tap closes an open frame at the
  next SOF or after 2 ms without data, the frame marked bad), also when the sensor stream has stopped. Studio waits for IDLE for up to 1 s; if it does
  not come, the PL needs a restore ("restore needed"). Three sticky `STORE_STATE` fault bits hold until the next ARM or a hardware reset
  (RESET_GENERATION does not clear them): bit 9 `RING_STALLED` (an unacknowledged record left the tracking window: **ring invalid, re-arm, no
  playback**), bit 10 `STOP_STUCK` (the tap or its clock is broken; the device stays DRAINING; the records below FINAL are stable, so playback of
  the frozen part continues under the reader rule, flagged **"stop incomplete"**) and bit 11 (RESET_GENERATION was refused because the ring was
  not idle: invalid). Frozen = STATE IDLE after STOP with no bit set. A new ring starts with RESET_GENERATION **only in IDLE**, then ARM; the provider
  waits for IDLE (bounded) before it does either, and never writes RESET_GENERATION in another state. Ruling (coordinator, 2026-10-09): **RING_STALLED stays "ring invalid, no playback, re-arm"**, although the records below FINAL are stable. A stall means a DDR
  write was never acknowledged for 255 frames (a memory-path fault), so nothing in that window is trusted, and the readable range could hold frames far
  older than the Stop, which would mislead a "last second" review. STOP_STUCK stays readable and flagged.
- **Which STATE** (review of #674, checked against the RTL): the contract's STATE is the **bridge STATE register (0x040)**: IDLE, ARMED, RUNNING, DRAINING, FAULT. In ring-only
  mode `STORE_STATE` (0x3A0) never reads DRAINING (the drain is off), so it is used **only for its sticky fault bits 9, 10 and 11**. Frozen = bridge STATE IDLE and
  FINAL = HEAD + 1 with no fault. The RTL ignores ARM unless the bridge is IDLE with no fault, and refuses RESET_GENERATION outside IDLE, so a new ring is started by:
  leaving the previous run (STOP a leftover ARMED or RUNNING, wait for IDLE within the 1 s bound, FAULT_CLEAR a FAULT; a fault register that stays set or a bridge that
  never reaches IDLE refuses with "restore the PL"), RESET_GENERATION in IDLE when a sticky bit was left (verified by the generation), programming the ring,
  ARM, and a check that the bridge reads ARMED (otherwise the start fails with the reason, never "armed"). The claim "a ring is armed" is dropped only at the first
  register write, so a failure before it (placement, capability, an unreadable `/proc/iomem`) leaves an intact frozen ring readable. Without a ring wanted, a PL with the
  store gets STORE_MODE = 0 so a mode left by an earlier Studio is not latched.
- Beyond the board owner's rule, the reader (review of #673): a copy is also dropped when the registers changed under it (HEAD restarted below its
  snapshot, a new epoch or generation, a changed record count, base or state: a re-ARM, which the HEAD' window test alone cannot see because the head
  restarts and the old FRAME header still matches); the FRAME header's own sequence is not the store sequence (each record's wire sequence restarts at 0), so
  it is not used as a check. `frozen` and `stop incomplete` can both be true: STOP_STUCK on a ring that reached IDLE after all is the state where playback is
  allowed and flagged. IMAGE INCOMPLETE and FRAME INVALID are honoured: an incomplete mask is never shown as present, a cut frame and an invalid
  frame are flagged. The record set area comes from `STORE_SET_BYTES`; the registers (base, record size, set size, count) are validated before any
  read (BASE_HI = 0, base inside the DDR window and at or above Linux's RAM end, no wrap), so a wrong register cannot expose Linux's RAM; duplicate
  MONO8/MASK1 records, a first record that is not FRAME, image blocks inside the set area or beyond the record and oversize records are malformed.
- Limit: sequences are 32 bits with unsigned arithmetic; a ring-only run near 2^32 (about 9.9 days at 5 kHz) is refused with "sequence limit reached: re-arm"
  (no wrap support).
- Placement needs Linux's RAM end: an unknown end (unreadable or all-zero `/proc/iomem`, as without CAP_SYS_ADMIN) refuses the plan and the run.
- Sequences restart at 0 at every ARM. Read before re-arming.
- The PL enforces no DDR floor. Studio validates base and size: at or above Linux's RAM end (from `/proc/iomem`), at or above
  0x00100000, ending at or below 0x3F000000 (the PL's result ring and preview slots). Otherwise it refuses to arm, with a gate reason.

## Studio side

| Piece | Where | State |
|---|---|---|
| Placement, programming, status, freeze wait, frame read under the reader rule, record decode, browser packet | `PzFrameRing` (`include/backend/pz/PzFrameRing.h`) over `IRingIo` (registers + physical reads) | **done**, tested against a fake ring (`pz_frame_ring_test`) |
| `/dev/mem` implementation of `IRingIo`, ring size (`MIB_PZ_RING_FRAMES`), programming before ARM in the provider, freeze after STOP, `freezeRun`/`resumeRun`, the `ring.placement` and `run.frozen` gates | `PzDevMemExecutionProvider`, `AppBackend`, `ExperimentCoordinator` | **done** (part 2), tested with a fake provider |
| `fetch_ring_status`, `fetch_ring_frame {seq}` (binary `MIBR`), `ring_freeze`, `ring_resume` | bridge, contract, dispatch, TS | next |
| Playback panel: capacity in frames and seconds, scrub, step, play at a display fps, overlays Off / Mask / Contours / Both, the frame's cells; Save clip disabled with the reason | `desktop/src` | next |

**Capacity.** Frames = ring bytes / 59,392; seconds = frames / the sensor fps (`fetch_instrument_status.sensor.fps`, 5000.8 in Run).
5000 frames (the PC default) need 283 MiB, 1.0 s at 5 kHz, and `mem=` of about 720M in the bundle's `bootargs` (the board owner's
bundle change); with today's `mem=1008M` the gate says so and names the remedy.

**Stop and resume.** In Run, Stop = stop the live-results session (provider STOP), wait for frozen, LED off; the page shows the
playback. Resume re-arms (a new ring from sequence 0: the buffered frames are discarded, said before it happens) and turns the cell
path and the LED back on. Align, idle and shutdown also end the ring. A running experiment blocks Stop (stop the experiment
first). The freeze never claims frozen before `STATE = IDLE` and `FINAL = HEAD + 1`; a timeout or a fault is shown as "ring not
frozen: re-arm needed" and playback is not offered for that window.

**Packet `MIBR` v1** (`buildRingPacket`): a 48-byte header (sequence, frame id, timestamp ticks, tick Hz, frame flags, width, height,
cells, mask present, results truncated), the gray frame, the packed mask (1 bit per pixel, LSB first), then per cell the 18 words of
the `MIBC` run preview (payload words 0-14, `x | y << 16`, `width | height << 16`, valid), so the browser reuses the run-preview
decoder for the cell table and the overlays.

**Overlays.** Off, Mask (tint, as the Run preview), Contours (the mask's boundary pixels, drawn in the browser), Both; cells
coloured valid (green) or invalid (red) from the record's own VALID flag, with bounding boxes. A frame whose mask block was
`NO_RESULT` shows "no result for this frame" (the U-Net dropped it) and no mask.

## Tests

`processing.pz_frame_ring`: placement (5000 frames fit above 720 MiB; `mem=1008M` refused with the remedy; too many frames say how
many fit; never below the floor; `/proc/iomem` parsing), programming (registers written and read back; an invalid plan never
written), status (empty, running, DRAINING and "IDLE with FINAL short" not frozen, fault, no ring, base below the floor),
`awaitFrozen` (frozen after the open frame completes; a ring that never freezes times out and is reported not frozen), reading
(fields, blocks of the right sequence, cells; oldest and newest readable; HEAD - N and FINAL out of range; the packet), the
reader rule (HEAD' past the record drops the copy; a newer record survives; a header that changes between the two reads is
dropped), and degraded records (NO_RESULT mask, no FRAME, corrupted RESULT). Bridge, provider and UI tests come with their parts.
Hardware: the first results13 slot (board owner), see the open items.

## Open items

- results13 build 3 (the freeze fix and the fault bit); until then a frozen claim from build 2 is not trusted.
- The bundle's `mem=` for the default ring (board owner).
- The exact STOP_FAULT bit (board owner).
- Save clip (#667 S3b) reads the same frames through the same reader.
