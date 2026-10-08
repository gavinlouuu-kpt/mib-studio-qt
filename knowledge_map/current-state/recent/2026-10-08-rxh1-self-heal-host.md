## 2026-10-08 — Host side of the PL receiver self-heal (results9 RXH1)

results9 carries a PL block that clears the sticky lane overflow flags itself (up to 8 automatic
receiver resets; diagnostics window `RXH1` at 0x40100400 = P[256]: tries, gave-up, auto-reset
count, per-lane counts and FIFO levels; reads 0 on results8). Studio detects it by the ID:
with the block present `awaitAlignLock` waits for the PL heal (up to 6 s, polling the block) and
sends no host reset; the operator sees "Align preview not locking: the receiver self-heal gave up
after N tries" only when the block reports gave-up (or never finishes). Without the block the host
recovery from #631 is unchanged. `link.rx_heal` in the instrument status shows present, gave-up,
tries and the auto-reset count; `fetch_run_accounting(last_run)` gains `receiver_auto_resets`, the
count's delta over the run (each auto-reset loses frames; null without the block), and the Run
outcome notice says "Receiver auto-resets during the run: N". Tested against a fake register map
only: results9 is not on the board yet. See [[Desktop-Shell]] and [[PZ7035-Records]].
