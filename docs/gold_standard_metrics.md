# Gold Standard Metrics

Uniform JSON format for processing pipeline metrics so **mib-studio-qt** pipeline output can be compared against the **gold standard** (MIB-Studio legacy pipeline).

## File format

- **Schema**: [gold_standard_metrics.schema.json](gold_standard_metrics.schema.json) (JSON Schema draft 2020-12; machine-checkable version of this document).
- **Top-level fields**:
  - `version`: Schema version (currently `1`).
  - `contract_version`: Optional explicit portable-processing contract version
    (required in conformance references): `1` (subtract-ring, the default) or
    `2` (absdiff-laplacian). Each contract has its own references
    (ADR 0007). `scripts/compare_metrics.py` reads it: Contract-1 records must
    carry `ring_ratio`; Contract-2 records carry none and are compared on
    `laplacian_variance` when present.
  - `wheel_version`: Optional `mib-processing` package version that produced a
    conformance candidate.
  - `fixture` / `input_frame_count`: Optional conformance-fixture identity and
    source-frame accounting.
  - `pixel_to_micron`: Conversion factor (1 pixel = X microns). Required for area in µm².
  - `source`: Optional label (e.g. `mib-studio-qt`, `MIB-Studio-gold`).
  - `frames`: Array of per-frame metric objects.

## Per-frame fields (units and meaning)

| Field | Type | Units / meaning |
|-------|------|-----------------|
| `frame_type` | string | `"valid"` or `"invalid"` (passed validation or not). |
| `index` | integer | Frame index from acquisition (monotonic). |
| `timestamp_ns` | integer | Timestamp in nanoseconds. |
| `object_id` | integer | One-based object candidate within the source frame/ROI, ordered left-to-right then top-to-bottom; `-1` when no object candidate was selected. |
| `object_count` | integer | Number of object candidates emitted for the source frame/ROI. |
| `track_id` | integer | Stable batch track ID; `-1` for invalid/untracked records (optional). |
| `track_first_frame` / `track_last_frame` | integer | Source-frame span merged into the retained track (optional). |
| `track_observation_count` | integer | Number of detections merged into the retained track (optional). |
| `deformability` | number | 1.0 − circularity; dimensionless. |
| `area` | number | Hull area in **pixels**. |
| `area_um2` | number | Area in **µm²** (optional; = area × pixel_to_micron²). |
| `area_ratio` | number | Hull area / contour area; dimensionless. |
| `ring_ratio` | number | **Legacy Contract 1** focus metric: sqrt(outer_area − inner_area). Present in Contract-1 documents; omitted by Contract-2 documents. Optional. |
| `laplacian_variance` | number | **Contract 2** per-object focus metric: variance of the Laplacian over the detected object (`ProcessingScience::calculateLaplacianVariance`). Present in Contract-2 documents; omitted by Contract-1. `NaN` serialized as `null`. Optional. |
| `youngs_modulus` | number | Young's modulus (kPa) from `EModulusLut` bilinear lookup on (area_um, deformability) (optional; omitted when the lookup falls outside LUT coverage or no LUT was loaded). |
| `is_valid` | boolean | True if frame passed all validation checks. |
| `is_target_group` | boolean | Target-group/trigger classification (optional). |
| `touches_border` | boolean | True if contour touches image border. |
| `has_single_inner_contour` | boolean | True if exactly one inner contour (informational; acceptance uses at least one nested contour). |
| `in_range` | boolean | True if area within configured min/max. |
| `inner_contour_count` | integer | Number of inner contours. |
| `brightness_q1` | number | 25th percentile brightness in masked region. |
| `brightness_q2` | number | 50th percentile (median) brightness. |
| `brightness_q3` | number | 75th percentile brightness. |
| `brightness_q4` | number | 100th percentile (max) brightness. |
| `mask_sha256` | string | SHA-256 over mask dtype + shape + bytes for exact conformance (optional). |
| `series_images_sha256` | string[] | Ordered SHA-256 values for trigger + following multi-image-series frames (optional). |

Field names align with `FilterResult` and `BrightnessQuantiles` in [ProcessingService.h](../include/backend/processing/ProcessingService.h) and with the CSV header used by `export_hdf5.py` (snake_case in JSON).

## Portable Processing Contract (`contract_version`)

