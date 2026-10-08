# HDF5 Reanalysis Tool

This guide describes the command-line reanalysis tool that re-runs the MIB Studio processing pipeline on an existing .h5 dataset and saves all intermediate images so they can be analysed later (e.g. in ImageJ, Python, or other tools).

**User download:** For the standalone `mib_reanalyse_hdf5.exe` and the full tools bundle, see [tools.md](tools.md).

## Purpose

- **Re-run the pipeline** on stored images: grayscale, Gaussian blur, (optional) background subtraction, binary threshold, morphology (close, open) to produce a mask. This matches the C++ `ProcessingService` pipeline used during live recording.
- **Save all intermediate images** per frame: `original.tiff`, `blurred.tiff`, `diff.tiff`, `thresh.tiff`, `mask.tiff`, and optionally `overlay.tiff` (contours drawn on the original).
- **Optionally recompute metrics** (contours, deformability, area, ring ratio, etc.) and export `metrics.csv` in the same format as the HDF5 export script.
- **Optionally write a new HDF5** (`reanalysis.h5`) with images, recomputed masks, and metadata so the reanalysed dataset can be opened in MIB Studio.

Use reanalysis when you want to:

- Inspect or analyse intermediate processing steps (blur, diff, threshold, mask) frame by frame.
- Re-run with different parameters (e.g. blur size, threshold) without re-recording.
- Produce a new .h5 with recomputed masks and metrics for comparison or downstream use.

## Prerequisites

Same as the HDF5 export script:

- Python 3.8 or later
- `h5py`, `numpy`, `opencv-python`

Install with:

```bash
pip install h5py numpy opencv-python
```

Or from the project:

```bash
pip install -r env/requirements-scripts.txt
```

## Usage

Run from the repository root or from the `scripts` directory:

```bash
# Reanalyse and save all intermediates (background = stored in .h5, or from_all if absent)
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis

# Process only valid or only invalid frames
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --frame-type valid

# Build background from pixel-wise mean of all images
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --background from_all

# No background subtraction
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --background none

# Save contour overlay and recompute metrics CSV
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --save-overlay --export-csv

# Export a new HDF5 with recomputed masks for MIB Studio
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --export-h5

# Use a config file (same image_processing section as app config)
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --config path/to/config.json

# Override parameters on the command line
python scripts/reanalyse_hdf5.py -i experiment.h5 -o ./reanalysis --blur 5 --threshold 10 --morph-kernel 5
```

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `--input`, `-i` | Path to input .h5 file | (required) |
| `--output`, `-o` | Output directory | (required) |
| `--frame-type`, `-t` | `valid`, `invalid`, or `both` | `both` |
| `--background` | `none`, `stored`, `from_all`, or path to image file. Use `stored` to use the background saved in the .h5 (if present; otherwise warns and falls back to `from_all`); `from_all` to build from pixel-wise mean of all images. | `stored` |
| `--config` | Path to JSON config with `image_processing` section | (none) |
| `--blur` | Gaussian blur kernel size (odd) | from config or 3 |
| `--threshold` | Binary threshold value | from config or 8 |
| `--morph-kernel` | Morphology kernel size (odd) | from config or 3 |
| `--morph-iterations` | Morphology iterations | from config or 1 |
| `--save-overlay` | Save contour overlay image per frame | off |
| `--export-csv` | Recompute metrics and write `metrics.csv` | off |
| `--export-h5` | Write `reanalysis.h5` with images, masks, metadata | off |
| `--pixel-to-micron` | Pixel to micron conversion for CSV area | 0.4886 |

## Output layout

- **Per-frame intermediates** (always written when processing that frame type):

  - `{output_dir}/valid/frame_{index:06d}/original.tiff`
  - `{output_dir}/valid/frame_{index:06d}/blurred.tiff`
  - `{output_dir}/valid/frame_{index:06d}/diff.tiff`
  - `{output_dir}/valid/frame_{index:06d}/thresh.tiff`
  - `{output_dir}/valid/frame_{index:06d}/mask.tiff`
  - `{output_dir}/valid/frame_{index:06d}/overlay.tiff` (only if `--save-overlay`)

  The same structure is used under `invalid/` when processing invalid frames. The `index` is taken from the HDF5 metadata so it matches the original experiment frame indices.

- **Optional**:
  - `{output_dir}/metrics.csv` (if `--export-csv`): same column format as the HDF5 export script, plus ROI columns (ROI X, ROI Y, ROI W, ROI H).
  - `{output_dir}/reanalysis.h5` (if `--export-h5`): HDF5 with `/valid_frames` and `/invalid_frames` groups, each containing `images`, `masks`, and `metadata` datasets, plus `/experiment_info` attributes (including `roi_x`, `roi_y`, `roi_w`, `roi_h` when present in the source or after reanalysis).

## Parameters and app config

For results that match the app as closely as possible, use the same processing parameters as at recording time. You can:

- Pass `--config path/to/config.json` where the JSON has an `image_processing` section (and optional `filters` subsection) as in the app’s config (e.g. `resources/defaults/config.json`).
- Or set `--blur`, `--threshold`, `--morph-kernel`, and `--morph-iterations` explicitly.

Defaults in the script match the app defaults (blur 3, threshold 8, morph kernel 3, morph iterations 1).

**ROI:** The reanalysis tool uses the same ROI as at recording when it is stored in the .h5. MIB Studio writes ROI to `/experiment_info` as attributes `roi_x`, `roi_y`, `roi_w`, `roi_h`. If the input .h5 has no ROI metadata (e.g. older files), the script auto-synthesizes an ROI from the region where objects are most densely populated: it uses `/valid_frames/masks`, computes object centroids across (sampled) valid frames, and defines the ROI as the 5th-95th percentile bounding box of those centroids with a small margin. If no objects are found, processing falls back to full-frame. The chosen ROI is printed at startup (source: `stored`, `synthetic`, or `full_frame`). Exported `metrics.csv` includes ROI columns (ROI X, ROI Y, ROI W, ROI H), and `reanalysis.h5` writes ROI into `/experiment_info` so downstream use is consistent.

From this version onward, MIB Studio saves the run’s background image in the .h5 (at `/experiment_info/background`) when a background was set for the run. Use `--background stored` when re-running the same pipeline for reproducible results.

## Related

- **Export only (no re-run):** use `scripts/export_hdf5.py` or the HDF5 Export GUI to export existing metrics and images from an .h5 without re-running the pipeline.
- **HDF5 Export GUI:** [hdf5-export-app.md](hdf5-export-app.md).
- **All tools and downloads:** [tools.md](tools.md).
