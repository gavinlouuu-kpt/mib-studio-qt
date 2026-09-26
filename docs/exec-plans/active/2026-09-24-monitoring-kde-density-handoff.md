# Handover: Monitoring scatter density (KDE) and core contour

Status: active

Date: 2026-09-24. Author: the agent sessions that implemented the feature on
the Windows bench PC (2026-09-23/24). Consumer: whoever reviews the PR,
watches CI and does operator acceptance. Companion documents: the completed
execution plan
[`../completed/2026-09-24-kde-core-region-split.md`](../completed/2026-09-24-kde-core-region-split.md)
(spec, decision log, progress per PR), task note
`knowledge_map/task/2026-09-23-monitoring-kde-density.md`, vault notes
`knowledge_map/frontend/ExperimentMonitoringTab.md` and
`knowledge_map/frontend/HdfReviewTab.md`, storage schema in
`knowledge_map/data-model/HDF5-Storage.md`.

## 1. Where the work is

| Item | Value |
|---|---|
| Repository | `gavinlouuu-kpt/mib-studio-qt` |
| Branch | `feat/monitoring-kde-density`, pushed to `origin`; **no PR opened** |
| Base | `develop` at `2fe0282` (tip of `develop` when pushed) |
| Commits (oldest first) | `9aa04d2` KDE density colouring · `8bf5243` mock-camera e2e + level-series rendering · `b56cd33` / `0886e14` core-region spec · `cfa903d` PR 1 live contour + pinned reference · `64f9c4b` PR 2 stored live record, reference from file, Review drawing · `1ba71ed` PR 3 full-run contour computed and saved from Review · `a33808f` this handover · `21741f6` root-safe read-only test · `3c19e0b` Linux results · then the move of the estimate into the backend (2026-09-26, §2a) |
| Continuation branch | `claude/monitoring-kde-density-handover-mf5q0m` = `feat/monitoring-kde-density` + the commits after `a33808f`; open the PR from this branch |
| Size | 38 files, +4608 / −98 before this document |
| Local worktree | `C:\Users\ERBG07\Developer\mib-studio-qt\.claude\worktrees\feat-monitoring-kde-density` (bench PC) |

Untracked local state that must **not** be committed: `build-ninja/`,
`build/vendor/` (YOLO asset copy, SDKs), `build-ninja-setup.bat` (bench build
wrapper). Running the Python-backed tests rewrites the tracked
`scripts/__pycache__/export_hdf5.cpython-313.pyc`; restore it with
`git checkout -- scripts/__pycache__/export_hdf5.cpython-313.pyc` before
committing.

## 2. What was delivered

The request: "Users would like to see KDE calculated periodically to see the
scatter plot density population in the Monitoring tab", then "between
experiments users want to see if the KDE 0.9 region has shifted", narrowed by
the user to **only a contour on the scatter** (no readouts, no tab buttons).

| Capability | Delivered | Where |
|---|---|---|
| Density colouring | **Density (KDE)** toggle in the Monitoring top row; Gaussian KDE at every sample with per-axis Silverman bandwidth × factor, normalised to [0, 1], on a worker (`QtConcurrent` + `QFutureWatcher`, one job at a time, unchanged buffers skipped) every 2 s by default; points routed into eight density-level `QScatterSeries` (target group as rectangles) | `include/frontend/tabs/MonitoringDensity.h`, `src/frontend/tabs/ExperimentMonitoringTab.cpp` |
| Core contour (live) | Solid blue iso-line enclosing the densest `Core %` (default 90%): level = ceil(p·n)-th largest point density, same kernel on a 128×64 grid over the fixed axes, marching squares; one loop per cluster | `MonitoringDensity.h` (`coreLevel`, `gaussianKdeGrid`, `isoContours`), tab `redrawKdeContours` |
| Reference contour | Dashed orange; *Pin current*, *From file…* (prefers the full-run record), *Clear* in Settings ▸ Monitoring Settings; file path persisted and restored | `src/frontend/dialogs/MonitoringSettingsDialog.cpp`, tab `loadKdeReferenceFromFile` |
| Settings | Bandwidth factor, update interval, core %, reference path in `QSettings` `Monitoring/Kde*` (versioned); the old never-called isotropic `computeKDE` grid and grid-resolution setting removed | tab `load/saveKdePreferences`, `resources/ui/MonitoringSettingsDialog.ui` |
| Stored live record | Provisional copy of the on-screen contour at Stop, `/monitoring @kde_live_json`; the tab pushes it to `ExperimentCoordinator::setLiveKdeCoreRecord` after every estimate while a run is Active; the coordinator writes it best-effort at finalization | `ExperimentCoordinator.{h,cpp}`, `Hdf5Service::{write,read}KdeLiveJson` |
| Record codec | Qt-free JSON (`kde_core_schema_version` = 1, nlohmann), contours as polylines in µm² / deformability, malformed or future documents rejected with a reason | `include/frontend/tabs/KdeCoreRecord.h` |
| Review tab | Draws stored contours (full-run solid, live dashed); right-click **Compute core contour from full run** (all valid cells, fixed-seed 5000-cell subsample above that) saves `/analysis @kde_core_json` after confirming an overwrite; read-only files show it unsaved | `src/frontend/tabs/HdfReviewTab.cpp`, `KdeCoreRecord.h::computeFullRunCoreRecord`, `Hdf5Service::openFileForUpdate` |

