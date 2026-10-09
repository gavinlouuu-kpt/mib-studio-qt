## 2026-10-09 — The frame ring over the bridge (bridge ABI 34, #649 v1 part 3)

New commands: `fetch_ring_status` (and a `ring` block in `fetch_instrument_status`): available, reason, frozen, invalid, run_frozen,
capacity, the readable range, sensor fps; `fetch_ring_frame {seq}` (binary `MIBR`); `ring_freeze` (Stop in Run) and `ring_resume`
(controller only). `PzPlatformMonitor::sensorFps` reads the XVS period without a full sample. The provider leaves a ring that the last
STOP did not complete (DRAINING, RING_STALLED, STOP_STUCK) by RESET_GENERATION before the next ARM. `desktop/src/bridge.ts` gains
`fetchRingStatus`, `fetchRingFrame`, `ringFreeze`, `ringResume` and the `ring` type. The playback panel follows. Tested: the facade
view in `instrument_modes_test`, the off-PZ contract in `contract.rs`, the dispatch and server lists.
