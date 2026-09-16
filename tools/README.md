# MIB Studio Tools

Standalone end-user tools for working with MIB Studio HDF5 files after recording. These are built as versioned executables (no Python required) and distributed separately from the main app.

## Supported tools

| Tool | Type | Output binary | Purpose |
|------|------|---------------|---------|
| HDF5 Export | GUI | `hdf5_export_app.exe` / `.app` | Export metrics and images from an .h5 file (CSV, TIFF). |
| Reanalyse HDF5 | CLI | `mib_reanalyse_hdf5.exe` | Re-run the processing pipeline on an .h5 and save intermediates (blur, diff, threshold, mask) and optional metrics CSV / reanalysis.h5. |

Source and build specs live under `tools/<toolname>/`. Core logic remains in `scripts/` and is referenced by the PyInstaller specs.

## Building

From the repository root:

- **Windows:** `.\tools\build_windows.ps1` (optionally `-Clean`). Output: `tools\dist\`.
- **macOS:** `./tools/build_mac.sh` (options: `--clean`, `--dmg`). Output: `tools/dist/`.

Requires Python 3.8+, a virtual environment is created under `tools/.venv` and dependencies from `tools/requirements-runtime.txt` and `tools/requirements-build.txt`.

## Packaging and distribution

1. **Create versioned zip** (from `tools/`): `.\package-tools.ps1` or `.\package-tools.ps1 -Version 0.1.7`  
   Output: `tools/dist/MIB_Studio_Tools_vX.Y.Z_windows.zip` (exes + README.txt).

2. **Publish to Cloudflare R2** (from repo root): `.\publish-tools.ps1 -Zip "tools\dist\MIB_Studio_Tools_v0.1.7_windows.zip"`
   Uploads the zip to `stable/tools/` and writes `tools-latest.json` under `https://updates.yofo.bio/stable/tools/` so users can fetch the latest tools programmatically.

## Classification

See [docs/supported-tools-classification.md](../docs/supported-tools-classification.md) for which scripts are supported end-user tools vs dev-only.

## Camera synchronization calibration

The [sync-tuning package](sync_tuning/README.md) is a separate Python API/CLI and CMake native sampler for MindVision/LED calibration. It is built and installed separately from the HDF5 tool bundles above; it is not automatically added to published tool releases.
