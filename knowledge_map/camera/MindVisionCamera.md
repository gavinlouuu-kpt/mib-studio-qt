# MindVisionCamera

> MindVision SDK-backed `ICamera` implementation. Used when the user selects a
> MindVision device and the build was configured with `MIB_ENABLE_MINDVISION=ON`.

**Source:** `src/backend/camera/mindvision/MindVisionCamera.cpp`,
`include/backend/camera/mindvision/MindVisionCamera.h`,
`include/backend/camera/mindvision/MindVisionConfig.h` (pure JSON parse + bounds),
`include/backend/camera/mindvision/MindVisionApply.h` +
`src/backend/camera/mindvision/MindVisionApply.cpp` (shared SDK apply helper)
**Tests:** `tests/backend/mindvision_config_test.cpp`
**Related:** [[ICamera]], [[../services/CameraControlService]],
[[../services/PulseGeneratorService]], [[../architecture/AppBackend]],
[[../frontend/ConnectTab]], [[../frontend/ConfigTabs]]

## Responsibility

- Own the MindVision camera lifecycle: open, start, grab frame, stop, and
  release the SDK handle.
- Apply an optional JSON config file before capture starts (acquisition
  trigger, strobe, exposure, ROI, …).
- Expose trigger-output helpers used by the backend camera wiring (sort pulse)
  and `softTrigger()` for software acquisition triggering.

## Key APIs

- `MindVisionCamera(int cameraIndex, std::string configPath = {})`
- `applyConfig(const CameraConfig&)`
- `start()` / `stop()`
- `grabFrame(Frame&)`
- `pollStats(CameraStats&)`
- `deliveryCapabilities()` / `activeDeliveryMode()` /
  `pollAcquisitionQueueStats(AcquisitionQueueStats&)`
- `checkDeviceHealth()`
- `configureTriggerOutput(lineSelector)` / `setTriggerOutput(high)` — **sort
  output pulse** (TriggerService)
- `softTrigger()` — **software acquisition trigger** (`CameraSoftTrigger`);
  requires the camera running with `trigger_mode: 1`

## Acquisition trigger modes

`trigger_mode` selects how exposures start: `0` continuous (free-run, default),
`1` software (`softTrigger()` fires one exposure × `trigger_count`), `2`
external (a TTL edge on the camera trigger input starts each exposure — in
this system the edge comes from the Zhongsheng pulse-output module, see
[[../services/PulseGeneratorService]]). External-trigger shaping keys:
`ext_trig_signal_type` (0 falling / 1 rising / 2 high level / 3 low level /
4 double edge), `ext_trig_jitter_us` (de-glitch filter),
`acq_trigger_delay_us` (edge→exposure delay), `trigger_count` (frames per
trigger). Strobe (`strobe_mode` 0 auto-sync / 1 manual delay+width / 2 always
high / 3 always low, plus pulse width/delay/polarity) fires per exposure for
illumination sync.

Under trigger modes 1/2, `checkDeviceHealth()` **skips the frame probe**: the
probe (a 100 ms `CameraGetImageBuffer`) would consume a real triggered frame,
and a timeout is the normal idle state so it carries no health signal. It
returns true on a valid running handle instead. (`CameraConnectTest` would be
the proper probe but its presence in every deployed Windows
`CameraApiLoad.h` function table is unverified.)

## Frame delivery modes (issue #330)

Capabilities: `supportsEveryFrame=true`, `supportsLatestFrame=true`,
`modeChangeRequiresRestart=true`, `timestampsHostComparable=false`
(`frameHead.uiTimeStamp` is a device tick counter, not the host clock).

- **EveryFrame** (default): unchanged ordered `CameraGetImageBuffer`
  retrieval; frames are never intentionally skipped. The priority API is
  deliberately never used in this mode.
