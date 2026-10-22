# Services — MOC

> All services live under `src/backend/services/`. Each owns one concern.
> Composition and wiring happens in [[../architecture/AppBackend]].

## Realtime acquisition & analysis
- [[CaptureService]] — dedicated thread; `camera->grabFrame()` → FrameStore
- [[ProcessingService]] — worker pool + realtime loop; OpenCV pipeline
- [[PlaybackService]] — UI-facing wrapper over FrameStore
- [[DotGridService]] — low-rate wafer localization from the dot-grid fiducial
  pattern (absolute position, rotation, scale, mirror, design + chip id);
  ADR 0008, design registry ADR 0009
- [[MonitoringDensityService]] — live Monitoring scatter KDE + core contour on a
  lowest-priority worker with load back-off and a compute budget
- [[MonitoringCellStore]] — live per-row cell images: a bounded strip of recent
  crops and run-scoped pins that outlive the ring (bridge ABI 41)
- [[CalibrationRecords]] — the calibration run: method purpose / policy, record
  derivation at close, the append-only instrument store, explicit apply, the
  `calibration.record` gate (bridge ABI 42)
- [[RunQuickLook]] — per-run statistics of closed HDF5 runs (one shared function in
  `mib_review_core`): the pass at close, the in-memory LRU, `run_quick_look` (bridge ABI 44) and
  Review's twin `review_run_statistics` (review ABI 6)

## Persistence
- [[Hdf5Service]] — batched write/read of experiment frames + metadata
- [[HdfExportService]] — Qt-free bounded/cancellable CSV+TIFF export job (issue #344)
- [[ReviewSession]] — YOFO Review's review implementation, also the Qt tab's factor (ADR 0014; `mib_review_core`)
- [[SqliteService]] — small metadata DB

## Hardware I/O
- [[DeviceDiscoveryService]] — backend discovery **jobs** (bounded, cancellable,
  provider-based) + startup selection/connection policy; every camera /
  nanopositioner / pulse-generator scan runs through it (#419, ADR 0005)
- [[CameraControlService]] — GenICam script apply, device reset, SDK enumeration
  (wrapped by the discovery providers)
- [[AutofocusService]] — backend-neutral nanopositioner control (OEABT serial
  on Linux/Windows; CoreMOR XMT on Windows)
- [[TriggerService]] — camera digital-output pulse on target-group detection
- [[SerialBus]] — shared RS485/Modbus bus sessions (one `QSerialPort` owner
  per adapter, strict response correlation); transport for the two below
- [[SyringePumpService]] — dual-pump Modbus RTU over serial
- [[ISerialPort]] — Qt-free serial transport interface (POSIX/Win32) + factory
- [[SerialPortClaims]] — in-process port arbitration: discovery probes yield to
  bus sessions, the connected nanopositioner and config.json pump ports
- [[SimulatedSerial]] — serial-sim rig: fake COM ports, Modbus/OEABT devices,
  startup timing faults, stale holders, ownership ledger (`MIB_SERIAL_MODE=sim`)
- [[PulseGeneratorService]] — Zhongsheng pulse module (camera ext-trigger
  source) via Modbus RTU over serial; addressed device on a shared bus
- [[StageService]] — motorized Z stage: read-only start-up, operator "Set zero
  here" (no Home), travel envelope around the zero, one-sided approach,
  once-per-power-up zero (ADR 0013 + Amendment 1, #464)
- [[ZC300Stage]] — Zolix ZC300 motorized Z stage driver (`IMotionStage`, µm
  API, observe-only connect, motion opcodes never re-sent); ADR 0013, #464
- [[RfGeneratorService]] — SIGLENT SSG3021X RF sort generator over SCPI
  (USBTMC / LAN): readback, preflight gate, provenance — never timing

## Optional / specialised
- [[RecorderService]] — raw frame container writer (recording mode)
- [[BatchMaskSources]] — adapters for offline mask regeneration
  (`processBatch` inputs/outputs)

## Diagnostics
- [[MinidumpUploader]] — posts pending crash dumps to Sentry with a per-dump HTTP status (the crash queue advances only on 2xx)
- [[CrashReporter]] — process-level crash handler + Sentry forwarder;
  pairs with [[../diagnostics/CrashStateMirror]]

**Up**: [[../README|Vault home]] · **See also**:
[[../architecture/Data-Flow]], [[../architecture/Threading-Model]],
[[../diagnostics/_MOC|Diagnostics MOC]]

- [[ProfileRegistryService]] — central registry/cache foundation (#398).
- [[WorkspaceStore]] — isolated versioned metadata document CAS/history core (#807); host integration pending.
