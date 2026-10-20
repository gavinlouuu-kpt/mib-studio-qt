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
  indices restart with a run and with a capture change, so the epoch moves on at the moment the
  capture changes (see **Captures** below); it starts at 1, only grows and never repeats, so no two
  held rows share an identity. `start_generation` is the same one the experiment commands use
  (ABI 39/40).
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
- **Pins.** `pin(key)` (`key.startGeneration` = the run the caller expects) in this order: a
  key whose `captureEpoch` is not the current one is **`CaptureMismatch`**, before every other
  rule; the expected run must be 0 or the latest run started (`scopeGeneration`), else
  `RunMismatch`; an existing pin is `AlreadyPinned`; 16 pins or 4 MiB of pinned pixels is
  `LimitReached` (a pin is never evicted to make room); the ring is read and its current capture
  checked again (a change in between is `CaptureMismatch` too); a strip entry with the exact key is
  shared (immutable, no new crop); otherwise the ring row with that tag is cropped and owned
  (`Pinned`); otherwise the bounded row index (4096 rows the snapshot served, `noteRows`) gives
  `PinnedWithoutImage` with the recorded measurements and a plain reason; a row found under another
  tag is `RunMismatch`; nothing at all is `NotFound`. Admission re-checks the scope and the capture,
  so a run start or a capture change while a pin is taken refuses it. `unpin(key)` and
  `clearPins()` are explicit; otherwise pins leave only at the next run start and at a capture
  change.
- **Captures (review round 2 of #749).** Pins belong to the capture that made them. The capture
  epoch moves on at the moment of the change, before any frame of the new capture is processed:
  `startRealtime()` (possibly on a replacement store), `setRealtimeRoi()` with another ROI and a
  pending ROI being applied, and `advanceMonitoringCapture()` from AppBackend when it replaces the
  FrameStore (MindVision / Aravis Overview <-> Experiment, realtime running or not), when the camera
  opens or closes a capture session, and from the facade after a frame buffer resize. Each advance
  notifies the store (`captureChanged(epoch, reason)`, the listener AppBackend wires): the strip, the
  pins and the row index of earlier captures are dropped at once and the reason is kept
  (`Listing.lastCaptureChange`, JSON `last_capture_change`). Late or repeated notifications change
  nothing (the highest epoch wins); a refresh under way when the capture changes admits nothing.
  As a backstop the append path moves the epoch, without a notification, on the first row whose
  frame has another store generation or capture session than the previous row's; the store applies
  that change at its next refresh or pin. Every read path compares the key's epoch with the current
  one (`MonitoringCellSource::currentCaptureEpoch()`, an atomic read before the store mutex) first,
  even before the store has applied a change: `pin` is `CaptureMismatch`, `image` is refused with
  `captureMismatch` and a plain reason ("image refused: frame F object O is from capture E, but the
  current capture is C (the camera stopped); pins belong to the capture that made them, ..."),
  `unpin` says the pin was dropped, and `listing()` lists only cells of the current capture. Review
  round 1 had kept earlier-capture pins and served their images under their own identity; round 2
  found that an old strip image stayed served until the next refresh and an old pin forever, and
  that a mode switch with realtime stopped or an ROI change did not move the epoch at all.
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
`unpin` → `ReadResult{ok, captureMismatch, message}`, `clearPins`, `captureChanged(epoch, reason)`,
`noteRows`, `listing()` → `Listing{scopeGeneration, ringGeneration, ringCaptureEpoch (the current
capture), revision, strip, pins, lastCaptureChange, stats, policy}`, `image(key, mask, out, info)` →
`ReadResult` (a private copy of the owned crop, or the refusal with its reason), `stats`,
`memoryStats` (owner `monitoring.cellStore`), static `cropRect`. `MonitoringCellSource` is the seam
(`currentCaptureEpoch`, `newestValid`, `find`, `releaseShares`): production
`makeProcessingCellSource(ProcessingService&)` over `visitMonitoringNewest`; tests use a fake ring.
`PinOutcome` values (0 Pinned … 5 LimitReached, 6 CaptureMismatch) and the caps are pinned by `static_assert`s in
`crates/mib-bridge/src/shim.cpp` and by `bridge-contract.json` `monitoring_cells` /
`monitoring_cell_pin_outcomes`.

## Threading

- Nothing runs on the acquisition / processing path: the only change there is the 24-byte tag the
  ring stores per append (run, capture epoch, geometry origin; two compares for the epoch's
  backstop), under the lock it already holds.
- A capture change is applied on the thread that made it (the UI or bridge thread for a mode
  switch, an ROI change or a resize; the capture thread or the stopping thread for a camera session;
  the realtime thread once when a pending ROI is applied, never during a run), outside the ring
  lock: lock order listener mutex → store mutex (and coordinator mutex → listener mutex when the
  change happens inside a coordinator transaction). The store never calls out under its mutex, and
  frees the dropped copies after releasing it. `setMonitoringCaptureListener({})` waits for a
  running notification, so AppBackend clears it before the store can be destroyed (shutdown,
  re-initialize).
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
- Measured (`performance.monitoring_cell_hot_path`, 512x96 realtime loop, this host, after review
  round 2): ring lock hold median ~1.4 µs for a strip read, ~6.5 µs for a pin lookup, ~1.5 µs to
  give the shares back (~39 µs in the worst case, when the ring evicted all 24 rows meanwhile and
  the reader frees 48 buffers under the lock, as an eviction would); append latency p50 unchanged
  (104–106 ns), p99 187–207 ns with a client polling vs 126–150 ns with none; realtime throughput
  ratio 0.983–1.011.
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
  no-image reasons, capture change before the store hears of it, capture notifications, geometry
  origin), `e2e.monitoring_cells_facade` (facade + coordinator + JSON/image agreement, a camera
  session opening and a realtime start as new captures, a batch row's ROI geometry, and at the
  moment of each change: another ROI, the camera stopping, a buffer resize, a MindVision switch to
  Overview with realtime stopped), `processing.monitoring_cell_pipeline` (the real realtime loop:
  crops hold their own marker pixels with the ROI at (40, 12) in both modes; a restart on a
  replacement store refuses A's strip and pinned images at once; a resize with recurring indices),
  `processing.monitoring_cell_concurrency`, `performance.monitoring_cell_hot_path`; Rust
  `contract.rs` `monitoring_cells_end_to_end` (an ROI change refuses the old images),
  `dispatch.rs` `monitoring_cells_through_dispatch` (`CAPTURE_MISMATCH` after a camera stop),
  `ws.rs` `live_cells_over_the_websocket`; `monitoringCells.test.ts`.
