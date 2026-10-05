# YOFO Review

**YOFO Review** is the Review tab as its own app for macOS and Windows. It
opens the `.h5` / `.hdf5` files MIB Studio records, on any computer: no
camera, hardware or MIB Studio installation is needed. Use it to browse a
run, check masks and charts, export metrics and images, and regenerate
masks after the experiment.

## Install

Download the installer for your computer from the project's GitHub
Releases page. YOFO Review releases are named **YOFO Review X.Y.Z**:

| Computer | File |
|---|---|
| Mac with Apple silicon, macOS 13 or later | `YOFO_Review_v<version>_aarch64.dmg` |
| Windows 10 / 11, 64-bit | `YOFO_Review_v<version>_x64-setup.exe` |

Each release also lists SHA-256 checksums
(`SHA256SUMS-yofo-review.txt`) so you can confirm the download.

**macOS** — open the DMG and drag **YOFO Review** into **Applications**.
The app is not notarised by Apple, so the first launch is blocked. Open
**System Settings ▸ Privacy & Security**, scroll to the message about
YOFO Review and click **Open Anyway**. Later launches open normally.

**Windows** — run the setup program. It installs for your user account
only and needs no administrator rights. The installer is not signed, so
SmartScreen may warn: click **More info ▸ Run anyway**.

## Open a file

- Double-click a `.h5` / `.hdf5` file (the installer registers YOFO Review
  for both), or drag it onto the app icon.
- Or use **File ▸ Open…** inside the app.

Opening another file replaces the current one. **Close File** on the
toolbar (or **File ▸ Close**) releases the file, so you can move or delete
it.

Files larger than 2 GB are fine: frames load as you browse. Recordings
made in raw recording mode have no masks or metrics. For those, YOFO Review
shows a single frame list, and only raw image export is available.

## Browse frames and charts

Browsing works as in MIB Studio's Review tab (see
[Review & post-process](review-and-postprocess.md)):

- **Frames** — thumbnails, the metrics table and the frame viewer, with
  mask, contour and ROI overlays. Double-click a thumbnail or a table row
  to open the viewer, then use **←/→** to step through frames.
- **Charts** — the deformability-vs-area scatter and the ring-width
  histogram. Click a point to show that cell, scroll to zoom and drag to
  pan. Right-click for **Reset zoom** and **Compute core contour from full
  run**.

The status bar shows the file, its frame accounting, and the
pixel-to-micron factor in use.

## Export

The toolbar has **Export Metrics to CSV…** and **Export All…**. **More…** holds
**Batch Metrics**, **Batch Export All**, **Export Charts** and
**Regenerate masks**.

- **Export Metrics to CSV…** writes `<file>_metrics.csv`. Existing files are never
  overwritten: a `_2`, `_3`, … suffix is added instead.
- **Export All…** writes a folder named after the file, containing
  `metrics.csv`, the frame TIFFs and the chart images. For multi-image
  series it first asks whether to export all series frames, a range (for
  example `9-15`), or none.
- **Batch Metrics / Batch Export All** process several files in one go.
  The batch continues past a failed file and reports a summary at the end.
  Raw recordings have no metrics, so Batch Metrics lists them as failed.
- **Export Charts** writes `scatter_plot.tiff` and
  `ring_width_histogram.tiff` (1200 × 1200) into a folder you choose.
  Exported charts always show the whole run, without zoom or the selection
  ring.

Every export runs behind a progress dialog with **Cancel**. A cancelled
export leaves nothing behind. When an export finishes, **Show in folder**
opens the output. Only one export runs at a time, and the app remembers the
last output folder.

## Regenerate masks

**More… ▸ Regenerate masks** runs mask processing again over the open file
(all of it or a frame range), an AVI, or a folder of images. The result is
a **new** HDF5 file, which opens when it is done; the original is never
changed.

For HDF5 files, YOFO Review uses the processing settings, ROI and
background the file was recorded with. Untick that option to use the
default settings instead. AVI and image-folder sources always use the
defaults.

## Pixel-to-micron factor

Files record the pixel-to-micron factor they were taken with, and YOFO
Review always uses it, for metrics, charts and exports (batches included).
Older files that recorded no factor use the fallback set under **File ▸
Preferences…** (default 0.4886 µm per pixel); the status bar then marks
the factor "(fallback)".

## Updates

YOFO Review checks for a new version once at launch, and the status bar
says when one is available. **Help ▸ Check for updates…** checks on
demand and offers **Install and restart**.

Before installing, the app verifies that the update was signed by the YOFO
Review release key and that its checksum matches the published one. If
either check fails, nothing is installed.

Under **File ▸ Preferences…**, pick the update channel: **stable** (the
default) or **beta** (pre-release builds, for testing).

## Differences from MIB Studio's Review tab

YOFO Review was checked against the Review tab on recorded runs: metrics
CSVs, exported images, chart axes and histogram bins, and saved core
contours match. The visible differences:

- The ring-width histogram spans the ring-ratio limits the file was
  recorded with (MIB Studio uses the current settings).
- **Batch Export All** writes metrics and images for each file, but no
  chart images.
- In a batch, the series question is asked once for all files.
- **Regenerate masks** uses the file's recorded settings by default (see
  above).
- Chart colours and legend order differ slightly.

## Troubleshooting

- **"YOFO Review can't be opened" / "Apple could not verify…"** (macOS):
  use **Open Anyway** as described under [Install](#install).
- **The file will not open**: close it in other programs (MIB Studio,
  HDFView, Python) first. A file that is still being recorded cannot be
  opened.
- **Sizes look wrong**: check the pixel-to-micron factor in the status
  bar. "(fallback)" means the file recorded none; set the right value under
  **File ▸ Preferences…**.
- **Check for updates fails**: the computer needs internet access to
  `updates.yofo.bio`. You can always install a newer version by hand from
  the GitHub Release.
