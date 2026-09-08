# MindVisionCamera

> MindVision SDK-backed `ICamera` implementation. Used when the user selects a
> MindVision device and the build was configured with `MIB_ENABLE_MINDVISION=ON`.

**Source:** `src/backend/camera/mindvision/MindVisionCamera.cpp` (SDK-header-free,
all vendor calls through the seam),
`include/backend/camera/mindvision/MindVisionCamera.h`,
`include/backend/camera/mindvision/MindVisionSdk.h` (injectable `SdkOps` seam),
`src/backend/camera/mindvision/MindVisionSdkReal.cpp` (real MVSDK binding; owns
`API_LOAD_MAIN` on Windows),
`include/backend/camera/mindvision/MindVisionFrameGeometry.h` (pure checked
format/geometry validation),
`include/backend/camera/mindvision/MindVisionConfig.h` (pure JSON parse + bounds),
`include/backend/camera/mindvision/MindVisionApply.h` +
`src/backend/camera/mindvision/MindVisionApply.cpp` (shared SDK apply helper)
**Tests:** `tests/backend/mindvision_config_test.cpp`,
`tests/backend/mindvision_conversion_fault_test.cpp`
(`backend.mindvision_conversion_fault`, fake SDK — no hardware)
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

## Fail-closed Mono8/geometry contract (issue #366)

`start()` proves the destination buffer is large enough for what the SDK will
write **before** any conversion can run, and refuses to stream otherwise:

1. `CameraSetIspOutFormat(MONO8)` must succeed — failure is a hard start
   failure (`mindvision.isp_format_rejected`), never "warn and continue".
2. `CameraGetIspOutFormat` must succeed and read back Mono8
   (`mindvision.isp_format_unverified` / `mindvision.geometry.unsupportedFormat`
   for RGB8/BGR8/Mono16/anything else). Relied-upon SDK guarantee: the
   readback is the format `CameraImageProcess` writes.
3. `validateSessionGeometry(width, height, effectiveFormat)`
   (`MindVisionFrameGeometry.h`) rejects `<= 0` / `> 65535` dimensions and
   uses checked `width*height*bpp` arithmetic (`checkedFrameBytes`) that also
   caps at `INT_MAX` for `CameraAlignMalloc`.
4. The buffer is allocated to exactly the validated `requiredBytes`; the
   validated `SessionGeometry` is retained (`sessionGeometry()`).
5. Every incoming frame header is checked with `validateIncomingFrame`
   against the session allocation **before** `CameraImageProcess`. A
   mismatch (larger *or* smaller, or a post-process header that disagrees)
   releases the buffer, counts `geometryRejectedFrames()`, and **faults the
   stream** (`isRunning()` → false, `lastFailure().code =
   mindvision.frame.frameGeometryMismatch`). [[../services/CaptureService]]
   reports it as `StreamEnded` with the message; the buffer is never resized
   under the SDK. Recovery is a controlled `stop()` → `start()` which
   re-validates.

Every failure is a structured `CameraFailure{code, message}` via
`lastFailure()` (codes are listed in `MindVisionCamera.cpp`); there is no
automatic fallback to a color/unknown output presented as Mono8. Without the
SDK compiled in, `start()` fails with `mindvision.sdk_unavailable`.

## In-flight SDK operation boundary (issue #365)

`grabFrame` wraps each SDK retrieval in an `InFlightOp` (counter under
`stateMutex_`, taken only while running). `stop()` clears `running_`, calls
`CameraStop` (which returns a blocked `CameraGetImageBuffer`), then waits on
`inFlightCv_` for the count to reach zero — bounded by
`setInFlightDrainTimeout` (5 s default; the SDK retrieval itself times out at
100 ms) — before freeing the buffer and `CameraUnInit`. If the drain times
out (wedged driver) the handle and buffer are **abandoned** (leaked) and
`mindvision.inflight_drain_timeout` is recorded rather than uninitializing a
handle under a live call. `setTriggerOutput`/`softTrigger`/stats calls take
`stateMutex_` and check `running_ && hCamera_ >= 0`, so they cannot reach a
closed handle either.

## Timestamps (issue #368)

`tSdkFrameHead::uiTimeStamp` is a 32-bit device capture counter in 0.1 ms
ticks; `grabFrame` stores `Frame::timestamp = ticks × 100000` (ns) and keeps
the native value in `Frame::rawDeviceTicks`. `timestampDescriptor()` reports
`deviceTicks @1e9 Hz (native 10000 Hz), deviceCapture, valid, 32-bit`. The
device clock is not comparable to `Tools::getTimestamp()`
(`timestampsHostComparable=false`), so frame age is `Unsupported` for this
backend.

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
(the GUI seeds a user-writable copy). Its defaults ARE the bench setup for the
MV-XGC51GM (10GigE, IMX426): **external trigger at 5000 fps** — the Zhongsheng
pulse generator on TRIG_IN determines the frame rate — with 512×96 ROI (the
5000 fps figure is only valid at this reduced ROI), `exposure_time_us: 1.0`
(sensor minimum is 0.8 µs), and strobe in semi-auto/manual mode
(`strobe_mode: 1`) with `strobe_delay_us: 10`, `strobe_pulse_width_us: 35`
driving the LED driver from STRB_OUT. Soft-trigger bench variant: change
`trigger_mode` to 1; free-run: 0 (code defaults for absent keys remain
free-run/conservative — only the shipped file carries the bench values).

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
- On Windows, `MindVisionSdkReal.cpp` owns the SDK dynamic-loader definitions
  by defining `API_LOAD_MAIN`; other MindVision users include the loader as
  extern declarations. Unix builds have ordinary linked C functions and do
  not define a loader table.
- `grabFrame` copies out of `outBuffer_` **while holding `stateMutex_`** —
  `stop()` frees the buffer under the same lock, so the copy must not be moved
  outside the locked region (use-after-free on stop/start churn).
- `CameraSetIspOutFormat(MONO8)` + readback is a hard gate (see the
  fail-closed contract above): `outBuffer_` is sized 1 byte/px and the
  pipeline is mono8-only, so a color sensor left at the ISP's 3-byte default
  would overrun the buffer.
- `MindVisionCamera.cpp` must stay free of vendor headers
  (`scripts/test_mindvision_release_gate.py` checks the SDK include lives in
  `MindVisionSdkReal.cpp`); inject a `SdkOps` table through the constructor
  to test any SDK-facing behavior (`tests/support/fake_mindvision_sdk.h`).
- Runtime deployment copies the MindVision DLL next to the app when Windows
  packaging is enabled. Linux build RPATH resolves the provisioned `.so`; the
  macOS provisioner converts the dylib install name to `@rpath` and re-signs it.
- The current backend treats MindVision as a separate camera provider; mock
  and EGrabber paths remain independent.
- Naming: "trigger output" / `TriggerService` vocabulary means the **sort
  pulse** (camera → sorter). The acquisition trigger (pulse generator →
  camera) uses `softTrigger` / `acq_trigger_*` / `ext_trig_*` naming.

## Abandoned handle and its buffer (2026-09-08)

When `stop()` cannot drain an in-flight SDK call within
`inFlightDrainTimeout_` it abandons the handle (never `CameraUnInit` under a
live call) and parks the ISP output buffer in `abandonedBuffer_`; the
destructor frees it once `inFlightOps_` is zero (the wedged call returned),
otherwise it stays leaked for good. LeakSanitizer flagged the unconditional
leak in `backend.mindvision_conversion_fault` ("wedged driver").

