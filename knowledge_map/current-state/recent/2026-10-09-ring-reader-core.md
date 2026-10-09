## 2026-10-09 — The results13 frame-ring reader core (#649 v1, part 1)

`PzFrameRing` (`include/backend/pz/PzFrameRing.h`) is the host side of the every-frame ring: it plans the ring's placement (above
Linux's RAM from `/proc/iomem`, at or above the 0x00100000 floor, below the PL's 0x3F000000 area), programs `STORE_BASE`,
`STORE_RECORDS` and `STORE_MODE` (CONTINUOUS | RING_ONLY) before ARM, reports status, waits for the frozen state (STATE = IDLE and
FINAL = HEAD + 1, never earlier; a fault is never frozen), and copies a frame under the board owner's final reader rule (re-read HEAD'
after the copy and drop it if the ring moved past it; the FRAME header compared before and after). `buildRingPacket` makes the `MIBR`
v1 packet for the browser. `encodeImageRecord` joins the record encoders. The provider, bridge commands and the playback panel follow;
the plan is `docs/exec-plans/active/2026-10-09-ring-playback-v1.md`. Tested against a fake ring (`processing.pz_frame_ring`); no board yet.

STOP handling follows the board owner's final wording: after STOP the device reaches IDLE by itself in about 2 ms, so the freeze wait is
bounded at 1 s ("restore needed" past it); `RING_STALLED` (bit 9) and `RESET_GENERATION` refused (bit 11) make the ring invalid, while
`STOP_STUCK` (bit 10) leaves the records below FINAL readable under the reader rule, flagged "stop incomplete" (`stopIncomplete`). The
sticky bits hold until the next ARM; a new ring starts with RESET_GENERATION in IDLE only.

Review of #673 (a Claude review agent): `planRing` and `PzFrameRing` now require Linux's RAM end (unknown or zeroed `/proc/iomem` refuses); a copy
is dropped when the epoch, generation, record count, base or state changed under it or the head restarted (a Resume during a fetch); `status()`
validates every register before any read (BASE_HI, window, Linux's end, no wrap, `STORE_SET_BYTES`); IMAGE INCOMPLETE and FRAME INVALID are carried in
the `MIBR` flags (bits 2 to 4) and an incomplete mask is never shown as present; `awaitFrozen` owns the 1 s bound and sets `restoreNeeded`;
duplicate and out-of-place records are malformed; a 32-bit sequence near the limit is refused. `frozen` with `stopIncomplete` is the intended state for
STOP_STUCK on a ring that reached IDLE.
