# TriggerService

> Emits a short high pulse on a configurable camera digital-output line
> when [[ProcessingService]] classifies a frame as "target group".

**Source:** `src/backend/services/TriggerService.cpp`,
`include/backend/services/TriggerService.h`
**Related:** [[ProcessingService]], [[CaptureService]],
[[../camera/ICamera]]

## Responsibility

- Hold a non-owning `ICamera*` **bound to one acquisition session
  generation** (handed to it by [[CaptureService]]::CameraReadyCallback via
  [[../architecture/AppBackend]]). `setCamera(camera, generation)` is the
  session boundary (issue #365): it waits for any in-flight pulse
  (`pulseMutex_`), clears every pending request (counted in
  `getDroppedStaleRequestCount()`), then swaps the pointer + generation.
  After it returns the trigger thread can never touch the previous camera.
  Requests are stamped with the generation bound at enqueue time and are
  refused at fire time if a different session is bound, so a request from
  session N cannot execute against session N+1 after reconnect/restart.
  Regression guard: `tests/backend/trigger_session_test.cpp`.
- On `onTargetGroupResult(signal)`, signal the trigger thread to raise the
  configured output line, sleep `pulseDurationUs_`, then lower it.
- `TargetGroupSignal` carries source identity (`objectId`, `trackId`) plus
  source-frame identity for latency correlation (`frameIndex`,
  `hostTimestampUs` — the host monotonic acquisition stamp) and is used
  for metadata at trigger fire time.
- Expose metrics: `getTriggerCount`, `getLastOnsetUs`, `resetMetrics`.
- Count **lost pulses** that were previously dropped silently: after a
  request is dequeued the loop may still fail to drive the TTL edge because
  no camera is bound (`getDroppedPulsesNoCameraCount`) or
  `setTriggerOutput(true)` returns false (`getDroppedPulsesSetFailedCount`);
  both are a selected sort target that never fired (`getDroppedPulseCount` =
  sum, throttled WARN). Each pulse also feeds the always-on live
  acquisition→pulse latency gauge via
  `PipelineTimingRecorder::noteTargetLatency`, and mirrors trigger count /
  onset / dropped requests / dropped pulses / target latency into
  [[../diagnostics/CrashStateMirror]] (`trigger` slot).
- Requests are queued per-request (issue #283): a bounded
  `pendingRequests_` deque (capacity `kMaxPendingRequests` = 8, under
  `triggerMutex_`) replaces the old single-bool flag, so every
  target-group request gets its own pulse in arrival order instead of
  silently coalescing while the thread is mid-pulse. Overflow drops the
  OLDEST entry and counts it (`getDroppedRequestCount()`, throttled WARN)
  — a backlog of stale pulses is worse than a counted drop.
- When [[../diagnostics/PipelineTimingRecorder]] is enabled, each pulse also
  writes a `TriggerTimingRecord` (request/wake/fire/pulse-done stamps)
  keyed by the source frame, enabling true end-to-end acquisition→pulse
  latency measurement. The record's `coalesced` field is now always 0;
  overload shows up in the dropped-request counter instead.

## Threading

Dedicated thread waiting on `triggerCV_`. Pulse duration is configurable in
microseconds (default 1 µs). On entry the loop elevates its own scheduling
priority (`THREAD_PRIORITY_TIME_CRITICAL` on Windows; best-effort `SCHED_FIFO`
elsewhere, silently unavailable without privileges) so background load — e.g.
the HDF5 writer draining an experiment flush — cannot preempt a pending pulse
(issue #227; at default priority the request→wake tail reached 30 ms of pure
OS scheduling in a 500 fps mock run).

## Manual & periodic test paths

- Manual single pulse: `sortTriggerBtn` in
  [[../frontend/ExperimentMonitoringTab]] calls
  `onTargetGroupResult(services::TargetGroupSignal{.isTargetGroup=true})` once.
- Periodic test pulses: `periodicTriggerBtn` + `periodicTriggerIntervalSpin`
  in the same tab arm a `QTimer` that calls
  `onTargetGroupResult(services::TargetGroupSignal{.isTargetGroup=true})`
  every N ms. Useful for bring-up / oscilloscope checks without needing
  a running pipeline that classifies real "target group" frames.

## Gotchas

- Camera pointer is non-owning and read via `std::atomic`. If the camera
  disappears (stop), `setCamera(nullptr)` and `stop()` must be called
  **before the camera object is destroyed** — `CaptureService::stop()` and
  the capture thread's `releaseCamera()` both do this through the ready
  callback, and `AppBackend::shutdown()` no longer clears that callback
  before stopping capture. The trigger loop holds `pulseMutex_` for the
  whole pulse (load pointer → rising edge → busy-wait → falling edge); lock
  order is `pulseMutex_` before `triggerMutex_` and the loop never holds
  both.
- Not all cameras support `setTriggerOutput`. `ICamera::setTriggerOutput`
  returns `false` by default (see [[../camera/ICamera]]).
- Requires `ProcessingService::enable_target_group` + thresholds to be set.
- `getLastOnsetUs()` measures only the duration of the
  `setTriggerOutput(true)` call, not end-to-end frame→trigger latency. For
  software-observable end-to-end latency enable
  [[../diagnostics/PipelineTimingRecorder]] (`MIB_PIPELINE_TIMING=1`, see
  `docs/howto/pipeline-latency-diagnosis.md`); for electrical ground truth
  use an oscilloscope on the TTL line. See
  [[ProcessingService]] "Callback ordering invariant" — the callback is
  dispatched outside `monitoringFramesMutex_` to keep wake-up latency flat,
  and the target-group callback fires **before** the ring-ratio callback
  so the CV notification is not serialised behind autofocus buffer
  maintenance + sort.
- The trigger thread is fully decoupled from the Qt event loop: it waits
  on `triggerCV_`, wakes on `onTargetGroupResult(signal)`, fires a pulse,
  busy-waits `pulseDurationUs_`, and lowers the line. The UI thread only
  calls `onTargetGroupResult` for the manual `sortTriggerBtn` /
  `periodicTriggerBtn` paths — those are non-blocking by construction. No
  UI mutex is held across a trigger wake-up.
- **Lost-wakeup fix:** `onTargetGroupResult` mutates the pending-request
  queue while holding `triggerMutex_` (the same mutex guarding the
  consumer's `wait()` predicate) before `notify_one()` — originally this
  applied to the `triggerRequested_` flag the queue replaced. Mutating the
  request state lock-free races with the consumer's predicate check — if
  it lands after the predicate is evaluated but before the thread blocks,
  the notification is lost and the trigger is delayed until the next
  request. This was the root cause of "variable delay / occasional missed
  trigger." Regression guard: `tests/integration/e2e_trigger_timing_test.cpp`
  (`integration.e2e_trigger_timing`) asserts zero missed pulses and reports
  the request→fire latency distribution under CPU load.
- **Lost-wakeup fix #2 — `stop()` deadlock (the more serious one):** `stop()`
  must also clear `running_` while holding `triggerMutex_` before
  `notify_all()`. `start()`/`stop()` run on every capture start/stop (wired via
  [[CaptureService]]::CameraReadyCallback). If `stop()` cleared `running_`
  lock-free and the just-started trigger thread was between its `wait()`
  predicate check (`running_ == true`) and actually blocking, the notify was
  lost and the thread blocked forever — so `stop()`'s `join()` deadlocked,
  which hung `CaptureService::stop()` and therefore the whole app shutdown /
  camera restart. Intermittent and platform-independent. Found by the
  `integration.e2e_pipeline_stress` lifecycle test (its watchdog pinpointed
  `capture.stop`); that test is the regression guard.
- **Resolved limitation (issue #283):** the old single-bool
  `triggerRequested_` coalesced target-group results arriving mid-pulse
  into one fire (measured: 5/2176 requests merged in a 500 fps mock run).
  The bounded per-request queue gives every request its own pulse; only
  sustained overload beyond `kMaxPendingRequests` drops (oldest-first,
  counted). Guards: `integration.e2e_pipeline_timing` phases 3-4 (paced =
  exactly one pulse per request; burst = conservation + newest survives).
  Residual latency jitter under load was addressed by issue #227: the
  trigger thread now runs at elevated priority (see Threading) and each
  pulse edge is a single GenICam `LineSource` register write
  (`LineSelector` is cached per camera session — see
  [[../camera/EGrabberCamera]]). The remaining onset jitter floor is the
  PCIe control round-trip of the register write itself; the actual pulse
  *width* is dominated by that write latency too (the 1 µs busy-wait is
  shorter than a PCIe control transaction), so hardware-exact width would
  need the Coaxlink I/O toolbox to shape the pulse.
