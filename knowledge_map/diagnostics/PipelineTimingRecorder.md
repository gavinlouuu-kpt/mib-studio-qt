# PipelineTimingRecorder

> Lock-free per-frame latency recorder for diagnosing realtime-pipeline and
> trigger-service delay. Stamps every frame on one host monotonic clock at
> acquisition, algorithm start/end, callback dispatch, and trigger pulse
> fire, then dumps joinable CSVs — without being able to delay the pipeline
> or drop frames itself.

**Source:** `src/backend/diagnostics/PipelineTimingRecorder.cpp`,
`include/backend/diagnostics/PipelineTimingRecorder.h` (part of
`mib_processing`, Qt-free)
**Related:** [[CrashStateMirror]], [[../services/CaptureService]],
[[../services/ProcessingService]], [[../services/TriggerService]],
[[../data-model/FrameStore]], [[../architecture/AppBackend]]
**How-to:** `docs/howto/pipeline-latency-diagnosis.md`

## Why

`getAlgoAvgUs1s` / `getLastOnsetUs` are windowed or partial gauges; the
end-to-end acquisition→trigger latency gauge was explicitly deferred in
[[../task/2026-04-15-trigger-timing-bug]]. This component fills that gap:
per-frame records on **one clock** (`Tools::getTimestamp()`, host monotonic
µs) so stage-to-stage differences are directly meaningful. The camera's own
frame `timestamp` is a device tick on a different clock and is carried along
only for device-side spacing analysis.

## Design constraints (why it cannot cause the problem it measures)

- Fixed pre-allocated rings (~64k frame records, ~16k trigger records); no
  allocation, no locks, no I/O on any hot path.
- One writer per ring: frame records from the realtime processing thread,
  trigger records from the trigger thread. Skip counters are relaxed atomics
  writable from any thread.
- Disabled (default) every hook is a single relaxed atomic load.
- Preserves the callback-ordering invariant: no lock is taken before the
  target-group callback in `publishRealtimeValidationCallbacks` (see
  [[../services/ProcessingService]]).

## Record schema

`FrameTimingRecord` (one per frame reaching the realtime callback stage):
`frameIndex` (FrameStore write index), `deviceTimestamp`, `grabUs` (stamped
in [[../services/CaptureService]] right after `grabFrame`), `algoStartUs`,
`algoEndUs` (0 in async-batch mode), `triggerDispatchUs`, `callbacksDoneUs`,
`validCount`, `invalidCount`, `isTargetGroup`.

`TriggerTimingRecord` (one per pulse actually driven): `frameIndex`,
`grabUs` (echoed from the source frame through
`TargetGroupEvent`→`TargetGroupSignal`), `requestUs`, `wakeUs`, `fireUs`,
`pulseDoneUs`, `coalesced`. Since the per-request trigger queue (issue
#283) `coalesced` is always 0 — overload appears in
`TriggerService::getDroppedRequestCount()` instead; non-zero values only
occur in recordings from before that change.

`PipelineSkipReason` counters make frame accounting conserved — pushed
frames == frame records + counted skips (`dropped_to_latest`, `ring_behind`,
`empty_frame`, `kernel_error`, `batch_queue_rejected`) — so silent frame
loss is detectable.

## Instrumented points

| Stage | Where |
| --- | --- |
| `grabUs` | `CaptureService::run` right after `grabFrame`, carried on `camera::common::Frame::hostTimestampUs` → `playback::Frame::hostTimestampUs` |
| `algoStartUs`/`algoEndUs` | the three realtime inline-loop paths in `ProcessingService` (next to the existing steady_clock stamps) |
| record write + `triggerDispatchUs`/`callbacksDoneUs` | `ProcessingService::publishRealtimeValidationCallbacks` (shared chokepoint of all realtime paths) |
| `requestUs` | `TriggerService::onTargetGroupResult` (pending metadata kept under `triggerMutex_`) |
| `wakeUs`/`fireUs`/`pulseDoneUs` | `TriggerService::triggerLoop` around `setTriggerOutput` |
| skip counters | drop-to-latest / ring-behind / empty / kernel-error sites in both realtime loops, batch enqueue rejection |

## Enabling and output

- Env: `MIB_PIPELINE_TIMING=1` (+ optional `MIB_PIPELINE_TIMING_DIR`), read
  in `AppBackend::initialize`. Default dump dir: `<dataDir>/pipeline_timing`.
- Runtime: `AppBackend::setPipelineTimingEnabled` /
  `dumpPipelineTiming(dir)`.
- Auto-dump of `pipeline_frames.csv`, `pipeline_triggers.csv`,
  `pipeline_skips.csv` on capture stop and on shutdown.
- Analyse with `python3 scripts/analyze_pipeline_timing.py <dir>` — prints
  per-stage percentiles (grab→algo wait, algo, request→fire, grab→fire
  end-to-end), host-vs-device inter-frame gaps (exposes SDK-side queueing),
  and the accounting summary.

## Live summary + always-on target latency

Two additions expose latency without the CSV round-trip:

- `summarize(sampleLimit=0)` reduces the retained rings to per-stage
  avg/max/p50/p95/p99 (`LatencySummary`: end-to-end target `fireUs−grabUs`,
  request→fire, end-to-end frame, frame age `algoStartUs−grabUs`, algo,
  dispatch). It allocates + sorts locally, so call it off the hot path (a
  ~1 Hz UI tick or at stop). Still the *detailed tier*: only populated for
  records captured while enabled.
- `noteTargetLatency(us)` / `lastTargetLatencyUs()` / `avgTargetLatencyUs()`
  (EWMA) / `maxTargetLatencyUs()` / `resetLiveLatency()` are an **always-on**
  gauge, updated unconditionally by `TriggerService::triggerLoop` on every
  driven pulse (acquisition→pulse onset). This is the headline "target seen →
  sorted" latency, visible in the status bar without `MIB_PIPELINE_TIMING`.
  `ProcessingService::startExperiment` calls `resetLiveLatency()`.

The identification funnel (valid/target-group/unserved) and invalid-reason
histogram live on [[../services/ProcessingService]] (`getIdentificationCounters`);
`analyze_pipeline_timing.py` also prints a funnel + pre-algo loss summary.

## Tests

- `processing.identification_metrics`
  (`tests/processing/processing_identification_metrics_test.cpp`) — the shared
  invalid-reason classifier, `summarize()` percentiles, and the live
  target-latency gauge.
- `backend.pipeline_timing_recorder`
  (`tests/backend/pipeline_timing_recorder_test.cpp`) — disabled no-op, ring
  wrap, concurrent writers, CSV dump.
- `integration.e2e_pipeline_timing`
  (`tests/integration/e2e_pipeline_timing_test.cpp`) — drives the real
  inline loop + TriggerService: monotonic stage stamps, strictly increasing
  indices, frame accounting conserved in both every-frame and drop-frames
  modes, trigger records echo source-frame identity, coalescing accounted.
  Gates are ordering/accounting invariants, no absolute-ms thresholds.

## Gotchas

- `dumpCsv` concurrent with a running pipeline can tear the oldest rows if
  a ring wraps mid-dump; dump at stop (AppBackend does) for exact snapshots.
- `clear()` only while pipeline threads are stopped.
- Async-batch mode records frame identity + callback stamps but zero
  `algoStartUs`/`algoEndUs` (batch algo timing stays aggregate).
- Realtime loop's `rtLastProcessed_` starts at 0, so FrameStore index 0 is a
  never-consumed sentinel — accounting checks tolerate exactly one frame.
