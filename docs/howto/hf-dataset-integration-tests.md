# Hugging Face Dataset Integration Tests

`backend.kin10_hf_dataset_pipeline` validates the async batch processing path
against the public Hugging Face dataset `gavinlouuu/512x96stream`.

The dataset id, config, split and default sample rows are declared once in
[`env/assets.json`](../../env/assets.json) (asset `512x96stream-kin10`); both
harnesses (`tools/kin10_run_hf_dataset_test.py` and `.sh`) read them from
there via `scripts/assets_manifest.py`. The test carries the CTest label
`network`: the Linux test presets exclude it, `linux-network-test` runs only
network-labelled tests, and `build-windows.yml` runs `ctest -LE network`.

The test runner downloads a small, stable sample set through the Hugging Face
Dataset Viewer API. It does not require `HF_TOKEN`, `huggingface-cli`, or manual
login for the public dataset.

The runner retries remote requests five times. If the public service still
returns HTTP 429/5xx or a connection/timeout error, it exits 77 and CTest marks
the network integration as skipped. Dataset schema, row, image, and processing
regressions remain hard failures.

## Run Locally

Windows Debug:

```powershell
cmake --preset windows-default
cmake --build --preset windows-default-build --target kin10_hf_dataset_pipeline_test
ctest --preset windows-debug-integration-test -R backend.kin10_hf_dataset_pipeline --output-on-failure
```

The Windows CTest runner writes downloaded frames, sample overlays, and
`metrics.json` under:

```text
build/test-output/kin10_hf_dataset_pipeline/
```

Linux:

```bash
cmake --preset linux-backend-only
cmake --build --preset linux-backend-only-build --target kin10_hf_dataset_pipeline_test
ctest --preset linux-network-test -R backend.kin10_hf_dataset_pipeline
```

CTest writes downloaded frames, sample overlays, and `metrics.json` under:

```text
build/linux-backend/kin10_hf_dataset_pipeline/
```

To regenerate the same harness output in a review bundle:

```bash
tools/kin10_run_hf_dataset_test.sh \
  review_artifacts/KIN-10/hf_dataset_pipeline \
  build/linux-backend/kin10_hf_dataset_pipeline_test
```

Cross-platform equivalent:

```powershell
python tools/kin10_run_hf_dataset_test.py `
  --out-dir review_artifacts/KIN-10/hf_dataset_pipeline `
  --binary build/Debug/kin10_hf_dataset_pipeline_test.exe
```

## Sample Selection And Cache

By default the runner uses the rows listed in `env/assets.json`
(`0,1,2,2500,4999` from config `default`, split `train`). The rows are cached in the selected output directory:

```text
<output-dir>/cache/hf_row_00000.jpg
<output-dir>/cache/hf_row_00001.jpg
...
```

Set `KIN10_HF_ROW_INDICES` to exercise a different public sample set:

```bash
KIN10_HF_ROW_INDICES=0,10,20,100,4999 \
  ctest --preset linux-backend-only-test -R backend.kin10_hf_dataset_pipeline --output-on-failure
```

On PowerShell:

```powershell
$env:KIN10_HF_ROW_INDICES = "0,10,20,100,4999"
ctest --preset windows-debug-integration-test -R backend.kin10_hf_dataset_pipeline --output-on-failure
Remove-Item Env:\KIN10_HF_ROW_INDICES
```

## Regression Signal

The C++ harness drives `ProcessingService::startBatchPipeline()` and
`enqueueBatchFrame()` over the downloaded frames. It fails with explicit stderr
messages when:

- the dataset split or requested rows are not available,
- image dimensions differ from the expected `512x96` stream frames,
- accepted, processed, or dropped frame counts regress,
- any default sample stops producing a non-empty mask or contour,
- per-sample area or deformability metrics leave expected image-space ranges.

On both pass and fail, the harness writes:

- `metrics.json` - aggregate counters and per-sample metrics,
- `samples/<sample-id>-input.png` - downloaded HF input frame,
- `samples/<sample-id>-mask.png` - processed batch mask,
- `samples/<sample-id>-overlay.png` - contour overlay for reviewer inspection.
