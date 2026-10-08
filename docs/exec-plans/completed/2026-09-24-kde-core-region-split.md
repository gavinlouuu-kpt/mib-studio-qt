# KDE core contour on the Monitoring scatter (live vs full-run records)

Status: completed

Builds on the Monitoring scatter density (KDE) colouring
(`knowledge_map/task/2026-09-23-monitoring-kde-density.md`,
branch `feat/monitoring-kde-density`). Vault notes to keep in step:
`knowledge_map/frontend/ExperimentMonitoringTab.md`,
`knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/frontend/Dialogs.md`,
`knowledge_map/data-model/HDF5-Storage.md`,
`knowledge_map/services/Hdf5Service.md`.

## Goal

While Density (KDE) is on, the Monitoring scatter shows one contour line
enclosing the densest share of the population (default 90%), redrawn with
every estimate. A previous experiment's contour can be shown on the same
axes as a dashed line so a shift is visible at a glance. Nothing else is
added to the tab: no readouts, no extra buttons. Each recorded experiment
stores the contour the operator saw at stop (provisional) and can store a
contour computed from the full recorded population (analysis); the two are
labelled and never confused.

## Definitions

- **Core fraction `p`** (0.05 to 1.00, default 0.90).
- **Level `t`**: with per-point normalised densities `d_i` from the existing
  kernel, `t` is the `ceil(p·n)`-th largest `d_i`. Points with `d_i >= t`
  are the core; by construction they are at least `p` of the population.
- **Contour**: the iso-line `density(x, y) = t` of the same Gaussian KDE
  (same per-axis bandwidth, same normalisation constant as the point
  densities) evaluated on a regular grid over the scatter's fixed axis
  ranges, traced with marching squares. It may consist of several closed
  loops (a bimodal population gives two). Loops are stored as polylines in
  native units (µm², deformability).

## Kernel additions (`include/frontend/tabs/MonitoringDensity.h`, Qt-free)

```cpp
double coreLevel(const std::vector<double>& density, double fraction);  // t

struct DensityGrid { int nx, ny; double x0, x1, y0, y1; std::vector<double> value; };
DensityGrid gaussianKdeGrid(const std::vector<DensityPoint>& points, DensityBandwidth bw,
                            double normaliser,   // max raw point density, so grid and points share [0,1]
                            double x0, double x1, double y0, double y1, int nx, int ny);

using Contour = std::vector<DensityPoint>;               // closed loop, last == first
std::vector<Contour> isoContours(const DensityGrid& grid, double level);   // marching squares
```

- `gaussianKdeAtPoints` gains an overload returning the raw maximum so the
  grid can be normalised identically.
- Grid default 128 × 64 over the axis ranges; cost for 1000 points is
  ~8 M `exp()` (~50 ms) and runs inside the existing worker job, so the GUI
  thread only draws polylines. Cells whose centre is farther than 5
  bandwidths from every point are skipped (the same `d2 > 50` cut).
- Marching squares uses linear interpolation on cell edges and the standard
  16-case table with the saddle cases resolved by the cell-centre value.

Property tests (`frontend.monitoring_density` extended):

- `coreLevel`: `p = 1` gives the minimum finite density; `ceil(p·n)` points
  are at or above the level; `n < 3` gives no level (NaN → no contour).
- A single Gaussian cloud yields exactly one loop that encloses at least
  `p` of the points and no more than `p + 0.05` (point-in-polygon count).
- Two well-separated clouds yield two loops.
- Translation invariance: shifting the points shifts every loop vertex by
  the same offset (grid aligned to the axes, so within one cell width).
- Degenerate input (identical points, `n < 3`, all non-finite) yields no
  loops and no NaN.

## Monitoring tab

- **Rendering**: one `QLineSeries` per loop for the live contour (solid,
  2 px, categorical slot 1 blue) and one per loop for the reference contour
  (dashed, 2 px, slot 2 orange). Series are created and removed in
  `onKdeJobFinished`; loops are already in axis units so no transform is
  needed. Both sets are hidden the moment the KDE toggle goes off; the off
  state is pixel-identical to today.
