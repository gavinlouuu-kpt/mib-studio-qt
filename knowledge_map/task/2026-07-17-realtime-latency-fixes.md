# 2026-07-17 — Realtime latency fixes: event-driven wake + trigger request queue

Fixes for the two findings measured by
[[2026-07-17-pipeline-latency-instrumentation]] (GitHub issues #282, #283).

**#282 — event-driven realtime wake.** The realtime loops slept a fixed
2 ms whenever caught up with the FrameStore write head, putting a uniform
0–2 ms wait in front of every frame (~70% of median end-to-end latency).
New `FrameStore::waitForFrame(lastSeenTotal, timeout)`: consumers block on
a condition variable that `pushFrame` notifies after the slot copy, behind
a Dekker-guarded waiter counter (seq_cst pairing on
`totalWritten_`/`waitWaiters_`), so the producer pays one relaxed atomic
load per push while nobody waits and no wakeup can be lost. Both
`realtimeInlineLoop` and `realtimeBatchLoop` use it (2 ms timeout keeps
stop paths responsive; the 5 ms empty-store sleep also converted).

**#283 — per-request trigger queue.** `TriggerService`'s single-bool
`triggerRequested_` silently coalesced target-group requests arriving
mid-pulse (measured 5/2176). Replaced with a bounded
`std::deque<PendingRequest>` (capacity `kMaxPendingRequests` = 8) under the
existing `triggerMutex_` (lost-wakeup discipline preserved): every request
fires its own pulse in arrival order; overflow drops the OLDEST entry,
counted via `getDroppedRequestCount()` + throttled WARN.
`TriggerTimingRecord.coalesced` is now always 0.

**Measured (mock harness, 500 fps × 20 s, `gavinlouuu/512x96stream`,
before → after):** grab→algo-start p50 1.03 → 0.083 ms (12×), p95 2.01 →
0.138 ms; end-to-end grab→fire p50 1.44 → 0.50 ms, p95 2.41 → 0.70 ms;
trigger accounting exact (2122 target frames → 2122 pulses, 0 coalesced,
0 dropped); zero frame drops, accounting conserved. Remaining multi-ms
tails are OS scheduling (#227 / P9 RT-priority debt).

**Tests:** new `backend.frame_store_wait` (immediate return, timeout,
wake-on-push, lost-wakeup stress); `integration.e2e_pipeline_timing`
phase 3 now asserts zero coalescing/drops for paced requests and a new
phase 4 asserts bounded-queue overflow conservation (pulses + dropped ==
burst; newest request always fires).

**Files:** `FrameStore.{h,cpp}`, `ProcessingService.cpp`,
`TriggerService.{h,cpp}`, tests, vault notes, howto before/after table.

Status: Done
