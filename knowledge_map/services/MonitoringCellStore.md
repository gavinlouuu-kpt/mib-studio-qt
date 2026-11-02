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
  when the row's frame entered processing (0 = outside a run), the **capture epoch** the frame
  entered with, the processing frame index and the object id within that frame. [[ProcessingService]]
  decides both tags once, when the frame enters processing, carries them with the frame and its
  results, and stores them in the ring slot at append (`MonitoringRowTag`). Frame indices restart
  with a run and with a capture change, so the epoch moves on at the moment the capture changes (see
  **Captures** below); it starts at 1, only grows and never repeats, so no two rows of one capture
  share an identity. Epoch 0 marks a row whose frame crossed a capture change: a monitoring row, never
  a cell. `start_generation` is the same one the experiment commands use (ABI 39/40).
- **Geometry.** Centroid, bounding box and the crop origin are in the coordinates of the row's
  image, the processing ROI, in both realtime modes. Inline rows are measured on the ROI image;
  async-batch rows are measured on the full frame and keep an ROI crop of it (the ROI the worker
  used), so their tag carries the ROI's top-left and the source subtracts it
  (`monitoringCellMetrics(v, ts, origin)`). The bridge snapshot rows use the same convention.
  Review round 1 of #749 found async-batch crops shifted by the ROI origin (background or the
  neighbouring object); `processing.monitoring_cell_pipeline` pins the fix on pixels.
- **Strip.** `refreshStrip()` (called by `fetch_monitoring_cells`) asks the source for the newest
  `stripMaxCells` valid rows, crops the new ones **outside every lock**, then admits them under the
  store mutex: rows of another run context (another run, rows outside a run while one runs) are
  dropped, and a row whose carried capture is not the current one is never admitted (the insert
  rule, checked under the store mutex); the strip is trimmed oldest-first to 24 cells and 4 MiB,
  every eviction counts in `stripEvicted`. A batch that raced a capture change, a `beginRun` or a
  `clearStrip` is discarded.
