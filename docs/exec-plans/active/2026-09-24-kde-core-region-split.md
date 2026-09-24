# KDE core region: live (provisional) vs full-run (analysis) records

Status: active

Builds on the Monitoring scatter density (KDE) colouring
(`knowledge_map/task/2026-09-23-monitoring-kde-density.md`,
branch `feat/monitoring-kde-density`). Vault notes to keep in step:
`knowledge_map/frontend/ExperimentMonitoringTab.md`,
`knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/data-model/HDF5-Storage.md`,
`knowledge_map/services/Hdf5Service.md`.

## Goal

Operators can see, during a run and between runs, where the population core
sits in the deformability-vs-area plane and whether it has shifted. The
"core" is the highest-density region holding a settable share of cells
(default 90%), summarised as an ellipse. Every recorded experiment carries the
core its operator saw on screen when the run stopped (provisional, from the
live buffer), and can additionally carry an authoritative core computed from
the full recorded, gated population. The live chart overlays a reference
ellipse from a chosen previous experiment and reports the shift in plain
units. The two records never get confused: each says what it is.

## Definitions

- **Core fraction `p`** (0.05 to 1.00, default 0.90): the share of cells
  inside the core.
- **Core (highest-density region)**: given per-point normalised densities
  `d_i` (existing kernel), the threshold `t` is the largest value such that at
  least `p·n` points satisfy `d_i >= t` (the `ceil(p·n)`-th largest density).
  The core is `{i : d_i >= t}`. This is the cytometry contour definition and
  is stable across sample sizes, unlike "90% of the peak".
- **Core ellipse**: centre = median area and median deformability of the core
  points (robust to residual outliers); axes and orientation from the 2x2
  covariance of the core points, scaled so the ellipse encloses the core at
  the 2-sigma level (`k = 2`, documented constant); reported as `cx`, `cy`,
  `semiAxisMajor`, `semiAxisMinor`, `angleDeg` (major axis vs the area axis).
- **Shift** between a live ellipse `L` and a reference `R`: centre delta
  `(dx = L.cx - R.cx, dy = L.cy - R.cy)` in native units (µm², deformability),
  a normalised distance `s = sqrt((dx/R.semiAxisMajor)² + (dy/R.semiAxisMinor)²)`
  measured along the reference's own axes (rotation applied), and the area
  ratio `L.area / R.area`. `s >= 1` means the live centre lies outside the
  reference ellipse.

## Records

Both records are JSON string attributes with a schema version, following the
`run_snapshot_json` precedent (`Hdf5Service::writeRunSnapshotJson`), so the
schema can grow without touching HDF5 typed attributes. The schema for both
is one document, `kde_core_schema_version = 1`:

```json
{
  "schema_version": 1,
  "provisional": true,
  "core_fraction": 0.90,
  "cell_count": 612,
  "population_count": 1000,
  "threshold": 0.137,
  "bandwidth_rule": "silverman",
  "bandwidth_factor": 1.0,
  "bandwidth_x_um2": 21.4,
  "bandwidth_y": 0.0049,
  "pixel_to_micron_factor": 0.4886,
  "ellipse": {"cx": 182.3, "cy": 0.038, "semi_major": 41.2, "semi_minor": 0.021, "angle_deg": 3.1, "sigma_scale": 2.0},
  "computed_at_ns": 1790000000000000000,
  "source": "live-buffer" | "full-run",
  "gating": "live-config" | "recorded-valid-frames",
  "excluded_points": 0
}
```

| Record | Location | `provisional` | `source` | Written by | When |
|---|---|---|---|---|---|
| Live core | `/monitoring` group, attribute `kde_live_json` | `true` | `live-buffer` | `ExperimentCoordinator` at experiment stop, from the Monitoring tab's last completed estimate | Only if the KDE toggle was on and at least one estimate completed during the run; otherwise absent |
| Analysis core | `/analysis` group, attribute `kde_core_json` | `false` | `full-run` | Review tab "Compute core from full run" (on demand) | Never automatic; overwrites a previous analysis record after confirmation |

Rules:

- The live record is a **copy of what was on screen**: the tab's last
  `KdeResult` plus the current fraction. It is never recomputed at stop.
