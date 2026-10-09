## 2026-10-09 — Stop in Run freezes the frame ring; the devmem provider arms it (#649 v1, part 2)

`PzDevMemExecutionProvider` reads `MIB_PZ_RING_FRAMES` (0 or unset: no ring). With a ring wanted, every ARM first checks the
placement against Linux's RAM (`/proc/iomem`, as root), the PL image's FRAME_STORE capability (results13) and programs
`STORE_BASE/RECORDS/MODE`; a ring that does not fit refuses the run with the reason, and the readiness list shows a blocking
`ring.placement` gate. `stop()` then waits for the device's frozen state (STATE IDLE, FINAL = HEAD + 1, neither sticky bit) and logs
the outcome; the Align preview arm drops the ring. `AppBackend::freezeRun` (Run only, no experiment or recording, a ring wanted)
stops the live session, switches the LED off and holds the Run (`runFrozen`); `resumeRun` re-arms (a new ring from sequence 0) with the
LED Run preset; a mode switch or idle ends a stopped Run. A stopped Run blocks an experiment (`run.frozen` gate).
`ringStatus`/`ringFrame` expose the provider's ring (a frame as a `MIBR` packet). The bridge commands and the playback panel
need bridge ABI 34 and come next. Tested with a ring-capable fake provider (`instrument_modes_test`).

Final STOP handling (board owner's wording): `stop()` waits up to 1 s for IDLE (the device gets there in about 2 ms by itself); a stop
that never reaches IDLE without a sticky bit marks `restoreNeeded` (a broken tap or clock; the PL needs a restore). A new run waits for
IDLE (bounded; otherwise the start fails with "restore the PL"), writes RESET_GENERATION only in IDLE and only when a sticky fault bit
(9, 10 or 11) was left, then ARMs; it never writes RESET_GENERATION in another state.
