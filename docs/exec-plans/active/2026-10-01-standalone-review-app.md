# MIB Review: the Review tab as its own application on macOS and Windows

Status: active

Date: 2026-10-01. Companion notes: `knowledge_map/frontend/HdfReviewTab.md`,
`knowledge_map/build-and-run/Build.md`, `docs/howto/build-installer.md`,
`docs/howto/auto-update-r2.md`, ADR
[0001](../../decisions/0001-react-tauri-migration.md) (React + Tauri is the
long-term shell), decoupling plan
[`2026-07-15-qt-decoupling-and-tauri-migration.md`](2026-07-15-qt-decoupling-and-tauri-migration.md),
review scatter plan
[`2026-09-30-review-scatter-click-to-view.md`](2026-09-30-review-scatter-click-to-view.md).

## Goal

A second shipped desktop product, **MIB Review**, that opens the HDF5 files
MIB Studio records and offers everything today's Review tab offers (browse
valid/invalid frames, metrics table, charts with zoom/pan/click-to-view,
KDE core contour, single and batch export, regenerate masks) on
**macOS (Apple Silicon) and Windows x64**, with no camera, no hardware SDK
and no experiment pipeline in the binary. It installs from a signed
Windows installer and a signed, notarised macOS DMG, updates from the
existing R2 channel layout, and is built and tested in CI on both
platforms. MIB Studio's own Review tab keeps working and is built from the
same source files.

## What the survey found (2026-10-01)

Why the Review tab is a good candidate to split out:

- **Its only link to the app is one reference.** `HdfReviewTab` takes
  `backend::AppBackend&` (`include/frontend/tabs/HdfReviewTab.h:81`) and
  calls nothing on it but `backend_.processing()` (pixel-to-micron factor
  at 7 sites, `getProcessingConfig()` once for the histogram range). It
  hands the same reference to `BatchMaskDialog`, which uses
  `getProcessingConfig`, `activeProcessingCoreIdentity` and `processBatch`.
  `MainWindow` creates it at `src/frontend/core/MainWindow.cpp:524` and
  connects no signal or slot to it; it is not even a member.
- **Every backend service it touches is in the Qt-free `mib_processing`
  library**: `Hdf5Service` (constructed directly, not through
  `AppBackend::hdf5()`), `HdfExportService`, `ProcessingService`,
  `BatchMaskSources`, `KdeCoreRecord` (header-only). It does not use
  `BackendFacade`, `PlaybackService`, `YoloService`, `FrameStore` or the
  `ExperimentCoordinator`.
- **Eight frontend units make up the feature**: `HdfReviewTab`,
  `BatchMaskDialog`, `FrameViewerDialog`, `HdfMetricsModel`,
  `OverlayRenderer`, `HdfReviewExportPaths`, `ElidingLabel`,
  `ZoomableChartView` (+ `RoiDrawCanvas`, header-only `ScatterHitTest`),
  about 5 500 lines, all compiled today into `mib_frontend_common`
  together with `MainWindow` and every other tab.
- **Runtime data it needs outside CMake**: the isoelastic curve file
  `resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt`
  (resolved relative to `applicationDirPath()`,
  `HdfReviewTab.cpp:2921-2931`) and, for regenerate masks, the bundled
  processing kernel or a signed processing-core plugin.
- **Settings it reads**: `HdfReviewTab/lastExportDir`,
  `Review/ChartsSplitter`, `Review/ChartsRightSplitter`,
  `Monitoring/KdeCoreFraction`, under the QSettings identity set by
  `frontend::applicationsettings::initialize()` (org "MIB Studio", app
  "MIB Studio Qt").
- **Tests**: `frontend.hdf_review_core`, `frontend.hdf_review_scatter`,
  `frontend.hdf_review_export_paths` build the tab on a bare
  `AppBackend` (`MIB_DISABLED_SERVICES=auto_update,autofocus,trigger,yolo,
  syringe_pump`); `integration.review_scatter_e2e` needs the whole
  `MainWindow` and stays with MIB Studio.

What stands in the way:

- **No macOS build exists.** No CMake preset, no Conan profile, no
  `MACOSX_BUNDLE`, no `macos-*` CI runner. `env/brew-packages.txt` and
  `scripts/doctor.sh` / `bootstrap.sh` already know Homebrew but say "no
  CMake preset yet". Apple-specific gates: OpenSSL (Ed25519 verification
  of processing cores) is required only `UNIX AND NOT APPLE`
  (`src/backend/CMakeLists.txt:103`); the native processing-core descriptor
  is generated only for `WIN32` or `UNIX AND NOT APPLE`
  (`src/backend/CMakeLists.txt:218-232`); two tests exclude Apple.
- **Packaging and updates are Windows-only.** Inno Setup (`resources/
  installers/*.iss`, AppId `{A1B2C3D4-…}`, AppName "MIB Studio Qt"),
  `windeployqt` from the Conan package, `release.yml` / `build-windows.yml`
  publish to GitHub Releases and R2 (`updates.yofo.bio/<channel>/
  latest.json`). `AutoUpdater` runs the installer with `ShellExecuteW
  runas`; the non-Windows branch returns "only implemented on Windows".
  Nothing shipped is code-signed except the processing-core DLL
  (`python-wheel.yml`, internal KPT root CA). There is no Apple signing or
  notarisation anywhere.
- **The React + Tauri shell is not ready to be the review product**: its
  Charts tab is a placeholder, chart export is a listed non-goal, Tauri
  bundling is off (`"bundle": {"active": false}`), `crates/mib-bridge/
  build.rs` has no macOS link path, and its updater reads a manifest
  format that does not match the Qt one.

## Decision log