- The analysis record is computed from `/valid_frames` metrics only
  (`area`, `deformability`, `isValid == true`), with the **same kernel**
  (`MonitoringDensity.h`), Silverman bandwidth, factor fixed at 1.0, and the
  file's own `pixel_to_micron_factor`. Points with non-finite metrics are
  excluded and counted in `excluded_points`. No density-tail trimming in v1
  (the gating already removes rejected objects); trimming is a schema-compatible
  later addition.
- Readers treat a missing record as "none", never as zeros. Files that
  predate the schema read back as "none".
- Reference resolution order for the overlay: analysis record if present,
  else live record, else none. The UI always names which one is in use.

## Kernel additions (`include/frontend/tabs/MonitoringDensity.h`, Qt-free)

```cpp
struct CoreSelection { double threshold; std::vector<std::size_t> indices; };
CoreSelection selectCore(const std::vector<double>& density, double fraction);

struct CoreEllipse { double cx, cy, semiMajor, semiMinor, angleDeg, sigmaScale; };
CoreEllipse fitCoreEllipse(const std::vector<DensityPoint>& points, const CoreSelection& core);

struct CoreShift { double dx, dy, normalisedDistance, areaRatio; };
CoreShift coreShift(const CoreEllipse& live, const CoreEllipse& reference);

std::vector<DensityPoint> ellipseOutline(const CoreEllipse& e, int segments = 64);
```

Properties the unit test must prove (`frontend.monitoring_density` extended
or a sibling `frontend.monitoring_core`):

- `selectCore` returns exactly `ceil(p·n)` points for distinct densities and
  every returned density is `>= threshold`; `p = 1` returns all finite
  points; `n < 3` returns an empty core (no ellipse).
- Translation invariance: shifting every point by `(a, b)` moves the ellipse
  centre by `(a, b)` and leaves axes and angle unchanged; the shift metric
  reports `(a, b)`.
- Anisotropy: a population stretched 10x along area yields a major axis
  along area with `angleDeg` near 0; the ellipse encloses at least 90% of the
  core points at `sigmaScale = 2`.
- Degenerate core (all identical points) yields a zero-area ellipse and
  finite shift values, never NaN.
- `coreShift` is antisymmetric in `dx, dy` and `normalisedDistance >= 1`
  when the live centre lies outside the reference ellipse.

## Monitoring tab (live)

- **Controls** (top row, next to the Density toggle): `Core %` spinbox
  (5 to 100, step 5, default 90), persisted as `Monitoring/KdeCoreFraction`
  with the existing `Monitoring/Kde*` keys. Reference menu button
  **Reference ▾**: *Pin current core*, *Load from experiment…* (file dialog,
  `.h5`), *Clear*. Enabled only while the KDE toggle is on.
- **Rendering**: two `QLineSeries` on the scatter axes: live ellipse solid,
  reference dashed, both 2 px, colours from the existing categorical slots
  (live = slot 1 blue, reference = slot 2 orange) so they cannot be confused
  with the density ramp. Outline is 64 points; cost is negligible next to
  the level series.
- **Readout label** under the scatter (elided, accessible name "Population
  core"): `Core 90%: 612 of 1000 · centre 182 µm² / 0.038` and, when a
  reference is set, `· shift +9 µm² / +0.006 (0.4 of reference axis) vs
  <run name>, <live|full-run> estimate`. When `normalisedDistance >= 1` the
  label takes the warning text colour and the accessible description says
  "core outside reference".
- **Cadence**: the core is derived inside `onKdeJobFinished` from the same
  result; no extra thread work. The ellipse fit runs on the GUI thread over
  at most 1000 points (microseconds).
- **Hand-off at stop**: the tab exposes `lastCoreRecordJson()` (empty when
  none). `ExperimentCoordinator` asks for it through the existing chart
  snapshot hook and writes it via `Hdf5Service::writeKdeLiveJson`.
- **Session memory**: the last pinned/loaded reference stays in memory for
  the session and its file path persists in `Monitoring/KdeReferencePath`;
  on startup it is reloaded if the file still exists, else cleared silently
  with an info log line.

## Review tab (post-run)

- On file open, read both records. Draw the analysis ellipse (solid) and, if
  present, the live ellipse (dashed) on the Review scatter; a legend row
  names each with its `source`.
- **Compute core from full run** button: runs the analysis on the file's
  valid frames via `QtConcurrent` (files may hold 10⁵ points; the O(n²)
  kernel is capped by subsampling to 5000 points uniformly at random with a
  fixed seed, recorded as `population_count` vs `cell_count`), then writes
  `kde_core_json` after a confirm dialog if a record already exists. The
  file must be opened writable; read-only files show the result without
  saving and say so.
- **Compare**: *Set as reference for Monitoring* pushes the file's best
  record into the Monitoring tab's reference slot.

## Hdf5Service

```cpp
bool writeKdeLiveJson(const std::string& json);           // /monitoring @kde_live_json
bool readKdeLiveJson(std::string& json) const;            // false when absent
bool writeKdeAnalysisJson(const std::string& json);       // /analysis @kde_core_json
bool readKdeAnalysisJson(std::string& json) const;
```

Groups are created on demand; attributes are variable-length UTF-8 strings
like `run_snapshot_json`. Writes are refused (return `false`, warn log) on a
file opened read-only.

## Error handling

- No estimate completed during the run: no live record; the file is
  otherwise complete. Stop never waits for a pending estimate.
- Reference file missing, unreadable, or without records: the reference
  slot stays empty and the readout says "reference: none (<reason>)".
- Fewer than 3 core points: no ellipse; readout says "core: too few
  cells". Records are still written with `cell_count` and no `ellipse`
  member (readers treat a missing `ellipse` as no ellipse).
- Schema version ahead of the reader: record ignored with a warn log naming
  the version.
- Pixel-to-micron factor differs between live and reference: shift is
  still reported (both are in µm²), and the readout appends "(different
  µm calibration)" so the operator knows.

