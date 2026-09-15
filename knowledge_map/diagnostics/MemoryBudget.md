# MemoryBudget

> Byte-budgeted ownership reporting for every host-path memory owner
> (issue #370): who keeps frame memory alive, how much, its peak, its
> declared bound, and how well the number is known.

**Source:** `include/backend/diagnostics/MemoryBudget.h` (header-only,
Qt-free, OpenCV-free), `include/backend/processing/ExperimentFrameBuffer.h`,
`src/backend/processing/ExperimentFrameBuffer.cpp`
**Related:** [[../services/ProcessingService]], [[../data-model/FrameStore]],
[[../architecture/AppBackend]] (`memoryBudgetSnapshot()`),
[[../frontend/MainWindow]] (Diagnostics dialog), [[CrashStateMirror]]

## Model

- **`MemoryOwnerStats`** — one record per owner: `name` (stable id such as
  `processing.experimentBuffer`), `knowledge` (`Measured` = counted from the
  owner's own allocations, `Estimated` = derived upper bound such as
  count × frame bytes, `Unknown` = not observable, e.g. vendor SDK memory),
  `currentBytes` / `peakBytes`, `currentCount` / `peakCount`,
  `capacityBytes` / `capacityCount` (0 = no declared bound),
  `evictedByBudget` (items dropped or replaced to honour the bound —
  owner-specific meaning), `note`. `overBudget()` is true only when a byte
  budget is declared and exceeded. Unknown is never rendered as a measured
  zero.
- **`ByteAccountant`** — relaxed-atomic current/peak/evicted counters for
  hot paths (`add` / `remove` / `set` / `noteEvicted` / `snapshot(...)`);
  a lost race can under-report a peak by one item, never over-report.
- **`HostMemoryBudgetSnapshot`** — whole-process view assembled by
  `AppBackend::memoryBudgetSnapshot()`: sample time, process RSS / peak
  RSS (0 where the platform helper is unsupported), the owner list,
  `accountedBytes()` (measured + estimated), `hasUnknownOwner()`, `find()`.

**Counting rule.** Images are refcounted `cv::Mat`s shared between owners
(the frozen-Mats invariant). Each owner reports what *it alone keeps
alive*, so a frame retained by both the monitoring ring and the experiment
buffer appears in both — per-owner numbers are upper bounds and the total is
an upper bound on what those owners keep resident together.

## Owners reported

| Owner | Knowledge | Bound | Where |
|---|---|---|---|
| `capture.sdkBuffers` | Estimated (SDK input-buffer count × last frame payload) or Unknown | count from telemetry | [[../architecture/AppBackend]] |
| `playback.frameStore` | Measured (slot allocations, resident even when stale) | capacity × reserved frame bytes | [[../data-model/FrameStore]] `memoryStats()` |
| `processing.experimentBuffer` | Measured | `maxBufferedFrames` **and** `maxBufferedBytes` (default 512 MiB) | `ExperimentFrameBuffer` |
| `processing.monitoringRings` | Measured | 2 × 1000 entries (bytes follow the frame geometry) | `FrameRingBuffer` |
| `processing.batchQueue` | Measured | `maxQueuedFrames` and `maxQueuedBytes` (default 256 MiB) | async batch pipeline |
| `processing.flushQueue` | Measured | 3 batches in flight | `HdfWriteQueue<ExperimentBatch>` |
| `processing.snapshot` | Measured | 1 | latest realtime snapshot mask |
| `export.jobs` | Estimated (0: streams one frame at a time, #344) | n/a | `HdfExportService` / Python exporter |

## `ExperimentFrameBuffer`

Extracted from `ProcessingService` so the retention/backpressure policy
has one owner and is testable without the realtime pipeline. Bounded by
frames **and** bytes (`Policy{maxFrames, maxBytes}`); `append(frame,
isValid)` returns `AppendResult{stored, droppedValid, droppedInvalid,
bufferedAfter, bytesAfter}` so the caller can account for every eviction
(`persistenceCancelledByPolicy` in [[../services/ProcessingService]]).
Policy (unchanged from the old in-service code): a tightened policy is
applied on the next admission; sampled invalid frames are evicted first
(oldest first); a valid frame evicts invalid frames to enter but never
another valid frame; an invalid newcomer is refused when the buffer is
full. `takeAll()` moves frames out for a flush (refcount transfers, no
copies); `processedFrameBytes()` counts original + mask + extra series
images (the trigger image shared with `originalImage` is counted once).

## Guards and evidence

- `processing.memory_budget` (`tests/processing/memory_budget_test.cpp`,
  deterministic; also run under ThreadSanitizer): accountant semantics,
  buffer byte budget + frame cap + eviction order + takeAll, FrameStore
  reserve/plateau/resize accounting, batch-queue byte budget drops counted
  separately, per-object sharing (one source + one mask allocation per
  frame, science unchanged), realtime monitoring plateau behind a consumer
  that never reads, experiment byte budget reconciled with the run
  accounting, one presentation snapshot.
- `performance.memory_budget` (`tests/performance/memory_budget_bench.cpp`):
  copies per object, slow-persistence plateau, larger frames (2048×1088)
  bounded by bytes before the frame cap, long monitoring session, FrameStore
  under EveryFrame and drop-to-latest consumers, 1M-frame metadata stress,
  multi-image series retention. Writes JSON to `$MIB_MEMORY_BENCH_REPORT`;
  the committed run is `docs/evidence/2026-09-07-memory-budget/`.

## Gotchas

- `processing.monitoringRings.capacityBytes` is 0 on purpose: the rings are
  count-bounded and their bytes follow the ROI geometry; the plateau is what
  the guard asserts.
- `FrameStore` retained bytes are slot *allocations* (`capacity()`), which
  is what stays resident; a stale slot still counts.
- The experiment byte budget is a declared drop policy exactly like the
  frame cap: exceeding it during a run yields `persistenceCancelledByPolicy`
  and the run cannot be reported Complete without those drops being
  explained (issue #367). Configure it per profile with
  `experiment_buffer_max_mb` (0 = count-only).

**Up**: [[_MOC|Diagnostics MOC]]
