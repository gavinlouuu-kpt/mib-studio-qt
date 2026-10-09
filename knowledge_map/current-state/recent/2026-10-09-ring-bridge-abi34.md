## 2026-10-09 — The frame ring over the bridge (bridge ABI 34, #649 v1 part 3)

New commands: `fetch_ring_status` (and a `ring` block in `fetch_instrument_status`): available, reason, frozen, invalid, run_frozen,
capacity, the readable range, sensor fps; `fetch_ring_frame {seq}` (binary `MIBR`); `ring_freeze` (Stop in Run) and `ring_resume`
(controller only). `PzPlatformMonitor::sensorFps` reads the XVS period without a full sample. The provider leaves a ring that the last
STOP did not complete (DRAINING, RING_STALLED, STOP_STUCK) by RESET_GENERATION before the next ARM. `desktop/src/bridge.ts` gains
`fetchRingStatus`, `fetchRingFrame`, `ringFreeze`, `ringResume` and the `ring` type. The playback panel follows. Tested: the facade
view in `instrument_modes_test`, the off-PZ contract in `contract.rs`, the dispatch and server lists.

The browser side: `Stop` in the Run status line (when a ring is available and no experiment runs) freezes the ring; the preview pauses and
`RingPlaybackPanel` takes its place: capacity in frames and seconds at the sensor rate, the buffered range, first / previous / play /
next / newest, a scrub slider, a display rate (1 to 60 fps), the overlay (Off, Mask, Contours, Both) and the frame's own cells with their
measurements decoded from the PL payload (`cellMetrics`, NaN shown as "—" where the PL left a word out). A frame whose mask was not
delivered says so; a ring that is invalid or empty shows the reason instead of controls; `Resume Run` re-arms (a new ring, said on the
button) and `Save clip` stays disabled with "Saving needs the SSD (not available yet)." until the SSD path (#667). The `MIBR` cell
record is 19 words: the `MIBC` words plus the payload validity mask.

Final STOP handling (board owner's wording): the status gains `stop_incomplete` (STOP_STUCK: not frozen, the records below FINAL stay
readable and the panel shows "Stop incomplete") and `restore_needed` (STOP never reached IDLE within 1 s: the PL needs a restore; no
playback, and the next run refuses to start with "restore the PL"). `invalid` is RING_STALLED, RESET_GENERATION refused (bit 11) or FAULT.
A new ring starts with RESET_GENERATION in IDLE only, then ARM.

The `MIBR` flags gain bit 2 (frame invalid), bit 3 (frame cut: the MONO8 block is INCOMPLETE) and bit 4 (mask incomplete); the panel says "This
frame was cut", "Invalid frame" and "The mask of this frame is incomplete" and never draws an incomplete mask.

An automatic FAULT_CLEAR at Run start never hides the fault (coordinator's condition): `PzFrameRing::quiesce` returns the FAULT register value it
cleared, the provider logs "PL fault 0x... cleared at Run start" and keeps it until the next start, the ring status carries `fault_cleared`, the Run status
line shows "PL fault 0x... cleared at Run start", the log gets it once, and Diagnostics lists it.

Review of #677: a camera mode switch while the Run is stopped is refused by the backend ("resume Run first"; the Camera & Alignment tab and a Run window change no
longer discard the frames the operator stopped to review: the tab says so in the log and switches after Resume); Stop is also refused in the backend when
the ring does not fit; a rejected cell's missing area shows "—" (the contour and hull areas are NaN when the PL left the word out); `fault_cleared_state`
reports a bridge FAULT cleared with the FAULT register at 0; Stop, Resume and the panel catch a rejected command (a viewer) and say so; the panel does not
fetch or set state after it is gone; `ring_freeze`/`ring_resume` are control commands and `fetch_ring_*` are not (server test); `fetch_ring_frame` rejects a
missing, negative, fractional or string `seq` (dispatch test).