## Testing (coverage matrix: save data → round-trip + fault-injection)

- `frontend.monitoring_core` (backend runner): kernel properties above.
- `recording.kde_core_roundtrip`: write both records, close, reopen, read
  back byte-identical JSON; absent records read as "none"; a file written
  by the current build without records reads as "none".
- `recording.kde_core_fault`: read-only file refuses writes and returns
  `false`; truncated/invalid JSON in the attribute is reported as unreadable
  and does not abort; schema version 99 is ignored.
- `frontend.monitoring_kde_density` extended: core fraction control,
  ellipse series appear only while the toggle is on, readout text, pin /
  load / clear, warning state when the live centre leaves the reference.
- `integration.monitoring_kde_e2e` extended: after the KDE-on phase the
  tab produces a core record; an experiment started and stopped inside the
  test yields a file with `kde_live_json`; the GUI gates are unchanged.
- Review tab test (`frontend.hdf_review_core`): open a fixture with a live
  record, compute the full-run core, confirm overwrite, reopen and find both.

## Acceptance criteria

- [ ] Core % control, live ellipse and readout on the Monitoring tab; off
      state identical to today.
- [ ] Reference pin / load / clear with dashed overlay and shift readout,
      warning state on exit from the reference ellipse.
- [ ] Live record written at experiment stop when available; never blocks
      stop.
- [ ] Review tab shows records, computes and saves the full-run core on
      demand, and can push a reference to Monitoring.
- [ ] Round-trip and fault-injection tests for both records; kernel property
      tests; widget and e2e tests extended.
- [ ] Vault notes, `HDF5-Storage.md` schema section, manual page, and a task
      note updated in the same PRs.

## Decision log

- 2026-09-24: core = highest-density region (mass fraction), not a fraction
  of the peak density, so the number means the same thing on tight and flat
  populations.
- 2026-09-24: two records, `provisional` flag mandatory. The live record is
  a copy of the on-screen estimate (buffer-limited, live gating, user
  bandwidth factor); the analysis record is computed from the full recorded
  valid population with a fixed bandwidth rule. Readers prefer analysis.
- 2026-09-24: JSON string attributes with a schema version, mirroring
  `run_snapshot_json`, instead of typed attribute sets.
- 2026-09-24: ellipse from core covariance at 2 sigma, centre from medians;
  a true KDE iso-contour is deferred (needs a grid pass; the ellipse is
  enough to see a shift and is comparable across runs).
- 2026-09-24: no automatic full-run computation at stop; it would extend
  stop time on large files and would silently overwrite an operator's
  earlier analysis.

## Sequencing

1. PR 1 — kernel additions + property tests; Monitoring tab core control,
   live ellipse, readout; in-memory pin reference. No file format change.
2. PR 2 — `Hdf5Service` records + round-trip/fault tests; live record at
   stop; Review tab read/draw; load reference from file.
3. PR 3 — Review tab full-run computation and save; e2e extension; manual
   and screenshots.

## Progress

- [ ] PR 1
- [ ] PR 2
- [ ] PR 3
