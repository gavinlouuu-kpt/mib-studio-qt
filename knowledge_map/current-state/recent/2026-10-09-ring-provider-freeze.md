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

Review of #673/#674: the provider holds one mutex over the pre-ARM recovery, the programming and ARM (the status poll and a frame fetch take the same
mutex, so a Resume during a fetch cannot interleave); the reader gets Linux's RAM end from the provider (an unreadable or zeroed `/proc/iomem` refuses the
run); the `/dev/mem` ring map is read with aligned word loads (device memory takes no unaligned access) and DMBs around the HEAD' re-read; `stop()` takes the
restore-needed verdict from `awaitFrozen`, which owns the 1 s bound.

Review of #674 (a Claude review agent, checked against the RTL): the device STATE is the **bridge STATE (0x040)**, not `STORE_STATE`'s state code, which never
reads DRAINING in ring-only mode; `STORE_STATE` is now read only for its sticky bits 9 to 11, `frozen` is bridge IDLE with FINAL = HEAD + 1, and the provider
leaves a previous run through `PzFrameRing::quiesce` (STOP a leftover ARMED/RUNNING, wait for IDLE within 1 s, FAULT_CLEAR, RESET_GENERATION in IDLE when a sticky bit
was left, verified by the generation), programs the ring, ARMs and checks that the bridge reads ARMED (`awaitArmed`). The "ring armed" claim drops at the first
register write only, so a refused start keeps an intact frozen ring readable. `resumeRun` starts the live session first and switches the LED on only after it
started; `freezeRun` keeps the Run held when the LED write fails and reports a ring that did not freeze, stopped incompletely or faulted. `MIB_PZ_RING_FRAMES`
is parsed strictly (1 to 1,000,000, else logged and no ring); a PL with the store and no ring wanted gets STORE_MODE = 0; the preview arm takes the ring mutex;
the result ring is also read with word loads. Tests: `quiesce`/`awaitArmed` against a model of the RTL's STATE machine, and a failed Resume in `instrument_modes_test`.
The provider's glue itself (`/dev/mem` mapping, `start()`/`stop()` sequencing) is not unit-tested; it needs a board.