## 2a. 2026-09-26: the estimate moved into the backend

The user asked how to guarantee the KDE never affects experiment
performance and noted the migration away from Qt, so the Qt worker job was
replaced by a backend service rather than hardened in Qt:

| Change | Where |
|---|---|
| `MonitoringDensityService`: one `std::thread` at the lowest OS priority (SCHED_IDLE / THREAD_PRIORITY_LOWEST); tick skipped when frames were dropped or the batch queue is ≥ 25% full; unchanged input skipped; next wake ≥ 20× the last compute (≤ ~5% of one core); immutable result + generation; provisional record handed to `ExperimentCoordinator::setLiveKdeCoreRecord` by the backend | `include/backend/services/MonitoringDensityService.h`, `src/backend/services/MonitoringDensityService.cpp`, wired in `AppBackend` (constructed in the constructor, stopped first at shutdown) |
| Metrics-only ring copy (no image refs under the ring lock), test seam, queue capacity in the batch stats | `ProcessingService::getMonitoringValidPoints`, `appendMonitoringFrameForTests`, `BatchPipelineStats::queueCapacity` |
| Kernel + record codec moved, namespace `backend::monitoring` | `include/backend/processing/{MonitoringDensity,KdeCoreRecord}.h` |
| Qt tab: no worker job; pushes settings + chart axes (enabled = toggle on and tab visible), polls the service generation every 100 ms, adopts results on the GUI thread; `QSettings` persistence unchanged | `ExperimentMonitoringTab.{h,cpp}` |
| Kernel cost: separable contour grid ((nx+ny)·n `exp()` calls instead of nx·ny·n) and one pairwise pass for densities + normaliser (`rawKdeAtPoints`, `normaliseByMaximum`); the budget charges thread **CPU** time, not wall time (a starved idle-priority worker must not stretch its own interval). In the running app on Linux: ~600 → ~90 ms CPU per 1000 cells | `include/backend/processing/MonitoringDensity.h`, service `compute()`, `KdeCoreRecord.h::computeFullRunCoreRecord` |
| e2e gate: "≥ 6 estimates at the 500 ms cadence" became "≥ 2 estimates, and the service keeps its interval + compute budget and runs at the lowest priority"; the Density tooltip shows the effective refresh | `tests/frontend/monitoring_kde_e2e_test.cpp`, tab `refreshKdeTooltip` |
| Not yet: bridge (`BackendFacade` pull + contract/ABI bump) and React scatter colouring; backend-owned settings persistence | follow-up PR |

## 3. Tests added or extended

