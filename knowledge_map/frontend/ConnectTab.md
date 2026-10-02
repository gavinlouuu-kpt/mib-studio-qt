# ConnectTab

> First tab. User picks a hardware device, a MindVision device, or a mock
> folder, then clicks Start.

**Source:** `src/frontend/tabs/ConnectTab.cpp`,
`include/frontend/tabs/ConnectTab.h`
**Related:** [[../services/CameraControlService]],
[[System-Utilities]] (`DeviceInitManager`),
[[Dialogs]] (`MockConfigDialog`),
[[../camera/_MOC]]

## Responsibility

- List framegrabbers / eGrabber cameras / MindVision cameras from a
  [[../services/DeviceDiscoveryService]] snapshot (`showDiscoveryResults`).
  The constructor and **Refresh** start an asynchronous camera+framegrabber
  job (`onRefresh`, origin `connect-tab`) and render its result through a
  `DiscoverySubscription`; repeated Refresh coalesces onto the running job.
  The tab never enumerates hardware itself (#419).
- Offer a "Mock camera" entry; opens `MockConfigDialog` to pick folder +
  interval + loop.
- Offer a dedicated MindVision tab so the user can select a MindVision device
  without mixing it into the EGrabber tree.
- Surface health indicators (model name, firmware, interface/device labels).
- Hand the chosen selection to `AppBackend::setHardwareCameraSelection`
  `AppBackend::setMindVisionCameraSelection`, or
  `AppBackend::configureMockCamera`.
- Own the **Delivery mode** combo (`deliveryModeCombo`, index 0 = Every Frame,
  1 = Latest Frame). A change applies a
  `CaptureService::Config` (buffer sizing left at service defaults, only
  `deliveryMode` set) via `CaptureService::setConfig` and emits
  `deliveryModeChanged(FrameDeliveryMode)`, which [[MainWindow]] wires into
  `AppConfigWatcher::writeBackCameraConfig` (per-profile persistence) and the
  status-bar badge. `setDeliveryMode()` drives the same path programmatically
  (used by the experiment safeguard); `syncDeliveryMode()` only updates the
  combo (signal-blocked) when a config load applied the mode already.

## Gotchas

- Device enumeration never runs on the UI thread: jobs run on
  [[../services/DeviceDiscoveryService]] workers; `tryAutoConnect()` only
  delegates to `DeviceInitManager` (see [[System-Utilities]]) and does
  nothing without one (the pre-#419 synchronous fallback is gone).
- A snapshot is rendered once per job id (`lastRenderedJob_`): the startup
  job reaches the tab both through `DeviceInitManager` (which also applies
  the selection status) and through the tab's own subscription.
- An incomplete snapshot names the first coverage gap in the status
  (busy SDK while capturing, timeout, ...); a compiled-out SDK is not shown.
- `MIB_CAMERA_MODE=mock` env var forces mock selection without the dialog
  (see [[../build-and-run/Run-Modes]]).
- `MIB_CAMERA_MODE=mindvision` selects the MindVision path on startup when the
  SDK is available; `MIB_MINDVISION_CAMERA_INDEX` and `MIB_MINDVISION_CONFIG`
  refine the startup selection.
- Changing the delivery mode while capture is running never restarts the
  camera; the status label tells the user the mode applies at the next
  capture start (real backends only honor it in `start()`).
