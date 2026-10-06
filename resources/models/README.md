# Models

The YOLO11n-seg weights are **not tracked in git**. They are hosted on the
Hugging Face Hub at
[`gavinlouuu/mib-yolo11n-seg`](https://huggingface.co/gavinlouuu/mib-yolo11n-seg)
(ONNX export, source `.pt` checkpoint and this export script) and pinned by
commit and SHA-256 in [`env/assets.json`](../../env/assets.json) under the asset
id `yolo11n-seg`.

```bash
python3 scripts/provision-assets.py --asset yolo11n-seg
# -> build/vendor/assets/models/yolo11n-seg/yolo11n-seg.onnx
```

CMake reads the same manifest entry (`MIB_YOLO_MODEL_PATH`), fails configure
with that command when ONNX Runtime is available but the file is missing, and
copies it next to the executable as `resources/models/yolo11n-seg.onnx`, the
path `AppBackend` loads at runtime. See
`knowledge_map/build-and-run/Assets.md`.

## Updating the model

1. Re-export (below) or retrain; upload the new `.onnx` (and `.pt`) to the
   Hub repo with `hf upload gavinlouuu/mib-yolo11n-seg <dir> .`.
2. Put the new commit SHA and the `.onnx` SHA-256 into `env/assets.json`.
3. Re-provision and run the YOLO-dependent tests; land the manifest change
   with a vault note.

## Convert `yolo11n-seg.pt` to `yolo11n-seg.onnx`

From the repo root (PowerShell):

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -U pip ultralytics onnx
.\.venv\Scripts\python .\resources\models\convert_yolo11n_seg_to_onnx.py   # optional: --dynamic --simplify
```

The script reads `yolo11n-seg.pt` from and writes `yolo11n-seg.onnx` to this
directory; download the `.pt` from the Hub repo first.