- **Controls**: none on the tab. The `Core %` spinner (5 to 100, default 90)
  lives in the Monitoring Settings dialog beside the bandwidth factor and is
  persisted as `Monitoring/KdeCoreFraction`. The reference is chosen in the
  same dialog: **Reference contour**: *None* / *Pin current* / *From file…*
  (`.h5`); the chosen file path persists as `Monitoring/KdeReferencePath`
  and is reloaded at startup when the file still exists.
- **Tooltip only**: hovering the Density toggle already shows the estimate
  summary; it gains "core 90%: 612 cells" and, with a reference, "reference:
  <file>, <live|full-run>". No other text is shown.
- **Hand-off at stop**: the tab exposes `lastCoreRecordJson()`; the
  experiment coordinator writes it through the chart-snapshot hook. Stop
  never waits for a pending estimate.

## Records

Both are versioned JSON string attributes (precedent: `run_snapshot_json`),
schema `kde_core_schema_version = 1`:

```json
{
  "schema_version": 1,
  "provisional": true,
  "source": "live-buffer" | "full-run",
  "core_fraction": 0.90,
  "level": 0.137,
  "cell_count": 612,
  "population_count": 1000,
  "bandwidth_rule": "silverman",
  "bandwidth_factor": 1.0,
  "bandwidth_x_um2": 21.4,
  "bandwidth_y": 0.0049,
  "pixel_to_micron_factor": 0.4886,
  "axis_range": {"x0": 0, "x1": 1000, "y0": 0, "y1": 1},
  "grid": {"nx": 128, "ny": 64},
  "contours": [[[x, y], [x, y], ...], ...],
  "computed_at_ns": 1790000000000000000
}
```

| Record | Location | `provisional` | Written by | When |
|---|---|---|---|---|
| Live | `/monitoring` @ `kde_live_json` | `true` | experiment stop, copy of the last on-screen estimate | only if KDE was on and an estimate completed |
| Analysis | `/analysis` @ `kde_core_json` | `false` | Review tab, on demand | never automatic; confirm before overwrite |

- The analysis record uses the file's `/valid_frames` metrics, the same
  kernel, Silverman bandwidth with factor 1.0, the file's pixel-to-micron
  factor, and the axis range stored in the live record (or the dialog
  defaults when there is none); non-finite metrics are excluded and counted.
  Files above 5000 valid frames are subsampled uniformly with a fixed seed;
  `population_count` records the subsample size.
- Reference resolution: analysis record if present, else live, else none.
  A missing record is "none", never zeros. A schema version ahead of the
  reader is ignored with a warn log.
- `Hdf5Service` gains `write/readKdeLiveJson` and `write/readKdeAnalysisJson`
  (groups created on demand, UTF-8 string attributes, `false` + warn on a
  read-only file).

## Review tab

- On open, draws the stored contours on its scatter: analysis solid, live
  dashed, with the existing legend naming each "full-run" / "live
  (provisional)".
- Context-menu action on the scatter, **Compute core contour from full run**:
  runs on `QtConcurrent`, confirms before overwriting an existing analysis
  record, saves when the file is writable, otherwise shows the contour and
  says it was not saved. No other controls.

## Error handling

- Fewer than 3 core points, or no estimate yet: no contour drawn; records
  carry `contours: []`.
- Reference file missing/unreadable/no records: reference cleared, one info
  log line, dialog shows *None*.
- Reference computed with a different pixel-to-micron factor: still drawn
  (both are in µm²); the toggle tooltip notes the different calibration.
- Population outside the fixed axis range is not contoured (the grid is the
  visible chart); the tooltip reports how many points fell outside.

## Testing

- `frontend.monitoring_density`: kernel properties above.
- `recording.kde_core_roundtrip`: both records write, close, reopen,
  byte-identical; absent reads as none.
- `recording.kde_core_fault`: read-only refuses; malformed JSON reported,
  not fatal; future schema ignored.
- `frontend.monitoring_kde_density` extended: contour series exist only
  while the toggle is on, loop count matches the kernel for the injected
  population, reference from a fixture file draws dashed, dialog persists
  fraction and reference path.
- `integration.monitoring_kde_e2e` extended: the KDE-on phase produces a
  contour; a run started and stopped in the test writes `kde_live_json`;
  GUI gates unchanged (grid work stays on the worker).
- `frontend.hdf_review_core`: fixture with a live record draws dashed;
  full-run computation adds the solid contour and survives reopen.

## Acceptance criteria

