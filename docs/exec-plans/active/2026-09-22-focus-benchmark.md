# Same-input focus and difference-policy screening benchmark

Status: blocked (publication authentication only)

## Goal

Implement an opt-in native subtract/absdiff comparison with object-local
Laplacian variance, and freeze a filtered NAS focus-sweep sample for private
Hugging Face publication. Supports issues #423 and #299; does not complete
Contract 2 or replace production autofocus/validation.

## Decisions

- 2026-09-22: Preserve the shared checkout; use `feat/423-focus-benchmark`.
- Preserve all focus levels and empty-frame controls. Uniformly sample up to
  256 frames per recording, exclude exact within-recording pixel duplicates,
  unreadable frames/files, and derived remasked/processed files. Do not use
  ring or Laplacian scores to decide dataset inclusion.
- Use 65 disjoint frames per recording for a temporal-median estimated
  background; record all indices. Do not call this a measured background.
- Use raw Gray8, filled selected-contour statistics, unmasked convolution,
  CV_64F Laplacian ksize=1, scale=1, delta=0, REFLECT_101 and population
  variance. Degenerate/invalid contours return NaN (JSON null). A one-pixel
  context participates in derivatives, not the statistics mask.
- Compare four variants using the same legacy downstream object science.
  Laplacian variants retain ring diagnostics; latency measures additive
  metric overhead, **not** the final ring-free successor.
- Preserve desktop defaults, persisted result schema, ABI and autofocus.
  Expose only additive offline Python/C++ entry points.
- Publish privately under `gavinlouuu/mib-cells-different-focus-benchmark`.
  Credentials never enter dataset artifacts, logs or source control.

## Acceptance/progress

- [x] Inventory source files and prior processing harnesses.
- [x] Implement opt-in difference kernels and shared native focus metric.
- [x] Verify synthetic blur/inversion/support/legacy invariants and dataset
  round-trip, read failure, deduplication and artifact tamper handling.
- [x] Extract frozen screening dataset and hashes; include filtering table (5,888 frames / 23 recordings).
- [x] Run all four variants; include aggregate/per-recording metric tables,
  per-object results, paired differences, and latency distributions.
- [ ] Publish private dataset and verify remote revision/file inventory.

## Open limits

Some recordings report acquisition-time empty-frame rejection; those counts
are preserved and prevent claims of unbiased empty-frame coverage. No recorded
channel ROI is available in the inspected source.

The saved Infisical CLI profile currently reports no valid login session.
No task-scoped protected HF credential is available yet. Local preparation
and evaluation continue independently; publication must not be reported as
complete without an authenticated Hub result.

Focus/segmentation labels, calibration, independent specimen/session IDs and
numeric acceptance criteria are not available. Accordingly, precision,
recall, focus false acceptance/rejection, calibrated errors, held-out claims,
and production go/no-go are unavailable, not assumed passes. This is a
screening comparison, not full validation or a replacement for issue #423.

## Verified local result

- 5,888 unique sampled frames, 23 recordings; 6 file exclusions (5 unreadable
  HEK293 sources and one remasked derivative). All 5,888 sampled frames
  decoded with matching pixel hashes during evaluation.
- Four variants completed; 25,410 greedy-IoU candidate pairs.
- Native p50 microseconds: subtract/ring 420.386, subtract/Laplacian 435.643,
  absdiff/ring 479.068, absdiff/Laplacian 498.032. This is one host screening
  run, not sorting latency or an acceptance threshold.
- 31 pytest tests; existing native science golden and seam tests; documentation
  and screenshot checks passed.
- Private-upload dry run validated 53 allowlisted files (~816 MB). No upload
  or remote revision verified. MLflow credentials also unavailable; local
  performance artifacts are retained rather than reporting a tracking success.