| Test | Covers |
|---|---|
| `processing.monitoring_density` (backend runner; was `frontend.monitoring_density`) | kernel invariants, per-axis separation with an isotropic control, degenerate/non-finite input, order invariance, colour ramp, ratio-gated quadratic cost; core level rank semantics, one loop per cloud enclosing ~90%, two clouds → two loops, translation invariance, border-cut loops closed |
| `recording.kde_core_roundtrip` | both records survive close/reopen byte-identical; codec reproduces every field; records independent; rewrite replaces |
| `recording.kde_core_fault` | writes refused with no file / read-only; codec rejects malformed, non-object, missing/future schema, bad vertices, wrong types; garbage stored verbatim then rejected; non-string attribute reads as absent |
| `recording.kde_full_run_core` | full-run record deterministic, subsample cap, exclusions counted, 89.2% of 20000 cells inside the 90% contour; `openFileForUpdate` keeps frames/info/live record, refuses missing and read-only files (the read-only check is skipped with a NOTE when the user bypasses permission bits, e.g. root) |
| `e2e.experiment_coordinator` (extended) | record offered while idle ignored, last record before Stop written, next run does not inherit it; a run where only `MonitoringDensityService` supplies the record (file carries it, 400 cells) |
| `backend.monitoring_density_service` | interval budget and load policy, clamping, disabled no-op, first estimate + record to the sink, fingerprint skip, interval-only change, fraction/axis change, back-off on drops and backlog, disable/re-enable, empty ring, prompt stop, four concurrent callers (TSan lane) |
| `performance.monitoring_density_contention` | uncontended duty cycle within the 20× budget; real `computeProcessedFrame` on every core with the service off vs on in alternating windows, median throughput ratio ≥ 0.90; starved worker: next wake bounded by the CPU cost, not the wait (fails with a wall-clock budget: 78.5 s) |
| `processing.monitoring_density` (extended) | separable grid equals the direct radial sum (≤ 1e-7) at ≤ 1/4 of its cost; one pass yields densities and normaliser |
| `frontend.monitoring_kde_density` (offscreen widget) | toggle, async estimate, late points, level series, contour series, pin/clear, fraction change, record contents, reference from file (preference order, refusals), persistence and restore, dialog controls |
| `frontend.hdf_review_core` | stored contours drawn/cleared, unreadable record ignored, full-run compute + save, decline/confirm overwrite, read-only file (refusal asserted only where the OS refuses the write, e.g. not as root) |
| `integration.monitoring_kde_e2e` | real `MainWindow` on the mock camera at 200 fps (asset `512x96stream-mock-frames`, synthetic ellipses if absent), KDE off/on/off phases gated on ratios; a real experiment with KDE on whose file must carry the live record |

## 4. Verification evidence (Windows 11 bench PC, `windows-ninja` Release)

