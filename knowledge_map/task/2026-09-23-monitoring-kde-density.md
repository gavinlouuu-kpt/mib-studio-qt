# Monitoring scatter density (KDE) colouring

Request: operators want to see the population density of the Monitoring
tab's deformability-vs-area scatter, computed periodically, without the
chart stalling. Note: [[../frontend/ExperimentMonitoringTab]] ("Scatter
density (KDE) colouring").

## Decisions

- **Colour the points, no heat map layer.** QtCharts has no `QCPColorMap`
  equivalent; a background pixmap would have to track `plotArea` on every
  resize/zoom. Per-point colouring through `QXYSeries::setPointsConfiguration`
  (Qt ≥ 6.2, the tree builds 6.7.3) is the cytometry pseudocolour dot plot
  operators already know and needs no geometry bookkeeping.
- **Evaluate the KDE at the samples, not on a grid.** The buffer is capped at
  1000 points, so the symmetric O(n²/2) pass is ~0.5 M `exp()` (≈3 ms).
  Exact per-point values, no grid/interpolation step, no grid-resolution
  setting. The pre-existing `computeKDE` (grid, single isotropic bandwidth in
  raw units, 1-D normalisation, never called) was deleted rather than wired.
- **Per-axis Silverman bandwidth × user factor.** Area (hundreds of µm²) and
  deformability (0..1) cannot share a bandwidth; the guard test carries an
  isotropic control that fails to separate two populations differing only in
  deformability.
- **Its own timer, its own thread.** A 2 s default `kdeTimer_` (500 ms–60 s)
  launches `QtConcurrent::run` with value copies; the 500 ms refresh only
  re-applies the last map. Fingerprint (count, first/last index, factor,
  µm conversion) skips unchanged buffers. Mirrors the HDF export idiom
  ([[../frontend/HdfReviewTab]]); documented in
  [[../architecture/Threading-Model]].
- **Target group by shape while on.** Both series take the density ramp, so
  the target group's blue would collide; rectangles keep it identifiable and
  the off state restores today's look exactly.
- **Persist in `QSettings`** (`Monitoring/Kde*`, versioned) — a view
  preference, not a scientific parameter, so not in `ProcessingConfig`.

## Verification (Windows, `windows-ninja`, 2026-09-23)

- `mib_backend_tests monitoring_density_test` — kernel invariants, per-axis
  separation + isotropic control, degenerate/non-finite input, order
  invariance, ramp monotonicity, 4× points → 17.6× time (gate 64×).
- `mib_frontend_tests monitoring_kde_density_test` — offscreen widget
  scenario (see the test header).
- `python scripts/check_docs.py`, `python scripts/check_screenshots.py`.
- Windows fast lane (`ctest --preset windows-ninja-test`): 119 / 120. The one
  failure, `frontend.mainwindow_shutdown`, has a fixed 1 s exit budget and on
  this host the shutdown drains an eGrabber discovery probe of a physical
  Coaxlink Quad CXP-12 that takes ~1.4 s (reproducible, unrelated to this
  change, not verified against the base commit).
- Linux / sanitizer lanes run in CI only; hardware acceptance not claimed.

## Follow-ups

- The Monitoring screenshot (`docs/manual/images/experiment-monitoring.png`)
  predates the toggle; regenerate with `screenshot_tour` when the manual is
  next refreshed.
- The other Monitoring Settings values (axis ranges, bin width) still do not
  persist; the KDE keys show the pattern if that is wanted.
