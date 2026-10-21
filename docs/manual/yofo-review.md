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

## The window

Along the top are four stages: **Open**, **Overview**, **Inspect** and
**Export**. Choosing a stage only changes what you see. It never opens,
closes, exports or changes a file.

- **Open**: choose a folder of runs, open one, and read what the file
  records about itself.
- **Overview**: per-run statistics. This is **not available yet**. It needs
  the statistics worker, which this version does not include, so the page
  says so and shows no numbers.
- **Inspect**: the open run's records: the scatter, the records table and
  the frames, with the selected record's image and stored values beside
  them, and the export and regenerate actions on its toolbar.
- **Export**: a list of where each export is on Inspect's toolbar. The
  export page itself comes in a later version.

The header shows the open run. **Settings**, **Updates…** and **About** are
at the top right, beside **Fit**, which switches the image scaling between
fit-to-window and 1:1. The status bar at the bottom shows the file, its frame
counts and the pixel-to-micron factor in use. **Log** opens the message log.

## Open

To open a run from a folder:

1. Click **Open folder…** and choose the folder where your runs are saved.
2. YOFO Review finds the `.h5` and `.hdf5` files in the folder. It reads
   what each file's header says about its run, and groups the runs by
   experiment. It does not load images, and it never changes a file.
3. Click **Open** beside a run.

**Experiments in this folder** shows one table per experiment, with its
runs in run order (Run 1, Run 2, …). The experiment's header line gives:

- the number of runs and samples;
- the start dates;
- the planned conditions;
- the processing core;
- the calibration record;
- one status for how its runs ended.

Each run's row gives:

- the sample (sample sheet reference and name);
- the condition;
- when it started;
- its valid and invalid record counts;
- the processing core and calibration record;
- how the run ended.

An experiment is shown by the id its files record. Experiment names,
methods and analysis plans are kept by the workspace, which YOFO Review does
not read.

Files that do not belong to an experiment have their own tables:

- **Runs without an experiment**: the file has a run snapshot, but the
  snapshot names no experiment. This is the case for older snapshots, and
  for snapshots that cannot be read.
- **Legacy files · no run snapshot**: files recorded before run identity
  existed. Their experiment, run, sample, condition and calibration show as
  **Not recorded**.
- **Cannot be read**: each row gives the reason, for example a file that is
  still being copied or is locked by another program.

YOFO Review remembers the folder and lists it again the next time it starts.
**Refresh** lists the folder again, for example after a run has finished
saving.

While a folder is being listed, the list says **Listing…**. While the files'
headers are being read, it says **Reading N of M files**. Large folders are
read 256 files at a time, and you can open a run that is already shown at
any point.

When you choose another folder, the previous folder's runs disappear at once.
Results that arrive late for a folder you have left are ignored. During a
**Refresh** the runs stay in view, but you cannot open them until the
listing is done.

If the headers cannot be read at all, a yellow banner says why. For example,
they cannot be read while an export or a mask regeneration is running. The
files stay listed under **Not read**, and you can still open them. Click
**Refresh** to try again.

**Open file…** opens a single file from anywhere. **Close file** releases the
open file, so you can move or delete it. Opening another file replaces the
current one.

After a run opens, the **Open run** card shows what the file records:

- its folder and kind (experiment run, or a raw recording with images only);
- its identity: the run snapshot, experiment, run number, sample, condition,
  the rest of the planned annotation, the calibration record, and the
  operator selected at Start;
- the valid and invalid record counts;
- when it started and ended;
- the pixel-to-micron factor, and whether the file recorded it or the
  fallback is used;
- the processing core that analysed it;
- how the run ended (its run accounting).

A value the file does not record is shown as **Not reported**. It is never
guessed. Files from older versions have no run accounting, and the card says
**Not recorded · legacy file** rather than showing a complete run. Click
**Inspect <file>** to browse the run.

If a file cannot be opened, a red banner names the file and gives the reason,
and no file is left open.

You can also open a file without going through the Open page:

- Double-click a `.h5` or `.hdf5` file (the installer registers YOFO Review
  for both).
- Drag the file onto the app icon.

These open the file straight into **Inspect**.

Files larger than 2 GB are fine: frames load as you browse. Recordings
made in raw recording mode have no masks or metrics. For those, YOFO Review
shows a single frame list, and only raw image export is available.

## Browse frames and charts

Browsing happens on **Inspect**, and works as in MIB Studio's Review tab
(see [Review & post-process](review-and-postprocess.md)). The first row of
its toolbar chooses what to show, and which set:

- **Scatter** — the deformability-vs-area scatter and the ring-width
  histogram of the valid records. Click a point to inspect that record,
  scroll to zoom and drag to pan. Right-click for **Reset zoom** and
  **Compute core contour from full run**.
- **Records** — the records table, 100 rows a page (**◀ / ▶**), with
  **Columns…** to choose the columns.
- **Frames** — the thumbnails above the records table.
- **Valid Frames** / **Invalid Frames** — the set to browse (a recording
  has one set, **Frames**).

Whatever you choose, the selected record stays selected, and its card on
the right shows:

- its name, **frame N · object M** (files carry no record id; a recording's
  frames are **frame N**), and the run it belongs to;
