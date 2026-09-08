# Qt → React/Tauri migration: Phase 1 exit gate (backend-only builds with no Qt SDK)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0001-react-tauri-migration.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

After slice 5, `mib_backend` linked no Qt, but the `linux-backend-only` build
still `find_package`d `Qt6::Core` because 7 `frontend;utility` test executables
link it directly and compile `src/frontend/utils/*.cpp` sources. Those sources
legitimately use Qt (Qt-JSON, `QSettings`, `QCryptographicHash`, `QUrl`,
`QVersionNumber`, `QDir`) and are also compiled into the Qt frontend with
Qt-typed public headers — so de-Qt-ing them in place would mean rewriting
frontend callers. They are frontend tests that were building in the backend-only
lane incidentally.

## What shipped

- `tests/CMakeLists.txt` — wrapped the 7 frontend-utility test targets
  (`json_flatten_roundtrip`, `json_config_merge`, `processing_core_catalog`,
  `application_settings`, `processing_core_settings`, `hdf_review_export_paths`,
  `update_catalog`) in `if(NOT MIB_BUILD_BACKEND_ONLY) ... endif()`. They still
  build/run in the full build (Windows CI — the only place the frontend is
  compiled), so no coverage is lost.
- `cmake/MIBDependencies.cmake` — `find_package(Qt6 ...)` now runs only for
  `NOT MIB_BUILD_BACKEND_ONLY`. Backend-only (⊇ processing-only) requires no Qt.
- `CMakeLists.txt` — moved the global `CMAKE_AUTOMOC/AUTOUIC/AUTORCC ON` after
  `include(MIBOptions)` and gated it behind `NOT MIB_BUILD_BACKEND_ONLY` (else
  CMake warns "No valid Qt version found" for every target when no Qt is
  present).
- `.github/workflows/backend-ci.yml` — dropped `qt6-base-dev` /
  `qt6-serialport-dev` from the apt install, so CI proves the no-Qt-SDK gate.

## Verification

- **Uninstalled the Qt6 SDK** (`apt-get remove qt6-base-dev qt6-serialport-dev
  qt6-base-dev-tools`; `Qt6Config.cmake` gone), then `cmake --preset
  linux-backend-only` → `cmake --build` → `ctest`: **configure/build/test all
  succeed, 66/66 tests pass** with zero Qt present. (66 = the 73 backend-only
  tests minus the 7 frontend-utility tests now gated to the full build.)

## Result

**Phase 1 of epic #246 is complete**: the C++ backend is fully Qt-free and the
backend-only build/test lane needs no Qt SDK. Remaining epic work is Phase 2
(production Rust ↔ C++ bridge — own ADR), Phase 3 (first Tauri vertical slice),
and Phases 4–5 (workflow migration, packaging, cutover).