- 2026-10-01: **Ship MIB Review v1 on the Qt shell, reusing `HdfReviewTab`
  unchanged in behaviour.** The Qt tab is complete, tested and documented;
  the React Charts view (#268/#470) does not exist yet and the Tauri shell
  cannot be bundled or updated today. ADR 0001 keeps the Qt shell building
  until Tauri passes the epic's exit gate, so a Qt review product is inside
  that transitional state. Record this in **ADR 0008** with the exit
  condition: when the Tauri review slices (PR 3a/PR 4 of the scatter plan,
  chart export, bundling, updater) reach parity, MIB Review switches shell
  under the same product name, installer identity and update channel. The
  decoupling plan's parity matrix gets a "MIB Review" column so the switch
  is measurable.
- 2026-10-01: **Narrow the constructor, do not fork the tab.** `HdfReviewTab`
  and `BatchMaskDialog` take a `backend::services::ProcessingService&`
  instead of `AppBackend&`. MIB Studio passes `backend_.processing()`; MIB
  Review owns one `ProcessingService` directly. One source tree, two
  executables; no `#ifdef MIB_REVIEW_APP` in the tab.
- 2026-10-01: **New static library `mib_review_common`** holding the eight
  review units plus `ApplicationSettings`, linked by both
  `mib_frontend_common` (which stops compiling those files itself) and the
  new `mib_review` executable. `mib_review` links `mib_processing`,
  `Qt6::Widgets Charts Concurrent`, spdlog and OpenCV, and **not
  `mib_backend`**, so the binary carries no camera, serial, SQLite, ONNX
  or discovery code. Crash reporting (`CrashReporter` lives in
  `mib_backend`) is therefore out of v1; a follow-up can move the
  reporter into its own small library if field crash data is wanted.
- 2026-10-01: **Separate product identity, shared version.** Product name
  "MIB Review", executable `mib_review`, QSettings application name
  "MIB Review" (reads the `Review/*` keys of "MIB Studio Qt" as a one-time
  fallback so an operator who has both keeps their splitter and export
  directory), bundle id `bio.yofo.mib-review`, a new Inno Setup AppId,
  update channels `review-stable/` and `review-beta/` on the same R2
  bucket. The version number is the repo's `PROJECT_VERSION`
  (`cmake/MIBVersion.cmake`): one tag `vX.Y.Z` releases both products, so
  a file recorded by MIB Studio X.Y and opened in MIB Review X.Y is an
  exact pairing and there is one changelog. Independent cadence can come
  later with a `review-v*` tag prefix if the products drift.
- 2026-10-01: **Pixel-to-micron factor is a MIB Review setting, pending
  TD-17.** Today the tab scales with the live backend's factor. MIB Review
  has no live backend, so v1 exposes the factor in a Preferences dialog
  (default from the file's run snapshot when present, else the last used
  value) and shows it in the status line. Fixing TD-17 (read the recorded
  factor) benefits both products and is scheduled as its own step here
  because the standalone app makes the gap visible on every file.
- 2026-10-01: **Regenerate masks ships in v1 with the bundled kernel; plugin
  cores on macOS wait for the Apple signing story.** The bundled kernel
  needs no signature check. Loading a `.dylib` core needs Ed25519
  verification (OpenSSL, currently not required on Apple) and a
  `macos`/`arm64` descriptor, which `publish-processing-core.py` already
  anticipates (`.dylib` extension). Enable OpenSSL on Apple and generate
  the descriptor in the macOS PR; publishing signed macOS cores is a
  separate follow-up.
- 2026-10-01: **macOS targets Apple Silicon only (arm64) in v1.** One Conan
  profile, one runner (`macos-14`), one DMG. An x86_64 or universal build
  is a later decision if an Intel Mac turns up in the field.
- 2026-10-01: **Signing is a user-side prerequisite.** The Windows installer
  is unsigned today; MIB Review's installer stays unsigned until an
  Authenticode certificate trusted outside KPT is available (the internal
  root CA used for the core DLL does not help SmartScreen). macOS requires
  an Apple Developer ID certificate and notarisation for the DMG to open
  without Gatekeeper warnings; PR 4 is blocked until the secrets exist.
  PR 2 and PR 3 produce installable but unsigned artefacts for internal
  testing.

## Design

### Executable and library layout

```
src/frontend/qt/CMakeLists.txt
  mib_review_common   STATIC  (new)  review units + ApplicationSettings
  mib_frontend_common STATIC         links mib_review_common; drops the 9 files
  mib_studio_qt                      unchanged
  mib_review                         (new) src/frontend/review/main.cpp + ReviewWindow
```

`mib_review` main (`src/frontend/review/main.cpp`), modelled on
`src/frontend/core/main.cpp:188-291` minus backend boot:

1. `QApplication`, `applicationsettings::initializeFor("MIB Review")` (the
   function gains a product parameter; the default stays "MIB Studio Qt").
2. Logging to `<app data>/MIB_Review/logs/review.log`. `Logger.cpp` is in
   `mib_backend` (`src/backend/CMakeLists.txt:273`), so either move it into
   `mib_processing` (it is spdlog-only) or give `mib_review` a plain
   spdlog rotating sink; prefer the move so both products log the same way.
3. One `ProcessingService` with the config loaded from the Preferences
   (pixel-to-micron, ROI unset, bundled kernel; processing core catalog
   reused from `ProcessingCoreDialog` only if it links without
   `mib_backend`, else deferred).
4. `ReviewWindow` (`QMainWindow`): menu bar (File: Open…, Open Recent,
   Close, Export submenu mirroring the tab's actions, Quit; Edit:
   Preferences…; Help: About, Check for updates…), the `HdfReviewTab` as
   central widget, a status bar showing the factor and file. File
   association `.h5`/`.hdf5` opened from the command line or Finder
   (`QFileOpenEvent` on macOS).
5. Standard exit; no `DesktopInstance` lock (several review windows are
   fine; opening a second file opens a second window is a non-goal for
   v1, the window just switches file).

### Platform build matrix

| | Windows x64 | macOS arm64 |
|---|---|---|
| Preset | `windows-review` (inherits `windows-ninja-ci`, `MIB_BUILD_REVIEW_ONLY=ON`) | `macos-review` (new; Conan toolchain, `MIB_BUILD_REVIEW_ONLY=ON`) |
| Deps | Conan `windows-msvc194` | Conan profile `macos-appleclang-arm64` (new), same `conanfile.py` requires; `onnxruntime` already Windows-only |
| Qt deploy | existing `mib_configure_windows_deployment` | `macdeployqt` from the Conan Qt package, `MACOSX_BUNDLE`, `Info.plist.in`, `resources/icons/mib_review.icns` |
| Package | Inno Setup `resources/installers/mib-review.iss` → `build/dist/MIB_Review_Setup_vX.Y.Z.exe` | `hdiutil` DMG (`build/dist/MIB_Review_vX.Y.Z.dmg`) via `scripts/release/package-macos.sh` |
| Sign | signtool (when a cert exists) | `codesign --deep --options runtime` + `notarytool submit --wait` + `stapler` (when a Developer ID exists) |
| Update | `AutoUpdater` pointed at `review-<channel>/latest.json`, silent Inno Setup run | v1: "Check for updates…" opens the DMG download URL from `latest.json`; in-app replace is a follow-up (Sparkle is the candidate) |
| CI | `build-review.yml` job `windows-2022` | `build-review.yml` job `macos-14` |

`MIB_BUILD_REVIEW_ONLY=ON` configures `mib_processing`, `mib_review_common`,
`mib_review` and the review tests; it does not configure cameras,
`mib_backend`, `mib_studio_qt`, the Tauri bridge or Sentry. A full build
(`windows-default`, `linux-release`) builds `mib_review` too, so the Linux
lanes catch compile breaks early even though Linux is not a release
target.

### Sharing the review tests

`frontend.hdf_review_core`, `frontend.hdf_review_scatter` and
`frontend.hdf_review_export_paths` are re-pointed at
`ProcessingService&` and registered in a `mib_review_tests` runner that
builds under `MIB_BUILD_REVIEW_ONLY`, so the macOS job runs them
offscreen (`QT_QPA_PLATFORM=offscreen`) as the first Apple test lane in
the repo. A new `review.app_smoke` test launches `mib_review` with a fixture
file on the command line, waits for the window, takes a screenshot and
quits (same pattern as `screenshot_tour`).

## Implementation plan

Five PRs. PR 1 is the structural change and lands first; PR 2 (macOS) and
PR 3 (Windows) are independent and can run in parallel; PR 4 needs the
signing secrets; PR 5 closes documentation. Every PR carries its vault
updates and passes `python3 scripts/check_docs.py`.

### PR 1 — Decouple the tab and add the `mib_review` target (Linux + Windows CI)

Files: `include/frontend/tabs/HdfReviewTab.h`, `src/frontend/tabs/HdfReviewTab.cpp`,
`include/frontend/dialogs/BatchMaskDialog.h`, `src/frontend/dialogs/BatchMaskDialog.cpp`,
`src/frontend/core/MainWindow.cpp:524`, `include/frontend/utils/ApplicationSettings.h`
(+ `.cpp`), new `src/frontend/review/{main.cpp,ReviewWindow.cpp,ReviewPreferencesDialog.cpp}`
(+ headers under `include/frontend/review/`, `.ui` under `resources/ui/`),
`src/frontend/qt/CMakeLists.txt`, `cmake/MIBOptions.cmake`, root `CMakeLists.txt`,
`CMakePresets.json` (`linux-review`), `tests/frontend/hdf_review_{core,scatter}_test.cpp`,
new `tests/frontend/review_app_smoke_test.cpp`, `.github/workflows/backend-ci.yml`
(or a new `review-ci.yml` on `ubuntu-24.04`), new ADR
`docs/decisions/0008-standalone-review-app.md`, vault:
`knowledge_map/frontend/HdfReviewTab.md`, new `knowledge_map/frontend/ReviewApp.md`
(+ `_MOC.md`, `README`, `Agent-Onboarding`), `knowledge_map/build-and-run/{Build,Run-Modes}.md`,
`knowledge_map/current-state/Recent-Work.md`, this plan.

1. Change both constructors to `ProcessingService&`; MainWindow and the
   tests pass `backend.processing()`. No behaviour change; the three
   review tests and `integration.review_scatter_e2e` pass unchanged.
2. Make the isoelastic-curve lookup take the resource directory from one
   helper (`frontend::review::resourceDir()`), used by both products, and
   install `resources/isoelastic_curve/` next to the executable in CMake
   so neither product depends on the working directory.
3. `applicationsettings::initialize(product)`; "MIB Review" with the
   one-time `Review/*` fallback read. Test: `frontend.application_settings`
   gains a case for the product parameter and the fallback.
4. `mib_review_common` + `mib_review` targets; `MIB_BUILD_REVIEW_ONLY`
   option; `linux-review` preset that configures with no `mib_backend`.
   `ReviewWindow` with the menu, status bar and the Preferences dialog
   (pixel-to-micron factor, output defaults). Command-line file argument.
5. Tests moved into `mib_review_tests`; `review.app_smoke` added.
6. ADR 0008 (shell choice, exit condition to Tauri); vault notes; a line in
   the decoupling plan's parity matrix.

### PR 2 — macOS build, bundle and DMG (unsigned)

Files: `conan/profiles/macos-appleclang-arm64`, `CMakePresets.json`
(`macos-review`, `macos-review-build`, `macos-review-test`), `cmake/MIBDependencies.cmake`
(Qt component lookup under Conan on Apple), `src/backend/CMakeLists.txt:103,218-232`
(OpenSSL and native-core descriptor on Apple), `src/frontend/qt/CMakeLists.txt`
(`MACOSX_BUNDLE`, `MACOSX_BUNDLE_INFO_PLIST`, icon, `macdeployqt` post-build),
new `resources/macos/Info.plist.in`, `resources/icons/mib_review.icns`,
new `scripts/release/package-macos.sh`, `scripts/doctor.sh`, `scripts/bootstrap.sh`
(drop the "no preset yet" warning, add the Conan step), `env/brew-packages.txt`,
`.github/workflows/build-review.yml` (job `macos-14`), new `docs/howto/macos-build.md`,
vault: `Build.md`, `Dependencies.md`, `Assets.md` if an asset is needed, this plan.

1. Conan profile and preset; first green configure + build of `mib_review`
   on `macos-14`. Expect to fix: Apple-only compiler warnings as errors,
   `<windows.h>` style guards already present, HDF5 shared-library
   rpaths inside the bundle (`macdeployqt` handles Qt; Conan HDF5/OpenCV
   dylibs need `install_name_tool` or `BUILD_RPATH`, decide from the
   first failure).
2. Bundle: `MIB Review.app` with Info.plist (bundle id, version from
   `PROJECT_VERSION_FULL`, `CFBundleDocumentTypes` for `.h5`/`.hdf5`,
   `LSMinimumSystemVersion` 13.0), icon, resources folder containing the
   isoelastic file and the bundled kernel.
3. `package-macos.sh`: `macdeployqt` → optional codesign (no-op without
   identity) → `hdiutil create` DMG with an Applications symlink.
4. CI: build, run `mib_review_tests` offscreen, run `review.app_smoke`,
   upload the DMG as an artifact.
5. Apple gates: require OpenSSL on Apple (Homebrew/Conan `openssl`),
   generate the `macos`/`arm64` core descriptor, unexclude the two Apple
   test skips if they pass.
6. `docs/howto/macos-build.md` and the vault.

### PR 3 — Windows installer, R2 channel and auto-update

Files: `resources/installers/mib-review.iss`, `cmake/MIBWindowsPackaging.cmake`
(`package_review_installer` target), `src/frontend/system/AutoUpdater.{h,cpp}`
(channel prefix parameter and product name), `src/frontend/review/ReviewWindow.cpp`
(Check for updates…), `scripts/release/publish-update.py` (`--product review`
→ `review-stable/`, `review-beta/`), `scripts/release/release.ps1`
(build both installers), `.github/workflows/build-review.yml` (job
`windows-2022`), `.github/workflows/release.yml` (attach the review
installer to the same tag release and publish to R2), `docs/howto/
build-installer.md`, `docs/howto/auto-update-r2.md`, vault `Build.md`,
`System-Utilities.md` (AutoUpdater), this plan.

1. `mib-review.iss`: new AppId, AppName "MIB Review", no EGrabber or VC++
   redist bundling beyond what Qt/OpenCV need, `.h5` file association,
   output `MIB_Review_Setup_vX.Y.Z.exe` (and the update-only variant).
2. `AutoUpdater` learns the channel prefix and product name; MIB Studio
   behaviour is unchanged (test: `frontend.update_catalog` with both
   prefixes).
3. `publish-update.py --product review` writes `latest.json` /
   `index.json` under the review channels with the Windows installer and
   the macOS DMG URL + SHA-256 (the Mac "Check for updates…" reads the
   same manifest).
4. CI: Windows job builds, runs the review tests, builds the installer,
   uploads it. `release.yml` on `v*` tags attaches both products and
   publishes both channels.

### PR 4 — Signing and notarisation (blocked on certificates)

Files: `.github/workflows/build-review.yml`, `.github/workflows/release.yml`,
`scripts/release/package-macos.sh`, `resources/installers/mib-review.iss`
(`SignTool` directive), `deploy/signing/README.md`, `docs/howto/build-installer.md`.

1. macOS: import the Developer ID certificate from a secret into a
   temporary keychain, `codesign` the bundle and every dylib with the
   hardened runtime, `notarytool submit --wait`, `stapler staple` the DMG.
2. Windows: `signtool` on `mib_review.exe` and the installer with a
   publicly trusted certificate (the internal KPT CA is not sufficient for
   SmartScreen; this is a purchase, outside the repo).
3. Verify Gatekeeper (`spctl --assess`) and SmartScreen on clean machines;
   record the procedure in the howto.

Blocked until `APPLE_DEVELOPER_ID_CERT_P12`, `APPLE_NOTARY_*` and a
Windows signing certificate exist as repository secrets.

### PR 5 — Manual, screenshots, TD-17 and hand-over

Files: new `docs/manual/mib-review.md` (install on Mac and Windows, open a
file, differences from the Review tab: Preferences, no live factor),
`docs/manual/review-and-postprocess.md` (cross-link), `mkdocs.yml`,
`src/frontend/tools/screenshot_tour_main.cpp` (or a `review_screenshot_tour`
mode) + `scripts/check_screenshots.py`, `src/frontend/tabs/HdfReviewTab.cpp`
(TD-17: read the recorded `pixel_to_micron` from the run snapshot, fall
back to the service factor), `tests/frontend/hdf_review_core_test.cpp`
(file recorded at another factor), `docs/exec-plans/tech-debt-tracker.md`,
decoupling plan parity matrix, `Recent-Work.md`, this plan → `completed/`.

## Acceptance criteria

- [ ] `HdfReviewTab` and `BatchMaskDialog` take `ProcessingService&`; MIB
      Studio's Review tab behaves exactly as before
      (`frontend.hdf_review_*` and `integration.review_scatter_e2e` pass
      unchanged).
- [ ] `mib_review` builds under `MIB_BUILD_REVIEW_ONLY` with no
      `mib_backend`, camera SDK, SQLite, ONNX or Sentry in the link; the
      Linux lane proves it on every PR.
- [ ] Opening an experiment file and a recording file in MIB Review shows
      the same frames, metrics, charts, click-to-view, contour, exports
      and regenerate-masks results as the Review tab for the same file
      and factor (compare the `metrics.csv` and chart TIFFs byte-for-byte
      in `review.app_smoke` against the tab's output).
- [ ] macOS: `macos-review` preset configures and builds on `macos-14`;
      `mib_review_tests` and `review.app_smoke` pass offscreen; the DMG
      installs by drag-and-drop and opens `.h5` files by double-click.
- [ ] Windows: the Inno Setup installer installs MIB Review beside MIB
      Studio without touching it (separate AppId, directory, settings);
      `.h5` association works; "Check for updates…" finds a newer
      `review-stable/latest.json` and installs it silently.
- [ ] One `vX.Y.Z` tag publishes MIB Studio and MIB Review installers to
      GitHub Releases and both R2 channels.
- [ ] Signed and notarised artefacts open without Gatekeeper or
      SmartScreen warnings (PR 4; blocked until certificates exist and
      reported as such in the tracker if v1 ships unsigned).
- [ ] ADR 0008 accepted; the decoupling plan's parity matrix carries the
      MIB Review column and the Tauri exit condition.
- [ ] Manual page, howtos, vault notes, screenshot harness updated;
      `check_docs.py` and `check_screenshots.py` clean.

## Non-goals (v1)

- A React + Tauri MIB Review (tracked by ADR 0008's exit condition and the
  scatter plan's PR 4).
- Intel macOS or universal binaries; Linux installers.
- Crash reporting / Sentry in MIB Review (needs `CrashReporter` out of
  `mib_backend`).
- Loading signed `.dylib` processing cores on macOS (needs published
  macOS cores; bundled kernel only in v1).
- Multi-window or multi-file sessions; live capture of any kind.
- In-app DMG replacement on macOS (Sparkle is the follow-up candidate).

## Progress

- [ ] PR 1 — decouple `HdfReviewTab`/`BatchMaskDialog`, `mib_review_common`,
      `mib_review`, `linux-review` preset, ADR 0008
- [ ] PR 2 — macOS Conan profile, preset, bundle, `macdeployqt`, DMG, CI job
- [ ] PR 3 — Windows Inno Setup, R2 review channels, `AutoUpdater` prefix,
      release workflow
- [ ] PR 4 — Apple notarisation and Windows Authenticode (blocked on
      certificates)
- [ ] PR 5 — manual, screenshots, TD-17, tracker, plan → completed
