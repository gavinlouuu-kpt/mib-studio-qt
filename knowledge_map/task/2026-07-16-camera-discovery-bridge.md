Title: Camera discovery, selection, and status bridge (BE-2, issue #272)

Context:
- Exposes camera enumeration and the existing facade selection actions
  through the bridge (epic #246). Bridge schema **v7** (additive). Hardware
  (EGrabber/MindVision) acceptance on Windows remains open on the issue.

Implementation Notes:
- **AppBackend** — new `CameraSelectionSnapshot` (`cameraSelection()`):
  authoritative mode (None/Mock/Hardware/MindVision), indices, label,
  MindVision config path, last applied camera-script path
  (`lastCameraScriptPath_`, recorded on successful apply), and the active
  mock parameters (recorded by both `configureMockCamera` and the
  initialize-time mock fallback). Values survive capture start/stop.
- **Facade** — `fetchCameraDiscovery` (typed
  `BackendDiscoveredCamera`/`BackendDiscoveredFramegrabber` over
  `CameraControlService::discoverAllCameras/discoverFramegrabbers`, plus a
  synthetic **Mock** entry, type 2, so discovery/selection is
  headless-testable) and `fetchCameraSelection` (snapshot + live `running`).
  `SelectHardwareCamera`/`SelectMindVisionCamera` now reject negative indices
  with structured errors before touching state.
- **Bridge/Tauri/TS** — `fetch_camera_discovery`, `fetch_camera_selection`,
  `select_hardware_camera`, `select_mindvision_camera`,
  `apply_camera_script`, `reset_hardware_camera`. Contract gains
  `camera_types` (EGrabber/MindVision/Mock) and `camera_selection_modes`
  (pinned by shim static_asserts).
- **Shell** — the Connect tab now lists real discovery results per source
  tab with Refresh/Connect, the authoritative "Found N…/selected:" line, and
  the mock entry routes to the Configure Mock modal; Start Camera gates on
  the backend's `configured` flag instead of local UI state.

Verification:
- `mib-bridge cargo test camera_discovery_and_selection_contract`: mock entry
  always present, boot-fallback selection is authoritative (no hardware claim
  without SDKs), structured errors for invalid indices / missing script /
  reset-without-selection, mock parameters round-trip, selection survives
  capture start/stop with a live `running` flag.
- Full backend CTest lane, desktop cargo tests, `npm run build`,
  `gen_bridge_contract.py --check`, `check_docs.py` green.

Follow-ups:
- Device-lost transitions and starting/running/stopped status metrics from
  real hardware, plus EGrabber/MindVision Windows validation, are the
  hardware half of #272.
- Camera-script/MindVision-config editing UI is UI-2 (#267).
