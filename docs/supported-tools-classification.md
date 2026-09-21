# Supported tools classification

Scripts in `scripts/` are classified as follows for distribution and packaging.

## Supported end-user tools (shipped as standalone binaries)

These are officially supported, documented, and distributed as versioned executables (no Python required).

| Tool | Type | Entrypoint | Packaged as |
|------|------|------------|-------------|
| HDF5 Export | GUI | `hdf5_export_app.py` | `hdf5_export_app.exe` / `.app` |
| Reanalyse HDF5 | CLI | `reanalyse_hdf5.py` | `mib_reanalyse_hdf5.exe` |

They live under `tools/` with stable entrypoints and are included in the MIB Studio Tools versioned zip.

## Dev / internal (remain in `scripts/`)

Not shipped as standalone binaries. Used for development, QA, or automation.

| Script / asset | Purpose |
|----------------|---------|
| `export_hdf5.py` | Core export logic; used by HDF5 Export GUI. Also runnable as CLI for dev/automation. |
| `export_worker.py` | Background worker for the HDF5 Export GUI. |
| `compare_metrics.py` | QA: compare metrics/conformance JSON, including optional masks, series, target, and tracking evidence. |
| `run_processing_conformance.py` | QA: run the installed wheel and fail on drift from the committed reference. |
| `gold_standard_dataset.json` | Committed deterministic full-parity wheel-conformance reference. |
| `build_windows.ps1`, `build_mac.sh` | Build scripts for packaging; remain in `scripts/` or are mirrored under `tools/` for building. |
| `env/requirements-scripts.txt` | Dependencies for `scripts/`; the tools bundle uses `env/requirements-tools-runtime.txt` and `env/requirements-tools-build.txt`; build-time packages (Conan, NumPy) are `env/requirements-build.txt`. |

Optional future: ship `export_hdf5.py` CLI or `compare_metrics.py` as part of the tools bundle if we want them user-facing.