- [x] Solid live contour on the Monitoring scatter while Density is on;
      off state identical to today; no new widgets on the tab.
- [x] Core % and reference selection in the Monitoring Settings dialog,
      persisted; dashed reference contour from a pinned run or a file.
- [x] Live record written at stop when available, never blocking stop.
- [x] Review tab draws stored contours; full-run contour computed and saved
      on demand.
- [x] Round-trip, fault-injection, kernel property, widget and e2e tests.
- [x] Vault, `HDF5-Storage.md`, manual and task note updated in the same PRs.

## Decision log

- 2026-09-24: core = highest-density region by mass fraction, not a
  fraction of the peak, so 90% means the same on tight and flat clouds.
- 2026-09-24: true KDE iso-contour via a grid + marching squares, not an
  ellipse. The operator asked to see a contour; a grid of 128 × 64 keeps the
  extra cost inside the existing worker job.
- 2026-09-24: no readout, no tab buttons. Controls go into the Monitoring
  Settings dialog; the toggle tooltip carries the numbers.
- 2026-09-24: two records, `provisional` flag mandatory; readers prefer
  the full-run record and never recompute at stop.
- 2026-09-24: contours stored as polylines in native units so any reader
  can overlay them without re-running the kernel.

- 2026-09-24 (PR 2): the live record reaches the file through a Qt-free
  setter on the coordinator, pushed after every estimate while `Active`,
  instead of a stop-time pull from the tab. The coordinator owns the run's
  file on its worker, and a push keeps Stop independent of the GUI thread.

- 2026-09-24 (PR 3): the full-run contour uses the same pixel-to-micron
  factor as the Review scatter (the current backend factor) so the two
  overlay, and records it; reading the recorded factor for both is TD-17.
  The core share follows `Monitoring/KdeCoreFraction` so the full-run and
  live contours are comparable. The grid spans the padded data range, the
  Review scatter's own axis rule, rather than a stored axis range.
- 2026-09-24 (PR 3): the two changed manual screenshots were regenerated
  with `screenshot_tour` on Windows (`QT_QPA_PLATFORM=windows`; the Conan
  offscreen platform aborts there); the other seven are unchanged.

## Sequencing

1. PR 1 — kernel (level, grid, marching squares) + property tests; live
   contour on the tab; Core % in the dialog; in-memory pin reference.
2. PR 2 — `Hdf5Service` records + round-trip/fault tests; live record at
   stop; Review tab draws records; reference from file in the dialog.
3. PR 3 — Review tab full-run computation and save; e2e extension; manual
   and screenshots.

## Progress

- [x] PR 1 — 2026-09-24: `coreLevel` / `gaussianKdeGrid` / `isoContours` /
      `fractionInside` in `MonitoringDensity.h` with property tests; live
      contour + dashed pinned reference on the tab; `Core %` and
      Pin/Clear in the settings dialog; e2e: 1000 points → 21 ms per
      estimate including the grid, GUI gates unchanged. Border-cut
      populations may yield more than one closed loop (each arc closed
      along the border), which the test now accepts.
- [x] PR 2 — 2026-09-24: `Hdf5Service::{write,read}Kde{Live,Analysis}Json`
      with round-trip and fault-injection tests; Qt-free codec
      `frontend/tabs/KdeCoreRecord.h`; live record at stop through
      `ExperimentCoordinator::setLiveKdeCoreRecord` (the planned
      "chart-snapshot hook" does not exist: `captureChartSnapshots` has no
      caller and the coordinator owns the file during a run); Review tab
      draws stored records; *From file…* reference in the settings dialog,
      path persisted. e2e now runs a real experiment with KDE on and reads
      the record back (900 of 1000 cells, 2 loops).
- [x] PR 3 — 2026-09-24: `Hdf5Service::openFileForUpdate` (never
      creates/truncates); `computeFullRunCoreRecord` in `KdeCoreRecord.h`
      (fixed-seed 5000-cell subsample; 20000-cell test: 89.2% of all cells
      inside the 90% contour); Review tab right-click *Compute core contour
      from full run* with overwrite confirmation, read-only fallback
      (shown, not saved); manual (Review, Monitoring) and the two affected
      screenshots regenerated on Windows. Tests:
      `recording.kde_full_run_core` (new), `frontend.hdf_review_core`
      extended.
