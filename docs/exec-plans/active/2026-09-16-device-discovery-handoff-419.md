# Handover: device discovery service (issue #419)

Status: active

Date: 2026-09-16. Author: the agent session that implemented #419 on the
Windows bench PC. Consumer: whoever pushes, reviews, runs CI and records
hardware acceptance. Companion documents: the execution plan
[`2026-09-15-device-discovery-service.md`](2026-09-15-device-discovery-service.md)
(Status, decision log, task checkboxes, progress), ADR
[`0005-device-discovery-service.md`](../../decisions/0005-device-discovery-service.md),
service note `knowledge_map/services/DeviceDiscoveryService.md`, task note
`knowledge_map/task/2026-09-15-device-discovery-service.md`.

## 1. Where the work is

| Item | Value |
|---|---|
| Repository | `gavinlouuu-kpt/mib-studio-qt` |
| Local clone | `C:\Users\ERBG07\Developer\mib-studio-qt` (Windows bench PC) |
| Branch | `feat/device-discovery-service`, **local only, not pushed** |
| Base | `develop` at `208b0d2` (includes #417 hardware shutdown and #418 MindVision Overview) |
| Commits (oldest first) | `f4b0e94` service core · `ece3ab4` fault tests · `84b64a4` policy + coordinator · `6e85317` providers · `b6f18c4` AppBackend wiring · `58546d3` Qt adapter/tabs/facade · `4fb04ad` bridge ABI 14 + e2e · `e562160` docs + timing fix |
| Size | 81 files, +8051 / −604 |
| Working tree | clean |

Untracked local build state that must **not** be committed: `build-ninja/`
(Ninja tree), `build/vendor/mindvision-sdk` (provisioned SDK),
`build-ninja/mib-bridge-link-manifest.json`, `desktop/node_modules/`.

## 2. What was delivered against the issue

| Issue scope item | Delivered | Where |
|---|---|---|
| One backend job API + bounded result model | `DeviceDiscoveryService` (start/cancel/snapshot/observers/shutdown; validation, 256 candidates, 64 errors, 16 retained jobs, 4 concurrent, coalescing, dedup, ambiguity, overflow, retries) | `include/backend/discovery/*`, `src/backend/discovery/DeviceDiscoveryService.cpp` |
| Providers for MindVision, eGrabber cameras/framegrabbers, CoreMorrow/OEABT nanopositioners, pulse generators | `CameraEnumerationProvider` (3 ids), `NanopositionerProvider`, `PulseGeneratorProvider`; all wrap existing enumeration/probe code through `std::function` seams; `scanBus` gained a callback-cancel overload | `src/backend/discovery/providers/*`, `PulseGeneratorService.{h,cpp}` |
| Migration of Qt startup discovery, Refresh/Try again/Scan/Cancel | `DeviceInitManager` is an adapter over `StartupDiscoveryCoordinator`; `ConnectTab`, `NanopositionerTab`, `ConfigTabs` consume snapshots; `DiscoverySubscription` marshals to the UI thread; no QtConcurrent worker, no UI-thread enumeration/fallback, no tab-owned `std::thread` | `src/frontend/system/DeviceInitManager.cpp`, `include/frontend/system/DiscoverySubscription.h`, `src/frontend/tabs/{ConnectTab,NanopositionerTab,ConfigTabs}.cpp` |
| Separate startup selection/connection policy preserving behaviour | `StartupDiscoveryPolicy` (pure) + `StartupDiscoveryCoordinator` (400 ms camera delay — also applied to the nanopositioner step when the camera step is skipped —, 3 × 4000 ms retries, unique match only, injectable executor) owned by `AppBackend`, started by the Qt adapter | `src/backend/discovery/StartupDiscovery{Policy,Coordinator}.cpp`, `AppBackend.cpp` |
| Facade / bridge contract | `startDeviceDiscovery` / `cancelDeviceDiscovery` / `fetchDeviceDiscovery` (+ `fetchCameraDiscovery` kept as a documented worker-only blocking wrapper); cxx/Tauri/TS `start_device_discovery`, `start_camera_discovery`, `fetch_device_discovery`, `cancel_device_discovery` replace `fetch_camera_discovery`; ABI 13 → 14; five new contract groups; `desktop/src/discovery.ts` poller | `BackendFacade.{h,cpp}`, `crates/mib-bridge/*`, `desktop/src-tauri/src/lib.rs`, `desktop/src/{bridge,discovery}.ts`, `App.tsx` |
| Shutdown order (#417) | `AppBackend::shutdown()` and `BackendFacade::shutdown()` stop the policy and drain discovery before capture stop and serial release | `AppBackend.cpp`, `BackendFacade.cpp` |
| Tooling | `tools/gen_bridge_link_manifest_ninja.py` derives the Windows bridge link manifest from the `windows-ninja` tree | `tools/` |

Deferred by the issue and untouched: new syringe-pump/FPGA providers,
hotplug monitoring, reconnect after device loss, plugin loader, driver
rewrites, broad serial sweeps.

## 3. Tests added (all green on the bench, see §4)

| Test | Covers |
|---|---|
| `backend.device_discovery_service` | job core: validation, zero/one/multiple, dedup, ambiguity, overflow, coalescing, limits, cancel, shutdown, one terminal notification per job |
| `backend.device_discovery_fault` | provider exception, verbatim structured errors, busy guard, deadline, cancellation in every phase, retry until identified |
| `backend.startup_discovery_policy` | pure decisions incl. MissingSdk decidability; coordinator sequence, retries, duplicates, stale results, stop, executor, skipped-camera delay |
| `backend.device_discovery_providers` | real providers over fakes: field mapping, identity strength, vendor filter, ambiguity merge, cancel between endpoints/addresses, zero write function codes, busy/open/malformed, established connection survives cancel |
| `backend.device_discovery_stress` | AppBackend wiring, busy guard behind live capture (queue-backed camera), shutdown drains before port close, multi-thread start/cancel/refresh with shutdown in flight |
| `backend.discovery_facade` | async facade trio, non-blocking pulls, mock entry, legacy wrapper equivalence, facade shutdown |
| `frontend.device_discovery` | offscreen Qt: responsive UI while a probe blocks, selection status, coalesced Refresh, tab destroyed mid-scan, `stop()`, ConfigTabs scan/cancel |
| `integration.e2e_device_discovery_lifecycle` | real `AppBackend`: startup policy → real `AutofocusService` connect (fake driver), real `PulseGeneratorService` scan on a fake Modbus bench, cancel, shared-bus scan with a live session, shutdown ordering |
| Rust `contract.rs` | ABI 14, contract groups, `camera_discovery_and_selection_contract` via the job API |
| `desktop/src/discovery.test.ts` | poller: terminal states, timeout → cancel, refused start, snapshot → UI shape |

Support: `tests/support/fake_discovery_providers.h`,
`tests/support/fake_modbus_bench.h`. Seams added for tests:
`DeviceDiscoveryService::unregisterProvider`,
`StartupDiscoveryCoordinator::setTiming`,
`AutofocusService::setBackendFactory`.

## 4. Verification evidence (Windows 11 bench PC, `windows-ninja` Release, MindVision SDK on, no eGrabber hardware)

| Lane | Result |
|---|---|
| Windows fast lane (`ctest --preset windows-ninja-test -j 4`), two consecutive full runs | 118/118, 118/118 |
| Integration lane (`ctest -L integration -LE soak`) | 11/11 |
| `frontend.*` under `-j 4`, three runs | 24/24 × 3 |
| `cargo test --release` in `crates/mib-bridge` (Ninja manifest) | 16/16 |
| `desktop`: `npx tsc --noEmit`, `npx vitest run` | clean, 124/124 |
| `scripts/gen_bridge_contract.py --check`, `scripts/check_docs.py`, `scripts/check_screenshots.py` | in sync / OK / 9 in sync |

Regression-first: every test file was built and run red before its
implementation. Defects found that way and fixed before green: a discovery
worker joining its own thread from an inline observer (service reaping);
ConnectTab status overwritten by a second render of the same job; the
nanopositioner probe starting 0 ms after a skipped camera step (made
`frontend.mainwindow_shutdown` exceed its 1 s budget under load); a test
fixture whose folder-backed mock camera exited on its own.

## 5. Not done — the consumer's list

1. **Push and PR.** `git push -u origin feat/device-discovery-service`, PR
   against `develop`, title "Centralize device discovery in a backend
   service with device-specific providers (#419)". Link ADR 0005 and the
   plan. Nothing was pushed from the bench.
2. **CI lanes not runnable here:** `backend-ci.yml` (Linux backend),
   `sanitizers.yml` (TSan, ASan/UBSan), `docs-ci.yml`, the desktop lane
   (`gen_bridge_contract.py --check`, cargo, vitest) and the Windows
   packaging lane. Watch for: TSan findings in
   `DeviceDiscoveryService.cpp` (observer/inflight counters, `Job::exited`),
   Linux `serial_bus_pty` interplay with the new `scanBus` overload, and the
   `linux-backend-only` build of `providers/CameraEnumerationProvider.cpp`
   (uses `MIB_HAS_EGRABBER` / `MIB_HAS_MINDVISION`, both 0 there).
3. **Hardware acceptance (TD-11)** on the rig, for each available device
   class (MindVision, eGrabber if present, CoreMorrow/OEABT if present,
   pulse generator): none / one / multiple devices, Refresh, unplug/replug
   between jobs, active-device conflict (Refresh while capturing must report
   Busy, never touch the SDK), Cancel, close during a scan, relaunch.
   Confirm no motion/illumination/capture starts because of discovery and
   that an established nanopositioner survives a cancelled scan. Record the
   matrix in the plan's Progress section, name unavailable hardware
   explicitly, then set `Status: completed` and `git mv` the plan to
   `completed/`.
4. **Debt decisions** (tracker): TD-12 (NanopositionerTab combo empty until
   the first job), TD-13 (Tauri shell does not start the startup policy;
   selection setters are not thread-safe for an inline executor).
5. **Optional cleanups noticed, not done:** `ConfigTabs.h` still includes
   `<thread>`/`<atomic>` (harmless); `tools/gen_bridge_link_manifest.py`
   (VS generator) and the new Ninja variant could share their JSON writer.

## 6. Behaviour notes a reviewer should know

- In mock camera mode the camera step is skipped and the nanopositioner
  job is **queued for 400 ms** before probing, exactly like the old timer;
  closing within that window cancels a queued job.
- `ConnectTab` lists devices only from snapshots; its constructor starts an
  asynchronous camera+framegrabber job. A snapshot is rendered once per job
  id so the adapter's "Connected to …" status is not overwritten by the
  tab's own render of the same job.
- A compiled-out SDK reports `MissingSdk`; the policy treats it as
  known-absent coverage so a mock-only bench still reaches "No cameras
  found". Any other coverage gap (Busy, Timeout, exception, overflow)
  blocks auto-selection.
- Two vendor protocols identifying the same persistent adapter merge into
  one `Ambiguous` candidate listing both claimants.
- Camera SDK enumeration is not interruptible; cancellation is honoured
  between provider steps and `shutdownDiscovery()` waits for the current
  vendor call (TD-10). `shutdownDiscovery()` must not be called from an
  observer or provider.
- The facade's `fetchCameraDiscovery` **blocks** (starts a job and waits);
  it is for worker-thread C++ callers only and is documented as such.

## 7. Bench recipe (for reproducing §4)

```text
conan install . -of build-ninja --build=missing -r conancenter -s build_type=Release -c tools.cmake.cmaketoolchain:generator=Ninja
# VS 2022 x64 developer shell, MIB_MINDVISION_SDK_ROOT=<repo>/build/vendor/mindvision-sdk/extracted/Demo/VC++
cmake --preset windows-ninja && cmake --build --preset windows-ninja-build
ctest --preset windows-ninja-test -j 4
ctest --test-dir build-ninja -L integration -LE soak
python tools/gen_bridge_link_manifest_ninja.py
set MIB_BRIDGE_NO_CMAKE=1 && set MIB_BRIDGE_LINK_MANIFEST=<repo>\build-ninja\mib-bridge-link-manifest.json && set PATH=<repo>\build-ninja\Release;%PATH%
cd crates\mib-bridge && cargo test --release
cd desktop && npm ci && npx tsc --noEmit && npx vitest run
python scripts/gen_bridge_contract.py --check && python scripts/check_docs.py
```

The `team-conan` remote prompts for credentials; `-r conancenter` avoids it
(every package is already in the local cache). ctest `-R` patterns with `|`
do not survive `cmd.exe`; use `-L` labels.

## 8. Prompt for the next agent

Copy verbatim into a fresh session in `C:\Users\ERBG07\Developer\mib-studio-qt`
(or any clone with the branch):

```text
You are continuing issue #419 of gavinlouuu-kpt/mib-studio-qt ("Centralize
device discovery in a backend service with device-specific providers").
The implementation is complete and verified locally on branch
feat/device-discovery-service (8 commits on top of develop@208b0d2, working
tree clean, not yet pushed). Read, in this order:
AGENTS.md; docs/exec-plans/active/2026-09-16-device-discovery-handoff-419.md
(this handover); docs/exec-plans/active/2026-09-15-device-discovery-service.md;
docs/decisions/0005-device-discovery-service.md;
knowledge_map/services/DeviceDiscoveryService.md.

Your job, in order:
1. Push the branch and open a PR against develop titled "Centralize device
   discovery in a backend service with device-specific providers (#419)";
   body: summary from the handover §2, verification table §4, the explicit
   "not verified" list §5, links to ADR 0005 and the plan. Do not squash or
   rewrite the existing commits.
2. Watch every CI lane (backend-ci, sanitizers TSan + ASan/UBSan, docs-ci,
   desktop/bridge, Windows packaging). Fix failures on the same branch with
   regression-first tests; the code under test is src/backend/discovery/*,
   src/frontend/system/DeviceInitManager.cpp, the tabs, BackendFacade,
   crates/mib-bridge, desktop/src/discovery.ts. Keep the vault notes in
   sync in the same commits and run python scripts/check_docs.py.
3. If a rig is available, run the hardware acceptance matrix in handover §5
   item 3 and record it in the plan's Progress section (name unavailable
   hardware explicitly; claim nothing untested). Then set the plan
   Status: completed, git mv it to docs/exec-plans/completed/, update
   TD-11 in docs/exec-plans/tech-debt-tracker.md, add a Recent-Work line.
4. Report: files changed, commands run with results, anything left open.

Rules: the issue body is the specification; providers own SDK/protocol
knowledge, the service owns lifecycle, the policy decides, existing services
connect. Never auto-connect from partial/cancelled/failed/overflowed scans.
No probe may write, move, enable or capture. Preserve the #417 shutdown
order (policy stop → discovery drain → hardware release). Use spdlog.
Headers mirror src. Bench build recipe: handover §7.
```
