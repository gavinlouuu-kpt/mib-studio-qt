# 2026-07-17 — Pipeline latency instrumentation (acquisition→algo→trigger)

**Problem:** realtime pipeline and trigger service show delay, but the only
software gauges were 1-second aggregates (`getAlgoAvgUs1s`) and the
pulse-call duration (`getLastOnsetUs`); the end-to-end trigger gauge was
explicitly deferred in [[2026-04-15-trigger-timing-bug]]. No way to tell
whether delay came from acquisition/SDK queueing, FrameStore wait,
algorithm time, callback dispatch, or trigger-thread wake-up — and no
per-frame record to prove frames weren't silently dropped.

**Solution:** [[../diagnostics/PipelineTimingRecorder]] — lock-free,
pre-allocated single-writer rings recording per-frame stage stamps on one
host monotonic clock, plus per-reason skip counters so frame accounting is
conserved. Stamps: grab (CaptureService, carried as
`Frame::hostTimestampUs` through FrameStore), algo start/end (three inline
realtime paths), trigger dispatch + callbacks-done
(`publishRealtimeValidationCallbacks`, preserving the callback-ordering
invariant), request/wake/fire/pulse-done (TriggerService, pending metadata
under `triggerMutex_`, coalesced requests counted).
`TargetGroupEvent`/`TargetGroupSignal` now carry `frameIndex` +
`hostTimestampUs` for correlation.

**Enablement:** `MIB_PIPELINE_TIMING=1` (+`MIB_PIPELINE_TIMING_DIR`); CSVs
auto-dump on capture stop/shutdown; `AppBackend::dumpPipelineTiming` on
demand. Analysis: `scripts/analyze_pipeline_timing.py` (per-stage
percentiles, host-vs-device gap comparison, accounting summary). How-to:
`docs/howto/pipeline-latency-diagnosis.md`.

**Tests:** `backend.pipeline_timing_recorder` (unit: no-op when disabled,
ring wrap, concurrent writers, CSV) and `integration.e2e_pipeline_timing`
(real inline loop + TriggerService: monotonic stamps, strictly increasing
indices, accounting conserved in every-frame and drop-frames modes, trigger
identity echo, coalescing accounted). Ratio/invariant gates only.

**Files:** `src/backend/diagnostics/PipelineTimingRecorder.cpp` (+header,
in `mib_processing`), `CaptureService.cpp`, `FrameStore.{h,cpp}`,
`camera/common/Frame.h`, `ProcessingService.{h,cpp}`,
`TriggerService.{h,cpp}`, `AppBackend.{h,cpp}`,
`scripts/analyze_pipeline_timing.py`, tests, vault notes.

**Follow-up (same day) — mock dry-run harness:** MockCamera now simulates
the trigger output line (atomic level + rising-edge counter, returns true),
and `tests/tools/mock_pipeline_timing_run.cpp` drives the full production
wiring headlessly from a frames folder (fetched with
`scripts/fetch_hf_512x96stream.py` from HF `gavinlouuu/512x96stream`):
ROI = right third of FOV, per-pixel-median background, wide-open
target-group gates so every valid detection fires TriggerService. First
measured session (500 fps × 20 s, every-frame): 9,904 captured / 0 dropped /
accounting conserved; grab→algo p50 1.03 ms (dominated by the realtime
loop's 2 ms idle poll), algo p50 0.30 ms, request→fire p50 0.053 ms
(max 30 ms scheduling outlier → P9 RT-priority debt), end-to-end grab→fire
p50 1.44 ms, 5/2176 requests coalesced.

Status: Done
