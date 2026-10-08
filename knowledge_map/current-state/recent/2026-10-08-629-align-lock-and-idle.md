## 2026-10-08 — Align ingress recovery and the unattended safe state (#629)

Run to Align sometimes never published a preview on the PZ7035: P[13] bits 15:8 (per-lane FIFO
overflow flags, sticky, OR'd into every frame's bad flag) stayed set, so the bridge counted every
frame as lost. Measured on the board (2026-10-08): only a receiver reset clears them, and a 100 ms
hold of P[8] bit 6 does it where a 100 µs pulse and the rx buffer clear (bit 5) do not reliably.
`pz::awaitAlignLock` (`AlignLock.h`, hooks, tested without a board) now waits 1 s for the first
preview; with the flags set it holds the receiver reset 100 ms (bit 4 kept), waits 500 ms and
re-checks, up to four times, logging and counting each attempt (`mode.align_lock`
`receiver_clears`, `failures`), then fails with "Align preview not locking: ..." and the readings.
Hold time and attempt count are `AlignLockPolicy` constants. Without the flags a slow start keeps the old 12 s wait.

When the last client has been gone for the server's grace time (5 s), `stop_and_save` also puts an
instrument in Align or Run into its safe state: `set_instrument_mode idle` turns the LED and the
cell path off and releases the camera (the sensor keeps its last timing). `mode.idle` is true until
the next switch, which the UI treats as a reason to re-enter the tab's mode. Additive within
bridge ABI 32. See [[Desktop-Shell]] and [[Rust-Bridge]].
