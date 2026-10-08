## 2026-10-06 — PZ7035 live result statistics during a run (#501)

The Run status line shows what the PL is producing while an experiment runs: frames/s,
results/s and the EMPTY, INVALID and truncated fractions, plus ring overruns, decode errors and
frame gaps when any occur. The provider now counts those three frame outcomes from the FRAME
flags, and `fetch_instrument_status` carries a `results` block, so there is no new command and no
new bridge ABI number. See [[architecture/Rust-Bridge]], [[architecture/Desktop-Shell]].