- **LatestFrame**: stale completed SDK buffers are dropped before the
  expensive `CameraImageProcess` copy so `grabFrame()` returns the freshest
  complete image. Two implementations, selected at compile time:
  - **SDK priority path** — when `CAMERA_GET_IMAGE_PRIORITY_NEWEST` is
    visible to the preprocessor (`#ifdef`), or the build passes
    `-DMIB_MINDVISION_USE_PRIORITY_API=1`, `grabFrame` calls
    `CameraGetImageBufferPriority(h, &head, &buf, 100,
    CAMERA_GET_IMAGE_PRIORITY_NEWEST)` and the SDK itself discards older
    completed buffers. **Limitation:** the SDK gives no per-skip callback, so
    `intentionallyDiscardedFrames` cannot count these SDK-internal skips
    exactly on this path.
  - **Bounded drain fallback** — otherwise: blocking
    `CameraGetImageBuffer(..., 100)` first, then at most
    `config_.numBuffers` non-blocking (`0` timeout) fetches; each newer
    completed buffer supersedes the held one, which is released immediately
    and counted. This path counts every intentional discard **exactly**.
  - Note: every public SDK mirror observed (2026-08) declares the priority
    constants as a plain C `enum emCameraGetImagePriority` in
    `CameraDefine.h`, which `#ifdef` cannot see — so on those SDKs the drain
    fallback is what actually compiles in unless the build opts in with
    `MIB_MINDVISION_USE_PRIORITY_API=1` after verifying the SDK ships
    `CameraGetImageBufferPriority`.

**Release-exactly-once invariant:** on every path through `grabFrame` —
priority, drain, EveryFrame, stop-while-holding, and `CameraImageProcess`
failure — each buffer acquired from the SDK is passed to
`CameraReleaseImageBuffer` exactly once. The drain loop holds exactly one
buffer at any moment (release old, then adopt new). Audit this whenever the
grab path changes.

Mode changes require a full `stop()` → `applyConfig()` → `start()` cycle;
the active SDK queue is never cleared or reordered mid-run
(no `CameraClearBuffer` on a running camera). `activeDeliveryMode()` reports
the mode confirmed at the most recent successful `start()`; before any start
it reports `config_.deliveryMode`.

## Acquisition-queue telemetry (issues #330/#333)

`pollAcquisitionQueueStats` returns `true` while running and fills:

- `deliveredFrames` — `frameCount_` (reset at `start()`).
- `intentionallyDiscardedFrames` — atomic counter (reset at `start()`;
  exact on the drain path, see limitation above for the priority path).
- `transportLostFrames` — `tSdkFrameStatistic::iLost` via
  `CameraGetFrameStatistic` (present in all known SDK variants, including the
  dynamic-loader `CameraApiLoad.h`), with `transportLossValid=true` when the
  call succeeds.