- its image: **Raw**, **Contour** or **Mask** show the cell cut out of its
  frame, **Full frame** the whole frame with the overlay and ROI chosen on
  the toolbar. A view the file cannot show is greyed out, with the reason
  as a tooltip. The cut-out follows how the file stores its object
  positions (relative to the ROI, or to the whole image; **Stored vs
  reprocessed** says which, and how that was decided). When the file does
  not say and its masks do not tell, only the full frame is shown;
- its stored values (area, deformability, E, track, …) and **What is
  recorded?** for it. A raw recording stores only each frame's index and
  time, so its other values read **Not recorded**;
- **Previous record**, **Next record** and **Record details**, which opens
  the frame viewer (also a double-click on a thumbnail, the full frame or a
  table row); there, **←/→** step through frames.

**Linked cell records** lists the other records of the selected record's
track when you press **Show linked records** (at most 256; the card says
when a track has more). Click a row to select that record.

**Stored vs reprocessed** names the processing core, calibration, profile
and E-modulus LUT recorded in the file (**Not recorded** when the file does
not say), and how its object positions are stored. **Reprocess this
record** runs the processing core on the selected record's stored image,
with the recorded or the default settings, and shows the stored and the
reprocessed values side by side. It writes nothing, and runs only when you
press it. If it cannot run (a job is running, the record has no stored
image, or the file does not say how its positions are stored), it says why.
Nothing in Inspect changes the file: **Reprocess into a new file…** opens
**Regenerate masks**, which writes a new file.

The status bar shows the file, its frame accounting, and the
pixel-to-micron factor in use.

## Export

Exports run from **Inspect**. Its toolbar has **Export Metrics to CSV…** and
**Export All…**, and **More…** holds
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

For HDF5 files, YOFO Review uses the processing settings, ROI,
background and pixel-to-micron factor the file was recorded with, and the
target group the recording used. Each object's area range and target group
are checked at that factor (at the factor set under **File ▸
Preferences…** when the file records none). Untick that option to use the
default settings instead, which check objects at 0.4886 µm per pixel. AVI
and image-folder sources always use the defaults.

The new file records the factor it was made with. When that is the file's
own factor, the status bar shows it, not "(fallback)", when the new file
opens. When the file recorded none (or you used the default settings), the
new file says so and opens with "(fallback)". The message at the end of the
job also names the factor, for example "px→µm 0.2500 recorded".

Young's modulus (E) is computed with the E-modulus look-up table the
recording used, never another one. The message names it, for example
"E-LUT scaled_isoelastic_data_LUT_6.16-4.24 bundled (sha256 8a86b2beef4f)
(the source run's)". If the recording used a table this version does not
have, the regeneration stops with a message that names it, and nothing is
written. If the recording had no table loaded, the new file has no E
values either ("E not computed"). The new file records which table it used;
when that is the recording's own table (or none), regenerating it again uses
the same one. Files recorded before the table was
recorded keep the earlier behaviour: YOFO Review computes no E for them.

Regenerating never replaces a file. **Save as…** suggests a name that is
not taken yet (`<name>_remasked.h5`, then `_2`, `_3`, …). If you pick a file
that already exists, the regeneration does not start: the dialog says
"Output already exists; choose a new file" and you can choose another name.

**Files regenerated with review-v1.1.3-beta.1 or earlier** (#737): those
versions wrote the wrong frame index, timestamp and track first/last frame
into most records whenever processing produced fewer records than frames,
which is the normal case for real cells. The images, masks and background
in those files are correct; the per-record frame references are not.
Regenerate such files again from the original recording with this version:
each record now names its own frame. MIB Studio's reanalysis was never
affected.

**Files regenerated with review-v1.1.3-beta.1 or earlier** (#758): those
versions checked each object's area range and target group at 0.4886 µm per
pixel, whatever factor the file was recorded with, and with the recorded
settings they selected no target group. They also did not record a factor,
so the new file opened at the fallback factor. For a file recorded at
another factor, the valid / invalid split, the target group and the sizes
shown can be wrong. A file whose recording used a target group has no target
group cells after regenerating. Images, masks and background are correct.
Regenerate such files again from the original recording with this version.

## Pixel-to-micron factor

Files record the pixel-to-micron factor they were taken with, and YOFO
Review always uses it, for metrics, charts and exports (batches included).
Older files that recorded no factor use the fallback set under
**Settings** (default 0.4886 µm per pixel). The status bar and the Open run
card then mark the factor as the fallback.

## Updates

YOFO Review checks for a new version once at launch, and the status bar
says when one is available. **Updates…** checks on demand and offers
**Install and restart**.

Before installing, the app verifies that the update was signed by the YOFO
Review release key and that its checksum matches the published one. If
either check fails, nothing is installed.

Under **Settings**, pick the update channel: **stable** (the default) or
**beta** (pre-release builds, for testing).

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
  **Settings**.
- **A regenerated file's frame indices or times do not match its images**:
  the file was probably written by review-v1.1.3-beta.1 or earlier. See
  [Regenerate masks](#regenerate-masks) and regenerate it again.
- **A regenerated file shows "(fallback)", or its valid cells or target
  group differ from the original recording's**: it was probably written by
  review-v1.1.3-beta.1 or earlier, which checked objects at 0.4886 µm per
  pixel. See [Regenerate masks](#regenerate-masks) and regenerate it again.
- **Check for updates fails**: the computer needs internet access to
  `updates.yofo.bio`. You can always install a newer version by hand from
  the GitHub Release.
