# MonitoringCellStore

> Backend-owned, bounded per-row cell images for the live Monitoring / Live views (bridge ABI 41,
> #716 A6a): a strip of the most recently detected cells and the operator's pins, each an owned
> crop (and mask) that outlives the monitoring ring. Every shell reads the same copies; pixels
> travel as frame packets, never in JSON.

**Source:** `src/backend/services/MonitoringCellStore.cpp`,
`include/backend/services/MonitoringCellStore.h`.
**Related:** [[ProcessingService]] (the ring and its run tags), [[../architecture/AppBackend]]
(owner), [[../architecture/ExperimentCoordinator]] (run start), [[../architecture/Rust-Bridge]]
(ABI 41 commands), [[../architecture/Desktop-Shell]], [[../diagnostics/MemoryBudget]].

## Responsibility

- **Identity.** A cell is `(start_generation, capture_epoch, frame_index, object_id)`: the run active
  when the row was appended to the monitoring ring (0 = outside a run), the **capture epoch** of its
  frame, the processing frame index and the object id within that frame. Both tags are written by
  [[ProcessingService]] under the ring lock the append already holds (`MonitoringRowTag`). Frame
  indices restart with a run and with a capture change: a new FrameStore (camera mode switch,
  Overview <-> Experiment) or a store that renumbers (resize). The epoch moves on at every
  `startRealtime()` and whenever a frame's store generation differs from the previous one; it starts
  at 1 and never repeats, so no two held rows share an identity. `start_generation` is the same one
  the experiment commands use (ABI 39/40).
- **Geometry.** Centroid, bounding box and the crop origin are in the coordinates of the row's
  image, the processing ROI, in both realtime modes. Inline rows are measured on the ROI image;
  async-batch rows are measured on the full frame and keep an ROI crop of it (the ROI the worker
  used), so their tag carries the ROI's top-left and the source subtracts it
  (`monitoringCellMetrics(v, ts, origin)`). The bridge snapshot rows use the same convention.
  Review round 1 of #749 found async-batch crops shifted by the ROI origin (background or the
  neighbouring object); `processing.monitoring_cell_pipeline` pins the fix on pixels.
- **Strip.** `refreshStrip()` (called by `fetch_monitoring_cells`) asks the source for the newest
  `stripMaxCells` valid rows, crops the new ones **outside every lock**, then admits them under the
  store mutex: rows of another context (another run, rows outside a run while one runs, or another
  capture) are dropped, the strip is trimmed oldest-first to 24 cells and 4 MiB, every eviction counts in
  `stripEvicted`. A batch that raced a `beginRun` / `clearStrip` (epoch changed) is discarded.
- **Pins.** `pin(key)` (`key.startGeneration` = the run the caller expects) in this order: the
  expected run must be 0 or the latest run started (`scopeGeneration`), else `RunMismatch`; an
  existing pin is `AlreadyPinned`; 16 pins or 4 MiB of pinned pixels is `LimitReached` (a pin is
  never evicted to make room); the ring is read first, and a key whose `captureEpoch` is not the
  ring's current one is **`CaptureMismatch`** (an earlier capture's identity is never resolved
  against the new capture's rows, nor against a strip entry not yet refreshed); a strip entry with
  the exact key is shared (immutable, no new crop); otherwise the ring row with that tag is cropped and owned
  (`Pinned`); otherwise the bounded row index (4096 rows the snapshot served, `noteRows`) gives
  `PinnedWithoutImage` with the recorded measurements and a plain reason; a row found under another
  tag is `RunMismatch`; nothing at all is `NotFound`. Admission re-checks the scope, so a run that
  starts while a pin is taken refuses it. `unpin(key)` and `clearPins()` are explicit; pins never
  leave on their own except at the next run start. A pin taken before a capture change stays and
  keeps serving its own pixels under its own identity (epoch included).
- **Run start.** `beginRun(g)` (the coordinator, when the run reaches Active) sets the scope, drops
  the strip, the pins and the row index, and releases the copies after unlocking.
- **No image, never fabricated.** A row without pixels (results-only instruments), a format other
  than 8/16-bit, or a row that left the ring before the pin carries its metadata and
  `noImageReason` ("no image: …"); the image commands return an empty packet.
- **Crop.** Bounding box grown by `max(8, longer side / 2)` per side, at most 256 square, shifted
  inside the source (the processing ROI) rather than shrunk; unknown box → centred on the centroid
  (else the image centre). Mono8: channel 0, 16-bit scaled by 1/256. The mask crop is the same
  rectangle of the processed mask when it has the image's size.

