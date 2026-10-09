## 2026-10-09 — The results13 frame-ring reader core (#649 v1, part 1)

`PzFrameRing` (`include/backend/pz/PzFrameRing.h`) is the host side of the every-frame ring: it plans the ring's placement (above
Linux's RAM from `/proc/iomem`, at or above the 0x00100000 floor, below the PL's 0x3F000000 area), programs `STORE_BASE`,
`STORE_RECORDS` and `STORE_MODE` (CONTINUOUS | RING_ONLY) before ARM, reports status, waits for the frozen state (STATE = IDLE and
FINAL = HEAD + 1, never earlier; a fault is never frozen), and copies a frame under the board owner's final reader rule (re-read HEAD'
after the copy and drop it if the ring moved past it; the FRAME header compared before and after). `buildRingPacket` makes the `MIBR`
v1 packet for the browser. `encodeImageRecord` joins the record encoders. The provider, bridge commands and the playback panel follow;
the plan is `docs/exec-plans/active/2026-10-09-ring-playback-v1.md`. Tested against a fake ring (`processing.pz_frame_ring`); no board yet.
