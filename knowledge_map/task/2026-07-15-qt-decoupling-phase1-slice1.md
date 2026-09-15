# Qt → React/Tauri migration: Phase 0 + backend de-Qt slice 1

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0001-react-tauri-migration.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Epic #246 replaces the Qt desktop app with React + Tauri v2 **and** makes the
C++ backend Qt-free. `MIB_BUILD_BACKEND_ONLY` still hard-requires Qt6
(`Core/Gui/SerialPort/Network`) because a few backend clusters use Qt value
types. Backend coupling is shallow — no `Q_OBJECT`/moc in headers. This is the
first execution step: Phase 0 groundwork plus the first, lowest-risk backend
de-Qt slice (two pure header contracts that already have unit tests).

## What shipped

Phase 0 docs:
- ADR 0001 (accepted): React + Tauri v2 target; `BackendFacade` is the
  UI-neutral C++ seam; incremental vertical slices from `main`; PR #59 is
  reference-only. Index row added to `docs/decisions/README.md`.
- Active exec-plan: Qt dependency inventory (6 clusters), feature-parity
  matrix, performance-budget table (baselines TBD), incremental PR order.

Code (backend contracts, Qt removed from public headers):
- `include/backend/services/ModbusRtu.h` — frames are `std::vector<uint8_t>`
  (`modbus::Frame`) instead of `QByteArray`. Pure functions, no behavior
  change. `SyringePumpService.{h,cpp}` updated: private helpers take/return
  byte vectors; conversion to/from `QByteArray` happens **only** at the
  `QSerialPort` read/write seam (a `toHexString` helper replaces
  `QByteArray::toHex` for debug logs). `SyringePumpService.h` no longer
  forward-declares `QByteArray`. `scanModbusAddresses` keeps its local
  `QByteArray`+`QSerialPort` I/O (serial abstraction is a later slice).
- `include/backend/camera/mindvision/MindVisionConfig.h` — parses with
  `nlohmann_json` instead of Qt JSON; `parseConfig` takes `const std::string&`.
  Field accessors preserve `QJsonValue::toInt/toDouble/toBool(default)`
  leniency (missing/wrong-typed key → default, no throw). Callers
  `MindVisionCamera.cpp` and `CameraControlService.cpp` read the file with
  `std::ifstream` instead of `QFile`.
- Tests `modbus_rtu_test.cpp` and `mindvision_config_test.cpp` updated to the
  new APIs; both are now Qt-free and pass (behavior pinned unchanged).

## Verification

- `modbus_rtu_test` compiles standalone with plain g++ (no Qt) and passes.
- `mindvision_config_test` compiles against `nlohmann/json.hpp` (no Qt) and
  passes all clamp/default/malformed/negative-strobe cases.
- Full `linux-backend-only` build was not run in the dev container (no Qt6 /
  OpenCV / HDF5 / spdlog dev packages present); CI's `backend-ci` lane covers
  it. `python3 scripts/check_docs.py` run for vault/doc integrity.

## Not done here (tracked in the exec-plan)

Syringe-pump `QSerialPort` serial abstraction; LUT-catalog `QtNetwork`/paths
seam; mock-camera `QImage` decode; crash-reporter glue; then dropping `Qt6::*`
+ `AUTOMOC` from the backend so `MIB_BUILD_BACKEND_ONLY` builds with no Qt SDK
(Phase 1 exit gate). The React/Tauri frontend itself is Phase 3+.
