# Focus/core benchmark — 2026-09-22

## Scope

Issue #423 screening work, reusing shared native processing science and
OpenCV. The user's NAS focus recordings are read-only sources. No default
algorithm, HDF5 contract, native plugin ABI or autofocus migration.

## Implementation

- Native `calculateLaplacianVariance` and optional ordered per-object scores.
- Offline subtract/absdiff factory and Python benchmark API.
- `scripts/prepare_focus_benchmark.py`: deterministic sampling, exact pixel
  deduplication, disjoint median backgrounds, Parquet Image features,
  source/artifact provenance, exclusions and filtering table.
- `scripts/run_focus_benchmark.py`: same-input four-variant comparison,
  per-recording/aggregate tables, per-frame candidates and IoU-matched deltas.
- Synthetic scientific invariants plus dataset round-trip/fault tests.

## Status and limits

Local extraction/evaluation complete: 5,888 frames / 23 recordings, all four
variants, 25,410 matched candidate pairs. 31 pytest tests, native science
golden/seam tests and docs/screenshot checks pass. Upload dry-run validates
53 artifacts (~816 MB). Infisical CLI reports no valid login session; no
private HF publication or MLflow tracking has been verified. Some sources
filtered empty frames at acquisition; counts are recorded in the dataset.
A full-frame baseline is used because no saved channel ROI was found. No independent
focus/segmentation annotations or optical calibration are available.
This is a screening dataset, not held-out scientific qualification.

See [runbook](../../docs/howto/focus-core-benchmark.md) and
[execution plan](../../docs/exec-plans/active/2026-09-22-focus-benchmark.md).
