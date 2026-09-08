# Screenshot Tour

> Headless harness executable (`screenshot_tour`) that drives the real UI in
> mock-camera mode and regenerates the user-manual screenshots under
> `docs/manual/images/`.

**Source:** `src/frontend/tools/screenshot_tour_main.cpp`,
target defined in `src/frontend/qt/CMakeLists.txt`
**Related:** [[MainWindow]], [[../camera/MockCamera]],
[[../build-and-run/Run-Modes]]

## Responsibility

- Boot `AppBackend` + `MainWindow` exactly like `main.cpp`, but with
  `MIB_CAMERA_MODE=mock` forced, network/hardware services disabled by
  default (`MIB_DISABLED_SERVICES=auto_update,autofocus,trigger,yolo`), and
  backend data + `QSettings` isolated into a `QTemporaryDir` so every run is
  clean and deterministic. The stable application/settings identity is
  initialized only after that temporary settings path is installed, so the
  tour cannot read, migrate, or alter an operator's local preferences.
- Walk the documented workflow: Connect tab → start capture → Overview →
  Experiment Preview/Monitoring → Review, then the Processing, Monitoring,
  and Pixel-to-Micron settings dialogs.
- Save one PNG per registered shot plus a `manifest.json`; exit non-zero if
  any registered shot was not captured.

## Key entry points

- `kShots[]` — the shot registry between `SCREENSHOT_REGISTRY_BEGIN/END`
  markers. `scripts/check_screenshots.py` parses these ids and fails CI when
  they drift from the `images/<id>.png` references in `docs/manual/*.md`.
- `ShotSink::scheduleModalShot` — modal dialogs block in `exec()`, so the
  grab-and-close is scheduled via `QTimer::singleShot` *before* the menu
  action is triggered.
- CLI: `--out` (default `docs/manual/images`), `--frames`, `--width`,
  `--height`. Run headless with `QT_QPA_PLATFORM=offscreen`.
- Builds and runs on Linux via the `linux-system-release` preset (system Qt
  packages, `docs/howto/linux-build.md`) — the committed screenshots were
  first generated that way. UI sources compile once into the
  `mib_frontend_common` static library shared with `mib_studio_qt` (see
  [[../build-and-run/Build]] for the AUTOUIC/qrc constraints).

## Viewport assertions (issue #358)

The tour tells the window its available desktop explicitly
(`setAvailableGeometryOverrideForTests`, requested size + margin) and, before
every grab, `assertViewport()` checks that the window still has the
requested size and that `minimumSizeHint()` fits the requested viewport. A
window silently enlarged by minimum-size constraints therefore fails the
tour (non-zero exit) instead of producing a misleading PNG. The registry
includes `sidebar-collapsed` (hardware panel hidden through
`MainWindow::setHardwarePanelVisible(false)`).

## CI wiring

- `docs-ci.yml` runs `scripts/check_screenshots.py` on every push/PR.
- `build-windows.yml` builds and runs the tour after the app build, uploads
  the PNGs as a `manual-screenshots-<version>` artifact, and on release
  builds commits the refreshed images back to `main`. A tour failure emits a
  workflow warning instead of blocking the release.

## Gotchas

- The tour locates the main `QTabWidget` by object name `"tabs"` and treats
  `tabs->widget(2)` as the nested Experiment tab widget — renaming or
  reordering tabs in [[MainWindow]] requires updating the tour.
- It invokes the private slots `onStartCapture`/`onStopCapture` via
  `QMetaObject::invokeMethod`; renaming those slots breaks it silently at
  runtime (logged error, non-zero exit).
- Adding a shot means three edits in the same PR: `kShots[]`, the tour step
  that captures it, and a manual page that embeds `images/<id>.png` —
  otherwise `check_screenshots.py` fails.
