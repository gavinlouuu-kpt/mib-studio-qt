# 0001. Migrate the desktop application from Qt to React + Tauri

Date: 2026-07-15
Status: accepted

## Context

MIB Studio ships as a Qt 6 Widgets desktop application. Two forces drive a
change of platform:

1. **The frontend.** The operator UI (`src/frontend/`) is Qt Widgets + Qt
   Charts. Epic #246 calls for replacing it with a React + Tauri v2 shell to
   modernise the UI stack and reduce the Qt surface area.
2. **The backend still requires Qt even without a UI.** `MIB_BUILD_BACKEND_ONLY`
   exists, but `cmake/MIBDependencies.cmake` still `find_package`s Qt6
   `Core Gui SerialPort Network` unconditionally and `src/backend/CMakeLists.txt`
   links them, so a headless backend cannot build without a Qt SDK. The
   coupling is shallow — no `Q_OBJECT`/moc in backend headers; Qt survives only
   as value/utility types in a handful of clusters (LUT-catalog networking,
   syringe-pump serial, mock-camera image decode, MindVision JSON, crash-reporter
   glue).

A prototype (PR #59, branch `claude/migrate-qt-to-react-tauri-r6znA`) explored
React + Vite + TypeScript over a Tauri v2 shell with a Rust ↔ C++ `cxx` bridge.
It validated the direction but diverged far from `main` and carries unresolved
correctness concerns (callback lifetimes, overlapping async capture, a per-frame
PNG/base64 hot path). It is reference material, not a merge candidate.

An existing boundary already anticipates this move: `backend::bridge::BackendFacade`
(`include/backend/app/BackendFacade.h`, see
[`../architecture/frontend-neutral-backend-bridge.md`](../architecture/frontend-neutral-backend-bridge.md))
is a Qt-free command/event bridge over `AppBackend` that both Qt and a future
Tauri IPC layer can share.

### Alternatives considered

- **Electron + Node.** Heavier runtime, larger installers, and no natural path
  to reuse the existing C++ backend without a separate native-addon boundary.
  Rejected: Tauri's Rust shell binds the C++ backend more directly and ships
  smaller signed Windows artifacts.
- **Qt Quick / QML rewrite.** Keeps us on Qt — the opposite of goal (2). Does
  not remove the backend Qt dependency. Rejected.
- **Merge PR #59 as-is.** Rejected by the epic itself; diverged from `main`
  with open correctness issues.

## Decision

Adopt **React + Tauri v2** as the target desktop platform, and make the C++
backend **Qt-free**, migrating incrementally from current `main`:

1. **The stable C++ boundary is `BackendFacade`.** The Tauri IPC adapter maps
   IPC calls to `BackendCommand` values and `BackendEvent`s back to Tauri
   events. No `frontend/*` or Qt types cross this boundary.
2. **Migrate in vertical slices**, each usable, tested, and benchmarked. Keep
   the Qt shell building until Tauri passes the epic's exit gate.
3. **Backend contracts carry no Qt types.** Remove Qt from backend public
   headers cluster by cluster (this ADR ships the first: `ModbusRtu.h` and
   `MindVisionConfig.h`). Serial I/O, networking, settings, and paths move
   behind platform-neutral seams before Qt is dropped from `mib_backend`.
4. The living breakdown, inventory, feature-parity matrix, and performance
   budgets are tracked in
   [`../exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`](../exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md).

## Consequences

- **Easier:** headless/CI backend builds once Qt is removed; a modern,
  testable frontend; a single UI-neutral backend contract.
- **Harder / must respect:** every backend change must keep the contract
  Qt-free and route through `BackendFacade`; the production frame path must meet
  throughput/latency budgets (a per-frame PNG/base64 design is not accepted by
  default); existing HDF5 files, profiles, configuration, logs, update channels,
  and crash reporting must remain compatible or migrate through tested tooling.
- **Transitional cost:** two shells build during migration; the Rust ↔ C++
  bridge design (`cxx` vs C ABI) is deferred to its own decision in Phase 2.
- PR #59 stays open as reference and is superseded when the first production
  Tauri slice lands; the exec-plan records that point.