Any consumer that ports or reimplements the mib-studio-qt processing pipeline
(for example, Biowork's `services/mib-processing` runtime — see the
[Biowork portability epic](https://github.com/KPT1020/mib-studio-qt/issues/220))
must agree on three things: the **input config**, the **output metrics
shape**, and the **Young's-modulus LUT format**. `contract_version` is a single
number that names one frozen combination of all three, so a consumer can
check compatibility with one comparison instead of three.

For how a consumer actually *fetches* the pinned config, LUT, and processing
engine (the `mib-processing` Python wheel from `bindings/python/`) over
HTTPS with no Qt/app dependency, see
[portable-processing-sync.md](portable-processing-sync.md).

**`contract_version: 1`** (current) bundles:

| Piece | Version field | Defined by |
|---|---|---|
| Metrics JSON shape | `version: 1` (this document) | [gold_standard_metrics.schema.json](gold_standard_metrics.schema.json) |
| Processing config shape | `config_schema_version: 1` | [resources/defaults/config.json](../resources/defaults/config.json), `image_processing` block |
| Young's-modulus LUT format | tab-separated `area_um  deform  emodulus`, optional `# BEGIN METADATA` JSON header (`channel_width`, `flow_rate`, `fluid_viscosity`) | `EModulusLut::loadFromFile` ([EModulusLut.h](../include/backend/processing/EModulusLut.h)); example file under `resources/isoelastic_curve/` |

Bump `contract_version` whenever any of the three pieces changes in a way
that is not backward compatible (new required metric field, renamed config
key, changed LUT column order, etc.), and update all three version markers
together in the same change.

### `ProcessingConfig` contract (input)

The `image_processing` block of the app config JSON (`config_schema_version: 1`)
maps directly onto the C++ `ProcessingConfig` struct
([ProcessingService.h](../include/backend/processing/ProcessingService.h)).
A portable engine that wants byte-identical results must apply the same
fields with the same semantics:

| JSON key (`image_processing.*`) | `ProcessingConfig` field | Type | Meaning |
|---|---|---|---|
| `gaussian_blur_size` | `gaussian_blur_size` | int | Gaussian blur kernel size (pre-threshold smoothing). |
| `bg_subtract_threshold` | `bg_subtract_threshold` | int | Background-subtraction difference threshold. |
| `morph_kernel_size` | `morph_kernel_size` | int | Morphological close kernel size (`MORPH_CROSS`). |
| `morph_iterations` | `morph_iterations` | int | Morphological close iteration count. |
| `area_threshold_min` | `area_threshold_min` | int (µm²) | Minimum accepted hull area. |
| `area_threshold_max` | `area_threshold_max` | int (µm²) | Maximum accepted hull area. |
| `deformability_threshold_min` | `deformability_threshold_min` | double | Minimum accepted deformability. |
| `deformability_threshold_max` | `deformability_threshold_max` | double | Maximum accepted deformability. |
| `filters.enable_border_check` | `enable_border_check` | bool | Reject contours touching the ROI border. |
| `filters.enable_area_range_check` | `enable_area_range_check` | bool | Gate on `area_threshold_min/max`. |
| `filters.enable_deformability_range_check` | `enable_deformability_range_check` | bool | Gate on `deformability_threshold_min/max`. |
| `area_ratio_threshold_max` | `area_ratio_threshold_max` | double | Maximum accepted hull-area / contour-area ratio. |
| `filters.enable_area_ratio_check` | `enable_area_ratio_check` | bool | Gate on `area_ratio_threshold_max`. |
| `ring_ratio_min` | `ring_ratio_min` | double | Minimum accepted ring ratio (focus gate). |
| `ring_ratio_max` | `ring_ratio_max` | double | Maximum accepted ring ratio. |
| `filters.enable_ring_ratio_check` | `enable_ring_ratio_check` | bool | Gate on `ring_ratio_min/max`. |
| `filters.require_single_inner_contour` | `require_single_inner_contour` | bool | Require at least one nested (inner) contour. |
| `empty_frame_pixel_threshold` | `empty_frame_pixel_threshold` | int | Pixel-count threshold used to detect empty frames (auto-background). |
| `auto_background_enabled` | `auto_background_enabled` | bool | Auto-capture background after N empty frames. |
| `auto_background_empty_frames` | `auto_background_empty_frames` | int | Empty-frame count before auto-capturing background. |
| `auto_background_cooldown_frames` | `auto_background_cooldown_frames` | int | Frames to wait before re-arming auto-background capture. |
| `target_group.enabled` | `enable_target_group` | bool | Enable the second (target-group) gate within valid frames. |
| `target_group.area_min` / `area_max` | `target_group_area_min` / `target_group_area_max` | int (µm²) | Target-group area range. |
| `target_group.deformability_min` / `deformability_max` | `target_group_deformability_min` / `target_group_deformability_max` | double | Target-group deformability range. |
| `target_group.emodulus_enabled` | `enable_target_group_emodulus` | bool | Gate target-group membership on Young's-modulus LUT lookup. |
| `target_group.emodulus_min` / `emodulus_max` | `target_group_emodulus_min` / `target_group_emodulus_max` | double | Target-group Young's-modulus range (kPa). |
| `multi_image.enabled` | `multi_image_enabled` | bool | Capture a series of frames per valid detection. |
| `multi_image.count` | `multi_image_count` | int | Frames per series (1 = disabled). |
| `pixel_to_micron_factor` (top-level, not under `image_processing`) | passed separately to `computeProcessedFrame`/`processBatch` | double | Pixel→micron conversion; default `0.4886`. Also the `pixel_to_micron` field of this metrics format. |

### Processing pipeline (nested contours)

When `require_single_inner_contour` (or equivalent) is enabled, the pipeline keeps any object that has **at least one** nested (inner) contour. Batch metrics are computed per nested contour/object candidate. Frame-level realtime snapshots still expose the first selected object for compatibility. The field `has_single_inner_contour` is informational: it is true only when there is exactly one inner contour; acceptance is based on having at least one nested contour.

### Multi-object ROI records

Batch processing emits one metrics record per detected object candidate. Multiple records can therefore share the same `index` and `timestamp_ns`; use one-based `object_id` values from `1` to `object_count` to distinguish duplicate detections from the same source frame/ROI. Each object candidate is validated independently, so an edge-touching object can be rejected while another object in the same ROI remains valid.

Known failure modes:

- **Edge touch**: contours within two pixels of the ROI border are marked `touches_border` and rejected when border checking is enabled because the object may be cropped.
- **Overlapping halos**: touching or overlapping rings/halos can merge into one contour hierarchy. In that case the pipeline may emit fewer object records than the true object count or attach an inner contour to the wrong outer contour.

## Workflow (summary)

1. **Gold standard**: Run MIB-Studio pipeline → export CSV from saved data → convert CSV to gold-standard JSON with [scripts/convert_legacy_csv_to_json.py](../scripts/convert_legacy_csv_to_json.py).
2. **Qt pipeline**: Run mib-studio-qt → save experiment to HDF5 → export to gold-standard JSON with `export_hdf5.py --format json`.
3. **Compare**: Run [compare_metrics.py](../scripts/compare_metrics.py) on the two JSON files (with optional per-field tolerances).

## Always-on wheel conformance

Issue #225's anti-drift check is a standalone command:

```bash
python scripts/run_processing_conformance.py
```

It runs the **installed** `mib-processing` wheel over a deterministic
three-frame nested-contour fixture, hashes every output mask and multi-image
series frame, and compares metrics + hashes + target/tracking metadata against
[`scripts/gold_standard_dataset.json`](../scripts/gold_standard_dataset.json).
The command exits non-zero for missing/extra records or any value/hash drift;
`.github/workflows/python-wheel.yml` runs it after building and installing each
wheel. Fixture identity and input-frame count are also exact, so a candidate
from the wrong corpus window cannot pass. Biowork can invoke the same
entrypoint after installing its pinned wheel.

The reference uses a small generated fixture because experiment HDF5 files are
runtime data and must not be committed. After an intentional, reviewed
algorithm or contract change, regenerate it with:

```bash
python scripts/run_processing_conformance.py --update-reference
python scripts/run_processing_conformance.py
```

For an external frame corpus, either store an `N x H x W` uint8 array named
`frames` in an NPZ, or point the runner directly at a grayscale `N x H x W`
uint8 HDF5 dataset. HDF5 reads are bounded so a multi-gigabyte recording is not
loaded into memory:

```bash
python scripts/run_processing_conformance.py \
  --hdf5 path/to/recording.h5 \
  --hdf5-dataset /recorded_frames/images \
  --frame-offset 0 --frame-limit 3 \
  --fixture-id "stable-corpus-provenance" \
  --reference path/to/reference.json
```

Use `--frames-npz path/to/frames.npz` instead for an NPZ corpus. These are the
cross-repository hooks for local/Biowork data without checking recordings into
git.

## Z-adjustment real-corpus verification

The available real-corpus evidence is the private Hugging Face dataset
[`gavinlouuu/z_adjustment-data`](https://huggingface.co/datasets/gavinlouuu/z_adjustment-data),
approved as the replacement for issue #225's unavailable PANC1 recording. The
committed [`scripts/z_adjustment_50v_reference.json`](../scripts/z_adjustment_50v_reference.json)
pins eight frames starting at offset 500 of the in-focus recording at Hub revision
`fc62e3147fb0237e46e6eebd0fb09e669abef12f`; the source file's LFS SHA-256 is
`7ed2a721c85adc600688e32d4c4dbb7a58f0c725e351559ebc946eddab311bdc`.

The recording is declared in [`env/assets.json`](../env/assets.json) as the
private asset `z-adjustment-50v`. With `HF_TOKEN` set to a token that can read
the dataset, provision it (1.5 GB, SHA-256 verified) and verify the installed
wheel:

```bash
HF_TOKEN=... python3 scripts/provision-assets.py --asset z-adjustment-50v
python scripts/run_processing_conformance.py \
  --hdf5 build/vendor/assets/datasets/z-adjustment-50v/50V_in_focus/recording_20260511_160006.h5 \
  --hdf5-dataset /recorded_frames/images \
  --frame-offset 500 --frame-limit 8 \
  --fixture-id "hf:gavinlouuu/z_adjustment-data@fc62e3147fb0237e46e6eebd0fb09e669abef12f:50V_in_focus/recording_20260511_160006.h5#sha256=7ed2a721c85adc600688e32d4c4dbb7a58f0c725e351559ebc946eddab311bdc:/recorded_frames/images[500:508]" \
  --reference scripts/z_adjustment_50v_reference.json
```

The recording stays under the ignored `build/vendor/assets/` tree; only metrics
and exact mask/series hashes are committed. The private corpus is therefore a local
release-evidence lane, while the generated ring fixture remains the always-on
CI lane. Offset 500 was selected because this bounded window contains valid,
multi-object tracked detections and non-empty series payloads. Add
`--update-reference` only for an intentional reviewed refresh.

## Original PANC1 dataset

Issue #225 originally designated **PANC1 PDE3A CONTROL.h5** for quality-control
and regression testing. It was not available during implementation, but the
existing export path remains supported:

- **HDF5 path**: the recording is not published anywhere the team can fetch
  it (it was unavailable when #225 was implemented and is not in
  `env/assets.json`). If a copy is added to the private Hub dataset later,
  declare it as an asset and pin it there rather than referencing a local path.
- **Pixel-to-micron**: Use `-p 0.4886` (script default) unless the experiment uses a different value.

**Generate a metrics reference JSON** from a local copy of this HDF5:

```bash
python scripts/export_hdf5.py -i "path/to/PANC1 PDE3A CONTROL.h5" -o data/gold_standard_export --format json -p 0.4886
```

This writes `data/gold_standard_export/PANC1 PDE3A CONTROL_metrics.json` (collision-safe,
source-derived filename — same `<h5-basename>_metrics.<ext>` policy as `--format csv`).

**Compare a candidate** to that local PANC1 reference:

```bash
python scripts/compare_metrics.py "data/gold_standard_export/PANC1 PDE3A CONTROL_metrics.json" path/to/candidate_metrics.json
```

To compare against the committed deterministic wheel-conformance reference,
pass only the candidate:

```bash
python scripts/compare_metrics.py path/to/candidate_metrics.json
```

## End-to-end workflow (step-by-step)

### 1. Produce gold-standard metrics (MIB-Studio)

- Run the **MIB-Studio** (legacy) processing pipeline on your data so it saves results to disk (e.g. master files: `{condition}_images.bin`, `{condition}_masks.bin`, `{condition}_data.csv`, etc.).
- From the same saved data, run MIB-Studio’s metrics export so you get a CSV (e.g. the output of `calculateMetricsFromSavedData` or equivalent). That CSV may have columns like: `Batch`, `Condition`, `ImageIndex`, `Timestamp_us`, `Deformability`, `Area`, `RingRatio`, `Valid`, `Method`, `ProcessingConfig`.
- Convert that CSV to gold-standard JSON:
  ```bash
  python scripts/convert_legacy_csv_to_json.py -i path/to/metrics_output.csv -o path/to/gold_standard.json -p 0.4886 --source MIB-Studio-gold
  ```
  Use `-p` to set the pixel-to-micron factor if needed.

### 2. Produce candidate metrics (mib-studio-qt)

- Run **mib-studio-qt**, record an experiment (or use the same input as in step 1 if using playback), and save to HDF5.
- Export metrics from the HDF5 file to gold-standard JSON:
  ```bash
  python scripts/export_hdf5.py -i path/to/experiment.h5 -o path/to/qt_export --format json -p 0.4886
  ```
  This writes `path/to/qt_export/experiment_metrics.json` (source-derived filename) in the
  same gold-standard format.

### 3. Compare gold vs Qt output

- Run the comparator with the gold-standard file as reference and the Qt export as candidate:
  ```bash
  python scripts/compare_metrics.py path/to/gold_standard.json path/to/qt_export/metrics.json
  ```
- Optionally set per-field tolerances (e.g. for floating-point differences):
  ```bash
  python scripts/compare_metrics.py gold_standard.json qt_export/metrics.json --tolerance deformability 0.001 --tolerance area 0.01 -o report.txt
  ```
- The script prints (or writes with `-o`) a summary: frame counts, matched vs missing, per-field failure counts, and max/mean deltas for numeric fields. Exit code 0 only if all frames match and no differences exceed the tolerances.
