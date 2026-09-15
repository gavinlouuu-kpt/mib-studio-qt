# Run Modes

> Two executables, three camera sources, a handful of env vars.

**Related:** [[../camera/MockCamera]], [[../camera/MindVisionCamera]],
[[../frontend/ConnectTab]], [[../frontend/Screenshot-Tour]]

## Executables

- **`mib_studio_qt.exe`** — the app. Hardware camera via
  [[../camera/EGrabberCamera]] or MindVision; the mock camera is reachable
  from [[../frontend/ConnectTab]] ("Configure Mock…") or forced via
  `MIB_CAMERA_MODE=mock`. (The former separate `mock_studio_qt` target was
  removed — mock support lives in the production binary.)
  Desktop builds compile the MindVision provider by default on Windows,
  Linux, and macOS. Official Windows binaries compile both EGrabber and MindVision;
  `MVCAMSDK_X64.dll` ships beside the executable, while the workstation still
  needs the vendor device driver installed.
- **`screenshot_tour.exe`** — headless mock-mode UI tour that regenerates
  the user-manual screenshots. See [[../frontend/Screenshot-Tour]].

## Mock camera env vars

Read at startup (see `main.cpp` and [[../architecture/AppBackend]]):

| Variable | Default | Effect |
|---|---|---|
| `MIB_CAMERA_MODE=mock` | unset | Force mock (bypasses ConnectTab) |
| `MIB_CAMERA_MODE=mindvision` | unset | Force MindVision selection (uses `MIB_MINDVISION_CAMERA_INDEX` / `MIB_MINDVISION_CONFIG` if present) |
| `MIB_MOCK_CAMERA_DIR=<path>` | — | Folder with PNG/TIFF/JPEG frames |
| `MIB_MOCK_CAMERA_INTERVAL_MS=<ms>` | 33 | Frame cadence |
| `MIB_MOCK_CAMERA_LOOP=true\|false` | true | Loop or stop at end |
| `MIB_MINDVISION_CAMERA_INDEX=<n>` | 0 | MindVision device index used by startup selection |
| `MIB_MINDVISION_CONFIG=<path>` | — | JSON config applied before MindVision capture starts |
| `MIB_DISABLED_SERVICES=<csv>` | unset | Disable startup paths (`sqlite,hdf5,processing,yolo,autofocus,trigger,capture/camera,playback,auto_update,all`) |

Sample frames ship at `data/mock_frames/frame_00000.tiff`.

The GUI Settings action **Boot Service Toggles...** stores a persisted
`QSettings` value (`Startup/DisabledServices`) that `main.cpp` maps into
`MIB_DISABLED_SERVICES` before backend startup (unless the env var is already set externally).
- The startup camera mode picker now recognizes `mock`, `mindvision`, and the
  hardware/eGrabber default path.

## MLflow (test metrics only)

Test performance scripts (`scripts/empty_frame_detection.py` and the
Kedro pipeline under `scripts/kedro_frame_detection/`) log to MLflow at
`mlflow.yofo.bio`. Set credentials via env vars
`MLFLOW_TRACKING_USERNAME`, `MLFLOW_TRACKING_PASSWORD` — never hard-code.
See top-level `AGENTS.md`.

## Backend-only validation mode (build/test)

For backend CI loops without Qt Widgets/Charts frontend binaries:

- Configure/build preset: `linux-backend-only`, `linux-backend-only-build`
- Test preset: `linux-backend-only-test`
- Backend tests can be selected by label:
  `ctest --preset linux-backend-only-test -L backend`

Full frontend builds also register `frontend.processing_core_dialog`. CTest
runs it with `QT_QPA_PLATFORM=offscreen`; its deliberately invalid registry
URL exercises the offline selector state without opening a window or reaching
the network.

## Troubleshooting

- `docs/howto/troubleshoot-crashes.md`
- `docs/howto/safe-start-stop-egrabber.md`
- `knowledge_map/task/qt_qpa_platform_plugin_missing_windows.md`
