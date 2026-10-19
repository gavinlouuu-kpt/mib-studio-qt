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

- **Identity.** A cell is `(start_generation, frame_index, object_id)`: the run active when the row
  was appended to the monitoring ring (0 = outside a run, tagged by [[ProcessingService]] under the
  ring lock it already holds), the processing frame index and the object id within that frame.
  Frame indices restart with every run, so the run's `start_generation` is what makes the key stable
  across runs; it is the same `start_generation` the experiment commands use (ABI 39/40).
- **Strip.** `refreshStrip()` (called by `fetch_monitoring_cells`) asks the source for the newest
  `stripMaxCells` valid rows, crops the new ones **outside every lock**, then admits them under the
  store mutex: rows of another context (another run, or rows outside a run while one runs) are
  dropped, the strip is trimmed oldest-first to 24 cells and 4 MiB, every eviction counts in
  `stripEvicted`. A batch that raced a `beginRun` / `clearStrip` (epoch changed) is discarded.
- **Pins.** `pin(expected, frameIndex, objectId)` in this order: `expected` must be 0 or the latest
  run started (`scopeGeneration`), else `RunMismatch`; an existing pin is `AlreadyPinned`; 16 pins or
  4 MiB of pinned pixels is `LimitReached` (a pin is never evicted to make room); a strip entry is
  shared (immutable, no new crop); otherwise the ring row with that tag is cropped and owned
  (`Pinned`); otherwise the bounded row index (4096 rows the snapshot served, `noteRows`) gives
  `PinnedWithoutImage` with the recorded measurements and a plain reason; a row found under another
  tag is `RunMismatch`; nothing at all is `NotFound`. Admission re-checks the scope, so a run that
  starts while a pin is taken refuses it. `unpin(key)` and `clearPins()` are explicit; pins never
  leave on their own except at the next run start.
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
`clearStrip` (Monitoring Clear; pins stay), `pin` → `PinResult{outcome, message, cell}`,
`unpin`, `clearPins`, `noteRows`, `listing()` → `Listing{scopeGeneration, ringGeneration,
revision, strip, pins, stats, policy}`, `image(key, mask, out, info)` (a share of the owned copy;
the facade copies pixels after the store lock is released), `stats`, `memoryStats` (owner
`monitoring.cellStore`), static `cropRect`. `MonitoringCellSource` is the seam: production
`makeProcessingCellSource(ProcessingService&)` over `visitMonitoringNewest`; tests use a fake ring.
`PinOutcome` values (0 Pinned … 5 LimitReached) and the caps are pinned by `static_assert`s in
`crates/mib-bridge/src/shim.cpp` and by `bridge-contract.json` `monitoring_cells` /
`monitoring_cell_pin_outcomes`.

## Threading

- Nothing runs on the acquisition / processing path: the only change there is the 8-byte tag the
  ring stores per append, under the lock it already holds.
- The ring lock is held only while the source copies scalars and cv::Mat headers; crops and clones
  happen outside every lock; the store mutex is never held while the ring is read (lock order:
  never store → ring).
- Measured (`performance.monitoring_cell_hot_path`, 512x96 realtime loop, this host): ring lock hold
  median ~1 µs for a strip read, ~6 µs for a pin lookup; append latency p50 unchanged (~105 ns), p99
  160–220 ns with a client polling vs 133–182 ns with none; realtime throughput ratio 0.996–1.019.
  `processing.monitoring_cell_concurrency` (TSan lane) races the real realtime loop against two strip
  clients, a snapshot reader, a pinner and a run switcher.

## Gotchas

- Pins are **one shared set per instrument**: pin / unpin / clear are control commands on the
  WebSocket server (a viewer gets `VIEWER_ONLY`); reads and image pulls are not.
- The strip refreshes on `fetch_monitoring_cells`; nothing refreshes it in the background, so a
  shell that never reads it costs nothing. The ring itself only fills while Monitoring is active.
- The async batch-pipeline rows use the full-frame coordinate space; their crops are clamped to the
  image like any other row.
- Tests: `backend.monitoring_cell_store` (caps, eviction, pins surviving eviction, run rules,
  no-image reasons), `e2e.monitoring_cells_facade` (facade + coordinator + JSON/image agreement),
  `processing.monitoring_cell_concurrency`, `performance.monitoring_cell_hot_path`; Rust
  `contract.rs` `monitoring_cells_end_to_end`, `dispatch.rs` `monitoring_cells_through_dispatch`,
  `ws.rs` `live_cells_over_the_websocket`; `monitoringCells.test.ts`.
