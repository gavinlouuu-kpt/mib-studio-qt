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
| Commits (oldest first) | `9aa04d2` KDE density colouring · `8bf5243` mock-camera e2e + level-series rendering · `b56cd33` / `0886e14` core-region spec · `cfa903d` PR 1 live contour + pinned reference · `64f9c4b` PR 2 stored live record, reference from file, Review drawing · `1ba71ed` PR 3 full-run contour computed and saved from Review · this handover |
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

## 3. Tests added or extended

| Test | Covers |
|---|---|
| `frontend.monitoring_density` (backend runner) | kernel invariants, per-axis separation with an isotropic control, degenerate/non-finite input, order invariance, colour ramp, ratio-gated quadratic cost; core level rank semantics, one loop per cloud enclosing ~90%, two clouds → two loops, translation invariance, border-cut loops closed |
| `recording.kde_core_roundtrip` | both records survive close/reopen byte-identical; codec reproduces every field; records independent; rewrite replaces |
| `recording.kde_core_fault` | writes refused with no file / read-only; codec rejects malformed, non-object, missing/future schema, bad vertices, wrong types; garbage stored verbatim then rejected; non-string attribute reads as absent |
| `recording.kde_full_run_core` | full-run record deterministic, subsample cap, exclusions counted, 89.2% of 20000 cells inside the 90% contour; `openFileForUpdate` keeps frames/info/live record, refuses missing and read-only files |
| `e2e.experiment_coordinator` (extended) | record offered while idle ignored, last record before Stop written, next run does not inherit it |
| `frontend.monitoring_kde_density` (offscreen widget) | toggle, async estimate, late points, level series, contour series, pin/clear, fraction change, record contents, reference from file (preference order, refusals), persistence and restore, dialog controls |
| `frontend.hdf_review_core` | stored contours drawn/cleared, unreadable record ignored, full-run compute + save, decline/confirm overwrite, read-only file |
| `integration.monitoring_kde_e2e` | real `MainWindow` on the mock camera at 200 fps (asset `512x96stream-mock-frames`, synthetic ellipses if absent), KDE off/on/off phases gated on ratios; a real experiment with KDE on whose file must carry the live record |

## 4. Verification evidence (Windows 11 bench PC, `windows-ninja` Release)

| Check | Result |
|---|---|
| `ctest --preset windows-ninja-test` | 123/124; the failure is `frontend.mainwindow_shutdown` (fixed 1 s exit budget; shutdown drains an eGrabber probe of the bench's physical Coaxlink board, ~1.4 s). Reproducible on this host, outside the changed code; **not re-run on the base commit** |
| `ctest -L monitoring` (7 tests incl. the e2e) | 7/7 |
| `integration.monitoring_kde_e2e` phases | capture 199–200 fps in every phase; processing algo FPS 141–145 off and on; overlay lag 1–2 frames; estimate incl. contour grid 18–21 ms per 1000 points on the worker; GUI 5 ms-tick p99 ~375 ms with KDE off (pre-existing, TD-16) vs ~46 ms with KDE on; stored record 900 of 1000 cells |
| `scripts/check_docs.py`, `scripts/check_screenshots.py` | OK / 9 in sync |
| HDF5 "attribute open failed" stderr traces during experiment finalization | pre-existing: identical count (242) from `experiment_coordinator_test` on the unmodified main-checkout build |

## 5. Not done — the consumer's list

1. **Open the PR** against `develop` (title suggestion: "Monitoring: KDE
   density colouring and core contour with live/full-run records"). Do not
   squash; the commits map to the plan's PR 1–3.
2. **CI lanes not runnable on the bench:** `backend-ci.yml` (Linux
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

## 6. Behaviour notes a reviewer should know

- The live contour and the stored live record describe the **rolling
  1000-cell buffer**, not the whole run; the record says so
  (`provisional: true`, `source: "live-buffer"`). Only the Review
  computation produces `provisional: false`.
- Stop never waits for the GUI: the coordinator keeps the last pushed
  record; an estimate landing after Stop is not stored. A failed record
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
verified locally on branch feat/monitoring-kde-density (pushed to origin, on
top of develop@2fe0282, no PR yet). Read, in this order:
AGENTS.md; docs/exec-plans/active/2026-09-24-monitoring-kde-density-handoff.md
(this handover); docs/exec-plans/completed/2026-09-24-kde-core-region-split.md;
knowledge_map/frontend/ExperimentMonitoringTab.md;
knowledge_map/frontend/HdfReviewTab.md; knowledge_map/data-model/HDF5-Storage.md.

Your job, in order:
1. Open a PR from feat/monitoring-kde-density into develop. Body: the
   delivered table (handover §2), tests (§3), verification (§4) and the
   explicit "not done / not verified" list (§5). Do not squash or rewrite
   the existing commits.
2. Watch every CI lane (backend-ci, sanitizers TSan + ASan/UBSan, docs-ci,
   Windows packaging). Fix failures on the same branch with
   regression-first tests. Code under test: include/frontend/tabs/
   {MonitoringDensity,KdeCoreRecord}.h, src/frontend/tabs/
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
4. Only if asked: address TD-17 (Review should use the file's recorded
   pixel-to-micron factor for the scatter and the full-run contour) or
   TD-16 (plain scatter GUI stall), each with its own tests.
5. Report: files changed, commands run with results, anything left open.

Rules: the user wants the core region shown ONLY as a contour on the
scatter (no readouts, no new tab widgets; controls live in Monitoring
Settings). The live record is provisional and never recomputed at Stop;
the full-run record is computed only on explicit request in Review. No
client touches a run's HDF5 file while the coordinator owns it. KDE work
stays off the GUI thread. Use spdlog; headers mirror src; run Qt tests from
PowerShell on Windows. Bench recipe: handover §7.
```
