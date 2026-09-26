# Microscopy Pipeline

> What the app is actually measuring and why. Read alongside
> [[../services/ProcessingService]] and [[../architecture/Data-Flow]].

## What the app does

Microfluidic deformability cytometry: cells flow through a microfluidic
channel, a high-speed camera captures their passage, and software extracts
per-cell mechanical properties (deformability, area, stiffness).

## Per-frame work

1. **Acquire** — camera pushes an 8-bit grayscale image into
   [[../data-model/FrameStore]]. Frame rate is typically hundreds to
   thousands of FPS (EGrabber + fast CoaXPress camera).
2. **Background subtract** — optional; enabled via
   `ProcessingService::setRealtimeBackgroundGray`. Can be auto-captured
   after N consecutive empty frames.
3. **Segment** — Gaussian blur → fixed-threshold → morphological open/close
   → `cv::findContours`.
4. **Classify** — each contour is scored by the `ProcessingConfig` gates:
   - Border-touching? → invalid
   - Single inner contour required?
   - Area in `[area_threshold_min, area_threshold_max]` (μm²)
   - Deformability in range
   - Ring ratio in range (for focus check)
5. **Metrics** — `FilterResult` captures:
   `area`, `deformability`, `areaRatio`, `ringRatio`, `youngsModulus` (LUT),
   `brightness` quantiles.
6. **Target-group gate** — valid frames pass a second gate; matching
   frames fire a camera trigger pulse via [[../services/TriggerService]].

## Multi-image series mode

When `ProcessingConfig::multi_image_enabled` is true, each valid detection
captures `multi_image_count` consecutive frames (the "trigger" frame plus
subsequent ones). Metrics are computed only from the first frame.
`ProcessedFrame::seriesImages` carries the series; stored as 4D
`(N, seriesCount, H, W)` in HDF5.

## Focus feedback

`ring_ratio` is fed to [[../services/AutofocusService]] which drives a
piezo nanopositioner to keep the cell in focus across the channel.

## See also

- [[Glossary]] for term definitions, including **portable processing
  contract** / **`contract_version`**.
- `docs/gold_standard_metrics.md` — JSON format for comparing this
  pipeline's outputs against the legacy pipeline, and the frozen
  `contract_version` a portable (non-Qt) engine must match. Backed by
  `docs/gold_standard_metrics.schema.json`, `scripts/export_hdf5.py
  --format json`, `scripts/convert_legacy_csv_to_json.py`, and
  `scripts/compare_metrics.py`.
- `scripts/run_processing_conformance.py` — always-on installed-wheel parity
  check. It compares metrics, target/tracking metadata, and SHA-256 evidence for
  masks + multi-image series against `scripts/gold_standard_dataset.json`
  (synthetic). `--fixture-npz scripts/conformance/focus-50v-real.npz --reference
  scripts/conformance/focus-50v-real-contract1.json` does the same on real
  frames: 13 frames of the 50 V cells-in-different-focus recordings (C2C12,
  HEK293, HeLa, PANC-1), each processed with its recording's background and
  Contract-1 config (built by `scripts/build_real_conformance_fixture.py`). Both
  run in the wheel CI. References and fixtures change only in a PR labelled
  `gold-reference-change` (`.github/workflows/gold-reference-guard.yml`;
  owners in `.github/CODEOWNERS`).
- `scripts/empty_frame_detection.py` — offline Python pipeline (Kedro +
  MLflow at `mlflow.yofo.bio`). Not part of the Qt app's runtime.
