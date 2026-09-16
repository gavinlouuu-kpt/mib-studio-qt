# Device discovery service with device-specific providers (#419)

Status: active

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Issue: https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/419
Baseline: `208b0d2` on `develop` (includes #417 hardware shutdown, #418 MindVision Overview).
ADR: [0005 — Device discovery is a backend job service](../../decisions/0005-device-discovery-service.md)

**Goal:** One backend `DeviceDiscoveryService` (jobs, deadlines, cancellation, retries, bounded snapshots, dedup) fed by providers that wrap the existing MindVision / eGrabber / nanopositioner / pulse-generator enumeration and read-only identity probes, with a separate startup selection/connection policy, consumed identically by the Qt desktop and the headless facade/bridge.

**Architecture:** `backend::discovery` module inside `mib_backend` (no plugin loader, no new process). Providers own SDK/protocol knowledge and return value-typed candidates; the service owns job lifetime and resource coordination; `StartupDiscoveryCoordinator` (backend app layer) owns the camera-then-nanopositioner sequence and calls the existing selection/connection APIs; `frontend::DeviceInitManager` shrinks to a Qt adapter; `BackendFacade` gains additive async start/cancel/snapshot commands mirrored through cxx/Tauri/TypeScript.

**Tech stack:** C++17 (`std::thread`, `std::condition_variable`, value types only in public headers), Qt 6.7 widgets (adapter only), cxx bridge (Rust), Tauri 2 / TypeScript, CTest bare-`main()` tests with `tests/support/`.

**Spec:** the issue body (GitHub #419); the task note `knowledge_map/task/2026-09-15-device-discovery-service.md` records what was built against it.

## Global constraints

- Public discovery headers expose no `QObject`/`QWidget`, no vendor SDK handles; discovery stays out of the `mib_processing` ABI.
- No SDK enumeration or protocol probe runs on the UI thread; a scan never holds a lock that blocks status/cancel calls.
- Probes are read-only: no motion, capture-start, generator-enable, configuration writes, experiment commands. Adapter VID/PID or a generic Modbus reply never establishes identity.
- Never auto-select/connect from a partial, cancelled, failed, or overflowed snapshot; multiple identified devices always require user selection, even with a saved preference.
- Defaults preserve today's behaviour: camera step after 400 ms, then nanopositioner with 3 retries x 4000 ms; pulse-generator scans stay explicit (port + settings + address 1-16). No broad serial sweeps at startup.
- Shutdown order: stop startup policy/retries and refuse new jobs, then cancel/drain discovery workers, then release hardware (#417 guarantees preserved). Workers are joined, never detached.
- Bridge contract changes are additive (ADR 0004): append-only enum values, ABI 13 to 14, C++ static_asserts + Rust contract test + generated TS stay in parity.
- Vault notes update in the same commit as the code they describe; `python scripts/check_docs.py` passes.

## API and data contract (settled)

```cpp
namespace backend::discovery {
enum class DeviceKind { Camera, Framegrabber, Nanopositioner, PulseGenerator };
enum class JobState { Queued, Running, Completed, Cancelled, Failed };
enum class IdentityStrength { None, SessionLocal, Persistent };
enum class IdentificationStatus { Identified, Unidentified, Ambiguous, Unsupported };
enum class ErrorKind { None, InvalidRequest, Busy, OpenFailed, PermissionDenied, Timeout,
                       MalformedResponse, Unsupported, MissingSdk, ProviderException,
                       Cancelled, Overflow, ShuttingDown, TooManyJobs };

struct DeviceEndpoint { std::string systemPath, persistentId; int sdkIndex{-1};
                        int interfaceIndex{-1}, deviceIndex{-1}, streamIndex{-1};
                        int busAddress{-1}; std::optional<std::uint16_t> vendorId, productId; };
struct Diagnostic { ErrorKind kind; std::string message; };
struct DiscoveredDevice { DeviceKind kind; std::string providerId, displayName; DeviceEndpoint endpoint;
                          std::string stableIdentity; IdentityStrength identityStrength;
                          IdentificationStatus identification; std::vector<std::string> claimedBy;
                          std::vector<std::string> capabilities; std::vector<Diagnostic> diagnostics;
                          bool synthetic{false};
                          std::optional<services::DiscoveredCamera> camera;
                          std::optional<services::DiscoveredFramegrabber> framegrabber;
                          std::optional<nanopositioner::Endpoint> nanopositioner;
                          std::optional<PulseGeneratorHit> pulseGenerator; };
struct SerialScanScope { std::string portName; services::SerialSettings settings;
                         std::uint8_t addressFrom{1}, addressTo{16}; int perAddressTimeoutMs{250}; };
struct RetryPolicy { int maxRetries{0}; std::chrono::milliseconds delay{0}; };
struct DiscoveryRequest { std::vector<DeviceKind> kinds; std::vector<std::string> providers;
                          std::optional<nanopositioner::Endpoint> preferredNanopositioner;
                          std::optional<SerialScanScope> serialScope;
                          std::chrono::milliseconds initialDelay{0}, deadline{60000};
                          RetryPolicy retry; std::string origin; };
struct DiscoveryError { std::string providerId; ErrorKind kind; std::string message, endpoint; };
struct DiscoverySnapshot { std::uint64_t jobId{0}, generation{0}; JobState state{Queued};
                           bool complete{false}, overflow{false}; int attempt{0}, maxAttempts{1};
                           std::vector<DiscoveredDevice> candidates; std::vector<DiscoveryError> errors;
                           std::vector<std::string> providersRun; std::string origin; };
struct StartResult { bool accepted{false}, coalesced{false}; std::uint64_t jobId{0};
                     ErrorKind rejection{None}; std::string reason; };

class IDeviceDiscoveryProvider { id(); kind(); resourceClass(); cancellable();
                                 discover(const DiscoveryRequest&, const ProviderContext&) -> ProviderResult; };
class DeviceDiscoveryService {
  registerProvider(std::unique_ptr<IDeviceDiscoveryProvider>);
  setResourceGuard(DeviceKind, std::function<bool()> busy);
  StartResult startDiscovery(const DiscoveryRequest&);
  void cancelDiscovery(std::uint64_t jobId);                 // idempotent, non-blocking
  DiscoverySnapshot discoverySnapshot(std::uint64_t jobId) const; // never probes/waits
  bool waitForTerminal(std::uint64_t jobId, std::chrono::milliseconds) const;
  std::uint64_t addObserver(Observer); void removeObserver(std::uint64_t);
  void shutdownDiscovery();                                   // terminal; drains workers
  std::size_t activeWorkerCount() const;
};
inline constexpr std::size_t kMaxCandidates = 256, kMaxErrors = 64, kMaxRetainedJobs = 16, kMaxConcurrentJobs = 4;
inline constexpr int kMaxRetries = 10; // delay <= 60 s, deadline <= 10 min
}
```

Job states `Queued -> Running -> Completed | Cancelled | Failed`. `Completed` with zero candidates is not `Failed`. `complete=false` whenever a provider errored, the deadline hit, a guard reported Busy, or overflow truncated. Overflow never yields an apparent unique match (`StartupDiscoveryPolicy` refuses incomplete snapshots).

Dedup key: `providerId + stableIdentity` when strength is `Persistent`, else `providerId + kind + endpoint(systemPath, sdkIndex, busAddress)`. Two providers claiming one endpoint of the same kind mark both `Ambiguous`. Nanopositioner vendors identified more than once on one port mark the candidate `Ambiguous`.

Resource coordination: providers declare a resource class (`camera-sdk`, `serial:<port>`); the service serializes providers of the same class across jobs. Identical running requests (same kinds/providers/scope/origin class) are coalesced (`StartResult.coalesced=true`, same jobId). A `DeviceKind` guard (camera: `capture().isRunning()`) makes the provider report `Busy` and the snapshot incomplete.

Cancellation: cooperative between provider steps (ports/addresses/enumeration calls) and during retry delay. Per-provider bound: camera SDK enumerations are not interruptible (TD-10), nanopositioner cancels between endpoints, pulse generator between addresses.

## Acceptance criteria

- [x] Fake-provider tests: zero/one/multiple, same endpoint claimed twice, stable identity vs changed index/path, separate bus addresses, mock/framegrabber filtering, partial/overflow cannot auto-connect.
- [x] Fault tests: busy port, incompatible settings, missing SDK, permission/open failure, malformed response, provider exception, timeout, cancellation during enumeration/probe/retry, saved preference to missing/ambiguous device.
- [x] Instrumented fakes assert zero operational/configuration commands during discovery; cancellation never disconnects an established connection.
- [x] Stress: repeated start/cancel/refresh + shutdown with in-flight jobs; exactly one terminal outcome per accepted job; no stale selection; no worker leak; bounded queues; no access after teardown; watchdog, no naked joins.
- [x] Qt integration: startup, manual scans, cancellation, tab destruction during scan, UI responsive while a fake provider blocks; `frontend.mainwindow_shutdown` (#417) and `frontend.mindvision_overview` (#418) still pass.
- [x] Headless facade/bridge: equivalent results without widgets; async discovery does not block status/cancel; contract parity (static_asserts, `cargo test` contract, `gen_bridge_contract.py --check`).
- [x] Windows fast CTest lane (118/118) and docs checks pass on bench and rig PCs. Linux/TSan/ASan lanes run in CI on PR #421; GCC fix pushed as `9dfee2a`.
- [x] Hardware acceptance recorded explicitly (rig PC, partial — startup/shutdown/close/relaunch verified on real MindVision + OEABT + pulse-generator hardware; GUI-interactive and unplug/replug tests not claimed; eGrabber and CoreMorrow not installed).

## File structure

| Path | Responsibility |
|---|---|
| `include/backend/discovery/DeviceDiscoveryTypes.h` | value types above (request/result/error/limits) |
| `include/backend/discovery/IDeviceDiscoveryProvider.h` | provider interface + `ProviderContext`/`ProviderResult` |
| `include/backend/discovery/DeviceDiscoveryService.h`, `src/backend/discovery/DeviceDiscoveryService.cpp` | job service |
| `include/backend/discovery/StartupDiscoveryPolicy.h`, `src/backend/discovery/StartupDiscoveryPolicy.cpp` | pure decisions (unique camera / unique nanopositioner / retry) |
| `include/backend/discovery/StartupDiscoveryCoordinator.h`, `src/backend/discovery/StartupDiscoveryCoordinator.cpp` | camera-then-nanopositioner sequence, stop(), outcome listener |
| `include/backend/discovery/providers/*.h`, `src/backend/discovery/providers/*.cpp` | wrappers over existing enumeration/probe code with injectable seams |
| `tests/support/fake_discovery_providers.h` | scripted/blocking fake providers, instrumented fakes |
| `tests/backend/device_discovery_{service,fault,providers,stress}_test.cpp`, `startup_discovery_policy_test.cpp`, `discovery_facade_test.cpp` | headless coverage |
| `tests/integration/e2e_device_discovery_lifecycle_test.cpp` | e2e over a real `AppBackend` |
| `tests/frontend/device_discovery_ui_test.cpp` | Qt adapter/tabs |
| `include/frontend/system/DiscoverySubscription.h` | RAII observer with queued Qt delivery |
| `src/frontend/system/DeviceInitManager.*`, `ConnectTab.*`, `NanopositionerTab.*`, `ConfigTabs.*` | adapter/consumers |
| `include/backend/app/BackendFacade.h`, `src/backend/app/BackendFacade.cpp`, `crates/mib-bridge/*`, `desktop/*` | async facade + contract |

## Decision log

- 2026-09-15: Discovery is a compiled-in `mib_backend` module; providers are registered by `AppBackend::initialize`. No plugin loader, no separate process (issue architecture decision; ADR 0005).
- 2026-09-15: Providers wrap the existing `CameraControlService::discover*`, `AutofocusService::availableEndpoints/probeEndpoint`, `nanopositioner::discover`, and `PulseGeneratorService::scanBus` through `std::function` seams instead of duplicating SDK code, so fakes inject at the same seam the production wiring uses.
- 2026-09-15: Legacy per-kind result structs are embedded (`std::optional`) in `DiscoveredDevice` so existing UI/facade fields survive verbatim; the generic identity/endpoint/status fields are additive.
- 2026-09-15: One worker thread per job (max 4 concurrent), providers within a job run sequentially, same-resource providers are serialized across jobs by a class mutex. No broad parallel scans by default.
- 2026-09-15: Startup policy actions (select camera / connect nanopositioner) execute on the discovery worker via the service observer, never on the UI thread; the Qt adapter only marshals outcomes to widgets. `AppBackend::shutdown()` stops the coordinator and drains discovery before releasing serial hardware.
- 2026-09-15: The facade keeps `fetchCameraDiscovery` as a documented synchronous wrapper (worker-only) implemented over the service; the bridge/Tauri/TS sync command is replaced by `start_camera_discovery` + `fetch_device_discovery` + `cancel_device_discovery` (ABI 14) and `App.tsx` polls the job.
- 2026-09-15: Camera SDK enumeration cannot be interrupted; cancellation is honoured between provider steps and the limitation is tracked under TD-10. Shutdown joins the worker after the current SDK call returns.

## Tasks

### Task 1: Types, provider interface, service core (headless, fake providers)

Files: `include/backend/discovery/DeviceDiscoveryTypes.h`, `IDeviceDiscoveryProvider.h`, `DeviceDiscoveryService.h`, `src/backend/discovery/DeviceDiscoveryService.cpp`, `tests/support/fake_discovery_providers.h`, `tests/backend/device_discovery_service_test.cpp`, `src/backend/CMakeLists.txt`, `tests/CMakeLists.txt`.

- [x] Write `device_discovery_service_test.cpp` covering: request validation (empty kinds, retry > 10, delay > 60 s, deadline > 10 min give `InvalidRequest`), zero/one/multiple candidates, dedup by persistent identity across changed `sdkIndex`/`systemPath`, separate `busAddress` on one port are distinct, two providers claiming one endpoint give `Ambiguous`, overflow (300 candidates) gives `overflow && !complete`, coalescing of identical running requests, `kMaxConcurrentJobs` gives `TooManyJobs`, retained job eviction, snapshot returns while a provider blocks, cancel unblocks a blocked provider and yields exactly one terminal state, shutdown with a job in flight yields `Cancelled` and `activeWorkerCount()==0`, observer receives exactly one terminal notification per job and none after shutdown.
- [x] Run `ctest --test-dir build-ninja -R backend.device_discovery_service`; expected: build failure (types missing).
- [x] Implement types/interface/service; register sources in CMake; `add_test(backend.device_discovery_service ... LABELS "backend;concurrency;stress" TIMEOUT 60)`.
- [x] Build + run: PASS. Commit `feat(discovery): add DeviceDiscoveryService job core with fake-provider tests`.

### Task 2: Fault semantics

Files: `tests/backend/device_discovery_fault_test.cpp`, service tweaks.

- [x] Test: provider throws gives `ProviderException` error and `Completed` with `complete=false`; provider-reported Busy/OpenFailed/PermissionDenied/MalformedResponse/MissingSdk/Unsupported preserved verbatim per provider; deadline exceeded between providers gives `Failed` + `Timeout` with partial candidates retained; cancellation during initial delay, during provider, during retry delay each gives `Cancelled` promptly (gated on progress, not ms); retry policy re-runs providers until an identified candidate appears (attempt/maxAttempts visible), stops on cancel.
- [x] Run (FAIL), implement, run (PASS), commit `test(discovery): fault-injection coverage for discovery jobs`.

### Task 3: Startup policy + coordinator

Files: `StartupDiscoveryPolicy.{h,cpp}`, `StartupDiscoveryCoordinator.{h,cpp}`, `tests/backend/startup_discovery_policy_test.cpp`.

- [x] Policy tests: `decideCamera`: Completed+complete with exactly one physical identified camera gives `SelectUnique`; framegrabbers/synthetic ignored; 2 cameras give `RequireSelection`; zero gives `NoneFound`; incomplete/cancelled/failed give `NotDecidable`. `decideNanopositioner`: unique identified gives `ConnectUnique`; ambiguous or more than one gives `RequireSelection` even when one matches the saved preference; zero gives `Retry` while attempts remain else `NotFound`; incomplete gives `NotDecidable`.
- [x] Coordinator tests (fake providers + fake select/connect hooks): sequence camera (initialDelay) then nanopositioner; retry count x delay; hooks called exactly once; `stop()` before completion gives no hook calls; stale generation (a manual job finishing after a newer startup job) ignored; `runCameraStep()` refused while a camera job is running; camera skipped when guard says configured/capturing.
- [x] Implement, PASS, commit `feat(discovery): startup selection/connection policy and coordinator`.

### Task 4: Real providers with injectable seams + instrumented fakes

Files: `providers/*.{h,cpp}`, `tests/backend/device_discovery_providers_test.cpp`.

- [x] Tests: `CameraEnumerationProvider` maps `DiscoveredCamera`/`DiscoveredFramegrabber` fields verbatim, MindVision `cameraIndex` is `SessionLocal`, eGrabber `interfaceID/deviceID` is `Persistent`; enumerator throwing gives `ProviderException`; guard busy gives `Busy`. `NanopositionerProvider` orders preferred first, filters by requested vendor, copies baud/address, probes each endpoint once, records unidentified endpoints, cancels between endpoints, maps `identifiedVendors.size()>1` to `Ambiguous`; instrumented fake backend records zero `setVoltage` calls; an `AutofocusService` connected through a fake factory stays connected across a cancelled discovery. `PulseGeneratorProvider` over the fake Modbus bench (from `illuminated_live_test`): generator/ModbusDevice/Error hits mapped, `writeCommands==0`, busy port gives `Busy`, missing port gives `OpenFailed`, corrupt frames give `MalformedResponse`, cancel between addresses, request without `serialScope` gives `InvalidRequest`.
- [x] Implement, PASS, commit `feat(discovery): camera, nanopositioner and pulse-generator providers`.

### Task 5: AppBackend wiring, shutdown order, stress

Files: `AppBackend.{h,cpp}`, `tests/backend/device_discovery_stress_test.cpp`.

- [x] Tests: `AppBackend::deviceDiscovery()`/`startupDiscovery()` exist; shutdown with a blocked fake job in flight terminates it `Cancelled` before `autofocus().disconnect()` is reached (order asserted via observer + fake backend timeline); stress loop of start/cancel/refresh across 4 threads with watchdog; exactly one terminal per job; `activeWorkerCount()==0` after shutdown; second `shutdown()` idempotent.
- [x] Implement wiring (declare `deviceDiscovery_` after camera/autofocus/serial services; construct after `pulseGeneratorService_`; register providers; camera guard). PASS. Commit `feat(discovery): wire DeviceDiscoveryService into AppBackend shutdown order`.

### Task 6: Qt adapter migration (DeviceInitManager, ConnectTab, NanopositionerTab)

Files: `include/frontend/system/DiscoverySubscription.h`, `DeviceInitManager.{h,cpp}`, `ConnectTab.{h,cpp}`, `NanopositionerTab.{h,cpp}`, `tests/frontend/device_discovery_ui_test.cpp`, `src/frontend/qt/CMakeLists.txt`.

- [x] Test (offscreen): backend with fake blocking camera provider + fake nanopositioner provider; `DeviceInitManager::start()`; a 50 ms QTimer keeps ticking while the provider blocks (UI responsive); release gives `ConnectTab` camera list and selection status; `NanopositionerTab` shows identifying status then the auto-connect result; Refresh while running is coalesced (single job); destroying `ConnectTab` mid-scan does not crash; `stop()` cancels and returns promptly.
- [x] Implement: `DeviceInitManager` keeps its public API, drives the coordinator, no QtConcurrent/QFutureWatcher, no tab-owned workers; `ConnectTab::populateDevices` consumes snapshots (no synchronous enumeration in the constructor or Refresh); `tryAutoConnect` fallback removed; `NanopositionerTab` Refresh consumes snapshot candidates (identified + unidentified) for its combo.
- [x] PASS plus `frontend.config_tabs_state`, `frontend.mainwindow_shutdown`, `frontend.mindvision_overview`, `frontend.camera_action_state` still PASS. Commit `refactor(frontend): drive camera/nanopositioner discovery through the backend service`.

### Task 7: Pulse-generator scan migration (ConfigTabs)

Files: `ConfigTabs.{h,cpp}`, extend `device_discovery_ui_test.cpp`.

- [x] Test: Scan toggles a `PulseGenerator` job with explicit scope; Cancel gives job `Cancelled` and the button restored; result auto-fills the first generator address; destroying ConfigTabs during a scan is safe; no `std::thread` member remains.
- [x] Implement, PASS, commit `refactor(frontend): route pulse-generator scans through DeviceDiscoveryService`.

### Task 8: Facade + bridge + desktop contract

Files: `BackendFacade.{h,cpp}`, `crates/mib-bridge/src/{lib.rs,shim.h,shim.cpp}`, `crates/mib-bridge/contract/bridge-contract.json`, `crates/mib-bridge/tests/contract.rs`, `desktop/src-tauri/src/lib.rs`, `desktop/src/{bridge.ts,bridgeContract.ts,App.tsx}`, `tests/backend/discovery_facade_test.cpp`.

- [x] Facade test: `startDeviceDiscovery` returns jobId; `fetchDeviceDiscovery` polls; `cancelDeviceDiscovery`; `fetchCameraSelection` returns while a fake provider blocks; legacy `fetchCameraDiscovery` (worker) equals async result + mock entry; facade `shutdown()` drains jobs.
- [x] Bridge: add `BridgeDiscoveryStart/BridgeDiscoveredDevice/BridgeDiscoverySnapshot`, fns `start_camera_discovery`, `fetch_device_discovery`, `cancel_device_discovery`; remove `fetch_camera_discovery` from shim/Rust/Tauri/TS; contract JSON gains `discovery_job_states`, `discovery_device_kinds`, `discovery_identification_statuses`, `discovery_error_kinds`; ABI 14; static_asserts; contract test updated; `gen_bridge_contract.py`; `bridge.ts` helpers `startCameraDiscovery/fetchDeviceDiscovery/cancelDeviceDiscovery` + `pollCameraDiscovery()` returning the existing `CameraDiscovery` shape; `App.tsx` uses it.
- [x] Verify: `ctest -R backend.discovery_facade`, `python scripts/gen_bridge_contract.py --check`, desktop `npx tsc --noEmit` and `npx vitest run`, `cargo test -p mib-bridge` (Windows link manifest; if not producible locally record the exact blocker). Commit `feat(bridge): asynchronous device discovery contract (ABI 14)`.

### Task 9: e2e lifecycle test

Files: `tests/integration/e2e_device_discovery_lifecycle_test.cpp`, `tests/CMakeLists.txt`.

- [x] Real `AppBackend` (mock camera mode, fake serial factory, fake nanopositioner backend factory injected through `AutofocusService`), fake providers registered next to the real ones: startup coordinator selects nothing for camera (configured mock), connects the unique nanopositioner through the real `AutofocusService`, explicit pulse-generator scan over the fake Modbus bench reports the generator without writes; cancelled scan leaves the nanopositioner connected; shutdown with a blocked job terminates it `Cancelled` before hardware release; candidate accounting asserted (candidates == identified + unidentified + ambiguous). Labels `integration;e2e`, watchdog 60 s.
- [x] PASS, commit `test(discovery): end-to-end lifecycle over AppBackend`.

### Task 10: Consolidation, docs, vault

- [x] Remove dead code (QtConcurrent discovery, `ConnectTab` fallback, `pgScanThread_`); update vault notes (new `services/DeviceDiscoveryService.md`, `_MOC`, `README`, `Agent-Onboarding`, `architecture/AppBackend`, `Threading-Model`, `Rust-Bridge`, `frontend/System-Utilities`, `ConnectTab`, `NanopositionerTab`, `ConfigTabs`, `MainWindow`, `services/CameraControlService`, `AutofocusService`, `PulseGeneratorService`, `SerialBus`), `Recent-Work`, task note, tech-debt tracker (TD-10 refresh, bridge Windows verification, provider limitations).
- [x] `python scripts/check_docs.py`, `python scripts/check_screenshots.py`, full Windows fast lane + integration lane. Update this plan's Progress; keep `Status: active` until hardware acceptance is recorded, then move to `completed/`.

## Progress

- [x] 2026-09-15: Baseline build (`windows-ninja`, MindVision on) and fast lane green: 111/111.
- [x] 2026-09-16: Tasks 1–10 implemented on `feat/device-discovery-service` (commits `feat(discovery): …`, `refactor(frontend): …`, `feat(bridge): …`); every test file was run red before its implementation.
- [x] 2026-09-16: Windows fast lane 118/118 (after the two test-fixture fixes noted in the task note), integration lane 11/11 incl. `integration.e2e_device_discovery_lifecycle`, `frontend.device_discovery`, Rust `cargo test --release` 16/16 via `tools/gen_bridge_link_manifest_ninja.py`, `gen_bridge_contract.py --check` in sync, desktop `tsc --noEmit` clean + vitest 124/124, `scripts/check_docs.py` clean.
- [ ] Linux backend CI, TSan, ASan/UBSan (run by the PR workflows; not available on this Windows host). GCC aggregate-init fix pushed as `9dfee2a` on `feat/device-discovery-service`.
- [x] 2026-09-16: Rig PC hardware acceptance (partial — non-interactive session, see matrix below).
- Decision addenda (2026-09-16): `Started` outcomes were added to the coordinator so adapters can flip their scanning state without polling; a compiled-out SDK (`MissingSdk`) is treated as known-absent coverage by the policy so a mock-only bench still reaches "No cameras found"; two vendor protocols identifying one persistent adapter merge into a single `Ambiguous` candidate listing both claimants. When the camera step is skipped at startup (camera already configured, e.g. mock mode), the nanopositioner job still waits the camera delay, exactly as the pre-#419 timer did, so a close right after launch cancels a queued job instead of draining a serial probe (`frontend.mainwindow_shutdown` under parallel load exposed this).

### Hardware acceptance matrix (rig PC, 2026-09-16)

**Rig:** Windows 11, MindVision MV-XG51GM (GigE), OEABT nanopositioner on COM7, pulse generator on COM6 (Modbus addr 1), second generator on COM4, CH344 four-port (COM3–6) + CH340 (COM7). No eGrabber hardware. No syringe pump.

**Session type:** non-interactive (agent-driven, console only). Acceptance was performed by launching the built app (`build-ninja/Release/mib_studio_qt.exe`), capturing spdlog output, and verifying behavior from log lines. GUI interaction (Refresh/Cancel buttons, tab navigation, pulse-generator Scan/Cancel) was not possible.

| Test | MindVision camera | OEABT nanopositioner | Pulse generator | eGrabber |
|---|---|---|---|---|
| **Startup: one device** | ✅ Job 1: 1 camera found, auto-configured (index 0). ConnectTab listed 1 MindVision camera. | ✅ Job 2: 6 candidates probed, 1 identified (COM7 OEABT), auto-connected at observe-only voltage. | N/A (startup does not scan pulse generators) | N/A (not installed) |
| **Startup: zero devices** | N/A (camera present) | N/A (nanopositioner present) | N/A | Reports `MissingSdk` (2 errors in job 1 from egrabber/egrabber-framegrabber providers), `complete=false` — correct |
| **Startup: multiple devices** | N/A (only one camera) | N/A (only one identified on COM7; 5 other ports probed as unidentified) | N/A | N/A |
| **Close during scan** | ✅ Process killed 1.2 s after launch while job 2 (nanopositioner) in flight. Exited without crash or hang. | ✅ Same test — nanopositioner scan interrupted. | Not tested | N/A |
| **Graceful close** | ✅ WM_CLOSE after 5 s. Shutdown: coordinator stop → discovery drain (1 worker) → capture stop → nanopositioner disconnect → pulse generator disconnect. Clean exit. | ✅ Same test. Auto-connected nanopositioner disconnected in correct order. | ✅ Shutdown disconnected pulse generator after nanopositioner (correct order). | N/A |
| **Relaunch** | ✅ Second launch: camera found again, no stale port/SDK conflict from prior session. | ✅ Second launch: COM7 nanopositioner auto-connected again at different voltage reading (0.729 V vs 0.853 V), confirming a fresh probe. | Not tested | N/A |
| **No operational commands during discovery** | ✅ No capture-start, no exposure/ROI change, no Overview triggered by discovery (confirmed from logs). | ✅ No motion, no voltage write — `observe-only` mode confirmed. | Not tested | N/A |
| **Refresh (manual)** | Not tested (GUI) | Not tested (GUI) | Not tested (GUI) | N/A |
| **Cancel (manual)** | Not tested (GUI) | Not tested (GUI) | Not tested (GUI) | N/A |
| **Active-device conflict** | Not tested (requires capture + Refresh) | Not tested | Not tested | N/A |
| **Unplug/replug** | Not tested (physical) | Not tested (physical) | Not tested (physical) | N/A |
| **Pulse-generator Scan** | N/A | N/A | Not tested (GUI — requires ConfigTabs Scan button) | N/A |

**Untested hardware:** eGrabber cameras/framegrabbers (not installed on this rig), CoreMorrow nanopositioner (only OEABT present).

**Summary:** Startup discovery, auto-selection (camera), auto-connection (nanopositioner), shutdown ordering, close-during-scan, relaunch, and no-operational-commands-during-discovery are all verified on real hardware. GUI-interactive tests (Refresh, Cancel, Scan, active-device conflict, unplug/replug) require a manual session and are not claimed.
