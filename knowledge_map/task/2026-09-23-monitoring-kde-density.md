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

## End-to-end on the mock camera (`integration.monitoring_kde_e2e`)

`tests/frontend/monitoring_kde_e2e_test.cpp` boots the real `MainWindow` on
the real pipeline (mock camera → FrameStore → realtime processing →
monitoring rings → Monitoring tab) and runs KDE off / on / off phases of
8 s, 8 s, 4 s. Frames: the public `gavinlouuu/512x96stream` asset
(`python scripts/provision-assets.py --asset 512x96stream-mock-frames
--count 1000`, background = per-pixel median, ROI = whole frame, acceptance
criteria relaxed) at 200 fps; a synthetic ellipse set is generated when the
asset is absent. KDE runs at its harshest cadence (500 ms). Windows,
2026-09-23, this developer PC:

| phase | capture | algo FPS | ring appends / 8 s | overlay lag | GUI 5 ms-tick p99 / max | estimates |
|---|---|---|---|---|---|---|
| KDE off | 199 fps | 144.6 | 2366 | 2.9 frames | 376 / 402 ms | — |
| KDE on (500 ms) | 199 fps | 141.3 | 2339 | 2.1 frames | **45 / 56 ms** | 17 × 1000 pts, 2 ms each |
| KDE off again | 199 fps | 143.2 | 1029 / 4 s | 1.8 frames | 382 / 396 ms | — |

- Capture, processing throughput (−2 %, within run-to-run noise), ring
  filling and overlay lag are unaffected by the estimate.
- **Per-point configuration was the real cost.** The first cut applied
  colours with `QXYSeries::setPointsConfiguration`; the same harness then
  measured GUI 5 ms-tick p99 / max of 592 / 621 ms with KDE on (≈ +220 ms
  per 500 ms refresh over the baseline). Routing points into eight
  density-level series instead brought it to 45 / 56 ms.
- **Pre-existing finding:** with KDE *off*, every 500 ms refresh already
  stalls the GUI thread ~380 ms at this load (1000 points in one plain
  `QScatterSeries` plus thumbnails). The same points spread over eight
  series repaint in ~45 ms, so the plain series path is the suspect; not
  changed here (out of scope), worth a follow-up (TD candidate).
- Test-harness lessons: on Windows the Conan Qt 6.7.3 offscreen platform
  deadlocks in the `QApplication` constructor when anything was written to
  stderr before it (and, intermittently, when stdout/stderr are pipes); the
  e2e uses the native `windows` platform on Windows like
  `processing_core_dialog_test`. Starting capture re-applies the config
  file, so the relaxed criteria and the ROI (config ROI 704,500 is outside
  512x96 frames; the screenshot tour moves it too) are applied afterwards.
  Screenshots of both states go to `MIB_KDE_E2E_OUT`.

## Verification (Windows, `windows-ninja`, 2026-09-23)

- `mib_backend_tests monitoring_density_test` — kernel invariants, per-axis
  separation + isotropic control, degenerate/non-finite input, order
  invariance, ramp monotonicity, 4× points → 17.6× time (gate 64×).
- `mib_frontend_tests monitoring_kde_density_test` — offscreen widget
  scenario (see the test header).
- `mib_frontend_tests monitoring_kde_e2e_test` (label `integration`, not in
  the Windows fast lane) — the mock-camera end-to-end above, 28 s.
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

## 2026-09-24 — core contour (PR 1 of the core-region plan)

Solid 90% core contour (true KDE iso-line: level from the point densities,
128x64 grid, marching squares) plus a dashed pinned reference, controls in
the settings dialog only. See
`docs/exec-plans/completed/2026-09-24-kde-core-region-split.md`.

## 2026-09-24 — stored records and full-run contour (PR 2, PR 3)

Provisional record at stop via the coordinator, reference from file, Review
tab drawing and the right-click full-run computation with save. Plan
completed; open debt: TD-16 (plain-series GUI cost), TD-17 (Review uses the
current pixel-to-micron factor).

## 2026-09-25 — Linux backend lane (container, pre-PR)

`linux-backend-only` (GCC 13, Ubuntu 24.04 system packages) builds cleanly
with the branch; `ctest --preset linux-backend-only-test` passes every KDE
test. `recording.kde_full_run_core` failed only when run as **root**: root
ignores permission bits, so the "read-only on disk" refusal cannot be
provoked. The test now asserts that refusal only when the OS refuses a
read-write open of the chmod'ed file for the current user, and prints a NOTE
otherwise; verified both ways (root: NOTE + pass; `runuser -u nobody`:
refusal asserted + pass). The unrelated `scripts.run_processing_conformance_input`
failed there because the container's Python 3.11 loaded Ubuntu's 3.12 numpy.

