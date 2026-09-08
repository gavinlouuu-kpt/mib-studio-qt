Title: Make missing MindVision build support visible (issue #338)

Context:
- Issue #338: "software can't find camera hardware mindvision". Root cause
  analysis showed official CI builds configure `-DMIB_ENABLE_MINDVISION=OFF`
  (hosted runners lack the proprietary SDK), so every shipped installer has
  MindVision compiled out. Discovery then returns empty and the UI said only
  "No cameras found." — indistinguishable from unplugged hardware. Worse,
  `AppBackend::setMindVisionCameraSelection` silently swapped in the mock
  camera, so a user could believe they were recording hardware frames.

Implementation Notes:
- [[../services/CameraControlService]]: new statics `eGrabberSupported()` /
  `mindVisionSupported()` expose compile-time SDK availability to UIs.
- [[../architecture/AppBackend]]: `setMindVisionCameraSelection` now returns
  `bool` + `errorOut` and fails with an actionable message
  ("compiled with MIB_ENABLE_MINDVISION=OFF ... install a build with the SDK
  enabled") instead of the silent mock fallback; selection state untouched on
  failure. The startup-profile path keeps the mock fallback (app must stay
  usable) but logs an ERROR naming the build flag.
- Callers updated to surface the failure: [[../frontend/ConnectTab]] (warning
  dialog + status label, both the Connect button and auto-select paths),
  `DeviceInitManager` (init summary message), `BackendFacade`
  (`BackendErrorEvent` + failed command result).
- [[../frontend/ConnectTab]] discovery UX: refresh status line and
  `reportNoCameras()` append "this build does not include X support" note;
  the MindVision list shows a non-selectable placeholder in unsupported
  builds; `DeviceInitManager`'s "No cameras found." message carries the same
  note.
- Tests: `tests/backend/camera_control_support_test.cpp`
  (`backend.camera_control_support`) pins the unsupported-build contract:
  queryable support flags, empty (not failing) discovery, actionable errors
  from mutating operations. CI builds with both SDKs off exercise exactly
  these branches.

Follow-ups:
- Decide how to ship MindVision support in official releases (self-hosted
  runner or redistributable SDK step) — tracked in issue #338.
