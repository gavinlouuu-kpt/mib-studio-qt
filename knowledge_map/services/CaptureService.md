# CaptureService

> Owns the acquisition thread and the **single host-camera acquisition
> session lifecycle** (issue #365). Calls `camera->grabFrame()` in a loop and
> pushes each frame into [[../data-model/FrameStore]].

**Source:** `src/backend/services/CaptureService.cpp`,
`include/backend/services/CaptureService.h`,
`include/backend/services/CaptureLifecycle.h` (Qt-free lifecycle types)
**Tests:** `tests/backend/capture_lifecycle_test.cpp`
(`backend.capture_lifecycle`), `tests/backend/trigger_session_test.cpp`,
`tests/integration/e2e_pipeline_stress_test.cpp`
**Related:** [[../camera/ICamera]], [[../data-model/FrameStore]],
[[ProcessingService]], [[TriggerService]]

## Responsibility

- One thread per service: `run(generation)` blocks on the camera's blocking
  `grabFrame`.
- **Lifecycle owner.** Exactly one session at a time, tagged with a
  monotonically increasing *generation*. State machine
  (`CaptureLifecycleState`): `Idle → Starting → Running → Stopping → Idle`,
  plus `Faulted` when the worker exits on its own (factory/camera start
  failure, health-check loss, stream ended, exception). `Faulted` keeps the
  worker thread joinable until the next `requestStart()`/`stop()`/destructor
  **reaps** it — a direct restart after a natural failure can therefore never
  assign over a joinable `std::thread` (`std::terminate`).
- Stamps `Frame::hostTimestampUs` (host monotonic µs, `Tools::getTimestamp`)
  the moment `grabFrame` returns — the acquisition anchor for all
  downstream latency measurement ([[../diagnostics/PipelineTimingRecorder]]).
  The camera's own `timestamp` is a device tick on a different clock.
- Copies frame bytes into `FrameStore` (including the host stamp) and fires
  `FrameCallback` (used by UI for live preview).
- Exposes `CaptureStats` — `framesProcessed`, `lastFrameRate`,
  `lastDataRateMBps` (the latter two come from EGrabber StreamModule), plus
  delivery-mode and acquisition-queue telemetry: requested vs
  backend-confirmed mode (`deliveryModeConfirmed`), intentional discards,
  transport loss, underruns, SDK queue depths, `lastFrameAgeUs` (only when
  the backend reports host-comparable timestamps), and
  `lastPublishLatencyUs` (dequeue → post-publish copy duration).
- Owns the delivery-mode handshake: pre-checks the requested
  `FrameDeliveryMode` against `deliveryCapabilities()` (unsupported mode →
  `CaptureFailureKind::UnsupportedDeliveryMode`, capture never starts),
  forwards the mode via `CameraConfig`, records the backend-confirmed mode
  after `start()`, and warns (rate-limited to the 1 s stats poll) when an
  EveryFrame backlog is growing. `activeDeliveryMode()` is what the UI badge
  should display.

## Key APIs

```cpp
void setConfig(const Config& cfg);              // bufferPartCount, numBuffers, deliveryMode
camera::common::FrameDeliveryMode activeDeliveryMode(); // backend-confirmed
void setFrameCallback(FrameCallback cb);        // UI live preview hook
void setFrameStore(shared_ptr<FrameStore>);     // ring buffer sink
void setCameraFactory(CameraFactory);           // injects ICamera builder
void setCameraReadyCallback(CameraReadyCallback); // (ICamera*, generation): ptr on start, nullptr on stop
bool start();  void stop();  bool isRunning();   // compatibility API
CaptureStartOutcome requestStart();             // Accepted | AlreadyActive | RejectedStopping | RejectedNoFactory
CaptureLifecycleSnapshot lifecycleSnapshot();   // authoritative state/generation/cameraReady/lastFailure
CaptureLifecycleState waitForState({...}, timeout);
bool softTriggerActiveCamera();  // one software ACQUISITION trigger on the
                                 // live camera (not the sort pulse)
```

**`start()` returning true is request acceptance, not hardware readiness.**
`lifecycleSnapshot().cameraReady` (true only between a successful
`camera->start()` and that camera's release in the same generation) and
`state == Running` are the readiness truth; UI controllers and experiment
preflight must consume the snapshot instead of inferring from `start()`.
`lastFailure` / `lastFailureMessage` / `lastFailureGeneration` carry the
structured reason (including the camera's own `ICamera::lastFailure()`
message, e.g. a MindVision Mono8 rejection) and survive an explicit `stop()`
so the UI can still explain why the previous session ended; a successful
camera start clears them.

Camera factory is how `AppBackend` chooses between
[[../camera/EGrabberCamera]], [[../camera/MindVisionCamera]] and
[[../camera/MockCamera]] without this service knowing which one.

`setConfig` has its single call site in
`frontend::AppConfigWatcher::loadAndApplyFromPath` (fed by the
`camera.frame_delivery_mode` profile key and the
[[../frontend/ConnectTab]] delivery-mode combo).
`deliveryModeConfirmed` is cleared when the capture loop releases the
camera, so `activeDeliveryMode()` falls back to the requested config mode
between runs.

## Threading

Dedicated thread. `grabFrame` blocks until a frame is available or `stop()`
returns false. `lifecycleMutex_` serializes `requestStart()`/`stop()` and
guards the snapshot + `thread_`; the worker takes it only for short
transitions, never while blocked in `grabFrame`. `stop()` joins outside every
lock. On stop, this thread is joined before [[ProcessingService]]'s realtime
loop shuts down.

Teardown order inside `stop()` (issue #365): publish `Stopping` → fire
`cameraReadyCallback_(nullptr, gen)` (so [[TriggerService]] unbinds while the
camera is still valid) → `activeCamera_->stop()` under `cameraMutex_` → join
the worker → publish `Idle`. The worker's own `releaseCamera()` repeats the
unbind (idempotent) before destroying the camera. A `stop()` that lands while
the worker is still opening the camera wins: readiness is never confirmed for
a session whose owner already asked it to end.

## Telemetry validity and freshness (issue #368)

`telemetrySnapshot(freshnessWindowUs = 3 s)` returns an
`AcquisitionTelemetrySnapshot` (`TelemetrySample.h`) in which **every metric
carries its own** `MetricValidity` (`Valid` / `Unavailable` / `Unsupported` /
`Error` / `Stale`), sample host time, age, and session generation:
`framesDelivered`, `captureFrameRate`, `captureDataRateMBps`,
`sdkCompletedQueueDepth`, `sdkInputBufferCount`, `bufferUnderruns`,
`transportLostFrames`, `intentionallyDiscardedFrames` (deliberate LatestFrame
drops — never merged with loss), `frameAgeUs`, `publishLatencyUs`, plus the
session's `TimestampDescriptor`. `CaptureStats` keeps the raw atomics and
now per-metric validity/sample-time atomics; the old aggregate
`queueStatsValid` remains only as a compatibility flag. Rules: a backend
field with `*Valid == false` is `Unsupported` (never a measured zero); a
Valid sample older than the window is `Stale` (value retained, structurally
distinct); `requestStart()` resets every metric to `Unavailable` and re-tags
the generation synchronously, so a reconnect can never show the previous
camera's numbers. `timestampDescriptor()` describes `Frame::timestamp` for the
session ([[../camera/ICamera]] contract) and is cleared to `Unavailable` on
release. `frameAgeUs` is Valid only for host-comparable domains; host
receipt/publish timing is host-side latency, never exposure time. Consumers:
[[../frontend/MainWindow]] status/statistics rendering
(`StatsDisplayManager::formatMetric` shows `n/a` / `unsupported` /
`N (stale x s)`), `Hdf5Service::writeAcquisitionProvenance`. Guard:
`backend.timestamp_telemetry`.

## Gotchas

- Always refresh EGrabber `StreamModule` stat counters before calling
  `stop()` — see [[../conventions/Code-Conventions]] and
  `docs/howto/safe-start-stop-egrabber.md`.
- `setCameraReadyCallback` fires from this thread (and from the `stop()`
  caller for the nullptr case); [[TriggerService]] uses it to bind a live
  `ICamera*` + generation and start itself.
- `softTriggerActiveCamera` takes `cameraMutex_` then the camera's own state
  mutex — the same order as `stop()`, so GUI-thread soft triggers cannot
  deadlock against teardown.
- The periodic `checkDeviceHealth()` call is frame-consuming on MindVision
  only in free-run mode; under trigger modes the camera skips the probe (see
  [[../camera/MindVisionCamera]]).
- A `requestStart()` while another thread is inside `stop()` returns
  `RejectedStopping` instead of racing it; callers retry after the stop
  completes (or use `waitForState`).
- Old-generation transitions never overwrite a newer session's snapshot (an
  old worker finishing after a restart is ignored by generation compare).
- Platform default factory:
  - Windows (`MIB_HAS_EGRABBER=1`) defaults to [[../camera/EGrabberCamera]].
  - Non-Windows defaults to [[../camera/MockCamera]] (`data/mock_frames`) so
    cloud/Linux builds can exercise non-hardware pipeline paths.
