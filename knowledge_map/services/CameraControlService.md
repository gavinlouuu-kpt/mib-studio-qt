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
  **Never runs in a process that has an EGrabber framegrabber** (see Gotchas);
  it then logs one INFO line and returns an empty list.
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

- **MindVision enumeration locks out EGrabber for the whole process.** The
  MindVision SDK's `CameraEnumerateDevice()` (not `CameraSdkInit`) leaves the
  Euresys GenTL producer unopenable: every later `EGenTL` construction fails
  with `GenTL error -1004, GCInitLib: Requested resource is already in use`,
  regardless of thread or call order, and nothing in the MindVision API
  reverses it. Verified 2026-09-08 on the Coaxlink Quad CXP-12 bench with
  MindVision SDK 2.1.10 and eGrabber 25.10. `discoverMindVisionCameras()`
  therefore decides once per process (`discoverFramegrabbers()` non-empty →
  blocked) and skips enumeration. `MIB_MINDVISION_ENUMERATE_WITH_EGRABBER=1`
  forces the old behaviour for future SDK versions. Regression guard:
  `hardware.discovery_reentry` (`tests/hardware/hw_discovery_reentry_test.cpp`)
  replays the boot sequence (ConnectTab refresh → DeviceInitManager worker).
  A hybrid EGrabber + MindVision bench cannot work in one process until the
  vendor SDK stops doing this.
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