Sanitizers (same configure flags and label filter as `sanitizers.yml`):
TSan 84/85 and ASan+UBSan 84/85, with every KDE test and
`e2e.experiment_coordinator` clean under both; the one failure is the
numpy mismatch above.

## 2026-09-26 — estimate moved into the backend (migration away from Qt)

The user asked how to guarantee the KDE never affects experiment
performance, then noted the migration away from Qt. The Qt-specific
hardening (a low-priority Qt pool) would have been thrown away, so the
estimate moved into `backend::services::MonitoringDensityService`:
lowest-priority `std::thread`, load back-off (dropped frames / queue ≥ 25%),
compute budget (next wake ≥ 20× last compute), fingerprint skip, record
sink to the coordinator (no GUI in the record path). Headers moved to
`include/backend/processing/` (`backend::monitoring`); the tab polls the
service generation every 100 ms and pushes settings + axes; settings
persistence stays in `QSettings` for now.

Verification (Linux container, GCC 13, Ubuntu 24.04 packages):
- `linux-backend-only` build + `ctest --preset linux-backend-only-test`:
  119/119 (conformance-input test excluded, container numpy mismatch).
- `backend.monitoring_density_service`, `e2e.experiment_coordinator`
  (new run exp3: the file carries the service's record, 400 cells).
- `performance.monitoring_density_contention`: 4 workers running
  `computeProcessedFrame` flat out, 24212 frames/s density off vs 24864 on
  (ratio 1.03); uncontended, a 2500-cell estimate took 121 ms and the next
  wake was 2420 ms (duty 6.2% over a 4 s window including the first,
  unbudgeted estimate).
- Qt `linux-system-release` (apt Qt 6.4.2): `frontend.monitoring_kde_density`
  5/5 runs, all 26 `frontend` tests pass (`frontend.hdf_review_core` after
  making its read-only case conditional on the OS refusing writes, as for
  `recording.kde_full_run_core`; verified as root and as `nobody`).
- `integration.monitoring_kde_e2e` with the synthetic-frames fallback does
  not run in this container (processing stays at 0 fps, on the unchanged
  branch too); with the Hugging Face frames it does — see below.
- Sanitizers (flags and label filter of `sanitizers.yml`): ASan+UBSan 85/85;
  TSan 84/85 with `backend.monitoring_density_service` and
  `e2e.experiment_coordinator` clean. The one TSan failure,
  `recording.hdf_export_service` (last-round timing ratio), ran while
  parallel builds loaded the CPU and passed when re-run alone; the export
  code is untouched by this branch.

### 2026-09-26 — e2e with the Hugging Face frames (before the PR)

`python3 scripts/provision-assets.py --asset 512x96stream-mock-frames
--count 1000`, then `mib_frontend_tests monitoring_kde_e2e_test`. First run:
KDE on gave one estimate per 9 s phase — each estimate cost ~600 ms of CPU
inside the busy app (profile: per-point pass 70 ms, normaliser 145 ms, grid
368 ms; the same code costs ~60 ms uncontended, and the bench's MSVC build
~20 ms), and the budget, charged on wall time, then waited 12 s. Fixes, each
with a regression test that fails without it:
- separable grid: `exp(-(dx²+dy²)/2) = exp(-dx²/2)·exp(-dy²/2)`, (nx+ny)·n
  `exp()` calls + nx·ny·n multiply-adds (direct 42 ms → 7.4 ms uncontended,
  max |diff| 3.6e-12);
- one pairwise pass for densities and normaliser (`rawKdeAtPoints` +
  `normaliseByMaximum`) instead of a second full n² `rawKdeMaximum` pass;
- budget charged on thread CPU time (`CLOCK_THREAD_CPUTIME_ID` /
  `GetThreadTimes`); the contention test's starved phase fails with the
  wall-clock budget (next wake 78.5 s) and passes with CPU time (3.2 s).

Result (two runs): capture 200 fps in every phase; processing 145.9 / 144.3
fps off vs 143.1 / 140.9 on; overlay lag 0.9–1.4 frames on; ~90 ms CPU per
1000-cell estimate, next wake ~1.8 s; 4–5 estimates per 8 s phase; stored
live record 900 of 1000 cells; GUI p99 ~97 ms on vs ~684 ms off (TD-16).
The e2e's "≥ 6 estimates at 500 ms" gate became "≥ 2 estimates, interval +
budget kept, lowest priority". Suites afterwards: backend 119/119, `frontend`
27/27 (e2e included).

