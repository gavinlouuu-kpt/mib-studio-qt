# Rust ↔ C++ Bridge

> The Phase 2 seam of the React + Tauri migration (epic #246): a `cxx` bridge
> that lets a Rust shell drive the Qt-free C++ backend with no Qt, no webkit,
> and no display. Wraps [[AppBackend]] via `backend::bridge::BackendFacade`.

**Source:** `crates/mib-bridge/` (`src/lib.rs`, `src/shim.h`, `src/shim.cpp`,
`build.rs`, `tests/contract.rs`)
**Backend seam:** `include/backend/app/BackendFacade.h`,
`src/backend/app/BackendFacade.cpp`
**Decision:** [`docs/decisions/0003-rust-cxx-bridge.md`](../../docs/decisions/0003-rust-cxx-bridge.md)
**Related:** [[AppBackend]], [[Threading-Model]], [[Data-Flow]],
[[../build-and-run/Dependencies]]

## Why cxx

Safe-by-construction FFI: ownership/lifetimes are expressed in the
`#[cxx::bridge]` module and checked at compile time on both sides, versus
hand-rolled `unsafe extern "C"` marshalling. A PoC proved it links our static
`mib_backend`/`mib_processing` archives and calls in headless before any
production code was written. `autocxx`/`bindgen` are not adopted (would be a new
ADR).

## Shape

Rust owns an opaque `BackendBridge` (`UniquePtr`) that composes an `AppBackend`
+ a `BackendFacade`. Rust never sees `AppBackend`, OpenCV, or HDF5 — only:

- **Lifecycle:** `new_backend_bridge()`, `initialize(data_dir)`, `shutdown()`,
  `is_initialized()`. Dropping the `UniquePtr` calls `shutdown()` then destroys
  the backend.
- **Commands (flat submitters over `BackendFacade::dispatch`):**
  `configure_mock_camera`, `start_capture`, `stop_capture`,
  `start_frame_recording`, `stop_frame_recording`, `playback_seek_latest`, the
  v2 review commands `load_recording`, `playback_seek_index`, and the v3
  `apply_processing` (realtime enable + pixel→micron). Each returns a flattened
  `BridgeCommandResult { ok, command, message }`. No exceptions cross the
  boundary — the shim catches any C++ exception and returns `ok=false`.
- **Events (poll-drained bounded queue):** `poll_events() -> Vec<BridgeEvent>`.
  The facade emits `BackendEvent`s from **backend threads** (the
  background-capture callback runs on the capture/processing thread). The
  shim's event sink does the minimum on that thread — serialise to a
  typed-slot `BridgeEvent` and push onto a mutex-guarded queue — then returns;
  Rust drains it. This is the non-blocking-sink rule from ADR 0003. Since v4
  the queue is **bounded drop-oldest** (default 4096, `MIB_BRIDGE_MAX_QUEUE`
  override, floor 4): a stalled poller coalesces to the newest state, and the
  loss is observable — the next poll batch starts with a synthetic
  `QueueOverflow` event (u0 dropped-since-last-poll, u1 total) and
  `queue_overflow_total()` exposes the counter (ADR 0004).
- **Operation state (v4, ADR 0004):** long-running actions are tracked
  operations. `BackendFacade::beginOperation/reportOperationProgress/
  finishOperation` emit `OperationStatus` events (u0 id, u1 kind, u2 state,
  u3/u4 progress/total); command results carry a non-zero `operation_id`;
  `cancel_operation(id)` requests cancellation and fails safely for
  unknown/finished IDs; `shutdown()` cancels all active operations first.
  RecordingLoad is the first tracked operation; BE-4/BE-6 build on this.
- **Autofocus / nanopositioner (v11, BE-8):** `autofocus_connect/disconnect/
  set_enabled/jog/set_config`, `fetch_autofocus_status` (explicit ring-ratio
  freshness/age), `fetch_autofocus_config` (plain-value round-trip). Linux
  builds the platform stub; connect fails structurally without the Coremor
  SDK.
- **Syringe pumps (v10, BE-7):** flat `pump_*` commands (connect/disconnect/
  flow rate/direction/start/stop/purge/syringe volume/poll) with structured
  validation + COM-port conflict rules, `fetch_pump_status(pump)` snapshots
  for both identities, and `pump_scan_addresses` as a tracked `PumpScan`
  operation (addresses in the Completed event's text).
- **Paged HDF5 review + export jobs (v9, BE-6):** `fetch_review_metadata`,
  `fetch_review_metrics_page` (bounded, metadata-only cache),
  `fetch_review_image(dataset, index)` (contract `review_image_datasets`,
  hyperslab reads), `review_export_csv` (tracked Export operation on its own
  read-only reader; Qt-parity CSV; partial outputs cleaned on
  cancel/failure). RecordingLoad replaces an open file and is rejected during
  an active experiment.
- **Processing config/ROI/background/core (v8, BE-3):**
  `fetch/apply_processing_config_json` (lossless document, merge semantics,
  monotonic `config_version`), `set_processing_roi`,
  `fetch_background_image`/`set_background_image`/`clear_background_image`
  (binary Mono8), `fetch_processing_core_status` (identity + admin pin).
- **Camera discovery/selection (v7, BE-2):** `fetch_camera_discovery`
  (EGrabber + MindVision + a synthetic mock entry; typed DTOs),
  `fetch_camera_selection` (authoritative snapshot incl. mock params, applied
  script/config paths, configured/running), `select_hardware_camera`,
  `select_mindvision_camera`, `apply_camera_script`, `reset_hardware_camera`
  (structured errors for invalid indices/paths/no-selection).
- **Monitoring + trigger (v6, BE-5):** `monitoring_set_active` /
  `monitoring_clear` / `fetch_monitoring_snapshot(max_rows)` (bounded,
  metrics-only rows with stable `(frame_index, object_id)` identity; evictions
  observable as appended − held) and `trigger_set_pulse_duration` /
  `trigger_manual_pulse` / `trigger_periodic_start/stop` /
  `fetch_trigger_status` over `TriggerService` (mock camera emulates the
  trigger output line for headless tests).
- **Experiment lifecycle (v5, BE-4):** `experiment_start(path)` /
  `experiment_stop` / `experiment_cancel` / `fetch_experiment_status` over the
  backend-owned `backend::ExperimentCoordinator` state machine (see
  [[AppBackend]]): atomic preconditions, periodic + final flush on a worker
  thread, metadata/provenance only after data flush, fatal-save recovery.
  `ExperimentStatus` events (kind 8) push transitions; the running experiment
  is a tracked operation. Camera stop is rejected while an experiment is
  active (Qt parity).
- **Frame pull:** `fetch_latest_frame() -> BridgeFrame` (metadata + one owned
  byte copy out of the playback store), and `fetch_frame_by_index(index)` for
  review scrubbing. Frames are **pulled on demand, never pushed** through the
  event channel and **never base64-encoded per frame** (epic principle #4 /
  ADR 0003 hot-path rule).
- **Processing stats pull:** `fetch_processing_stats() -> BridgeProcessingStats`
  (fps + pixel→micron). Backed by a new `BackendFacade::fetchProcessingStats`
  const accessor over `backend_.processing()` — a pull (symmetric with the frame
  pull), not a callback stream, so the shell polls live metrics without any
  event-sink wiring.

### `BridgeEvent` typed slots

Rather than mirror every `BackendEvent` variant field, events flatten into a
small pool of typed slots (`u0..u5`, `f0..f2`, `b0..b1`, `text`) whose meaning
depends on `kind` (`FrameReady` / `CameraStatus` / `RecordingStatus` /
`ProcessingResult` / `PlaybackPosition` / `BackendError` / `OperationStatus` /
`QueueOverflow`). The per-kind mapping is documented in `toBridgeEvent` in
`shim.cpp` and is part of the versioned contract — new fields append slots,
never repurpose them.

## Versioned contract + test

Since v4 the identities live in one machine-checked source of truth:
`crates/mib-bridge/contract/bridge-contract.json` (ADR 0004). C++ pins to it
via static_asserts in `shim.cpp`, Rust via `rust_enums_match_contract_json`,
TypeScript via the generated `desktop/src/bridgeContract.ts`
(`scripts/gen_bridge_contract.py --check` is a desktop-CI drift gate).

The command/event set is a versioned schema: `bridge_abi_version()` returns `11`
(v2 added the review commands — `load_recording`, `playback_seek_index`,
`fetch_frame_by_index`; v3 added the processing commands — `apply_processing`,
`fetch_processing_stats`; v4 added operation state, `cancel_operation`,
`queue_overflow_total`, the bounded queue, and the extended error sources;
v5 added the experiment lifecycle (BE-4); v6 added monitoring snapshots and
the sorter trigger (BE-5); v7 added camera discovery/selection (BE-2); v8
added the processing-config round-trip, ROI/background, and core status
(BE-3); v9 added paged HDF5 review and export jobs (BE-6); v10 added the
syringe-pump surface (BE-7); v11 added the autofocus/nanopositioner surface
(BE-8) —
all additive over the v1 live-capture set); additive
changes bump it. `tests/contract.rs` is the boundary gate and regression guard:
`lifecycle_produces_status_and_frame_events` drives
init → configure mock camera → start → poll `fetch_latest_frame` (asserts the
512×96 sample dims and `stride×height` byte count) → `playback_seek_latest` →
observe a `FrameReady` event → stop → shutdown, and
`record_then_load_and_review` drives record → `load_recording` →
`playback_seek_index(0)` → `fetch_frame_by_index(0)` — all headless.

## Build

`build.rs` drives the `linux-backend-only` CMake preset to produce
`libmib_backend.a` / `libmib_processing.a`, then `cxx_build` compiles the bridge
+ `shim.cpp` and links the archives plus their system deps (OpenCV / HDF5 /
SQLite / spdlog / fmt / crypto). Set `MIB_BRIDGE_NO_CMAKE=1` to skip the cmake
step when the caller already built the archives (the CI lane does this). HDF5
shared libs need `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/hdf5/serial` at
runtime on Ubuntu.

CI: `.github/workflows/bridge-ci.yml` builds the archives then runs
`cargo test` — no Qt, no webkit, no display.

## Gotchas

- The bridge links the **static** archives, so it depends on them being built
  first; `build.rs` handles that unless `MIB_BRIDGE_NO_CMAKE=1`.
- `FrameReady` events fire from the **background-capture / playback** paths, not
  from plain live capture — plain live frames flow into the playback store and
  are read via `fetch_latest_frame`. To emit a `FrameReady` for the latest live
  frame, dispatch `playback_seek_latest` (the push path the webview subscribes
  to). This mirrors `tests/backend/backend_facade_boundary_test.cpp`.
- Keep the event sink non-blocking on the C++ thread; do not add a per-frame
  base64/JSON pixel channel — extend the command/event variants (with a version
  bump) instead.
- Tests that construct a `BackendBridge` (i.e. an `AppBackend`) must be
  `#[serial]` (via the `serial_test` dev-dependency): the C++ backend has
  process-global state (spdlog, HDF5, OpenCV), so cargo's default **parallel**
  test execution can `SIGSEGV` when two backends race in one process. This bit
  the `desktop-ci` lane once — annotate every backend-constructing test.
- `BackendBridge` is `Send` but **not** `Sync` (`unsafe impl Send` in
  `lib.rs`): it may be moved between threads / held in a Tauri
  `State<Mutex<UniquePtr<BackendBridge>>>`, but commands funnel through the one
  owner and are not safe to call concurrently. A `const _` assertion locks in
  that the `Mutex<UniquePtr<..>>` stays `Send + Sync`.
- **PIC / crate-type:** the C++ archives are built position-dependent, so they
  cannot link into a `cdylib`/`staticlib` (Tauri's default mobile crate-types) —
  the linker fails with `recompile with -fPIC`. A desktop Tauri app must use a
  binary + `rlib` (an executable, like this crate's own tests). If a mobile
  target ever needs the `cdylib`, build the backend with
  `CMAKE_POSITION_INDEPENDENT_CODE=ON` instead.

## Agent B frame transaction slice (2026-09-07)

The Tauri adapter now encodes one owned BridgeFrame into one binary response;
metadata and pixels are never separate mutable-cache pulls. Native calls retain
the existing bridge mutex serialization. Encoding uses the returned owned value
after releasing that mutex; no worker or second backend authority was added.
The frontend scheduler bounds aggregate pending pulls and discards retired view
responses. Details and limitations: `docs/architecture/frame-packet-v1.md`.
The accepted readiness/configuration/finalization/recovery handoff is still open
under #372; this slice does not establish native experiment acceptance.

## Exact event contract continuation (ABI 12)

The desktop now uses versioned `poll_events_exact` JSON and one named event
adapter. Commands/experiment snapshots retain u64 identities as decimal strings;
legacy cxx slots are preserved with exact companion fields where floats previously
lost precision. Processing metrics preserve unavailable/non-finite values instead
of zero; unknown clock domains cannot produce elapsed-time claims. See
`docs/architecture/event-json-v1.md` and
[[../task/2026-09-07-agent-b-event-contracts]] for executed evidence and gaps.

## Shared-backend experiment lifecycle (ABI 13, issue #372)

The migration `ExperimentCoordinator` is gone; the bridge drives the
reliability coordinator through `BackendFacade::ExperimentCommand`
(`experiment_start` evaluates readiness and presents its generation,
`experiment_cancel` is Stop + cancelled). New contract groups:
`experiment_command_actions`, `experiment_start_outcomes`,
`experiment_stop_outcomes`, `run_completion_states`,
`readiness_gate_statuses` (pinned by `static_assert`s in `shim.cpp` against
the C++ enums, in `contract.rs` for Rust and by the generated
`bridgeContract.ts`). `BridgeEvent` gains exact companions
(`experiment_start_generation`, `experiment_persistence_{admitted,committed,failed}`,
`experiment_completion`, `experiment_terminal`, `experiment_finalization_ok`);
legacy slots: `u3` = persistence committed, `u4` = 0, `experiment_dropped_valid`
= persistence pending, `experiment_dropped_invalid` = persistence failed.
`fetch_experiment_status` carries the full status (generations, completion
reason, fault code/message); `fetch_experiment_readiness(output_path)` the
gate list. `bridge_abi_version()` returns `13`. The reliability serial bus
([[../services/SerialBus]]) is ported onto [[../services/ISerialPort]] on this
branch, so `mib_backend` links no Qt and the bridge link manifest carries no
Qt libraries (handoff gap G7, backend part). Guards:
`experiment_lifecycle_end_to_end` (readiness gate `camera.session` blocks,
Start → Active → Stop → terminal Complete with the remainder committed, typed
terminal event, file reloads), `rust_enums_match_contract_json`.

## ABI 14: local analysis (#399)

Adds `initialize_analysis` without changing the instrument initialization call.
Review metadata carries accounting availability, completion state/reason and
reconciliation from the native file reader. Measurement pages now use bounded
hyperslabs. A native allowlist denies instrument commands in the analysis context.