## Key APIs

`MonitoringCellStore(source[, Policy])`, `beginRun`, `scopeGeneration`, `refreshStrip`,
`clearStrip` (Monitoring Clear; pins stay), `pin(key)` → `PinResult{outcome, message, cell}`,
`unpin`, `clearPins`, `noteRows`, `listing()` → `Listing{scopeGeneration, ringGeneration,
ringCaptureEpoch, revision, strip, pins, stats, policy}`, `image(key, mask, out, info)` (a share of the owned copy;
the facade copies pixels after the store lock is released), `stats`, `memoryStats` (owner
`monitoring.cellStore`), static `cropRect`. `MonitoringCellSource` is the seam: production
`makeProcessingCellSource(ProcessingService&)` over `visitMonitoringNewest`; tests use a fake ring.
`PinOutcome` values (0 Pinned … 5 LimitReached, 6 CaptureMismatch) and the caps are pinned by `static_assert`s in
`crates/mib-bridge/src/shim.cpp` and by `bridge-contract.json` `monitoring_cells` /
`monitoring_cell_pin_outcomes`.

## Threading

- Nothing runs on the acquisition / processing path: the only change there is the 24-byte tag the
  ring stores per append (run, capture epoch, geometry origin; one compare for the epoch), under the
  lock it already holds.
- The ring lock is held only while the source copies scalars and cv::Mat headers; crops and clones
  happen outside every lock; then `releaseShares` drops the shares under the ring lock again
  (`ProcessingService::releaseMonitoringShares`); the store mutex is never held while the ring is
  read (lock order: never store → ring).
- **Why the release goes back under the ring lock, and why `image()` copies.** cv::Mat's reference
  count lives in the uninstrumented OpenCV library, so ThreadSanitizer cannot see that a buffer is
  freed only after its last reader let go. The CI `thread` lane flagged exactly that: two image
  pulls sharing one store copy, one reading while the other dropped the last share. The fix makes
  the order visible as a lock order. Ring shares are dropped under the lock the ring's evictions
  take. A store copy never leaves the store: `image()` hands out a private clone made under the
  store mutex (≤ 64 KiB), and the store's own copies are freed only by the thread that removed
  them under that mutex. Pinned by `backend.monitoring_cell_store` "shares go back" (no share of
  a ring image stays with the store after a refresh or a pin, refused or not; two `image()` calls
  give two private buffers).
- Measured (`performance.monitoring_cell_hot_path`, 512x96 realtime loop, this host): ring lock hold
  median ~1 µs for a strip read, ~6 µs for a pin lookup, ~1.5 µs to give the shares back (36 µs
  in the worst case, when the ring evicted all 24 rows meanwhile and the reader frees 48 buffers
  under the lock, as an eviction would); append latency p50 unchanged (~105 ns), p99
  160–220 ns with a client polling vs 133–182 ns with none; realtime throughput ratio 0.996–1.019.
  `processing.monitoring_cell_concurrency` (TSan lane) races the real realtime loop against two strip
  clients, a snapshot reader, a pinner and a run switcher.

## Gotchas

- Pins are **one shared set per instrument**: pin / unpin / clear are control commands on the
  WebSocket server (a viewer gets `VIEWER_ONLY`); reads and image pulls are not.
- The strip refreshes on `fetch_monitoring_cells`; nothing refreshes it in the background, so a
  shell that never reads it costs nothing. The ring itself only fills while Monitoring is active.
- Async-batch rows were also not counted in the ring's appended totals (a snapshot could not see
  their evictions); they are now, like inline rows.
- Tests: `backend.monitoring_cell_store` (caps, eviction, pins surviving eviction, run rules,
  no-image reasons, capture change, geometry origin), `e2e.monitoring_cells_facade` (facade +
  coordinator + JSON/image agreement, a realtime start as a new capture, a batch row's ROI
  geometry), `processing.monitoring_cell_pipeline` (the real realtime loop: crops hold their own
  marker pixels with the ROI at (40, 12) in both modes; a FrameStore swap and a resize with
  recurring indices), `processing.monitoring_cell_concurrency`, `performance.monitoring_cell_hot_path`; Rust
  `contract.rs` `monitoring_cells_end_to_end`, `dispatch.rs` `monitoring_cells_through_dispatch`,
  `ws.rs` `live_cells_over_the_websocket`; `monitoringCells.test.ts`.
