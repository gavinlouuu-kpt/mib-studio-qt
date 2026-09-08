# ProcessingService

> The heart of the analysis pipeline. Runs OpenCV-based detection, classifies
> frames valid/invalid, accumulates monitoring + experiment buffers, feeds
> ring-ratio to [[AutofocusService]] and target-group events to
> [[TriggerService]].

**Source:** `src/backend/processing/ProcessingService.cpp`,
`src/backend/processing/BundledProcessingKernel.cpp`,
`src/backend/processing/ProcessingCoreLoader.cpp`,
`src/backend/processing/ProcessingCoreCache.cpp`,
`include/backend/processing/ProcessingService.h`,
`include/backend/processing/ProcessingCoreAbi.h`
**Related:** [[CaptureService]], [[Hdf5Service]], [[AutofocusService]],
[[TriggerService]], [[../architecture/Data-Flow]],
[[../domain/Microscopy-Pipeline]]

**Build target:** compiled into `mib_processing` (Qt-free static library;
see `src/backend/CMakeLists.txt`), not `mib_backend` directly. `mib_backend`
links `mib_processing` publicly, so nothing about consuming this service from
the desktop app changes. This is the portable core a non-Qt consumer (e.g.
Biowork's `services/mib-processing`) can build and link standalone — see
`docs/gold_standard_metrics.md` ("Portable Processing Contract").

The desktop can also load a signed, versioned `mib_processing_core` native
plugin through the stable C ABI in `ProcessingCoreAbi.h`. The ABI contains no
C++, Qt, OpenCV containers, exceptions, RTTI, or cross-module allocation: the
host supplies borrowed Gray8 image views and owns the output buffer. The
bundled implementation and plugin adapter share `IProcessingKernel`, so the
same mask/empty-frame algorithm is used on both sides of the boundary.

## Processing-core selection

- `activateProcessingKernel(kernel)` swaps the selected kernel only at a safe
  between-operation boundary. It rejects active leases first, then resets the
  swap while realtime, an experiment, recording/export, the async batch
  pipeline, or a synchronous offline batch owns an operation lease. An optional
  pre-commit callback runs under the selection lock after all of those guards
  and immediately before the pointer swap. The desktop uses it to synchronize
  the exact `QSettings` selection; a false return or exception preserves the
  previous usable kernel and does not mark it unavailable. The callback must
  not re-enter `ProcessingService`.
  A successful swap clears experiment/monitoring accumulation, realtime
  background and snapshot data, motion history, and bumps the config version;
  rejected reactivation never resets the live context. The watchdog-protected
  activation stress test exercises concurrent processing and repeated A→B→A.
- `activeProcessingCoreIdentity()` returns the exact selected version,
  contract, engine ABI, artifact/manifest hashes, release tag, build ID,
  runtime fingerprint, and source. `processBatch(..., processingCore)` captures
  that identity under the same selection lock for provenance.
- `MIB_STUDIO_PROCESSING_CORE_VERSION` is an administrator hard pin. A
  different candidate cannot activate, and experiment/mask/empty-frame paths
  fail closed while the pin is unsatisfied.
- `CoreOperationLease` holds the selected kernel and exact identity for the
  whole realtime, async/synchronous batch, raw-recording, or buffer-export
  operation. Activation cannot interleave after work starts and before its
  provenance is finalized.
- Loaded modules remain resident until process exit. A dynamic kernel leases a
  single-owner ABI context per call from a protected pool; parallel workers do
  not invoke one plugin context concurrently.
- Registry, cache, signature, and UI behavior live in
  [[../frontend/ProcessingCoreDialog]].

## Threads

- **Worker pool** (`start(size_t n = hardware_concurrency())`) — generic
  `Job` queue (not heavily used at present; realtime loop carries most work).
- **Realtime thread** (`startRealtime(frameStore)`) — consumes FrameStore
  by write-index; when caught up it blocks in
  `FrameStore::waitForFrame` (event-driven wake from `pushFrame`, issue
  #282 — the old fixed 2 ms sleep-poll put a uniform 0-2 ms wait in front
  of every frame and dominated end-to-end latency). Processes every frame
  or only latest depending on `setRealtimeDropFrames`. Experiments force
  every-frame.
- **Async batch workers** (`startBatchPipeline`) — consume a bounded frame
  queue in configured-size batches. `enqueueBatchFrame` returns immediately
  with accepted/dropped status so capture can keep running while workers process
  behind it.

The destructor is self-sufficient: it calls `stopRealtime()` +
`stopBatchPipeline()` + `stop()`, so destroying the service with any thread
still live is safe (previously a joinable `realtimeThread_` at destruction
`std::terminate`d unless GUI teardown had called `stopRealtime()` first).
`isRealtimeRunning()` exposes the realtime thread state.

All three thread families contain exceptions instead of letting them escape
the thread entry function (which is `std::terminate`): worker jobs and batch
compute/callback sections log-and-drop the failing job/batch;
`realtimeLoop()` catches, cleans up the batch pipeline, sleeps 100 ms, and
restarts the loop (same policy as `CaptureService::run`). Verified by
`tests/processing/processing_fault_injection_test.cpp`.

## Pipeline (per frame)

1. The selected `IProcessingKernel` performs optional background subtraction,
   Gaussian blur, threshold, and morphology and returns the mask.
2. `filterProcessedImage`/`filterProcessedObjects` route through the selected
   kernel's `analyzeObjects` (A7): the shared science implementation lives in
   `src/backend/processing/ProcessingScience.cpp` and produces a
   `FilterResult`:
   - `deformability`, `area` (μm² via `pixelToMicronFactor_`),
     `areaRatio`, `ringRatio`, `youngsModulus` (LUT lookup)
   - `brightness` quantiles (Q1/Q2/Q3/Q4)
   - border check, single-inner-contour check, range gates
   - `isTargetGroup` (second gate for trigger-worthy frames)
3. Emits:
   - **Ring ratio** via `RingRatioCallback` → [[AutofocusService]].
   - **Target-group** event via `TargetGroupCallback` carrying owner identity
     (`objectId`, `trackId`) → [[TriggerService]].
   - **Background capture** (if auto-background enabled) via
     `BackgroundCaptureCallback` → UI notifier.

## Background accessor — zero-copy hot path

Two getters exist for the current background:

- `cv::Mat getRealtimeBackgroundGray()` — clones the background (safe for
  cold callers, e.g. `MainWindow`, `BufferSaveDialog`).
- `std::shared_ptr<const cv::Mat> getRealtimeBackgroundGrayShared()` — returns
  the shared_ptr directly (no clone). Use in hot paths where you only need to
  read the background. The pointed-to Mat is immutable; `setRealtimeBackgroundGray`
  always replaces the pointer atomically rather than mutating in place.

`getConfigVersion()` returns a monotonic counter bumped by `setProcessingConfig`
and `setRealtimeRoi`. Hot loops can compare against a cached version to skip
per-frame config reads.

`isFrameEmptyWithActiveKernel(frame, config, roi, shared_ptr<const cv::Mat>)`
extracts only the ROI pixels (no full-frame copy) and delegates the decision
to the selected kernel. Recording and buffer-save filtering use this path, so
they cannot silently drift from the active core.

Realtime empty-frame/auto-background decisions also call the selected kernel.
For the bundled legacy semantics the host supplies its pre-blurred current and
previous images with the ABI's absolute-difference flag, so the kernel owns the
final classification without changing the established result.

## Config — `ProcessingConfig`

All gates in one struct. Notable fields:

- `area_threshold_min/max` (μm²), `deformability_threshold_min/max`
- `ring_ratio_min/max` + `enable_ring_ratio_check`
- `empty_frame_pixel_threshold` — drives empty-frame skipping
- `auto_background_enabled` + `auto_background_empty_frames`,
  `auto_background_cooldown_frames`
- Target-group gate: `target_group_area_*`, `target_group_deformability_*`,
  `enable_target_group_emodulus` + `target_group_emodulus_*` (uses
  `EModulusLut`, which is now fed from the managed LUT cache prepared by
  `AppBackend` at startup)
- Multi-image mode: `multi_image_enabled`, `multi_image_count`

## Accumulation modes

- **Monitoring rings** — `monitoringValidFrames_` / `monitoringInvalidFrames_`,
  fixed 1000-frame capacity. **Gated** by `setMonitoringActive(bool)` (default
  off). When inactive, `appendRealtimeMonitoringFrame` returns immediately with
  no allocations. Wired to [[../frontend/ExperimentMonitoringTab]] show/hide —
  the rings only fill while that tab is visible. Stored frames share cv::Mat
  refcounts with the processing loop (no per-frame clone); consumers are
  read-only.
- **Experiment accumulation** — the extracted, bounded
  `ExperimentFrameBuffer` (`experimentBuffer_`, issue #370 —
  [[../diagnostics/MemoryBudget]]) populated while `experimentActive_` is
  true, bounded by **frames** (`maxBufferedFrames_`, derived from the flush
  interval) **and bytes** (`setMaxBufferedBytes`, default 512 MiB, config
  key `experiment_buffer_max_mb`; 0 = count-only). Every eviction is returned
  to `appendExperimentFrame` and accounted as `persistenceCancelledByPolicy`.
  `flushBufferedFrames(Hdf5Service&)`
  moves frames out with `takeAll()` (O(1) per Mat, refcount transfer),
  tracks the bytes in flight (`flushQueueBytes_`) and submits to a 3-slot [[Hdf5Service]] `HdfWriteQueue`
  whose writer thread does the slow append, so capture/processing never blocks
  on disk. The write queue is created lazily on first flush and torn down by
  `finishFlush()` at experiment stop (drains + joins before any direct HDF5
  write, so the file is never written by two threads at once). A write failure
  or queue overflow (disk too slow) is fatal: it fires `setFlushErrorCallback`
  (→ [[../architecture/AppBackend]] → UI stop + dialog) instead of the old
  silent trim-and-drop. `totalValidFlushed_` advances only on a confirmed write.
- **Frozen-Mats invariant**: every `cv::Mat` published from `realtimeInlineLoop`
  (`gray`, `mask`, `grayROI`, `grayFull`, `fullMask`) is freshly allocated per
  iteration and never written after publication. Experiment frames and monitoring
  ring entries share refcounts instead of cloning (PR3 clone elimination).
  Since issue #370 the per-object records of `processBatch` and the async
  batch workers share the frame's source + mask the same way (no clone per
  object; `processing.memory_budget` asserts one source and one mask
  allocation per frame and identical science).
  Consumers (`Hdf5Service::appendFrames`, monitoring/HDF readers, overlay) are
  all read-only. Enforced by comment at the top of `realtimeInlineLoop`.
