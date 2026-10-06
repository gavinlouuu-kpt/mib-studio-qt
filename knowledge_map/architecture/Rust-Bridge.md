# Rust ↔ C++ Bridge

> The Phase 2 seam of the React + Tauri migration (epic #246): a `cxx` bridge
> that lets a Rust shell drive the Qt-free C++ backend with no Qt, no webkit,
> and no display. Wraps [[AppBackend]] via `backend::bridge::BackendFacade`.

**Source:** `crates/mib-bridge/` (`src/lib.rs`, `src/shim.h`, `src/shim.cpp`,
`build.rs`, `tests/contract.rs`); review bridge `src/review_bridge.rs`,
`src/review_shim.{h,cpp}`, `tests/review_bridge.rs` (ADR 0014)
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
- **Device discovery jobs (v14, #419, ADR 0005):** `start_device_discovery
  (request)` / `start_camera_discovery()` → `BridgeDiscoveryStart{job_id}`,
  `fetch_device_discovery(job_id)` → bounded `BridgeDiscoverySnapshot`
  (contract-typed `state`, `kind`, `identity_strength`, `identification`,
  error kinds; camera jobs carry the synthetic mock entry, `synthetic=true`,
  camera_type 2), `cancel_device_discovery(job_id)`. None block on hardware
  or on the bridge mutex beyond the call itself. Replaces the v7 synchronous
  `fetch_camera_discovery`; the TS client polls through
  `desktop/src/discovery.ts` (`bridge.discoverCameras()`), unit-tested. New
  contract groups: `discovery_device_kinds`, `discovery_job_states`,
  `discovery_identity_strengths`, `discovery_identification_statuses`,
  `discovery_error_kinds`. Windows `cargo test` against the `windows-ninja`
  tree uses `tools/gen_bridge_link_manifest_ninja.py`.
- **Central profile registry (v25, #398):** `registry_sign_in(email,
  password)`, `registry_sign_out()`, `registry_refresh()`,
  `registry_download(revision_id)` → job ID (0 = refused),
  `registry_cancel_all()`, `fetch_registry_snapshot()` →
  `BridgeRegistrySnapshot` (session, connectivity, projects, cached
  revisions with `central_state`, corrupt IDs, last job; never a token or
  password), `fetch_registry_job(job_id)`. New contract groups:
  `registry_session_states`, `registry_connectivity`, `registry_job_kinds`
  (`Materialize` = 4 and `RecordValidation` = 5 appended for #398 M2, and
  `SaveDraft` 6, `DeleteDraft` 7, `SubmitDraft` 8, `Transition` 9,
  `FetchHistory` 10 for M3, before the registry ABI was released),
  `registry_job_states`, `registry_central_states`, `registry_local_validation`
  (M2b). M2b also adds `registry_materialize(revision_id)` → job ID and
  `registry_record_validation(revision_id, evidence_file, passed)` →
  `BridgeRegistryValidationRequest { job_id, error }` (the evidence check runs
  before queueing), per-revision `materialized_dir` / `local_validation` /
  `validated_by` / `validated_at_utc`, and snapshot `instrument_id` /
  `instrument_name` — all part of ABI 25. M3b (ABI 28) adds
  `BridgeRegistryDraft` / `Method` / `HistoryEntry` / `Conflict` in the
  snapshot (`drafts`, `methods`, `history_revision_id` + `history`,
  `submit_conflict`), per-revision `parent_revision_id` / `release_notes` /
  `newer_revision_id`, and the authoring functions
  `registry_new_draft_from_revision`, `registry_new_method_draft`,
  `registry_set_draft_notes`, `registry_draft_from_head`,
  `registry_submit_draft`, `registry_delete_draft`, `registry_transition`,
  `registry_fetch_history` → `BridgeRegistryCommand { job_id, error }`. **Transport seam (ADR
  0002 addendum):** the shell installs its HTTPS POST with
  `set_registry_transport(fn(&BridgeHttpRequest) -> BridgeHttpResponse)`
  *before* `initialize` (refused afterwards). Each request carries a
  `cancel_handle`; the transport polls the free function
  `registry_request_cancelled(handle)` and returns status 0 once it is true
  (registry cancel or backend shutdown). Response bodies cross as bytes, and
  every snapshot/job conversion catches exceptions so non-UTF-8 text can
  never cross the FFI. The Rust test transports are plain `fn`s reporting
  through statics (a `fn` pointer cannot capture).
- **Camera selection (v7, BE-2):** `fetch_camera_selection` (authoritative
  snapshot incl. mock params, applied script/config paths,
  configured/running), `select_hardware_camera`,
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

**Review-only link manifest (macOS / Windows, YOFO Review).** The
`macos-review-core` / `windows-review-core` presets build the review core
against static Conan deps (`conanfile.py` `review_core=True`) plus
`mib_review_link_probe` (`tools/review_link_probe/main.cpp`, option
`MIB_BUILD_REVIEW_LINK_PROBE`). `tools/gen_review_link_manifest.py` reads the
probe's link edge from `build.ninja` (`LINK_LIBRARIES` / `LINK_PATH`, Ninja
escapes and MSVC quoting handled) and the review core's flags from
`compile_commands.json`, and writes `"format": "mib-review-link-v1"`.
`build.rs` (`review_manifest` / `manifest_build`) replays it: static archives
as `static:-bundle` in CMake's order, frameworks, system libraries, include
dirs and defines. Picked from `MIB_BRIDGE_LINK_MANIFEST`, else
`build/review-core/` for review-only builds on macOS (required) and Windows
(when present); a review manifest with the default features is an error.
Linux keeps its fixed list unless the env points at a manifest (how the
mechanism is exercised locally). Parser tests:
`tools/test_gen_review_link_manifest.py` (synthetic macOS + MSVC trees).

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


### OEABT link dependencies

Since #464 slice 3 the backend links the Z-stage archives too: the bridge
links `stage_zc300` and `stage_zc300_protocol` between the backend/processing
archives and the OEABT ones, and the Windows manifest path marks them static
(`crates/mib-bridge/build.rs`). `SerialBus.cpp` lives in `oeabt_serial`.

The Linux bridge links `oeabt_serial` and then `oeabt_core` from
`<build-dir>` (the CMake archive output directory), after the backend/processing archives. These contain
both the nanopositioner protocol and the shared native serial transport. The
no-CMake path validates all four archives, and Cargo watches the OEABT archives
for relinking. Windows uses the CMake-generated dependency manifest and marks
the OEABT libraries as static. Missing these dependencies produces undefined
SerialTransport/ControllerSession and platform serial symbols in bridge CI.

## Checked processing document seam

`BackendFacade::fetchConfigDocument` / `applyConfigDocument` provide required
SHA256 baselines and separate saved/applied/verified/conflict outcomes for
image-processing patches; see [[task/2026-09-23-tauri-config-transactions]].


## ABI 15: checked config and shared exports

Additive `fetch_config_document` and `apply_config_document` carry typed
snapshot/result structs (required SHA256 baseline; 4 MiB documents, 64 KiB
image_processing-only patches). Outcomes saved/applied/verified/conflict are
independent. The facade owns lifecycle serialization and durable replacement.
Only effective processing fields update runtime provenance: selecting a saved
file does not claim its other device/ROI settings were applied.

Shared exporter request/status JSON is carried through the cxx/Tauri bridge;
operation identity and counters remain decimal strings. The previous CSV entry
point delegates to the same HdfExportService. Terminal status is retained for
reconciliation even if operation events are missed. Full general resnapshot,
frame source/session/config identities and native cross-shell acceptance remain
open; this addition does not close #372/#246.

ABI 16 adds preview-buffer range/save JSON. Latest-frame facade pulls now fetch
the exact queried committed index, never a later frame under an earlier label;
concurrent frame-identity stress regression covers index/timestamp/pixel agreement.

### Typed hardware and acquisition pulse controls (2026-09-23)

The additive `autofocus_connect_endpoint` command preserves an explicit OEABT/CoreMOR
backend and persistent endpoint ID; status carries the actual connected backend and
endpoint. Legacy numeric COM commands remain compatible. The facade rejects missing
identity and ambiguous `auto` connections before opening a driver.

`pulse_generator_command` and `pulse_generator_status` route through BackendFacade
and the existing PulseGeneratorService, not a second serial implementation. Commands
validate serial settings, address, channel and numeric ranges before driver access.
Configuration/output-on serialize against experiment Start with `withIdleConfiguration`;
output-off remains possible while an experiment runs unless coordinated live view owns
the generator. Status preserves that ownership so manual controls cannot steal it.

`startup_discovery_run(start|camera|nanopositioner)` schedules the shared startup policy;
`startup_discovery_status` drains its queued completion actions on the serialized bridge
caller before reporting running flags, selected state and bounded per-job errors. The
status call is therefore also the startup event-pump tick, not a passive hardware read.
Shutdown stops the coordinator and drains discovery workers before facade destruction.

`set_processed_preview_enabled` opts into immutable source retention;
`fetch_processed_preview` returns an atomic binary MIPO envelope (magic, LE u32 version,
LE u32 JSON byte length, UTF-8 JSON, tightly packed Mono8 source, tightly packed mask).
The TS decoder validates envelope/geometry/lengths/contour budgets and canonical exact
u64 identities before drawing. It reports unavailable snapshots without stale bytes.

## Remembered discovery and named pump endpoints (2026-09-23)

Startup selection now installs validated, per-user remembered vendor/endpoint/baud/address preferences into the shared startup coordinator before optional automatic selection. Malformed persistence skips automatic selection; failed persistence is distinguished from a session-only applied preference. Preference changes do not connect hardware.

Pump connections accept system serial names (including Linux paths), reusing the existing shared SerialBus string transport. Status exposes the actual port name; legacy Qt config edits preserve connected transport identity. Two pumps can share a bus at distinct slave addresses, while duplicate pump/pulse slave identities and autofocus port collisions are refused before connection writes. Legacy numeric COM bridge calls remain supported. Native fake-serial tests cover named endpoint roundtrip and shared-bus identity guards; real hardware acceptance remains deferred.

### Windows manifest XML decoding

The VS link-manifest reader decodes XML entities before splitting MSBuild semicolon lists, including per-source include paths. This preserves quoted version/signer macros and ampersands in dependency paths instead of turning entity terminators into invalid linker/compiler arguments. Portable CLI regression: `python3 tools/test_gen_bridge_link_manifest.py` exercises dependency paths, compile macros, inherited-list filtering and Release-only include selection without requiring Windows.

The Windows candidate CI runs the portable escaped-XML manifest regression before
provisioning native dependencies.

Windows regression fixtures canonicalize temporary paths before comparison,
matching the generator when RUNNER~1 and runneradmin name the same directory.

## ABI 20: Camera & Alignment (2026-10-01)

`set_camera_overview(overview)`, `save_camera_roi(x, y, w, h)` and
`fetch_camera_geometry() -> JSON` expose the Qt Overview-tab workflow to every
shell: the whole sensor is shown, the experiment window (ROI 1, sensor
coordinates) is placed on it and saved, and Experiment acquires that window.
They go through `BackendFacade` camera actions `SetCameraOverview` (restarts a
capture that was running) and `SaveCameraRoi`, and `fetchCameraGeometryJson`.
MindVision keeps its profile-based overview; Aravis cameras gained one (see
[[AppBackend]]). `crates/mib-bridge/tests/contract.rs`
`camera_alignment_commands_without_overview_camera` covers a camera without
an overview (mock).

## ABI 21: science on the PL (2026-10-01)

`fetch_platform_info() -> {science: host|pl, host_processing, aravis}`.
With `MIB_PL_SCIENCE` (the `linux-armv7-yocto` preset) or `MIB_PL_SCIENCE=1`
in the environment, `backend::app::hostProcessingAvailable()` is false:
`ProcessingService::setRealtimeEnabled(true)` is refused and `startRealtime`
is a no-op, `apply_processing` with realtime on fails with the reason,
experiment start does not start the host pipeline, and the readiness gates
`processing.*` are replaced by `science.pl` (Warn until the record path B3
connects the PL's results). Test: `backend.pl_science`.

## ABI 22: pump models (2026-10-04)

`pump_connect_model(pump, model, port_name, baud_rate, modbus_address,
microliters_per_rev)` connects a Sample or Sheath slot to a contract
`pump_models` device: 0 Longer dLSP syringe, 1 Tushui peristaltic.
`BridgePumpStatus` gains `model`, `microliters_per_rev` and `speed_rpm`.
`BackendFacade` validates the model and a calibration in (0, 100000] µL/rev;
the peristaltic semantics are in [[../services/SyringePumpService]]. The old
`pump_connect_endpoint` / `pump_connect` stay and connect a dLSP. Test:
`contract.rs` `pump_commands_fail_safely_without_hardware`.

## ABI 23: one contract for develop and the instrument (2026-10-05)

ADR 0011's single renumber. `develop` was at 19 and the instrument line at
20 (Camera & Alignment), 21 (`fetch_platform_info`) and 22 (pump models). The
merged contract is all of them, so it takes a number no earlier build has
carried. It adds no commands of its own. The #398 profile-registry stack
takes 25 (24 went to #501 P0).

## ABI 24: PZ7035 status and capabilities (#501 P0a, 2026-10-05)

- `fetch_platform_info` gains `capabilities`: instrument (desktop or
  pz7035), the MIB-only surfaces, `pl_identity`, `led_strobe`, align and run
  mode, and the pump model, port, per-slot address (Sample 3, Sheath 4) and
  µL/rev.
- `fetch_instrument_status` returns the read-only `PzPlatformMonitor` sample:
  - the PL core against the expected core and the pinned weights;
  - LED preset and guard;
  - link rates;
  - latency.

  `available: false` with the reason off the PZ7035. Test: `contract.rs`
  `platform_capabilities_and_instrument_status_on_the_desktop`.
- `yofo-studio-server` serves `GET /auth` (200/401 JSON) so the browser can
  prompt for the token (test `auth_probe_reports_the_token_without_a_socket`).
- 25 is reserved for the #398 profile-registry stack. 26 = the ZC300 stage
  bridge (#464); 27 = #501 P1 camera modes; 28 = central method authoring
  (#398 M3b); 29 = central-method Apply in the React shell (#398 M2c).

## ABI 26: Z stage commands (#464, ADR 0013)

The Z stage landed before #501 P1, so under the landing-order rule it took 26;
27 is reserved for #501 P1.

- **Commands:** `stage_connect(port_name, usb_serial, modbus_address)`,
  `stage_disconnect`, `stage_move_to(target_um)`, `stage_move_by(delta_um)`,
  `stage_home`, `stage_stop`, `stage_apply_profile` and `fetch_stage_status`
  (a `BridgeStageStatus` snapshot including `referenced`, `limits_verified`,
  `busy` and the soft limits). *(`stage_home` and `referenced` were removed
  at ABI 30, below.)*
- **Contract additions** (all appended): `command_types.Stage = 13`;
  `operation_kinds` `StageMove = 7` and `StageReference = 8`;
  `discovery_device_kinds.MotionStage = 4`; a new `stage_move_states`
  group.
- **Safety lives in the facade and `StageService`, not the shell:**
  - moves are refused until Home in this power-up, and outside the soft
    limits;
  - Home needs the supervised limits-verified record, which no bridge path
    can write;
  - Connect is observe-only, and there is no start-up/auto-Home command;
  - `stage_stop` is always accepted;
  - everything else is refused while an experiment is active.
- **Operations:** moves are tracked operations (Home was one until ABI 30). A
  facade waiter thread mirrors the `StageService` operation, and a cancel
  stops the axis.
- **Server:** every stage command except `stage_stop` and
  `fetch_stage_status` is a `CONTROL_COMMANDS` entry. `stop_and_save` stops
  a busy stage when the last client leaves.
- **Tests:** `contract.rs` `stage_commands_fail_safely_without_hardware`;
  `stage_motion_is_control_only_but_stop_is_not` in the server;
  `backend.stage_bridge_facade`.

## ABI 30: no homing for the Z stage (#464, ADR 0013 Amendment 1)

28 belongs to #482 and 29 to #493 (allocated by the coordinator); the ZC300
change took 30. The stage is never homed.

- **Removed:** the `stage_home` command and the `StageReference` operation kind
  (`operation_kinds` is now `…, StageMove = 7`).
- **Added:** `stage_set_zero(mid_travel)` (Tauri `stage_set_zero`, server
  `CONTROL` command, dispatch args `{midTravel}`): one write of the position
  counter, no motion, no operation id.
- **`fetch_stage_status` changes:** `referenced` → `zero_set`; new
  `mid_travel_declared` and `session_only_zero` (power-up token off: a power
  cycle is not detected; hardware acceptance only); `soft_min_um` / `soft_max_um` → `envelope_min_um` /
  `envelope_max_um` (0/0 until zero is set). `limits_verified` is now only a
  badge; `home` is the raw, floating controller input.
- **Backend rules** (the shell enforces none of them): moves are refused until
  zero is set this power-up and outside the envelope (±1000 µm, ±2900 µm after
  a mid-travel declaration), refused rather than clamped; an e-stop or driver
  alarm clears `zero_set`; limit bits only stop a move toward an active switch;
  `stage_set_zero` is locked during an experiment like every stage command
  except `stage_stop`. Details: [[../services/StageService]].
- **Tests:** `contract.rs` (`abi_version_is_stable` = 30,
  `stage_commands_fail_safely_without_hardware` incl. `stage_set_zero`),
  `stage_bridge_facade_test`, the server's `stage_motion_is_control_only_…`.

## ABI 27: PZ7035 camera modes (#501 P1, 2026-10-05)

- `set_instrument_mode(mode: align|run, x, y)` runs
  `AppBackend::setInstrumentMode` ([[AppBackend]]).
- `set_service_mode(on)` sets the backend latch for Service /
  Commissioning mode.
- `set_instrument_led(delayUs, widthUs)` sets raw LED values. The backend
  refuses them:
  - outside Service mode;
  - during an experiment;
  - outside the per-mode limits, which `fetch_platform_info` reports as
    `capabilities.led_limits`.
- `fetch_run_preview` returns a binary `MIBC` packet in Run (layout in
  `bridge-contract.json` and `PzInstrumentControl.h`), else the error
  `RUN_PREVIEW_UNAVAILABLE`.

The three set commands are CONTROL commands on `yofo-studio-server`. All of
them are refused while the PL is unconfigured (PCFG_DONE) or is not the
U-Net cell image. `capabilities.align_mode` and `run_mode` are true when the
register writer exists. `fetch_instrument_status` adds `mode{name, run_x,
run_y, service}` and `storage{path, writable, ram, free_bytes, filesystem,
warning}`: the recording target of the data directory, from
`app::recordingTarget`. Test: `contract.rs`
`instrument_mode_commands_off_the_instrument`.

Final numbering from merge coordination: P0 (#502) takes 24, #398 takes 25,
the ZC300 stage bridge (#513) takes 26, and this takes 27 (it was offline
while #513 landed first).

**Bulk byte copies.** C++ fills every `Vec<u8>` it returns (frame packets,
processed previews, review overlays) through the Rust function
`bytes_to_vec(&[u8])`, one FFI call and one memcpy. `rust::Vec::push_back`
crosses the bridge per element: a 509 KB full-field frame took ~75 ms on the
PZ7035's Cortex-A9 that way (88 ms per pull, ~10 fps in the browser; now
27 ms per pull, display ~26 fps = all delivered images).

## Review bridge (YOFO Review, review contract v1, ADR 0014)

A second `#[cxx::bridge]` module, `review_ffi` (namespace
`mib_review_bridge`), wraps [[../services/ReviewSession]] as an opaque
`ReviewBridge`. It is YOFO Review's whole backend surface and is deliberately
separate from `ffi::BackendBridge`; MIB Studio's Review tab does not use it
(it keeps the facade's review commands, ADR 0014 decision A):

- **Link set.** `review_shim.cpp` includes only `backend/review/*`,
  `Hdf5Service.h` and `KdeCoreRecord.h`, so it links `mib_review_core` +
  `mib_processing`. The cargo feature `review-only` compiles just this
  bridge (`build.rs`: `bridge_sources()` / `shim_sources()` /
  `archives()`), and the YOFO Review binary carries no `AppBackend`
  (`review-ci.yml` checks `nm` for `backend::AppBackend`). Without the
  feature both bridges compile (the review tests run in both configurations).
- **Calls:** `review_open/close`, `set_fallback_pixel_to_micron`,
  `fetch_review_info` (counts, ROI, datasets, series, multi-image window,
  accounting + summary text, recorded factor, KDE JSON, recorded
  ring-ratio range),
  `fetch_review_rows` (full-column `ReviewRow`s), `fetch_review_frame(dataset,
  index, overlay, roi)` → `ReviewFrame` Mono8 or **RGB8**
  (`review_pixel_formats`), `fetch_review_series_count/frame`,
  `fetch_review_thumbnails` (one frame of `size × (size·count)`),
  `fetch_review_scatter` (columnar, with ring ratios), `review_save_core_record`,
  `poll_review_events` (job lifecycle, `review_operation_kinds` ×
  `operation_states`), `cancel_review_operation`, the jobs
  `review_export_metrics/all`, `review_batch_export`,
  `review_regenerate_masks` (`review_regenerate_sources`),
  `review_export_charts` (kind `ExportCharts` = 6),
  `review_compute_core` + `fetch_review_computed_core_json`,
  `review_request_density` + `fetch_review_density` (contract
  `review_density` constants), `review_jobs_busy`,
  `review_bridge_abi_version()` (`review-contract.json` `review_abi_version`,
  independent of `bridge_abi_version()`),
  and the fixtures `review_fixture_write_experiment(path)` and
  `review_fixture_write_population(path, cells, seed)` (two seeded
  populations for the Charts view; `examples/review_fixture --population N`).
- **Contract.** YOFO Review has its own versioned contract,
  `contract/review-contract.json` (`review_abi_version` 1), so the shared
  `bridge-contract.json` is never touched for it: `overlay_modes`,
  `review_density` (incl. `ramp_rgb`, checked against `MonitoringDensity.h`
  `kStops` by the generator), `review_pixel_formats`,
  `review_operation_kinds`, `review_regenerate_sources`, and the review
  frame packets (the bridge contract's header with pull kinds `review` (3),
  `review_thumbnails` (5), `review_series` (6), RGB8 and zero capture
  identities). `scripts/gen_bridge_contract.py` renders it to
  `desktop/src/review/reviewContract.ts` and
  `desktop/src-tauri/src/review_packet_contract.rs` (same `--check` gate);
  `review_shim.cpp` pins the enums with `static_assert`s;
  `tests/review_bridge.rs` runs in both feature configurations;
  `tests/contract.rs` is `#![cfg(not(feature = "review-only"))]`.
- **Tauri:** chart snapshots reach the jobs over the raw IPC body, not
  JSON number arrays: `review_stage_chart` (body = PNG bytes, header
  `x-chart-name`, ≤ 32 MB, ≤ 8 staged, name rule = the backend's),
  `review_clear_charts`; `review_export_all` / `review_export_charts` take
  the staged set. `review_list_dir(dir)` lists file names for the shell's
  default export names.
- **Tauri:** `desktop/src-tauri/src/review.rs` exposes the commands to
  YOFO Review's binary (`review_app.rs`); `review_packet.rs` /
  `desktop/src/review/reviewPacket.ts` encode and decode its packets (RGB8
  allowed, stride = width × bytes/pixel), leaving MIB Studio's
  `frame_packet.rs` / `framePacket.ts` as develop has them.
  `desktop/src/review/reviewBridge.ts` is the TypeScript client.
