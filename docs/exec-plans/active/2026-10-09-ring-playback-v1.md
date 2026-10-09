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
- Two sticky `STORE_STATE` bits hold until the next ARM (results13 fb1d858): bit 9 `RING_STALLED` (an unacknowledged record fell out of the
  256-entry tracking window; FINAL is frozen below it) and bit 10 `STOP_STUCK` (STOP did not complete in about 84 ms; the device stays
  DRAINING). Either one means "ring invalid": no playback is offered, the reason is shown, and recovery is stop, RESET_GENERATION, re-arm.
  Frozen = STATE IDLE after STOP with neither bit set.
  Recovery: RESET_GENERATION (CONTROL bit 3) written in DRAINING or with a sticky bit set, then ARM; the provider does this at the start
  of a run (before it reads the generation) and logs it. A third sticky bit (11) is named in the board owner's wording to come.
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
