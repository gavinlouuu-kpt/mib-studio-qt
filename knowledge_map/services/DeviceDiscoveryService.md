# DeviceDiscoveryService

> Backend job service for device discovery (issue #419, ADR 0005). Every
> camera, framegrabber, nanopositioner and pulse-generator scan — startup,
> Refresh/Try again/Scan, and the headless facade/bridge — runs through it.
> It finds devices; a separate policy decides; the existing services connect.

**Source:** `src/backend/discovery/DeviceDiscoveryService.cpp`,
`include/backend/discovery/{DeviceDiscoveryTypes,IDeviceDiscoveryProvider,DeviceDiscoveryService,StartupDiscoveryPolicy,StartupDiscoveryCoordinator}.h`,
`src/backend/discovery/providers/*`
**Related:** [[../architecture/AppBackend]] (owner), [[CameraControlService]],
[[AutofocusService]], [[PulseGeneratorService]], [[SerialBus]],
[[../frontend/System-Utilities]] (`DeviceInitManager` adapter),
[[../frontend/ConnectTab]], [[../frontend/NanopositionerTab]],
[[../frontend/ConfigTabs]], [[../architecture/Rust-Bridge]] (ABI 14),
`docs/decisions/0005-device-discovery-service.md`,
`docs/exec-plans/active/2026-09-15-device-discovery-service.md`

## Responsibility

- **Jobs.** `startDiscovery(request) -> StartResult{jobId | rejection}`,
  `cancelDiscovery(jobId)` (idempotent, non-blocking, scoped to that job),
  `discoverySnapshot(jobId)` (value copy, never probes or waits),
  `waitForTerminal` (tests / worker-only wrappers), observers
  (`addObserver`/`removeObserver`; removal blocks until an in-flight call
  returns), `shutdownDiscovery()` (terminal: refuse jobs, cancel, join every
  worker). States `Queued -> Running -> Completed | Cancelled | Failed`.
- **Bounds.** Requests are validated before any work (kinds non-empty,
  retries ≤ 10, delays ≤ 60 s, deadline ≤ 10 min, serial address range
  sane). Limits: 256 candidates, 64 errors, 16 retained terminal jobs, 4
  concurrent jobs (`TooManyJobs`). Overflow is explicit
  (`overflow=true`, `Overflow` error) and makes the result incomplete.
- **Coalescing.** An identical non-terminal request (same kinds, providers,
  scope, delay/deadline/retry) returns the running job
  (`StartResult.coalesced`).
- **Providers** (`IDeviceDiscoveryProvider`: `id`, `kind`, `resourceClass`,
  `cancellable`, `discover(request, context)`). Providers sharing a resource
  class (`camera-sdk`, `serial-probe`) never run concurrently across jobs.
  Exceptions become `ProviderException`; the job is `Failed` only when every
  selected provider failed (zero matches is `Completed`).
- **Guards.** `setResourceGuard(kind, fn)`: while `fn()` is true the kind's
  providers report `Busy` instead of touching hardware. `AppBackend`
  installs `capture().isRunning()` for cameras/framegrabbers.
- **Identity and dedup.** Candidates carry `stableIdentity` +
  `identityStrength` (`Persistent` for GenTL IDs / USB adapter IDs,
  `SessionLocal` for MindVision indices, OS port paths and Modbus
  addresses — never persisted as identity), a structured `endpoint`
  (system path, persistent id, SDK/GenTL indices, bus address),
  `identification` (`Identified` / `Unidentified` / `Ambiguous` /
  `Unsupported`), `claimedBy`, `capabilities`, `diagnostics`, `synthetic`
  (mock), and the legacy payload (`camera`, `framegrabber`,
  `nanopositioner`, `pulseGenerator`) verbatim. Dedup key is
  provider + persistent identity, else provider + endpoint. Two identified
  claims for one endpoint (two providers, or two vendor protocols on one
  adapter) are merged/marked `Ambiguous` with every claimant listed —
  never silently chosen.
- **Retries.** `RetryPolicy{maxRetries, delay}` re-runs the providers until
  an identified candidate appears; the partial attempt is published
  (`attempt`/`maxAttempts`) and the delay is cancellable.
- **`complete`** is true only when every selected provider ran without
  error, no guard fired, no overflow, no timeout, no cancel.

## Providers

| Provider id | Kind | Wraps | Cancellation bound |
|---|---|---|---|
| `mindvision` | Camera | `CameraControlService::discoverMindVisionCameras` | none — SDK enumeration is not interruptible (TD-10) |
| `egrabber` | Camera | `CameraControlService::discoverCameras` | none |
| `egrabber-framegrabber` | Framegrabber | `CameraControlService::discoverFramegrabbers` | none |
| `nanopositioner` | Nanopositioner | `AutofocusService::availableEndpoints` + `probeEndpoint` via `nanopositioner::discover` | between endpoints |
| `pulse-generator` | PulseGenerator | `PulseGeneratorService::scanBus` (callback-cancel overload) over the shared `SerialBusManager` session | between addresses |

A compiled-out camera SDK reports `MissingSdk` (known-absent coverage, still
decidable by the policy); a pulse-generator request without an explicit
`serialScope` (port, serial settings, address range) is refused with
`InvalidRequest` — no broad serial sweep can start from here.

**Adding a provider:** implement `IDeviceDiscoveryProvider` in
`providers/`, wrap the driver's existing read-only enumeration/identity
probe through `std::function` seams (so fakes inject at the same seam),
fill identity strength/status honestly, check `context.cancelled()` between
bounded steps, register it in `AppBackend::initialize`, add a policy rule
only if it should be auto-selected, and cover it in
`tests/backend/device_discovery_providers_test.cpp`.

## Startup policy

`StartupDiscoveryPolicy` (`decideCamera`, `decideNanopositioner`) is pure:
only a `Completed`, complete (or missing-SDK-only), non-overflowed snapshot
is decidable; one identified physical camera → `SelectUnique`
(framegrabbers and the synthetic mock never count); one identified
nanopositioner → `ConnectUnique`; any ambiguity or more than one identified
device → `RequireSelection`, even when a saved preference matches
(`orderByPreference` reorders only).

`StartupDiscoveryCoordinator` (owned by `AppBackend`, started by the Qt
adapter) runs camera (400 ms delay, kinds Camera+Framegrabber) then
nanopositioner (3 retries × 4000 ms); when the camera step is skipped
(already configured / capturing) the nanopositioner job still waits the
400 ms so an immediate close cancels a queued job rather than draining a
serial probe. It calls the existing
`AppBackend::set*CameraSelection` / `AutofocusService::connect` hooks.
Decision actions and outcome listeners run through an injected
**executor** (default inline on the worker; the Qt adapter posts to the UI
thread). Outcomes: camera `Started/Skipped/NoneFound/Selected/
RequireSelection/Incomplete/Refused`; nanopositioner `Started/Searching
(attempt n/m)/NotFound/Connected/ConnectFailed/RequireSelection/Incomplete`.
`runCameraStep()` (Try again) / `runNanopositionerStep()` (Refresh) refuse
duplicates; `stop()` is terminal and cancels owned jobs. The Tauri shell
does not start the coordinator (no auto-connect there, as before).

## Threading

One worker thread per job; providers run sequentially inside a job. Workers
publish snapshots under the service mutex, then notify observers with no
lock held (observers run on the worker). Threads are joined only when they
have exited (`reapFinished`, never under the service lock) or at
`shutdownDiscovery()`. Cancellation is cooperative; an uninterruptible
vendor call delays the join until it returns.

## Shutdown order (#417 preserved)

`AppBackend::shutdown()` and `BackendFacade::shutdown()`: coordinator
`stop()` → `shutdownDiscovery()` (refuse, cancel, join) → capture stop →
serial hardware release. `backend.device_discovery_stress` and
`integration.e2e_device_discovery_lifecycle` assert the job ends before the
shared serial port closes.

## Tests

`backend.device_discovery_service` (job core), `backend.device_discovery_fault`,
`backend.startup_discovery_policy`, `backend.device_discovery_providers`,
`backend.device_discovery_stress`, `backend.discovery_facade`,
`frontend.device_discovery` (Qt adapter/tabs, offscreen),
`integration.e2e_device_discovery_lifecycle`, Rust `contract.rs`
(`camera_discovery_and_selection_contract`), `desktop/src/discovery.test.ts`.
Fakes: `tests/support/fake_discovery_providers.h`,
`tests/support/fake_modbus_bench.h`.

## Gotchas

- Never call `shutdownDiscovery()` from inside an observer/provider (it
  cannot join its own worker; logged as an error).
- Observers must stay valid until `removeObserver`/`shutdownDiscovery`
  returns; Qt receivers use `frontend::DiscoverySubscription`.
- `AppBackend`'s camera selection setters are not thread-safe against the
  widgets reading them: the Qt adapter's executor keeps policy actions on the
  UI thread. Headless consumers that start the coordinator with the inline
  executor run hooks on the discovery worker.
- The facade's `fetchCameraDiscovery` is a documented **blocking**
  compatibility wrapper (starts a job and waits) for worker-thread C++
  callers only; the bridge/Tauri/TS use the asynchronous trio.
