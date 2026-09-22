# Focus/core screening benchmark

The offline API reuses native `BundledProcessingKernel` and
`ProcessingScience`; it is not a Python reimplementation of microscopy
metrics. See the [execution plan](../exec-plans/active/2026-09-22-focus-benchmark.md).

## Build and run

```bash
uv venv .venv
uv pip install --python .venv/bin/python ./bindings/python h5py pillow pyarrow pytest opencv-python-headless
.venv/bin/python scripts/prepare_focus_benchmark.py \
  --source-root '/path/to/Cells in different focus' \
  --output /path/to/new/dataset --samples-per-recording 256
.venv/bin/python scripts/run_focus_benchmark.py \
  --dataset /path/to/new/dataset --output /path/to/new/dataset/benchmark
.venv/bin/python -m pytest scripts/test_prepare_focus_benchmark.py bindings/python/tests
```

Use a mounted bulk-data filesystem for the dataset. Sources are opened
read-only; output must be a new directory outside the source tree. No repair
is attempted. For hosts with conflicting Homebrew/system fmt packages, select
consistent system CMake dependencies via `CMAKE_ARGS`; do not edit system libraries.

## Dataset contract

Parquet shards contain lossless image bytes with Hugging Face Image metadata,
source-relative recording name, filename-derived voltage, cell line, frame
index, timestamp, raw-pixel SHA-256 and background path. `manifest.json`
records source shape/size/mtime, exact sample and background selection policy,
retained/duplicate/read-failure counts, exclusions and artifact SHA-256 hashes.
Source HDF5 files are not fully scanned/hashed: only selected frames are
checked. Do not claim full-source integrity.

Sampling is score-independent. Empty and blurred frames still present in the
sources are retained. Some acquisition files report discarded empty frames;
these counts are included in the card/manifest. The benchmark cannot recover
those controls or claim an unbiased empty-frame sample. No saved ROI was found
in the inspected recording, so the declared baseline analyzes the full frame;
channel-wall artifacts can appear among candidates.
Backgrounds are estimates from disjoint temporal-median frames, not acquisition
backgrounds. All frames form one `screening` split. Do not train/tune on this
split and then describe it as held-out validation. Dataset README contains the
filtering metrics table and limitations. Excluded-file details are in the manifest.

## Metric definition

`calculateLaplacianVariance(image, contour)` uses raw uint8 intensities, a
filled selected contour, one-pixel clipped context, and OpenCV
[Laplacian](https://docs.opencv.org/4.0.1/d4/d86/group__imgproc__filter.html)
with `CV_64F`, `ksize=1`, scale 1, delta 0, `BORDER_REFLECT_101`.
Convolution happens before masking; masked `meanStdDev` squared gives
population variance. Invalid/out-of-bounds/degenerate supports yield NaN.
The context affects boundary derivatives; it is excluded from statistics.
This is not a claim that boundary pixels are mathematically independent of
their immediate neighbors. No image normalization, binary-mask Laplacian,
or blur prefilter is applied to focus scoring.

Nested candidates use the inner contour; outer-fallback candidates use the
selected top-level contour. There is no Laplacian call when no object exists.
An optional score vector in native science preserves result ordering and
leaves all historical fields untouched.

The historical field `ring_ratio` is actually `sqrt(outer_area-inner_area)`
in pixel-space, not a physical radial wall thickness or dimensionless ratio.
Unavailable ring metrics are JSON null in the benchmark. Do not silently
reinterpret historical thresholds or compare raw ring and Laplacian units.

## Reading results

Four variants: subtract/ring, absdiff/ring, subtract/Laplacian, absdiff/Laplacian.
All use identical input, background and shared downstream settings. The
Laplacian variants **retain ring diagnostics** to support paired comparison.
They are not complete Contract-2 implementations or production migrations.

The runner disables uncalibrated area/focus/deformability gates, keeps border
checks, and permits outer-contour fallback. Thus `valid_objects` means only
the configured checks passed, not independently confirmed cells or focus.
`metrics.csv` contains aggregate and per-recording counts/coverage/medians
and native processing p50/p95/p99. `paired_objects.csv` matches boxes by greedy
IoU >= 0.3 and records absdiff-minus-subtract deltas. These are correspondence
statistics, not detection accuracy. Always inspect unmatched candidates too.

Timing uses native steady-clock mask+science, one OpenCV thread, three warmups
per variant/recording and rotating variant order. I/O and Python conversion are
excluded. Timing is not a hardware capture-to-sort measurement. `results.json`
records configuration, native-library hash, versions and dataset hash.

## Publication

Upload only after extraction and evaluation succeed, to a private dataset
repository. Use a supported credential provider/protected environment; never
include tokens in commands, logs, dataset files or git. Verify the remote
commit and artifact inventory before reporting publication complete. The helper
`scripts/publish_focus_benchmark.py --dataset <path> --repo <owner/name> --dry-run`
validates the exact upload allowlist without contacting HF. Remove `--dry-run`
only in an authenticated protected environment; the helper enforces a private,
empty destination and verifies remote revision inventory/byte counts. Install
`huggingface_hub` in that environment for publication. MLflow
performance logging requires configured task credentials; retain the same
local results when tracking is unavailable and report the gap explicitly.