- **Memory ownership report (issue #370)** — `memoryStats()` returns
  `MemoryStats{experimentBuffer, monitoringRings, batchQueue, flushQueue,
  snapshot}` as [[../diagnostics/MemoryBudget]] `MemoryOwnerStats`
  (current/peak bytes + counts, declared bounds, evictions/replacements).
  `FrameRingBuffer` tracks its bytes and replacements; the async batch queue
  has a byte budget (`RealtimeBatchSettings::maxQueuedBytes` /
  `BatchPipelineConfig::maxQueuedBytes`, default 256 MiB) whose drops are
  counted in `BatchPipelineStats::framesDroppedByByteBudget` (a subset of
  `framesDropped`) next to `currentQueueBytes` / `maxQueueBytes`.
- Invalid frame sampling rate defaults to 1-in-100 to bound HDF5 size.

## Snapshot model (PR4)

`latestSnapshot_` is `std::shared_ptr<const RealtimeSnapshot>`. Producers
(`realtimeInlineLoop`, `publishRealtimeBatchFrame`) build a new `RealtimeSnapshot`
outside `snapshotMutex_`, then pointer-swap inside the lock (O(1) hold time —
no full-frame memcpy under the mutex). Consumers call `getLatestSnapshot(out)`,
which copies the shared_ptr under the lock (O(1)) and reads fields outside.
`out.mask` is a shallow refcount share of the snapshot mask — no clone on the
read path. Snapshot is immutable; readers are safe without extra locking.

`configVersion_` is also bumped by `setRealtimeBackgroundGray` (in addition to
`setProcessingConfig` / `setRealtimeRoi`), so the realtime loop's hoisted-config
cache refreshes when the background changes.

## Metrics exposed

1-second rolling window: `getAlgoFps1s`, `getValidFps1s`, `getInvalidFps1s`,
`getAlgoAvgUs1s`. Plus `getBufferedFrameCounts()` for cheap UI/status polling,
`getTotalValidFlushed` for experiment totals, and dropped-frame counters for
the bounded experiment backlog.

**Identification funnel + loss** (`getIdentificationCounters` →
`IdentificationCounters`, monotonic, always-on, reset by `resetRealtimeMetrics`
/ `startExperiment`): the quantitative basis for "loss of target
identification". Accumulated once per frame in `accumulateIdentificationCounters`,
off the trigger-critical path, using the loop's cached config —
`framesProcessed`, `framesWithObjects`, `validObjects`, `invalidObjects`,
`targetGroupObjects`, `unservedTargetGroupObjects` (target-group objects
beyond the frame's first, which `selectTargetGroupTriggerOwner` never
dispatches a pulse for), and a per-reason invalid histogram
(`reasonCounts[6]`). The reason set comes from
`science::classifyInvalidReasons` (ProcessingScience) — the single source of
truth shared with the [[../frontend/ExperimentMonitoringTab]] tooltip so they
cannot disagree. Headline loss values mirror into
[[../diagnostics/CrashStateMirror]] (`processing` slot).

**Per-frame latency records** ([[../diagnostics/PipelineTimingRecorder]],
opt-in): the three realtime inline-loop paths capture host-monotonic
`algoStartUs`/`algoEndUs` next to the existing steady_clock stamps and hand a
`RealtimeFrameTiming{frameIndex, grabUs, algoStartUs, algoEndUs}` into
`publishRealtimeValidationCallbacks` — the shared chokepoint of all realtime
paths — which writes one `FrameTimingRecord` per processed frame and attaches
`frameIndex`/`hostTimestampUs` to the `TargetGroupEvent` so
[[TriggerService]] can measure end-to-end latency. Skips are counted by
reason (drop-to-latest, ring-behind, empty frame, kernel error, batch queue
rejected) so pushed == records + skips (frame accounting conserved; index 0
is the realtime loop's never-consumed sentinel). Skip ranges are counted
only when the consumer actually advances `rtLastProcessed_` past them — a
failed slot fetch (mid-write/evicted) retries without recounting, which
previously double-counted drop-to-latest skips on slow machines. The timing capture is
lock-free and keeps the callback-ordering invariant: nothing is taken before
the target-group callback.

## Batch processing (offline / re-runs)

For re-generating masks from stream images that are **not** coming from a
live camera — e.g., frames stored in an HDF5 experiment file or a folder of
TIFFs — use:

- `ProcessingService::computeProcessedFrame(grayInput, background, config, roi, index, ts)`
  — pure helper: blur → (optional) background subtract → threshold →
  morphology → `filterProcessedImage`. Zero side-effects (no monitoring
  rings, no experiment accumulation, no callbacks, no auto-background).
  Returns a `ProcessedFrame` with a full-size mask (zero outside ROI).
- `ProcessingService::processBatch(grayImages, config, background, roi, progressCb,
  processingCore)`
  — wraps `computeProcessedFrame` over a vector of grayscale `cv::Mat`
  inputs, returns `std::vector<ProcessedFrame>`. Progress is reported via
  `BatchProgressCallback(BatchProgress{done, total})`. The optional identity
  output records the exact core held for the whole operation; activation is
  blocked until the call returns.

  When `multi_image_enabled` and `multi_image_count > 1`, each newly retained
  valid track also carries the trigger image plus the available following
  source frames in `seriesImages`. The Python binding exposes these with
  `include_series_images=True`; issue #225's conformance harness hashes the
  ordered payloads so an empty or reordered series fails CI.

The Python result dict also preserves `isTargetGroup` and batch tracking
identity/span/count. `scripts/run_processing_conformance.py` locks these fields,
all metrics, and mask/series bytes to the committed reference. Its optional
bounded HDF5 input mode also validates the installed wheel against the pinned
private `gavinlouuu/z_adjustment-data` 50V in-focus corpus without loading or
committing the multi-gigabyte recording.

Input/output adapters live in [[BatchMaskSources]] —
`loadFromHdf5` / `loadFromFolder` (input), `saveMaskImages` /
`saveMasksToHdf5` (output). `HdfReviewTab` exposes a "Regenerate masks…"
button that drives this via `BatchMaskDialog`.

Batch calls do **not** touch realtime state or monitoring buffers, so
they're safe to run concurrently with live capture. Dynamic cores allocate or
reuse separate ABI contexts for concurrent calls.

## Async batch processing (capture decoupling)

Use `ProcessingService::startBatchPipeline(config, callback)` when capture
should only enqueue frames and let workers process them later. The config
contains `batchSize`, `maxQueuedFrames`, `workerCount`, and the same
`ProcessingConfig` / background / ROI inputs used by `computeProcessedFrame`.

`enqueueBatchFrame(gray, index, timestampNs)` clones the frame into the bounded
queue and returns:

- `true` when the frame was accepted,
- `false` when the pipeline is stopped, the frame is empty, or the queue is
  already full.

Workers wait for `batchSize` queued frames, process with
`computeProcessedFrame`, and emit `std::vector<ProcessedFrame>` through
`BatchResultCallback`. `stopBatchPipeline` drains residual partial batches
before joining workers.

`getBatchPipelineStats()` exposes accepted, dropped, processed, batch count,
current/max queue depth, batch size, worker count, and running state. See
`docs/batch_pipeline_architecture.md` for the migration plan from
`FrameStore -> realtimeLoop` to capture enqueue -> batch worker.

## Frame classification and experiment accounting (issue #367)

- `classifyFrameWithActiveKernel(frame, config, roi, bg)` → `FrameClassification
  {Empty | Candidate | Malformed | ProcessingFailed, detail}`. A core error or
  exception is `ProcessingFailed`, a bad geometry/short payload is
  `Malformed`; neither is ever reported as a valid empty frame.
  `isFrameEmptyWithActiveKernel` remains as a compatibility wrapper that
  returns true for everything except `Candidate` — the raw-recording loop no
  longer uses it.
- On the realtime inline path every frame that reaches the empty check is
  **admitted** to `experimentAccounting_` while an experiment is active and
  ends in exactly one `FrameOutcome`: `Empty`, `ProcessingFailed` (empty-check
  failure *or* mask failure — the frame is skipped, counted in
  `getProcessingFailureCount()`, and reported as `PipelineSkipReason::KernelError`),
  `Processed` (a valid object) or `RejectedByScientificFilter`. Ring-behind
  skips are admitted as `StoreOverwritten`. `appendExperimentFrame` is a
  persistence admission; frames evicted by the bounded experiment buffer are
  `persistenceCancelledByPolicy`; the flush writer adds `persistenceCommitted`.
  `experimentAccountingSnapshot()` derives the pending / failed persistence
  terms from the current buffer and flush-queue error state and returns the
  reconciled snapshot (`RunCompletionState`). `setExperimentAccountingContext(
  captureGeneration, policyAllowsDrops)` must be called before
  `startExperiment()` ([[../architecture/ExperimentCoordinator]] does). Frame
  counts are separate from `objectsDetected`. Guard:
  `processing.experiment_accounting`.
- **Start/stop boundaries (2026-09-08).** Outcomes, validations and the
  experiment-buffer append are gated on
  `RecordingAccountingTracker::wasAdmitted(idx)` (index range of the run,
  atomics), *not* on `experimentActive_`: a frame in flight across
  `startExperiment()` / `endExperiment()` was otherwise counted on one side
  only (admitted but no outcome, or outcome without admission) and a clean
  run read "Failed: accounting does not reconcile" by one frame (hit by the
  CI fast lane and a bench run the same afternoon). `endExperiment()` also
  waits (≤ 250 ms) for the realtime thread to finish the last admitted frame
  so the caller's snapshot is complete. Guard: experiment 3 of
  `processing.experiment_accounting` (40 short runs against a free-running
  pusher; 2 mismatches per pass before the fix, 0 after).

## Background identity + bounded calibration (issue #369)

- `backgroundGeneration()` increments on **every** background publication or
  clear (`setRealtimeBackgroundGray`, auto-capture, calibration); 0 = never
  set. `backgroundSha256()` hashes the active background bytes. Both feed the
  [[../architecture/ExperimentCoordinator]] invalidation key and the frozen
  run snapshot, so a background change after preflight is a stale Start.
- `startBackgroundCalibration(BackgroundCalibrationRequest{requiredAccepted
  = 10, maxAttempts = 200, timeoutMs = 5000})` turns background sampling into
  a **finite, cancellable operation** on the realtime path: the recipe
  (config version) is frozen; each realtime frame outcome is observed
  (`noteRealtimeOutcome` / `noteRealtimeValidation` → `bgCalObserve`), empty
  frames are accepted into a `CV_64F` mean, non-empty frames are counted as
  `rejectedNonEmpty` (contamination), failures as
  `rejectedProcessingFailed`. It ends with exactly one
  `BackgroundCalibrationState`: `Succeeded` (candidate published atomically
  under `rtMutex_`, new generation + sha), `FailedInsufficient`
  (`maxAttempts` reached), `FailedTimeout` (also detected lazily by
  `backgroundCalibrationStatus()` so a stalled source never looks Running
  forever), `FailedProcessing` (config changed mid-operation), or
  `Cancelled`. The previous background stays active on every non-success
  path. `startBackgroundCalibration` refuses when realtime is not running or
  another calibration is active; the UI entry point is the Preview canvas
  context menu ([[../frontend/PreviewPage]]).

## Gotchas

- **The kernel seam (`IProcessingKernel`) now owns the science decisions:**
  mask generation, empty-frame classification, `analyzeObjects` (contours,
  metrics, LUT lookup, range/target gating, brightness, object ordering), and
  `matchTrack` (batch track matching). Threads, queues, callbacks, track
  lifecycle state, `FrameStore`, experiments, and HDF5 stay host-owned.
  `ProcessingScience.cpp` is the single shared implementation; do not call it
  from pipeline code — route through the selected kernel
  (`filterProcessedObjects` / `matchTrackWithActiveKernel`).
- **The v1 C ABI still transports mask/empty decisions only.** ABI v1 dynamic
  cores inherit the default kernel science (identical to bundled), so a
  release that changes contour/metric/tracking semantics still requires an
  ABI v2 that marshals object records across the plugin boundary; that
  residual is tracked in GitHub issue #242 (A7). The
  `processing.science_golden` test pins the science outputs and
  `processing.science_seam` proves the batch/offline routing.
- Activating a core is intentionally not an in-flight migration. Stop capture,
  experiment, recording, and offline/async batch work before switching.
- Plugin modules are intentionally never unloaded. Repeatedly preparing many
  versions in one app process grows resident code until restart.
- Realtime drop-frames mode is ignored while an experiment is active.
- **Live-view overlay backlog / `rtDropFrames_` defaults ON:** the inline
  realtime loop consumes `FrameStore` sequentially via `rtLastProcessed_`. If
  drop-frames is *off* and capture outpaces processing, the processed snapshot
  (`getLatestSnapshot`, source of the mask/contour overlay and trigger decision)
  falls progressively behind the live write head — an accumulating backlog that
  only resets when realtime restarts. The raw preview is unaffected (it reads
  `FrameStore::getLatest`). Because of this, `rtDropFrames_` now **defaults to
  ON**, so the live overlay jumps to the newest frame and stays bounded.
  Experiments are unaffected: the loop gates the flag behind `!experimentActive_`
  (`rtDropFrames_ && !experimentActive_`), so every frame is still
  processed/recorded during a run. Users can still disable it via
  ProcessingSettingsDialog. Verified by
  `tests/processing/realtime_drop_frames_default_test.cpp` (default value +
  toggle) and `tests/integration/e2e_live_view_latency_test.cpp`
  (`integration.e2e_live_view_latency`): under sustained overload the default
  stays a few hundred frames behind, vs. tens of thousands with drop-frames
  forced off (~30x).
- When the experiment backlog reaches `maxBufferedFrames_` **or the byte
  budget**, sampled invalid frames are dropped first. Valid frames can evict
  old invalid frames; valid drops only happen if the backlog is entirely
  valid and still over the bound. This is a last-resort RAM safety valve for
  long runs where HDF5 is slow or failing; the policy lives in
  `ExperimentFrameBuffer` and every drop is reported (never silent).
- `pixelToMicronFactor_` default is `0.4886` — UI lets users change this.
- YOLO is a separate service ([[YoloService]]); this pipeline does not use it.
- **Callback ordering invariant**: `TargetGroupCallback` and
  `RingRatioCallback` are invoked **before** `monitoringFramesMutex_` is
  taken (and with no other locks held) so the UI thread's periodic
  `getMonitoringValidFrames()` / `getMonitoringInvalidFrames()` snapshot
  cannot stall the [[TriggerService]] wake-up. Do not move these callback
  calls back inside the monitoring-mutex scope; hold-time there directly
  becomes trigger-onset jitter (see 2026-04-15 task record).
- **Target-group ownership policy**: after each frame's detections are
  evaluated, at most one `TargetGroupCallback` is emitted per frame, by
  taking the first target-group object in deterministic contour-sort order
  (the same order used for `objectId` assignment). This is the source object
  for that frame's trigger request.
- **Callback order: target-group THEN ring-ratio.** Within the hoisted
  callback block, `TargetGroupCallback` fires **first** so the
  [[TriggerService]] condition variable is notified before
  `RingRatioCallback`. As of 2026-04-16 `AutofocusService::onRingRatio`
  is O(1) (push into an inbox + atomic updates + `notify_one`) with the
  sort / deque trim moved onto a dedicated stats thread, so this ordering
  is no longer strictly required — but target-group first is still the
  safest invariant in case the ring-ratio callback target changes.
- Per-frame `previousFrameForAutoCapture_` assignment uses cv::Mat's
  refcounted shallow copy (`= blurredCurr`, **not** `.clone()`). Cloning
  every non-empty frame was an allocator-pressure source for algo-time
  variance when auto-background was enabled.
- `computeProcessedFrame` intentionally omits the auto-background /
  previous-frame-diff path used in `realtimeLoop()`. Callers needing that
  should continue to drive frames through `FrameStore` + `startRealtime`.
- Young's modulus gating still depends on the LUT path loaded during backend
  startup; if the managed cache cannot be updated, the pipeline keeps using
  the last known-good local copy or the bundled fallback.
- `FilterResult::allContours` is a `shared_ptr<const ...>`, **not** a value.
  All per-object `FilterResult`s of a frame share one allocation (assigned
  once by `filterProcessedObjects` after evaluation), so the monitoring /
  experiment copies are refcount bumps rather than deep copies of every
  contour point. Consumers must deref (`*validation.allContours`) and
  null-check. The write-only `hierarchy` field was removed — nothing read it.
- `calculateBrightnessQuantiles` takes an optional bbox `region`: the per-
  object evaluators pass the object's bounding box so the scan only covers
  pixels that can be non-zero in that object's mask (identical sample set,
  not the whole ROI). It also uses row pointers instead of `cv::Mat::at<>`
  and skips the `clone()` for already-single-channel input. These were
  per-object allocator/CPU costs that scaled with objects-per-frame.
