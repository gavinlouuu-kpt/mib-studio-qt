## 2026-10-07 — The Review tab shows how a run ended; loss fractions use the admitted frames (#549, bridge ABI 31)

YOFO Studio's Review tab now shows the outcome saved in the opened file, the same notice as after a
run: declared partial result with the malformed count, undeclared loss, a failure, or a quiet note for
a raw recording or an older file without accounting. A file whose counters do not reconcile reads as a
failure.

New read-only command `fetch_run_accounting(source)` (`review` = the file loaded for review,
`last_run` = the run that finished last in this session) returns the reconciled accounting: the
completion and its reason, `admitted` (the frames the run claimed) and every loss counter. With it the
loss fractions use the admitted frames: the status alone only knew the rows it saved, so a run with
many empty frames overstated its loss (0.004 % of 27,162 rows instead of 0.002 % of 50,407 frames, for
the 10 s PZ7035 run). `ExperimentCoordinator` keeps the last run's accounting for this.

The standalone YOFO Review app has its own contract (`review-contract.json`) and still shows only
the text summary; it is not changed here. See [[Rust-Bridge]],
[[Desktop-Shell]].