- **Explicitly unavailable** (left `0` with valid flags `false`, per #333):
  `sdkCompletedQueueDepth`, `sdkInputBufferCount`, `bufferUnderruns` — the
  MindVision SDK exposes no queue-depth or underrun observability.

## Platform behavior

- **Windows, Linux, macOS (default `MIB_ENABLE_MINDVISION=ON`)**: real
  SDK-backed implementation. Windows uses the SDK's `CameraApiLoad.h`
  function table; Linux/macOS include `CameraApi.h` and link the platform
  shared library directly.
- **`MIB_ENABLE_MINDVISION=OFF` or processing-only builds**: safe stub that
  logs unsupported calls and returns failure/no-op.

## JSON config parsing (`MindVisionConfig.h`) and applying (`MindVisionApply.h`)

The config-file parse + validation is a pure, QtCore-only function shared by
both `MindVisionCamera::applyJsonConfig` and
[[../services/CameraControlService]]`::applyMindVisionJsonToCamera`:

- `backend::camera::mindvision::parseConfig(QByteArray)` → `ParseResult{ok,
  config, error, warnings}`. Malformed JSON or a non-object root → `ok=false`
  with `error`. Otherwise every numeric field is clamped to a safe range and one
  warning is recorded per clamp.
- Critical clamps: `width`/`height` forced `>= 1`, `exposure_time_us > 0`,
  `strobe_pulse_width_us` / `strobe_delay_us` / `ext_trig_jitter_us` /
  `acq_trigger_delay_us` forced `>= 0` (negative values wrapped to huge
  unsigned SDK durations), `trigger_count >= 1` (0 silently captures nothing).

The **application** of a parsed config is also shared now:
`backend::camera::mindvision::applyConfigToHandle(hCamera, cfg, firstError)`
(`MindVisionApply.cpp`) pushes every field into the SDK. Both call sites route
through it, which closed the historical 19-vs-7-field drift — strobe and
trigger extras now apply on the control-service path too. It must run before
`CameraPlay` on the streaming path; `firstError` reports only a
`CameraSetImageResolution` failure (the control service's historical error
contract). `MindVisionApply.cpp` includes the SDK header **without**
`API_LOAD_MAIN`.

A documented sample config ships at `resources/defaults/mindvisionConfig.json`
(the GUI seeds a user-writable copy). It carries the original planned
MV-XGC51GM (10GigE, IMX426) 5000 fps starting point: external trigger,
512×96 ROI (5000 fps is only valid at this reduced ROI), 1 µs exposure, and
manual strobe mode with 10 µs delay / 35 µs width. The 2026-09-07 hardware
validation below found that this exact trigger type and illumination timing do
not work on the current R5D bench; apply the runbook overrides before testing.
Soft-trigger variant: change `trigger_mode` to 1; free-run: 0 (code defaults
for absent keys remain free-run/conservative).

### MV-XG51GM + R5D bench validation (2026-09-07)

The Linux bench required `trigger_mode: 2` with high-level
`ext_trig_signal_type: 2`; signal types 0 and 1 returned no frames. Physical
OUT1 is SDK output index 0 and drives the R5D constant-current LED driver.
At 5 kHz, a 35 µs strobe command produced no reliable current pulse; commands
>=50 µs followed `measured ≈ 0.9969 * commanded - 31.75 µs`. An auto-strobe
exposure sweep inferred exposure start at approximately 47 µs after the
external trigger. The former 20 µs exposure ended before LED current began;
100 µs exposure with a 100 µs strobe command provides about 67 µs overlap.

Use the complete wiring, Rigol gating procedure, calibration data, exposure
overlay, and safety notes in
[`docs/howto/mindvision-xgc-r5d-led-strobe.md`](../../docs/howto/mindvision-xgc-r5d-led-strobe.md).

## Gotchas

- The SDK is discovered at configure time through `MIB_MINDVISION_SDK_ROOT`
  or the `MIB_MINDVISION_SDK_DIR` environment variable; runtime lookup can be
  separated with `MIB_MINDVISION_RUNTIME_DIR`.
- Every pinned platform SDK declares both `CameraGetImageBufferPriority` and
  `CAMERA_GET_IMAGE_PRIORITY_NEWEST`; SDK-backed builds therefore define
  `MIB_MINDVISION_USE_PRIORITY_API=1` and use native newest-buffer retrieval.
- The implementation accepts namespaced or flat SDK include layouts. Windows
  looks for `MindVision/CameraApiLoad.h` / `CameraApiLoad.h`; Linux/macOS look
  for `MindVision/CameraApi.h` / `CameraApi.h`.
- On Windows, `MindVisionCamera.cpp` owns the SDK dynamic-loader definitions
  by defining `API_LOAD_MAIN`; other MindVision users include the loader as
  extern declarations. Unix builds have ordinary linked C functions and do
  not define a loader table.
- `grabFrame` copies out of `outBuffer_` **while holding `stateMutex_`** —
  `stop()` frees the buffer under the same lock, so the copy must not be moved
  outside the locked region (use-after-free on stop/start churn).
- `CameraSetIspOutFormat(MONO8)` is applied unconditionally: `outBuffer_` is
  sized 1 byte/px and the pipeline is mono8-only, so a color sensor left at
  the ISP's 3-byte default would overrun the buffer.
- Runtime deployment copies the MindVision DLL next to the app when Windows
  packaging is enabled. Linux build RPATH resolves the provisioned `.so`; the
  macOS provisioner converts the dylib install name to `@rpath` and re-signs it.
- The current backend treats MindVision as a separate camera provider; mock
  and EGrabber paths remain independent.
- Naming: "trigger output" / `TriggerService` vocabulary means the **sort
  pulse** (camera → sorter). The acquisition trigger (pulse generator →
  camera) uses `softTrigger` / `acq_trigger_*` / `ext_trig_*` naming.
