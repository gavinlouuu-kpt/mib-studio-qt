## 2026-10-08 — Coalesce experiment flushes under writer backpressure (#597)

Confirmed the callback-driven queue-overflow regression on `aa1ff340`: with
5,000 fps, 512x96 image/mask pairs, interval 100 and a 150 ms writer stall,
submission fails at 500 admissions (~100 ms), despite buffer headroom.
Serialize the capacity check and buffer handoff in
[[services/ProcessingService]]; a full queue retains frames for a later larger
batch. Existing buffer-capacity and HDF5-write errors remain explicit.
[[architecture/ExperimentCoordinator]] retains its Stop queue/remainder drain.

Regression uses real HDF5 writes with 25 ms delay per batch plus one 150 ms
stall: all 10,000 admissions persist with no losses. A permanently stalled
writer fails at buffer capacity (1,401 admissions; 1,400 committed plus one
cancelled after cleanup), with reconciled persistence accounting.

The stronger hypothesis that pre-#597 necessarily passed these exact settings
is unsupported: a diagnostic model of its 250 ms polling cadence persists
8,000/10,000 frames, cancelling 2,000 through the unchanged 1,000-frame buffer
cap. This is a cadence comparison, not a full historical executable or ARM
hardware measurement. No Rust bridge ABI or Qt frontend code changes.

Validation: new regression passes five consecutive runs; all four
`e2e.recording_403_*` tests pass, and `check_docs.py` passes. The broader
experiment/processing/recording/accounting/HDF regex has 77 passes, four skips
and six unrelated test-environment failures (loopback server, Qt settings,
Python interpreter missing NumPy). The Python conformance script passes with
system Python. TSan builds, but sanitizer validation is blocked by runtime
`unexpected memory mapping` startup failures; CI TSan remains required.
