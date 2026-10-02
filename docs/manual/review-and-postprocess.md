# Review & Post-process

## Review tab

![Review tab with thumbnails, the metrics table, and export actions](images/review-tab.png)

Open a recorded `.h5` / `.hdf5` file to inspect it without leaving the app.
Files larger than 2 GB are fine — frames load lazily as you browse.

- **Browse** — thumbnails load incrementally as you scroll; double-click a
  thumbnail (or a metrics-table row) to open the frame viewer, then use
  **←/→** to step frames.
- **Metrics table** — every saved per-frame metric, with optional scatter
  and histogram charts over the whole dataset.
- **Charts: find a cell from the scatter** — on the **Charts** tab, click a
  point on the deformability-vs-area scatter to show that cell in the panel
  to the right of the plot (it never covers the scatter). The point is
  ringed in orange and its row is selected in **Valid Frames**. Use
  **Prev/Next** (or **←/→**) in the panel to step through frames, and
  **Open in window…** for a large view. Scroll to zoom around the pointer
  (Ctrl: area only, Shift: deformability only), drag to pan, and
  double-click empty space or right-click → **Reset zoom** to see the whole
  run again. Hovering a point shows its frame, area and deformability.
  Exported chart images always show the whole run without the orange ring.
- **Core contour** — if the experiment was recorded with Density (KDE) on,
  its scatter shows the contour that was on screen at Stop as a dashed
  orange line ("live, provisional"). Right-click the scatter and choose
  **Compute core contour from full run** to trace the contour of the densest
  90% of *all* recorded cells (the share set under Monitoring Settings); it
  is drawn solid blue and saved into the file, after asking before it
  replaces an earlier one. Close the file in other programs first; if the
  file cannot be written, the contour is shown but not saved and the status
  line says so. The saved full-run contour is what *From file…* in
  Monitoring Settings uses as the reference for later runs.
- **Overlays** — mask/contour overlays and the ROI rectangle can be toggled
  on the loaded frames.
- **Close File** releases the file handle (do this before moving or
  deleting the file).

Recordings made in raw recording mode (as opposed to experiments) have no
masks or metrics: the tab shows a single **Frames** list and disables the
overlay and metrics exports, but raw TIFF export still works.

## Exporting

- **Export Metrics** — writes `<file>_metrics.csv` next to your chosen
  location (auto-suffixed `_2`, `_3`, … rather than overwriting).
- **Export All** — writes a folder named after the file containing
  `metrics.csv`, frame TIFFs, and chart images. For multi-image series you
  are asked whether to export all series frames, a range (e.g. `9-15`), or
  skip them.
- **Batch Metrics / Batch Export All** — select several files at once; the
  batch continues past individual failures and reports a summary.

The last successful output directory is remembered between sessions.

## Regenerate masks (reanalyse in-app)

**Regenerate masks…** re-runs the current processing configuration over the
loaded file (a frame range or all of it), an AVI, or a folder of images.
The result is written to a **new** HDF5 file — the original is never
modified — and opened in the Review tab. If no background is available,
one can be synthesized from the least-changing image tiles.

Use this to rescue an experiment recorded with a bad threshold, or to
compare configurations on identical input.

## YOFO Review (standalone review app)

**YOFO Review** is the Review tab as its own app for macOS and Windows: it
opens the same `.h5` / `.hdf5` files on a machine with no camera or
hardware. Double-click a recording (the app registers for `.h5` / `.hdf5`)
or use **File ▸ Open…**. Browsing, the frame viewer, overlays and the
Charts tab work as described above, with these differences:

- **Exports run behind a progress dialog** with **Cancel**; a cancelled
  export leaves nothing behind. When it finishes, **Show in folder** opens
  the output. Only one export runs at a time.
- **Batch Metrics, Batch Export All, Export Charts** and **Regenerate
  masks** are under **More…** on the toolbar. **Export Charts** writes
  `scatter_plot.tiff` and `ring_width_histogram.tiff` (1200 × 1200) into a
  folder you choose.
- **Regenerate masks** uses the file's *recorded* processing settings, ROI
  and background by default (untick to use the defaults); AVI and folder
  sources always use the defaults. The new file opens when it is done.
- **Pixel-to-micron** — files record the factor they were taken with, and
  the app always uses it. For older files that do not, set the factor under
  **File ▸ Preferences…**; the status line marks it "(fallback)".
- The ring-width histogram spans the ring-ratio limits the file was
  recorded with.
- The app is not signed: on first launch, macOS asks you to confirm in
  **System Settings ▸ Privacy & Security** and Windows SmartScreen needs
  **More info ▸ Run anyway**.

## Standalone tools

For working with recordings on machines without the full app (and without
Python), two tools ship separately, versioned to match the app — download
the zip whose version matches **Help ▸ About**:

- `MIB_Studio_Tools_v<version>_windows.zip` from
  `https://updates.yofo.bio/stable/tools/` (manifest:
  `tools-latest.json`).

**HDF5 Export** (`hdf5_export_app.exe`, GUI) — export metrics CSV and/or
frame TIFFs from a recording: pick input file, output directory, format
(CSV / images / all), frame type (valid / invalid / both), and the
pixel-to-micron factor.

**Reanalyse HDF5** (`mib_reanalyse_hdf5.exe`, command line) — re-run the
processing pipeline outside the app, saving per-frame intermediate images
(original, blurred, diff, threshold, mask, optional overlay), an optional
`metrics.csv`, and an optional reanalysis HDF5. Run it with `--help` for
the full flag list; defaults match the app's processing defaults.

See [`docs/howto/tools.md`](https://github.com/KPT1020/mib-studio-qt/blob/main/docs/howto/tools.md)
for the full tool reference (developer docs, hosted in the repository).