| Check | Result |
|---|---|
| `ctest --preset windows-ninja-test` | 123/124; the failure is `frontend.mainwindow_shutdown` (fixed 1 s exit budget; shutdown drains an eGrabber probe of the bench's physical Coaxlink board, ~1.4 s). Reproducible on this host, outside the changed code; **not re-run on the base commit** |
| `ctest -L monitoring` (7 tests incl. the e2e) | 7/7 |
| `integration.monitoring_kde_e2e` phases | capture 199–200 fps in every phase; processing algo FPS 141–145 off and on; overlay lag 1–2 frames; estimate incl. contour grid 18–21 ms per 1000 points on the worker; GUI 5 ms-tick p99 ~375 ms with KDE off (pre-existing, TD-16) vs ~46 ms with KDE on; stored record 900 of 1000 cells |
| `scripts/check_docs.py`, `scripts/check_screenshots.py` | OK / 9 in sync |
| Linux container, 2026-09-25 (pre-PR, same filters as `backend-ci.yml` / `sanitizers.yml`) | `linux-backend-only` GCC 13 build clean; `ctest --preset linux-backend-only-test` all pass after `21741f6` (`recording.kde_full_run_core` had failed only because the container runs as root); TSan and ASan+UBSan: 84/85, every KDE test and `e2e.experiment_coordinator` clean. The one failure in all three runs, `scripts.run_processing_conformance_input`, is the container's Python 3.11 loading Ubuntu's 3.12 numpy (not this branch) |
| Linux container, 2026-09-26, after the backend move (§2a) | `linux-backend-only`: 119/119 (conformance-input test excluded, container numpy); `performance.monitoring_density_contention`: processing 24212 vs 24864 frames/s density off/on (4 workers), duty 5.2% of one core between the 1st and 3rd estimate, worker at SCHED_IDLE; TSan 84/85 with the service test and `e2e.experiment_coordinator` clean (the one failure, `recording.hdf_export_service`'s last-round timing ratio, happened while parallel builds loaded the CPU and passed when re-run alone); ASan+UBSan 85/85; Qt `linux-system-release` (apt Qt 6.4.2): all 26 `frontend` tests, `frontend.monitoring_kde_density` 5/5 runs. `integration.monitoring_kde_e2e` not runnable in the container (§5 item 7) |
| Linux container, 2026-09-26, e2e with real frames | `integration.monitoring_kde_e2e` with the Hugging Face `512x96stream-mock-frames` asset (1000 frames): capture 200 fps off/on, processing 145.9/144.3 fps off vs 143.1/140.9 on (two runs), overlay lag 0.9–1.4 frames, 4–5 estimates per 8 s phase at ~90 ms CPU each spaced ~1.8 s by the budget, stored record 900 of 1000 cells; passes via ctest too (27/27 `frontend`). The first run exposed the kernel's real cost inside the busy app (~600 ms CPU per 1000 cells: 8 M grid `exp()` calls plus a second pairwise pass) and a budget charged on wall time; fixed by the separable grid, the one-pass normaliser and a CPU-time budget (§2a). After the fix: backend 119/119, `frontend` 27/27, TSan 85/85, ASan+UBSan 85/85 |
| HDF5 "attribute open failed" stderr traces during experiment finalization | pre-existing: identical count (242) from `experiment_coordinator_test` on the unmodified main-checkout build |

## 5. Not done — the consumer's list

1. **Open the PR** against `develop` (title suggestion: "Monitoring: KDE
   density colouring and core contour with live/full-run records"). Do not
   squash; the commits map to the plan's PR 1–3.
2. **CI lanes not runnable on the bench** (backend and both sanitizer filters since run in a Linux container, §4; the PR's own runs are still owed): `backend-ci.yml` (Linux
   backend-only: `MonitoringDensity.h`/`KdeCoreRecord.h` are compiled by the
   backend runner tests; watch `std::nan`, `<random>` and nlohmann includes
   on GCC), `sanitizers.yml` (TSan/ASan on the coordinator setter and the
   Hdf5Service attribute helpers), `docs-ci.yml`. No PR lane runs
   `frontend.*` tests (TD-9), so the widget, Review and e2e tests are
   verified only on the bench.
3. **Operator acceptance with real cells** (no rig on the bench): enable
   Density during a real run, confirm the contour sits on the visible core,
   stop, open the file in Review, compute the full-run contour, then load it
   as the reference for the next run and confirm a known shift is visible.
4. **Debt left open** (tracker): **TD-16** (plain 1000-point scatter refresh
   stalls the GUI ~380 ms; the density-level series show the fix direction);
   **TD-17** (Review converts areas with the current pixel-to-micron factor,
   not the file's; the full-run contour follows the scatter and records the
   factor it used).
5. **Screenshots:** `experiment-monitoring.png` and
   `dialog-monitoring-settings.png` were regenerated on Windows
   (`QT_QPA_PLATFORM=windows`; the offscreen platform aborts there); the
   other seven are older Linux renders. Regenerate all nine on Linux if a
   consistent look matters.
6. After merge: move this handover to `docs/exec-plans/completed/` and add a
   Recent-Work line noting the merge.
7. **Re-run on the Windows bench after the backend move (§2a):**
   `integration.monitoring_kde_e2e` (green in the Linux container with the
   Hugging Face frames, §4; its synthetic-frames fallback leaves processing
   at 0 fps in the container, on the unchanged branch too) and the full
   `windows-ninja-test` preset; confirm `priorityLowered` in the service
   stats (THREAD_PRIORITY_LOWEST).
8. **Follow-up PR (migration):** expose `MonitoringDensityService` through
   `BackendFacade` + the Rust bridge (append-only contract change, ABI bump
   per ADR 0004) and colour the React Monitoring scatter from it; move the
   `Monitoring/Kde*` settings persistence into the backend.

## 6. Behaviour notes a reviewer should know

- The live contour and the stored live record describe the **rolling
  1000-cell buffer**, not the whole run; the record says so
  (`provisional: true`, `source: "live-buffer"`). Only the Review
  computation produces `provisional: false`.
- The estimate runs in the backend at the lowest OS priority: under full
  CPU load it simply does not run (the contour goes stale) and ticks are
  skipped while the pipeline drops frames or the batch queue backs up. A
  run watched with the Monitoring view hidden gets no estimate and no
  stored record (the monitoring ring is visibility-gated).
- Stop never waits for the estimate: the coordinator keeps the last record
  the service handed over; an estimate landing after Stop is not stored. A failed record
  write logs a warning and never changes the run outcome.
- The full-run core share follows `Monitoring/KdeCoreFraction` so live and
  full-run contours are comparable; its grid spans the padded data range
  (the Review scatter's axis rule).
- Saving from Review closes the review reader, writes through
  `openFileForUpdate`, and reopens; it refuses while an export runs.
- With KDE off the Monitoring chart is the previous plain chart; all
  contour and level series are removed.

## 7. Bench recipe

```text
conan install . -of build-ninja --build=missing -r conancenter -s build_type=Release -c tools.cmake.cmaketoolchain:generator=Ninja
# VS 2022 x64 dev shell; MIB_MINDVISION_SDK_ROOT=<main clone>\build\vendor\mindvision-sdk\extracted\Demo\VC++
# a fresh worktree also needs build\vendor\assets\models\yolo11n-seg\yolo11n-seg.onnx (copy from resources\models)
cmake --preset windows-ninja && cmake --build --preset windows-ninja-build
ctest --preset windows-ninja-test
ctest --test-dir build-ninja -L monitoring --output-on-failure
python scripts/provision-assets.py --asset 512x96stream-mock-frames --count 1000   # optional, e2e input
python scripts/check_docs.py && python scripts/check_screenshots.py
```

Run ctest and Qt test binaries from PowerShell, not Git Bash: the Conan Qt
6.7.3 offscreen platform deadlocks in the `QApplication` constructor when
launched from Git Bash or with stderr written first.

## 8. Prompt for the next agent

Copy verbatim into a fresh session in a clone of the repository:

```text
You are continuing the "Monitoring scatter density (KDE) and core contour"
feature of gavinlouuu-kpt/mib-studio-qt. The implementation is complete and
verified locally on branch claude/monitoring-kde-density-handover-mf5q0m
(= feat/monitoring-kde-density + the backend move of §2a; pushed to origin,
no PR yet). Read, in this order:
AGENTS.md; docs/exec-plans/active/2026-09-24-monitoring-kde-density-handoff.md
(this handover); docs/exec-plans/completed/2026-09-24-kde-core-region-split.md;
knowledge_map/services/MonitoringDensityService.md;
knowledge_map/frontend/ExperimentMonitoringTab.md;
knowledge_map/frontend/HdfReviewTab.md; knowledge_map/data-model/HDF5-Storage.md.

Your job, in order:
1. Open a PR from claude/monitoring-kde-density-handover-mf5q0m into develop. Body: the
   delivered table (handover §2), tests (§3), verification (§4) and the
   explicit "not done / not verified" list (§5). Do not squash or rewrite
   the existing commits.
2. Watch every CI lane (backend-ci, sanitizers TSan + ASan/UBSan, docs-ci,
   Windows packaging). Fix failures on the same branch with
   regression-first tests. Code under test: include/backend/processing/
   {MonitoringDensity,KdeCoreRecord}.h, src/backend/services/
   MonitoringDensityService.cpp, src/frontend/tabs/
   {ExperimentMonitoringTab,HdfReviewTab}.cpp,
   src/frontend/dialogs/MonitoringSettingsDialog.cpp,
   src/backend/app/ExperimentCoordinator.cpp,
   src/backend/recording/Hdf5Service.cpp (KDE record helpers,
   openFileForUpdate). Keep vault notes in sync in the same commits and run
   python scripts/check_docs.py.
3. If a rig with real cells is available, run the operator acceptance in
   handover §5 item 3 and record it in the task note
   knowledge_map/task/2026-09-23-monitoring-kde-density.md; claim nothing
   untested and name what was unavailable.
4. On the Windows bench, re-run integration.monitoring_kde_e2e and the
   windows-ninja-test preset after the backend move (handover §5 item 7).
5. Only if asked: address TD-17 (Review should use the file's recorded
   pixel-to-micron factor for the scatter and the full-run contour) or
   TD-16 (plain scatter GUI stall), each with its own tests.
6. Report: files changed, commands run with results, anything left open.

Rules: the user wants the core region shown ONLY as a contour on the
scatter (no readouts, no new tab widgets; controls live in Monitoring
Settings). The live record is provisional and never recomputed at Stop;
the full-run record is computed only on explicit request in Review. No
client touches a run's HDF5 file while the coordinator owns it. KDE work
stays in the backend MonitoringDensityService (lowest priority, load
back-off, compute budget); shells only push settings and read results. Use spdlog; headers mirror src; run Qt tests from
PowerShell on Windows. Bench recipe: handover §7.
```
