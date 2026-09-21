# Assets

> External datasets and model weights, hosted on the Hugging Face Hub and
> pinned in one manifest. Nothing binary is tracked in git.

**Source:** `env/assets.json` (manifest), `scripts/assets_manifest.py`
(stdlib reader), `scripts/provision-assets.py` (stdlib downloader),
`cmake/MIBOptions.cmake` (`MIB_ASSETS_DIR`), `cmake/MIBDependencies.cmake`
(`MIB_YOLO_MODEL_PATH` + configure-time check),
`cmake/MIBWindowsDeployment.cmake` (post-build copy).
**Related:** [[Dependencies]], [[Run-Modes]], [[../services/YoloService]],
[[../camera/MockCamera]], [[../services/ProcessingService]]

## The manifest

`env/assets.json` has one entry per asset: `id`, `kind` (`model` | `dataset`),
`repo` (Hub id), `revision` (commit SHA), `visibility`, `token_required`,
`required`, and either `files` (materialised by the provisioner, with optional
per-file `sha256`) or `viewer` / `hf_datasets` (fetched at use time by the
consumer). `consumers` lists what depends on it; `notes` says why.

| Asset id | Kind | Hub repo | Who uses it |
|---|---|---|---|
| `yolo11n-seg` | model, **required** | `gavinlouuu/mib-yolo11n-seg` (public) | `YoloService`; CMake fails configure if ONNX Runtime is found and the file is absent |
| `512x96stream-kin10` | dataset (viewer rows) | `gavinlouuu/512x96stream` (public) | `backend.kin10_hf_dataset_pipeline` (label `network`), `tools/kin10_*`, `tools/kin6_generate_hf_evidence.sh` |
| `512x96stream-mock-frames` | dataset (indexed TIFFs) | same | MockCamera folder, `mock_pipeline_timing_run`, `synthetic_condition_validation.py` |
| `z-adjustment-50v` | dataset, **private**, token | `gavinlouuu/z_adjustment-data` | real-corpus conformance (`run_processing_conformance.py --hdf5`) |
| `dc-ds` | dataset (`datasets` lib) | `gavinlouuu/dc_ds` (public) | `empty_frame_detection.py`, kedro pipeline |

## Provisioning

```bash
python3 scripts/provision-assets.py --list
python3 scripts/provision-assets.py --required-only            # what a build needs (default)
python3 scripts/provision-assets.py --public-only              # everything token-free
python3 scripts/provision-assets.py --asset 512x96stream-mock-frames --count 200
python3 scripts/provision-assets.py --check --required-only     # no network; exit 1 + fix command
```

Files land in `<root>/<kind>s/<id>/…` with a `provisioned.json` beside them.
`root` is `build/vendor/assets` (gitignored) or `MIB_ASSETS_DIR`, honoured
identically by CMake and Python. Downloads use `resolve/<revision>` URLs,
verify SHA-256 when the manifest declares one, and refuse to continue on a
mismatch (do not "fix" the pin without understanding why the Hub file
changed). Exit codes: 1 missing/mismatch, 2 needs `HF_TOKEN`, 3 network.

Private assets need `HF_TOKEN` (or the token saved by `hf auth login`). Public
assets never do. CI provisions `--required-only` before configuring on
Windows (`build-windows.yml`, `release.yml`, `python-wheel.yml` native job) and
`release.ps1` does the same locally.

## Consumers read the manifest, not literals

`scripts/assets_manifest.py` (`get_asset`, `asset_dir`, `resolve_url`) is the
only sanctioned way to name a Hub repo in code. `scripts/check_docs.py` fails
when a `gavinlouuu/<repo>` id appears under `scripts/`, `tests/`, `tools/`,
`docs/howto/` or `.github/` without a manifest entry.

## Bumping an asset

1. Upload the new files to the Hub repo (`hf upload <repo> <dir> .`).
2. Put the new commit SHA (and file SHA-256 for single-file assets) in
   `env/assets.json`.
3. Re-run the consumers (for the model: a Windows build + YOLO tests; for
   datasets: the `network` lane or the script), and land the manifest change
   with this note updated.

## Gotchas

- The runtime model path is unchanged: `<exe>/resources/models/yolo11n-seg.onnx`.
  Only the *source* moved (from a tracked file to the provisioned tree).
- `resources/models/` keeps the export script and a README; the weights are
  gone from git history going forward (old commits still contain them).
- Linux presets do not find ONNX Runtime, so the model check never fires
  there; `linux-network-test` is the only default-excluded lane.
- The Hub dataset `gavinlouuu/512x96stream` carries a stray
  `.claude/settings.local.json` (no credentials); remove it on the next
  dataset revision bump.
