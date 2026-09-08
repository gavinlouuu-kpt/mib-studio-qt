# CameraControlService

> Device discovery and one-shot parameter application for the hardware camera
> backends. Does **not** own the acquisition thread - that's [[CaptureService]].

**Source:** `src/backend/services/CameraControlService.cpp`,
`include/backend/services/CameraControlService.h`
**Related:** [[../camera/EGrabberCamera]], [[../camera/MindVisionCamera]],
[[CaptureService]], [[../frontend/ConnectTab]]

## Responsibility

- `discoverCameras()` / `discoverFramegrabbers()` — enumerate Euresys
  interfaces, devices, and streams. Returns `DiscoveredCamera` /
  `DiscoveredFramegrabber` with index triples and human labels.
- `discoverMindVisionCameras()` / `discoverAllCameras()` — enumerate
  MindVision devices and merge them with the EGrabber list for the connect UI.
  Reserves the shared Euresys GenTL handle before calling the MindVision SDK
  (see Gotchas).
- `applyMindVisionConfig(cameraIndex, configPath, errorOut)` — apply a JSON
  config to a selected MindVision device before capture starts. The parse +
  bounds validation is the shared pure `parseConfig` documented in
  [[../camera/MindVisionCamera]] (`MindVisionConfig.h`); the application also
  goes through the shared `applyConfigToHandle` (`MindVisionApply.cpp`), so
  this path now applies the full field set — strobe and acquisition-trigger
  extras included (the historical 7-field drift is closed).
- `applyScriptToDevice(ifIdx, devIdx, scriptPath, errorOut)` — push a
  GenICam JS file to a specific device.
- `deviceReset(ifIdx, devIdx, errorOut)` — issue SFNC `DeviceReset`;
  best-effort stops acquisition first.

## Threading

Synchronous, called from main thread.

## Platform behavior

- **Windows (`MIB_HAS_EGRABBER=1`)**: full EGrabber-backed discovery, script
  apply, and device reset.
- **Windows/Linux/macOS (`MIB_HAS_MINDVISION=1`)**: MindVision discovery and
  config application are enabled when the platform SDK is provisioned.
- **Disabled/processing-only SDK builds**: methods compile as safe fallbacks:
  discovery returns empty vectors; mutating operations return `false` and can
  populate `errorOut`.

## Gotchas

- **MindVision enumeration and the single GenTL handle.** Only one Euresys
  `EGenTL` may exist per process; a second construction fails with
  `GenTL error -1004, GCInitLib: Requested resource is already in use`. The
  MindVision SDK's CoaXPress plugin (`CXPCamera_X64.Interface`, loaded when
  the vendor SDK is installed and found via the `Industry Camera` registry
  key) opens `coaxlink.cti` itself during `CameraEnumerateDevice()`, so if it
  runs first every later EGrabber open in the process fails for good (seen on
  `v1.0.7-beta.fa6e6ba`: "No camera found" while the Connect tab listed the
  camera). All EGrabber code therefore shares one process-wide handle
  (`backend::camera::egrabber::sharedGenTL()`, `GenTLHolder.h`) and
  `discoverMindVisionCameras()` reserves it first; the plugin's attempt then
  fails harmlessly and both SDKs work in any order. Regression guard:
  `hardware.discovery_reentry` (`tests/hardware/hw_discovery_reentry_test.cpp`)
  with modes for thread, MindVision step, and MindVision-first order.
- Applying a script **stops capture** as a side effect (done in
  [[../architecture/AppBackend]]). Capture does not auto-restart.
- Mock cameras are not discovered here — they're configured via
  `AppBackend::configureMockCamera`.
- MindVision selection is handled separately from EGrabber selection in
  [[../frontend/ConnectTab]] and `AppBackend`.
- On Windows, MindVision SDK loader symbols are defined by
  [[../camera/MindVisionCamera]]; this service includes `CameraApiLoad.h`
  without `API_LOAD_MAIN`. Linux/macOS include `CameraApi.h` and call the
  linked shared library directly.
- See `docs/howto/external-config-and-camera-script.md` for script format
  and `docs/integration/egrabber.md` for SDK notes.
