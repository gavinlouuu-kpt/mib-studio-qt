# 0003. Rust ↔ C++ bridge uses `cxx` over the `BackendFacade` seam

Date: 2026-07-15
Status: accepted

## Context

Phase 1 of epic #246 made `mib_backend` fully Qt-free (verified: zero Qt
symbols, no `Qt6::*` link, `linux-backend-only` builds and tests green with the
Qt6 SDK uninstalled). Phase 2 defines how the future Tauri/Rust shell talks to
that C++ backend.

The backend already exposes a UI-neutral command/event seam,
`backend::bridge::BackendFacade` (`include/backend/app/BackendFacade.h`):

- **Lifecycle:** `initialize(dataDir)`, `shutdown()`, `isInitialized()`.
- **Commands in:** `dispatch(BackendCommand)` where `BackendCommand` is a
  `std::variant` of `CameraCommand` / `RecordingCommand` /
  `ProcessingSettingsCommand` / `RecordingLoadCommand` / `PlaybackSeekCommand`,
  returning a `BackendCommandResult{ok, command, message}`.
- **Events out:** `setEventSink(std::function<void(const BackendEvent&)>)` where
  `BackendEvent` is a `std::variant` of `FrameReadyEvent` / `CameraStatusEvent` /
  `RecordingStatusEvent` / `ProcessingResultEvent` / `PlaybackPositionEvent` /
  `BackendErrorEvent`.
- **Frame pull:** `fetchLatestFrame(BackendFrame&)` /
  `fetchFrameByIndex(index, BackendFrame&)` — the event carries frame
  *metadata* only; pixel bytes are pulled on demand.

The seam is deliberately shaped this way: the shell drives the backend with
value-type commands and receives value-type events, and the pixel payload is
pulled separately so the hot path never forces a copy through the notification
channel.

Candidate FFI approaches: hand-written `extern "C"` + `bindgen`, the `cxx`
crate, `autocxx`, or a C-ABI shim with manual marshalling. A cxx proof of
concept was built and **run headless**: it links the static `mib_backend` /
`mib_processing` archives into a Rust binary and successfully calls into backend
code (`EModulusLutCatalog::defaultManifestUrl` returned the expected URL through
the bridge). This de-risks the toolchain question before any production code.

PR #59 is an earlier full-shell prototype (also cxx-based). It is **reference
material only**; notably its per-frame `base64` frame channel is the hot-path
anti-pattern epic principle #6 warns against, and this ADR rejects it (see
Decision → Frame hot path).

## Decision

**Adopt `cxx`** as the Rust ↔ C++ bridge, wrapping `BackendFacade`.

### Why cxx
- Safe-by-construction bridge: ownership and lifetimes are expressed in the
  `#[cxx::bridge]` module and checked at compile time on both sides, versus
  hand-rolled `unsafe extern "C"` marshalling.
- No extra codegen toolchain at build time beyond the `cxx`/`cxx-build` crates
  (already validated); integrates with our CMake-built static libraries via a
  `build.rs`.
- The PoC proves it links our Qt-free archives and calls in headless — the exact
  configuration the Tauri shell and CI will use.

### Boundary shape
The Rust side owns an opaque handle to a C++ object that composes an
`AppBackend` + a `BackendFacade` (call it `BackendBridge`). Rust never sees
`AppBackend`, OpenCV, HDF5, or any backend internal type — only:
- lifecycle calls (`new`, `initialize`, `shutdown`),
- typed command submitters that build the C++ `BackendCommand` variant from
  plain scalar/string arguments and return a flattened `{ok, message}`,
- an event pump: Rust registers a sink; the C++ side serialises each
  `BackendEvent` into a flat POD/struct the bridge understands and hands it
  across,
- a frame pull (`fetch_latest_frame` / `fetch_frame_by_index`) returning frame
  metadata plus a byte buffer.

### Ownership
- C++ owns all backend state. The bridge object is heap-allocated and handed to
  Rust as a `UniquePtr<BackendBridge>`; dropping it calls `shutdown()` then the
  destructor. Rust holds exactly one owner.
- Frame bytes cross as an owned buffer (`Vec<u8>` / `rust::Vec<u8>`), copied
  once out of the backend's frame store at pull time. No raw backend pointers
  ever cross the boundary.

### Threading
- `BackendFacade` emits events from **backend threads** (the background-capture
  callback runs on the capture/processing thread, not the caller's thread).
  The event sink must therefore be `Send`-safe: the Rust sink does the minimum
  on the C++ thread (enqueue onto a channel / `tokio` mpsc) and returns
  immediately; all Tauri IPC emission happens on the Rust side off that queue.
- Commands are submitted from the shell thread and are internally synchronised
  by the facade/backend; `dispatch` is the single serialised entry point.
- The bridge object is `Send` but not `Sync` for command submission — commands
  funnel through one owner; events fan out through the channel.

### Cancellation & lifecycle
- `shutdown()` is idempotent and must be called (via `UniquePtr` drop) before
  the process exits; it tears down capture/recording and clears the sink so no
  event fires into a dropped Rust handle.
- Setting an empty sink detaches the background callback (already implemented in
  `setEventSink`), so Rust can stop receiving events without destroying the
  backend.

### Errors
- No exceptions cross the boundary. Command results are values
  (`BackendCommandResult` → `{ok, message}`); backend-internal failures surface
  as `BackendErrorEvent`s on the event channel, not as thrown exceptions.
- Any C++ exception at the FFI edge is caught at the shim and converted to an
  `ok=false` result (cxx would otherwise translate an uncaught exception into a
  Rust panic across the boundary — we don't rely on that).

### Frame hot path (rejects PR #59's base64 channel)
- Events carry frame **metadata only** (`FrameReadyEvent`: index, dims, format,
  stride, byteSize). Pixels are **pulled** via `fetch_latest_frame` as raw
  bytes — **never** base64-encoded per frame, and never pushed through the
  event/IPC JSON channel.
- The production frame transport to the webview (Tauri IPC binary / a shared
  buffer / a local blob channel) is deferred to the first Phase 3 vertical
  slice, but the C++ seam is already correct for it: one copy out of the store,
  raw bytes, on demand.

### Versioned schema & contract tests
- The command/event set is a **versioned contract**. The bridge exposes a
  `bridge_abi_version()` and the flat command/event structs are treated as a
  schema: additive changes bump a minor version; a field's meaning never
  changes silently.
- A headless **contract test** (Rust `cargo test`) drives a real
  init → mock-camera configure → start → observe `CameraStatus`/`FrameReady`
  events → `fetch_latest_frame` → stop → shutdown lifecycle against the linked
  backend, asserting the event sequence and frame metadata. This is the Phase 2
  gate and the regression guard for the boundary.

## Consequences
- Phase 2 delivers a committed Rust crate (`src-tauri/` bridge module or a
  dedicated `bridge` crate) with a `build.rs` that builds `mib_backend` via
  CMake and links it, plus the headless contract test above — runnable in CI
  with **no Qt, no webkit, no display**.
- The Tauri shell (Phase 3) consumes this bridge unchanged; only the frame
  transport and Tauri command/event wiring are added on top.
- Future agents: do not add a per-frame base64 or JSON pixel channel; extend the
  `BackendCommand`/`BackendEvent` variants (with a schema version bump) rather
  than adding side-channel FFI calls; keep the event sink non-blocking on the
  C++ thread.
- `autocxx`/`bindgen` are not adopted; if a future need outgrows `cxx`, that is a
  new ADR.
