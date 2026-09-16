# Device discovery service with device-specific providers (#419)

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/419 — the
issue body is the specification. Plan:
`docs/exec-plans/active/2026-09-15-device-discovery-service.md`. Decision:
`docs/decisions/0005-device-discovery-service.md`. Service note:
[[../services/DeviceDiscoveryService]].

## What changed

- **Backend module `backend::discovery`** (`include/backend/discovery/`,
  `src/backend/discovery/`): value types, `IDeviceDiscoveryProvider`,
  `DeviceDiscoveryService` (bounded jobs, cooperative cancellation, retries,
  dedup/ambiguity, overflow, coalescing, observers, shutdown draining),
  `StartupDiscoveryPolicy` (pure decisions), `StartupDiscoveryCoordinator`
  (camera-then-nanopositioner sequence with the pre-#419 400 ms / 3×4000 ms
  defaults, injectable executor), providers for MindVision, eGrabber
  cameras, eGrabber framegrabbers, nanopositioners (CoreMorrow/OEABT via
  the existing probe) and pulse generators (`scanBus`, callback-cancel
  overload).
- **AppBackend** owns the service and coordinator; `shutdown()` stops the
  policy and drains discovery before releasing serial hardware.
  `AutofocusService::setBackendFactory` is a new test seam.
- **Qt**: `DeviceInitManager` is an adapter (no QtConcurrent workers);
  `ConnectTab` lists devices only from snapshots (no constructor/Refresh
  enumeration, no UI-thread fallback); `NanopositionerTab` fills its combo
  from snapshots; `ConfigTabs` pulse-generator Scan/Cancel is a discovery
  job with an explicit port scope (no `std::thread`);
  `frontend::DiscoverySubscription` marshals worker callbacks to the UI.
- **Facade/bridge (ABI 14)**: `startDeviceDiscovery` /
  `cancelDeviceDiscovery` / `fetchDeviceDiscovery` with contract-typed DTOs;
  `fetchCameraDiscovery` kept as a documented worker-only blocking wrapper.
  cxx/Tauri/TS: `start_device_discovery`, `start_camera_discovery`,
  `fetch_device_discovery`, `cancel_device_discovery` replace
  `fetch_camera_discovery`; new contract groups (device kinds, job states,
  identity strengths, identification statuses, error kinds);
  `desktop/src/discovery.ts` polls the job and projects the snapshot onto
  the existing camera/framegrabber shape used by `App.tsx`.
- **Tooling**: `tools/gen_bridge_link_manifest_ninja.py` derives the
  Windows bridge link manifest from the `windows-ninja` tree so `cargo test`
  runs against the fast local build.

## Behaviour preserved

Camera step after 400 ms, then nanopositioner with three retries at 4 s;
one identified camera is selected (never started), one identified
nanopositioner is connected (observe-only); multiple devices, ambiguity,
partial/cancelled/failed scans never auto-connect; pulse-generator scans
stay explicit (port + settings + addresses 1–16), read-only, cancellable and
report unidentified Modbus devices; mock stays an explicit synthetic entry;
framegrabbers never inflate camera counts; #417 shutdown order; #418 idle
Overview untouched.

## Verification (Windows bench PC, 2026-09-16, `windows-ninja` Release)

- Backend: `backend.device_discovery_service`, `_fault`,
  `_providers`, `_stress`, `backend.startup_discovery_policy`,
  `backend.discovery_facade` — pass (repeated runs).
- Qt: `frontend.device_discovery` (offscreen; responsiveness while a fake
  probe blocks, coalescing, tab destruction mid-scan, `stop()`, ConfigTabs
  scan/cancel) — pass; `frontend.mainwindow_shutdown`,
  `frontend.config_tabs_state`, `frontend.mindvision_overview` still pass.
- e2e: `integration.e2e_device_discovery_lifecycle` over a real
  `AppBackend` (fake serial bench, fake nanopositioner driver) — pass.
- Bridge: `cargo test --release` in `crates/mib-bridge` with the Ninja link
  manifest — 16/16 contract tests pass; `scripts/gen_bridge_contract.py
  --check` in sync; desktop `tsc --noEmit` clean, `vitest` 124/124.
- Full Windows fast lane and integration lane: see
  [[../current-state/Recent-Work]] entry for counts.
- Regression-first evidence: each test file was built and run red before
  its implementation (compile failures on missing headers, then behavioural
  failures such as the coordinator self-join and the ConnectTab status
  overwrite, both fixed before green).

## Not verified here

- Linux backend CI, TSan and ASan/UBSan lanes (run on PR #421; GCC fix `9dfee2a`).
- Hardware acceptance (partial, rig PC 2026-09-16): startup discovery,
  auto-selection, auto-connection, shutdown ordering, close-during-scan,
  relaunch verified on real MindVision camera + OEABT nanopositioner. GUI
  interactive tests (Refresh, Cancel, Scan, active-device conflict,
  unplug/replug) and pulse-generator Scan not tested (non-interactive
  session). eGrabber and CoreMorrow not installed. Full matrix recorded in
  the execution plan.
- The React/Tauri desktop was type-checked and unit-tested, not launched.

## Limits and debt

- Camera SDK enumerations cannot be interrupted; cancellation is honoured
  between provider steps and shutdown waits for the current call (TD-10).
- `NanopositionerTab` no longer enumerates ports itself; until the first
  discovery job runs its combo is empty ("Click Refresh…").
- The headless coordinator (inline executor) runs selection hooks on the
  discovery worker; the Tauri shell does not start it.
