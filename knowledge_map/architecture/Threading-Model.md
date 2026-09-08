# Threading Model

> Who runs on which thread. Getting this wrong causes deadlocks, missed
> frames, or UI freezes.

**Related:** [[Data-Flow]], [[AppBackend]], [[../services/CaptureService]],
[[../services/ProcessingService]]

## Threads at a glance

| Thread | Owner | Blocks on | Role |
|---|---|---|---|
| Main / GUI | Qt event loop (`main.cpp`) | Qt events | UI, controllers, timer ticks |
| Capture | [[../services/CaptureService]] `run()` | `camera->grabFrame()` | Acquires frames, pushes to FrameStore, fires callback |
| Processing workers | [[../services/ProcessingService]] pool | job queue | Generic job executor (size = `hardware_concurrency()` by default) |
| Realtime processing | [[../services/ProcessingService]] `realtimeLoop()` | FrameStore writeIndex | Low-latency per-frame analysis; drop-frames mode skips to latest |
| Native processing-core contexts | selected `IProcessingKernel` | short context-pool mutex | Each concurrent processing call leases one plugin-owned context; a context is never shared concurrently |
| Autofocus stats | [[../services/AutofocusService]] `statsLoop()` | `pendingSamplesCV_` + 10 ms drain interval | Drains ring-ratio samples pushed by `ProcessingService` realtime thread, maintains 1000-sample deque, refreshes `{median,average,min,max}RingRatio_` atomics. Runs for the full lifetime of the service, not just while connected. |
| Autofocus control | [[../services/AutofocusService]] `controlLoop()` | serial COM | Reads ring-ratio stats atomics, writes voltage to nanopositioner. Runs only between `connect()` / `disconnect()`. |
| Trigger | [[../services/TriggerService]] `triggerLoop()` | `triggerCV_` | Issues camera digital-output pulse on target-group events |
| Syringe pump poll | [[../services/SyringePumpService]] per pump | serial (Modbus RTU) | UI-driven status polls |
| Frame-recording | `AppBackend` `frameRecordingThread_` | FrameStore | Only active in recording mode; drains non-empty frames into HDF5 |

## Sync primitives

- `std::atomic<bool>` flags gate the thread loops. [[../services/CaptureService]]
  additionally owns an explicit lifecycle state machine
  (`Idle/Starting/Running/Stopping/Faulted`, per-session generation) under
  `lifecycleMutex_`; a worker that exits on its own leaves the thread
  joinable in `Faulted` until the lifecycle owner reaps it (issue #365).
- [[../services/TriggerService]] holds `pulseMutex_` for the whole duration
  of a pulse; `setCamera()` takes it to swap the bound camera, so a camera is
  never destroyed under an in-flight pulse. [[../camera/MindVisionCamera]]
  counts in-flight SDK operations (`InFlightOp`) and `stop()` waits (bounded)
  for zero before `CameraUnInit`.
- `FrameStore` internal mutex serialises push/query. See
  [[../data-model/FrameStore]].
- `ProcessingService` uses `std::condition_variable_any` for the worker
  queue; the realtime loop consumes FrameStore by absolute write-index and,
  when caught up, blocks in `FrameStore::waitForFrame` (condition variable
  notified by `pushFrame` behind a Dekker-guarded waiter counter — one
  relaxed atomic load per push while nobody waits; issue #282 replaced the
  old 2 ms sleep-poll). TriggerService pulses drain a bounded per-request
  deque under `triggerMutex_` (issue #283).
- `processingKernelMutex_` is the operation/activation boundary. Realtime,
  experiment/batch, raw-recording, and buffer-export paths hold a shared
  `CoreOperationLease` for their full operation and provenance lifetime;
  activation takes exclusive ownership and is rejected while realtime,
  experiment, async batch, or synchronous batch state is active. Dynamic
  modules remain loaded until process exit to avoid teardown races.
- Qt signals from non-GUI threads go through
  [[../frontend/System-Utilities]] `BackgroundCaptureNotifier` (signal bridge).
- [[../diagnostics/PipelineTimingRecorder]] (opt-in latency instrumentation)
  adds no threads and no locks: single-writer rings (frame records written
  only by the realtime thread, trigger records only by the trigger thread)
  plus relaxed-atomic skip counters; disabled it is one relaxed load per
  hook. Trigger pending-request metadata rides under the existing
  `triggerMutex_`.

## Experiment vs Monitoring vs Realtime

`ProcessingService` has three accumulation modes:

- **Realtime snapshot** — always available when realtime loop is running;
  UI reads latest via `getLatestSnapshot`.
- **Monitoring ring buffers** — fixed 1000-frame ring for live charts in
  [[../frontend/ExperimentMonitoringTab]]; active whenever realtime is on.
- **Experiment accumulation** — bounded vectors collected while
  `experimentActive_ == true`, periodically flushed to HDF5 via
  `flushBufferedFrames`. The realtime thread drops sampled invalid frames first
  if HDF5 falls behind and the backlog reaches its cap.

## Shutdown order

Stopping capture first drains the realtime loop safely. Inside capture stop
the order is: publish `Stopping` → unbind the trigger service (waits for an
in-flight pulse, clears stale requests) → `camera->stop()` → join the
capture thread → publish `Idle`; the camera object is destroyed only after
the trigger thread has released it ([[AppBackend]] "Shutdown"). See
`docs/howto/safe-start-stop-egrabber.md` for EGrabber-specific shutdown
requirements (including `StreamModule` stat refresh — see [[../conventions/Code-Conventions]]).

Processing-core changes are never applied mid-operation. The GUI requires all
capture/experiment/recording/batch work to stop, then activates the prepared
kernel transactionally. Existing plugin modules are retained rather than
unloaded while thread teardown could still hold function pointers.
