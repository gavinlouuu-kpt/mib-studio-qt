# Qt → React/Tauri migration: Phase 2 — production Rust ↔ C++ bridge (cxx)

Date: 2026-07-15
Epic: #246 · ADR: `docs/decisions/0003-rust-cxx-bridge.md` ·
Plan: `docs/exec-plans/active/2026-07-15-qt-decoupling-and-tauri-migration.md`

## Context

Phase 1 made `mib_backend` fully Qt-free. Phase 2 defines how the future
Tauri/Rust shell talks to that backend. The backend already exposes a UI-neutral
command/event seam — `backend::bridge::BackendFacade` — so the bridge wraps that,
not the internals. `cxx` was chosen (ADR 0003) and de-risked with a headless PoC
that linked the static archives and called into backend code before any
production code was written.

## What shipped

New crate `crates/mib-bridge/`:

- `src/lib.rs` — the `#[cxx::bridge]` module. Shared structs `BridgeCommandResult`,
  `BridgeEvent` (typed-slot flattening of `BackendEvent`), `BridgeFrame`; opaque
  `BackendBridge` with lifecycle + command + `poll_events` + `fetch_latest_frame`.
- `src/shim.h` / `src/shim.cpp` — `BackendBridge` composes `AppBackend` +
  `BackendFacade`. Commands build the C++ `BackendCommand` variants and call
  `dispatch`; the event sink serialises each `BackendEvent` and enqueues it
  (non-blocking, mutex-guarded) for Rust to drain; `fetch_latest_frame` copies
  the pixel bytes out of the playback store once. No exceptions cross the FFI
  edge (caught → `ok=false`).
- `build.rs` — drives the `linux-backend-only` CMake preset to build
  `libmib_backend.a` / `libmib_processing.a`, then `cxx_build` compiles the shim
  and links the archives + OpenCV / HDF5 / SQLite / spdlog / fmt / crypto.
  `MIB_BRIDGE_NO_CMAKE=1` skips the cmake step when the caller pre-built.
- `tests/contract.rs` — headless `cargo test`: `bridge_abi_version() == 1`, and a
  full init → configure mock camera → start → poll `fetch_latest_frame` (asserts
  512×96 sample dims + `stride×height` bytes) → `playback_seek_latest` → observe
  `FrameReady` → stop → shutdown lifecycle against the linked backend.
- `.github/workflows/bridge-ci.yml` — builds the archives then runs the contract
  test. No Qt, no webkit, no display.

## Key decisions (ADR 0003)

- **cxx** (safe compile-checked bridge) over hand-rolled `extern "C"`;
  `autocxx`/`bindgen` deferred.
- **Frame hot path:** metadata in events, pixels **pulled** as raw bytes —
  rejects PR #59's per-frame base64 channel (epic principle #4).
- **Threading:** events emit on backend threads; the sink only enqueues and
  returns; Rust drains via `poll_events`.
- **Versioned contract:** `bridge_abi_version()` + typed-slot `BridgeEvent`;
  additive changes bump the version.

## Gotchas hit

- The committed `data/mock_frames/frame_000.png` is a 1×1 placeholder that the
  pipeline rejects; the contract test uses the real 512×96
  `data/mock_frames/frame_00000.tiff` instead.
- `FrameReady` events do **not** fire from plain live capture — live frames flow
  into the playback store and are read via `fetch_latest_frame`. Emitting a
  `FrameReady` for the latest frame requires dispatching `playback_seek_latest`
  (mirrors `tests/backend/backend_facade_boundary_test.cpp`).

## Result

Phase 2 is done: a committed, headless-tested Rust ↔ C++ bridge over the Qt-free
backend, with its own CI lane. Next is Phase 3 — the first Tauri vertical slice
(mock camera end to end), whose likely hard block is webkit2gtk / display
availability in this environment.