- **Pins.** `pin(key)` (`key.startGeneration` = the run the caller expects) in this order: a
  key whose `captureEpoch` is not the current one is **`CaptureMismatch`**, before every other
  rule; the expected run must be 0 or the latest run started (`scopeGeneration`), else
  `RunMismatch`; an existing pin is `AlreadyPinned`; 16 pins or 4 MiB of pinned pixels is
  `LimitReached` (a pin is never evicted to make room); the ring is read (only a row whose frame
  carried this capture is this identity); a strip entry with the exact key is
  copied under the store mutex (no new crop; the pin owns its copy, never a share of the strip's,
  #789); otherwise the ring row with that tag is cropped and owned
  (`Pinned`); otherwise the bounded row index (4096 rows the snapshot served, `noteRows`) gives
  `PinnedWithoutImage` with the recorded measurements and a plain reason; a row found under another
  tag is `RunMismatch`; nothing at all is `NotFound`. Admission re-checks, under the store mutex,
  the scope and that the row's carried capture is the current one, so a run start or a capture
  change while a pin is taken refuses it. `unpin(key)` and `clearPins()` are explicit (unpin also
  removes a pin whose capture ended); otherwise pins leave only at the next run start.
- **Captures (review rounds 2-3 of #749): one authority.** Pins belong to the capture that made
  them. The store owns the current capture epoch, and every capture change goes through one call,
  `advanceCapture(reason[, cameraSessionFloor])`: from [[ProcessingService]] (through the capture
  authority AppBackend wires) at a realtime start, a **realtime stop** and another processing ROI
  (set, or a pending ROI applied, both under the ROI lock); from AppBackend when it replaces the
  FrameStore (MindVision / Aravis Overview <-> Experiment, realtime running or not) and when the
  camera opens (floor = the new session) or closes (floor = session + 1) a capture session; from the
  facade after a frame buffer resize; and `beginRun` for a run start. Under the store mutex, in one
  critical section: the epoch moves on and is **published** to the source
  (`MonitoringCellSource::publishCapture`, atomic stores into ProcessingService, the only call out
  under the mutex), the strip and the row index are cleared, and every pin of the capture that
  ended is **ended**: it keeps its identity and measurements, its pixels are released and it says
  "no image: capture E ended (reason); the pin keeps its measurements" until it is unpinned (a run
  start drops the pins instead). `Listing.lastCaptureChange` (JSON `last_capture_change`: epoch,
  reason, `pins_ended`, `pins_dropped`, `strip_dropped`) says why.
  **Tag at entry.** A frame's capture is decided when it enters processing: the published epoch is
  read with the ROI under the ROI lock (where an ROI change also moves the epoch), the FrameStore's
  generation is bound to the epoch when it moves, and the frame must be of that generation and of a
  camera session at or above the published floor; an async-batch frame keeps it only if the worker
  measures it with the ROI read at its entry. Anything else enters with epoch 0. Nothing assigns
  "the current epoch" when a frame completes, so a frame that entered before a change and completes
  after it carries the old epoch, and **every insert** (strip admission, pin admission, the row
  index) compares the carried epoch with the current one under the store mutex and drops it
  (`SPDLOG_DEBUG`). **Every read** checks the key's epoch against the current one in the same
  critical section as its copy: `image()` (check, lookup, copy) and `listing()` (the current
  capture, then the cells) are each one critical section, so a change either precedes a read
  (refused) or follows it, never interleaves. `pin` of an old key is `CaptureMismatch` (an ended
  pin is not `AlreadyPinned`), `image` is refused with `captureMismatch` and a plain reason ("image
  refused: frame F object O is from capture E, but the current capture is C (realtime processing
  stopped); pins belong to the capture that made them, ..."). Round 2 sampled the epoch before the
  mutex and tagged rows at completion (a frame held across an ROI change entered the new capture,
  a closed camera session's late frame adopted the next one), and `stopRealtime()` left the capture
  valid; round 3 replaced the listener, the append-path backstop and the lock-free samples with
  this design (Codex round 3 on #749).
- **Run start.** `beginRun(g)` (the coordinator, when the run reaches Active) is a capture change
  ("a run started") that sets the scope and drops the pins instead of ending them; the copies are
  released after unlocking.
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
`unpin` → `ReadResult{ok, captureMismatch, message}`, `clearPins`,
`advanceCapture(reason[, cameraSessionFloor])` → the new epoch, `currentCapture()`, `noteRows`,
`listing()` → `Listing{scopeGeneration, ringGeneration, ringCaptureEpoch (the current capture),
revision, strip, pins (ended ones without an image), lastCaptureChange, stats, policy}`,
`image(key, mask, out, info)` → `ReadResult` (a private copy of the owned crop, or the refusal with
its reason), `stats`, `memoryStats` (owner `monitoring.cellStore`), static `cropRect`, and the test
seam `setReadHookForTests` (called under the mutex where `image()` / `listing()` have resolved what
they serve). `MonitoringCellSource` is the seam (`currentCaptureEpoch`, `publishCapture`,
`newestValid`, `find`, `releaseShares`): production `makeProcessingCellSource(ProcessingService&)`
over `visitMonitoringNewest` and `publishMonitoringCapture`; tests use a fake ring.
`PinOutcome` values (0 Pinned … 5 LimitReached, 6 CaptureMismatch) and the caps are pinned by `static_assert`s in
`crates/mib-bridge/src/shim.cpp` and by `bridge-contract.json` `monitoring_cells` /
`monitoring_cell_pin_outcomes`.

## Threading

- Nothing runs on the acquisition / processing path beyond the entry tag (per frame: one atomic
  load of the published epoch, two more for the session floor and the run, two compares; the ROI
  lock and a FrameStore read only when the epoch moved) and the 24-byte tag the ring stores per
  append (run, capture epoch, geometry origin), under the lock it already holds.
- A capture change is applied on the thread that made it (the UI or bridge thread for a mode
  switch, an ROI change, a realtime start or stop, a resize; the capture thread or the stopping
  thread for a camera session; the realtime thread once when a pending ROI is applied, never during
  a run), outside the ring lock. Lock order: coordinator → ROI lock (`rtMutex_`) → ProcessingService's
  capture authority mutex → store mutex. Under its mutex the store calls out only to
  `publishCapture` (atomic stores), and it frees the released copies after unlocking.
  `setMonitoringCaptureAuthority({})` waits for a running call, so AppBackend clears it before the
  store can be destroyed (shutdown, re-initialize).
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
- **One owner per store copy (#789).** Until #789 a pin of a strip cell took a refcount share of
  the strip's copy. When a refresh dropped that strip cell (the ring moved to another run; pins
  stay until the next run starts) it freed its share after unlocking; a reader that copied the
  pin's pixels meanwhile and an unpin then left the refresh holding the last share, so the free
  was ordered after the read by the refcount only. TSan reported it as a race in
  `processing.monitoring_cell_concurrency` (free in `refreshStrip` vs `cv::Mat::copyTo` in
  `image()` under the store mutex), about 1 run in 8 with 12 copies in parallel and never seen
  idle. A pin now copies the strip's crop under the mutex (≤ 2 × 64 KiB, off the hot path), so
  every buffer has one owner and the remover's free follows every read by the mutex.
  `backend.monitoring_cell_store` "a pinned strip cell owns its copy" reproduces the interleaving
  deterministically through the `refresh-dropped` read hook (fails 5/5 under TSan with a share).
- Measured (`performance.monitoring_cell_hot_path`, 512x96 realtime loop, this host, after review
  round 3): ring lock hold median 1.3–1.5 µs for a strip read, 6.4–7.0 µs for a pin lookup,
  ~1.5 µs to give the shares back (47–50 µs median when the ring evicted all 24 rows meanwhile and
  the reader frees 48 buffers under the lock, as an eviction would); append latency p50 105–115 ns
  and p99 200–218 ns with a client polling (round 2: 104–106 / 187–207); realtime throughput ratio
  0.94–1.04 over six runs on a loaded host (load average 14–20), median ~0.99.
  `processing.monitoring_cell_concurrency` (TSan lane) races the real realtime loop against two strip
  clients, a snapshot reader, a pinner and a run switcher.

## Gotchas

- Pins are **one shared set per instrument**: pin / unpin / clear are control commands on the
  WebSocket server (a viewer gets `VIEWER_ONLY`); reads and image pulls are not.
- The strip refreshes on `fetch_monitoring_cells`; nothing refreshes it in the background, so a
  shell that never reads it costs nothing. The ring itself only fills while Monitoring is active.
- Async-batch rows were also not counted in the ring's appended totals (a snapshot could not see
  their evictions); they are now, like inline rows.
- Tests: `backend.monitoring_cell_store` (caps, eviction, pins surviving eviction, run rules with
  the run start as a capture change, no-image reasons, the one-call capture change: published
  epoch, cleared strip and index, ended pins, old identities refused, a late row of the old capture
  never admitted nor indexed, unpin of an ended pin, the session floor published; and reads held by
  the read hook between resolving and copying while another thread changes the capture, for
  `image()` and `listing()`), `e2e.monitoring_cells_facade` (facade + coordinator + JSON/image
  agreement; a camera session opening, a realtime start and a run start as captures; at the moment
  of each change: another ROI, **realtime stopping with the camera running**, the camera stopping, a
  buffer resize, a MindVision switch to Overview with realtime stopped), `processing.monitoring_cell_pipeline`
  (the real realtime loop: crops hold their own marker pixels with the ROI at (40, 12) in both
  modes; a restart on a replacement store refuses A's images at once; renumbered frames are never
  cells until the resize's change; **a frame held by the append hook across an ROI change** in
  both modes keeps its entry capture and never becomes a cell; **stopping realtime while frames keep
  arriving** ends the capture; frames of a closed camera session never adopt the next capture),
  `processing.monitoring_cell_concurrency`, `performance.monitoring_cell_hot_path`; Rust
  `contract.rs` `monitoring_cells_end_to_end` (an ROI change refuses the old images, ends the pin),
  `dispatch.rs` `monitoring_cells_through_dispatch` (`CAPTURE_MISMATCH` after a camera stop, the
  ended pin listed and unpinned), `ws.rs` `live_cells_over_the_websocket`; `monitoringCells.test.ts`.
  Each round-3 test was checked against a mutant (completion-time tagging inline and in async
  batch, a copy outside the mutex in `image()`, a listing split in two critical sections, a
  `stopRealtime()` without the change, the strip insert ignoring the carried epoch, the session
  floor or the store generation ignored at entry): every one fails.
