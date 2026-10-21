## 2026-10-22 — The wheel's compute_processed_frame takes pixel_to_micron (#779)

Follow-up to #764 ([[services/ProcessingService]], [[services/BatchMaskSources]]).

**Wheel.** `compute_processed_frame(..., pixel_to_micron=None)` now maps the
factor onto `ProcessingService::BatchAnalysisInputs`, as `process_batch` has
since #764: a given factor decides area validity and the target group as well
as `area_um2` (before, it was used only for `area_um2` and validation ran at a
fresh service's 0.4886). Omitted, 0.4886 for both, as before. A factor that is
not finite and > 0 raises `ValueError`. No other arguments; no version bump
(the wheel version is the processing-core release identity, bumped at
release).

**Core.** `ProcessingService::computeProcessedFrame` gains a public overload
with a trailing `const BatchAnalysisInputs&`, which checks the factor as
`processBatch` does and runs the existing per-call path. The original
signature is unchanged.

Tests: the wheel's `TestComputeProcessedFrameCalibration` (a factor that
changes validity, one that changes the target group, agreement with
`process_batch`, the default unchanged,
invalid factors refused).
