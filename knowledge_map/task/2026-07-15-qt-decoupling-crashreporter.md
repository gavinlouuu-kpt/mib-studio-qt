# Qt → React/Tauri migration: backend de-Qt slice 5 (crash reporter; mib_backend Qt-free)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0001-react-tauri-migration.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

The last real Qt usage in the backend was the crash reporter's Qt message
handler plus a dead `<QString>` include in `AppBackend.cpp`. No `Q_OBJECT`
exists in the backend, so AUTOMOC had nothing to generate. Removing these makes
`mib_backend` link zero Qt.

## What shipped

- `src/backend/services/CrashReporter.cpp` — removed `qtMessageHandler`,
  `qInstallMessageHandler`, and the `<QtGlobal>`/`<QString>` includes.
  `CrashReporter.h` dropped the `installQtMessageHandler` Config flag.
  `captureMessage()` remains the injection point.
- `src/frontend/system/QtLogBridge.cpp` (new) — `mib::frontend::installQtLogBridge()`
  installs the moved `qInstallMessageHandler` (Qt logs → spdlog; criticals/fatals
  → `CrashReporter::captureMessage`). `main.cpp` installs it right after
  `CrashReporter::init()`.
- `src/backend/app/AppBackend.cpp` — removed the dead `#include <QString>`.
- `src/backend/CMakeLists.txt` — `mib_backend` `AUTOMOC ON` → `OFF`, and dropped
  `Qt6::Core` from its link. `mib_backend` now links only spdlog (+ mib_processing,
  OpenCV, HDF5, SQLite, nlohmann).
- Tests: 6 backend/integration/hardware tests only constructed a throwaway
  `QCoreApplication` (dead scaffolding since the LUT catalog stopped checking
  `QCoreApplication::instance()`); removed the include + construction
  (`backend_lifecycle_smoke`, `camera_script_apply` — also `qputenv`→`setenv`,
  `kin6_mib_app_capture_proof`, `e2e_pipeline_stress`, `hw_camera`,
  `hw_egrabber_script`).

## Verification

- Full `linux-backend-only` build + `ctest`: **73/73 pass** (incl. all
  integration/e2e). `nm libmib_backend.a` shows **0** undefined Qt symbols;
  `ldd` on a backend test binary shows **no** Qt6 library at all. QtLogBridge
  syntax-checked against Qt6 headers (frontend not built in this lane).

## Not done here (Phase 1 exit gate)

`mib_backend` is Qt-free, but the `linux-backend-only` *build* still
`find_package`s `Qt6::Core` for 7 `frontend;utility` tests that link it directly
(`json_flatten_roundtrip`, `json_config_merge`, `processing_core_catalog`,
`application_settings`, `processing_core_settings`, `hdf_review_export_paths`,
`update_catalog`). De-Qt-ing or relocating those, then removing the backend-only
`find_package(Qt6)`, makes `linux-backend-only` build with no Qt SDK — the
Phase 1 exit gate.
