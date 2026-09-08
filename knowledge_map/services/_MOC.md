# Services — MOC

> All services live under `src/backend/services/`. Each owns one concern.
> Composition and wiring happens in [[../architecture/AppBackend]].

## Realtime acquisition & analysis
- [[CaptureService]] — dedicated thread; `camera->grabFrame()` → FrameStore
- [[ProcessingService]] — worker pool + realtime loop; OpenCV pipeline
- [[PlaybackService]] — UI-facing wrapper over FrameStore

## Persistence
- [[Hdf5Service]] — batched write/read of experiment frames + metadata
- [[HdfExportService]] — Qt-free bounded/cancellable CSV+TIFF export job (issue #344)
- [[SqliteService]] — small metadata DB

## Hardware I/O
- [[CameraControlService]] — GenICam script apply, device reset, discovery
- [[AutofocusService]] — nanopositioner voltage via serial (Coremor XMT)
- [[TriggerService]] — camera digital-output pulse on target-group detection
- [[SerialBus]] — shared RS485/Modbus bus sessions (one `QSerialPort` owner
  per adapter, strict response correlation); transport for the two below
- [[SyringePumpService]] — dual-pump Modbus RTU over serial
- [[PulseGeneratorService]] — Zhongsheng pulse module (camera ext-trigger
  source) via Modbus RTU over serial; addressed device on a shared bus

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
