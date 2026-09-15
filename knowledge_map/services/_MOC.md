# Services — MOC

> All services live under `src/backend/services/`. Each owns one concern.
> Composition and wiring happens in [[../architecture/AppBackend]].

## Realtime acquisition & analysis
- [[CaptureService]] — dedicated thread; `camera->grabFrame()` → FrameStore
- [[ProcessingService]] — worker pool + realtime loop; OpenCV pipeline
- [[PlaybackService]] — UI-facing wrapper over FrameStore

## Persistence
- [[Hdf5Service]] — batched write/read of experiment frames + metadata
- [[SqliteService]] — small metadata DB

## Hardware I/O
- [[CameraControlService]] — GenICam script apply, device reset, discovery
- [[AutofocusService]] — backend-neutral nanopositioner control (OEABT serial
  on Linux/Windows; CoreMOR XMT on Windows)
- [[TriggerService]] — camera digital-output pulse on target-group detection
- [[SyringePumpService]] — dual-pump Modbus RTU over serial

## Optional / specialised
- [[YoloService]] — ONNX Runtime session (segmentation; placeholder-ish)
- [[RecorderService]] — raw frame container writer (recording mode)
- [[BatchMaskSources]] — adapters for offline mask regeneration
  (`processBatch` inputs/outputs)

## Diagnostics
- [[CrashReporter]] — process-level crash handler + Sentry forwarder;
  pairs with [[../diagnostics/CrashStateMirror]]

**Up**: [[../README|Vault home]] · **See also**:
[[../architecture/Data-Flow]], [[../architecture/Threading-Model]],
[[../diagnostics/_MOC|Diagnostics MOC]]
