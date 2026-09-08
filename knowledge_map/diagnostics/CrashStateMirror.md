# CrashStateMirror

> Lock-free, process-global snapshot of live service state. Services write
> atomic updates into named sub-structs as their state changes; the
> [[../services/CrashReporter]] reads the mirror from a signal/SEH context
> to produce the `.json` sidecar that accompanies every minidump.

**Source:** `src/backend/diagnostics/CrashStateMirror.cpp`,
`include/backend/diagnostics/CrashStateMirror.h`
**Related:** [[../services/CrashReporter]], [[../architecture/Threading-Model]]

## Why a mirror instead of polling services?

Calling service methods (e.g. `processing.queueDepth()`) from a crash
handler is unsafe because:

1. The crashing thread may hold a mutex the method takes → deadlock.
2. The faulting thread's stack may be unwinding → method body re-enters
   the crash.
3. Service objects may already be destructed (late-shutdown crashes).

The mirror sidesteps all of this: services write to atomics at the same
points they already log lifecycle events, and the crash handler reads
those atomics without locking.

## Layout

```cpp
struct CrashStateMirror {
    CaptureSlot       capture;       // running, framesProcessed, fps, dataRate
    ProcessingSlot    processing;    // running, realtime, experiment, queue, jobs,
                                     //   droppedValidFrames, targetGroupObjects,
                                     //   unservedTargetGroupObjects (sort-loss)
    Hdf5Slot          hdf5;          // fileOpen, pending/appended counts, path
    FrameStoreSlot    frameStore;    // capacity, totalWritten, latest/earliest
    AutofocusSlot     autofocus;     // connected, enabled, voltage, ringRatio
    SyringePumpSlot   syringePump;   // sample + sheath pump status
    TriggerSlot       trigger;       // running, count, lastOnset, droppedRequests,
                                     //   droppedPulses, targetLatencyUs
    RecorderSlot      recorder;      // recording, written, filtered
    AppSlot           app;           // cameraLabel, dataDir, buildVersion
};
```

All numeric fields are `std::atomic<T>`. String fields use a
mutex-protected fixed-size `char[]`; the crash-handler reader uses
`try_lock` and falls back to an unlocked memcpy on contention (worst case
is a truncated value in the JSON, never a hang).

## Wiring pattern

Each service writes to the mirror at the same call sites it already logs
lifecycle events. Examples:

| Site | Update |
|---|---|
| `CaptureService::run` (start) | `mirror.capture.running=true`, buffer config |
| `CaptureService::run` (per stats poll) | `lastFrameRate`, `lastDataRateMBps` |
| `CaptureService::run` (per frame) | `framesProcessed.fetch_add(1)` |
| `ProcessingService::start`/`stop` | `processing.running`, `workerCount` |
| `ProcessingService::submit` | `processing.jobsQueued` |
| `ProcessingService::workerLoop` | `processing.jobsProcessed.fetch_add(1)` |
| `ProcessingService::start/stopRealtime` | `processing.realtimeRunning` |
| `ProcessingService::start/endExperiment` | `processing.experimentActive` |
| `Hdf5Service::openFile`/`closeFile` | `hdf5.fileOpen`, `setHdf5Path()` |
| `FrameStore::pushFrame` | `totalWritten`, `latestIndex`, `earliestIndex` |
| `FrameStore::resize` | `capacity` |
| `AutofocusService::connect`/`disconnect` | `autofocus.connected`, `comPort`, `voltage` |
| `AutofocusService::setEnabled` | `autofocus.enabled` |
| `AppBackend::startFrameRecording`/`stop` | `recorder.recording` |
| `AppBackend::initialize` | `app.cameraLabel`, `app.dataDir`, `app.mockCamera` |

These are passive writes — they do not change service behavior, only
publish state.

## Snapshot format

`snapshotJsonString()` returns a pretty-printed JSON document (via
`nlohmann::json`). Falls back to a hand-rolled emitter when the header is
not available. Example fragment:

```json
{
  "timestamp_ms": 1747906215182,
  "capture": { "running": true, "frames_processed": 18432,
               "last_frame_rate_hz": 5000, "last_data_rate_mbps": 1234 },
  "processing": { "experiment_active": true, "jobs_queued": 3,
                  "jobs_processed": 18420 },
  "hdf5": { "file_open": true,
            "path": "C:/data/exp_2026-05-22.h5" },
  ...
}
```

## Gotchas

- **Do not store complex objects in the mirror.** Strings use a fixed
  buffer; everything else is a scalar atomic. Adding `std::vector` or
  `std::string` to a slot defeats the lock-free guarantee.
- **Atomic writes happen in hot paths** (per-frame in
  `FrameStore::pushFrame`). Keep them `memory_order_relaxed` and avoid
  read-modify-write where a `store` suffices.
- The mirror outlives all services (it's a function-local static). After
  shutdown its values reflect the final state, which is what you want for
  late-shutdown crashes.
