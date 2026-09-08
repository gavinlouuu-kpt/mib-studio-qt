# AppBackend

> Composition root. Owns every backend service and the shared `FrameStore`.
> Frontend code holds a single `backend::AppBackend&` and calls getters.

**Source:** `src/backend/app/AppBackend.cpp`, `include/backend/app/AppBackend.h`
**Related:** [[Overview]], [[Data-Flow]], [[Threading-Model]],
[[../data-model/FrameStore]]

## What it owns

All services are `std::unique_ptr`; [[../data-model/FrameStore]] is
`std::shared_ptr` (shared with capture, processing, playback).

```cpp
sqliteService_, hdf5Service_,
captureService_, processingService_, playbackService_,
cameraControlService_, autofocusService_,
triggerService_, yoloService_, syringePumpService_
frameStore_  // shared_ptr<FrameStore>(5000)
```

## `initialize(dataDir)` — what it wires

See `src/backend/AppBackend.cpp` around lines 79–200.

1. Creates `dataDir` and resolves a user-writable log path (falls back to
   `%LOCALAPPDATA%/MIB_Studio_Qt/logs/app.log` on Windows when `dataDir`
   is under `Program Files`).
2. Instantiates all services + `FrameStore(5000)`.
3. `sqliteService_->initialize(dataDir/app.sqlite3)`,
   `hdf5Service_->initialize(dataDir)`.
4. Loads optional YOLO model from `resources/models/yolo11n-seg.onnx`.
5. Resolves the Young's modulus LUT through the managed R2/cache helper,
   preferring the user-writable copy under the app-local data tree and
   falling back to the bundled `resources/isoelastic_curve/...` file on
   first run, offline launches, or update failures.
6. Starts the processing worker pool (`processingService_->start()`).
   Realtime loop is **not** started here — it starts when the Experiment tab
   becomes active.
7. Wires callbacks:
   - `ProcessingService::RingRatioCallback` → `AutofocusService::onRingRatio`
   - `ProcessingService::TargetGroupCallback` → `TriggerService::onTargetGroupResult`
   - `CaptureService::CameraReadyCallback` → starts/stops `TriggerService`
     and hands it the live `ICamera*`
   - `ProcessingService::BackgroundCaptureCallback` → emits Qt signal via
     [[../frontend/System-Utilities]] `BackgroundCaptureNotifier`
8. Seeds the [[../diagnostics/CrashStateMirror]] with initial app context
   (camera label, data dir, mock vs hardware vs MindVision, FrameStore
  capacity) and sets the Sentry tags (`camera_mode`, `data_dir`) on
  [[../services/CrashReporter]].
  The reporter itself is initialized earlier in `main()`, before AppBackend
  exists.

### LUT management

The Young's modulus LUT now follows the same managed-asset model as the
profile catalog:

- `EModulusLutCatalog` checks `https://updates.yofo.bio/stable/emodulus-lut/latest.json`
  by default, with `MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL` as an override.
- Remote payloads are verified with SHA-256 before replacing the local cache.
- The active LUT path is logged with source, revision, checksum status, and
  remote update outcome.
- `MIB_STUDIO_EMODULUS_LUT_CACHE_DIR` can redirect the cache path for tests
  or local validation.

### Boot-time service toggles (`MIB_DISABLED_SERVICES`)

`AppBackend::initialize` reads a comma-separated disable list from
`MIB_DISABLED_SERVICES` and conditionally skips selected startup wiring.

Supported backend tokens:

- `sqlite`
- `hdf5`
- `processing`
- `yolo`
- `autofocus` (disables ring-ratio callback wiring from processing)
- `trigger` (disables processing/camera trigger wiring)
- `capture` (alias: `camera`)
- `playback`
- `all` (disables all backend startup paths above)

Notes:

- Tokens are case-insensitive; `-` and `_` are treated the same.
- Services are still constructed to preserve existing references in frontend
  and backend code; toggles control startup wiring/initialization.

### Pipeline latency instrumentation (`MIB_PIPELINE_TIMING`)

`initialize` also reads `MIB_PIPELINE_TIMING` (`1`/`true`/`on` enables
[[../diagnostics/PipelineTimingRecorder]]) and `MIB_PIPELINE_TIMING_DIR`
(dump directory, default `<dataDir>/pipeline_timing`). The camera-ready
callback dumps the latency CSVs when capture stops (after the trigger thread
is joined), and `shutdown()` dumps again as a final snapshot. Runtime API:
`setPipelineTimingEnabled` / `isPipelineTimingEnabled` /
`dumpPipelineTiming(dir, errorOut)`. The target-group wiring forwards
`frameIndex` + `hostTimestampUs` from `TargetGroupEvent` to
`TargetGroupSignal` so [[../services/TriggerService]] can correlate pulses
with source frames. See `docs/howto/pipeline-latency-diagnosis.md`.

## Shutdown

`shutdown()` stops every service-owned thread in dependency order — capture
(joins the capture thread, which stops the trigger via the camera-ready
callback), trigger (defensive, idempotent), frame recording, then processing
(`stopRealtime()` + `stopBatchPipeline()` + `stop()`). The destructor calls
it, so teardown no longer depends on `MainWindow::closeEvent` having stopped
experiment services first. Ordering matters: members are destroyed in reverse
declaration order, so `triggerService_`/`autofocusService_` die before
`processingService_` — a realtime loop still running at member destruction
would invoke its callbacks on freed services. `shutdown()` is idempotent and
safe on a never-initialized backend. Verified by
`tests/backend/backend_lifecycle_smoke_test.cpp` (destroys the backend with
the realtime thread live).

## Camera selection

- `setHardwareCameraSelection(ifIdx, devIdx, label)` — choose device (no start)
- `setMindVisionCameraSelection(cameraIndex, label, errorOut) -> bool` —
  choose a MindVision device (no start); fails with an actionable error in
  builds compiled without MindVision support (issue #338)
- `configureMockCamera(options)` — choose mock folder instead
- `applyCameraScriptFromFile(path)` — push a GenICam JS config to the selected
  device (stops capture first, does not restart)
- `resetSelectedHardwareCamera()` — issue GenICam `DeviceReset`
- `applyMindVisionConfigFromFile(path)` — apply the selected MindVision JSON
  config and refresh the capture factory path
- `softTriggerCamera(errorOut)` — fire one software acquisition trigger on the
  live capture camera via `CaptureService::softTriggerActiveCamera` (requires
  capture running with `trigger_mode: 1`); exposed to the facade as
  `CameraCommandAction::SoftTriggerCamera`
- `pulseGenerator()` — accessor for [[../services/PulseGeneratorService]]
  (external-trigger pulse source, created alongside the syringe-pump service)

### Platform behavior

- On non-Windows builds, hardware camera initialization is forced to mock and
  logs a warning when a hardware mode is requested.
- On Windows, `MIB_CAMERA_MODE=mindvision` configures the MindVision capture
  factory when the build was configured with `MIB_ENABLE_MINDVISION=ON`; the
  startup parser clamps `MIB_MINDVISION_CAMERA_INDEX` to a non-negative index.
- `setHardwareCameraSelection()` becomes a guarded fallback on non-Windows:
  it logs a warning, switches the capture factory to `MockCamera`, clears
  selected hardware indices, and keeps `mockCameraConfigured_ = true`.
- `setMindVisionCameraSelection()` **fails** (returns `false` + errorOut) when
  the build lacks MindVision support instead of silently swapping in the mock
  camera — the user asked for hardware and must not unknowingly record mock
  frames (issue #338). Selection state stays untouched on failure. The
  startup-profile path (`camera_mode: mindvision`) still falls back to mock so
  the app remains usable, but logs an ERROR naming
  `MIB_ENABLE_MINDVISION=OFF` as the cause.

## Frame recording mode

Separate from experiments: record non-empty raw frames directly to HDF5 with
no contour processing.

- `startFrameRecording(hdf5Path)`, `stopFrameRecording()`, `isFrameRecording()`
- Counters: `frameRecordingCount()`, `frameRecordingFiltered()`
- Uses a dedicated `frameRecordingThread_`. Empty frames are dropped via
  `ProcessingService::isFrameEmptyWithActiveKernel` after the
  recording thread has hoisted config/ROI/background out of the per-frame loop.
  All three are refreshed once per poll batch, keyed off `getConfigVersion()`.
  (The old `FrameStore::setFrameFilter` API was dead code and has been removed.)
- Recording acquires a processing-core operation lease before the worker
  starts and owns it through `/recording_info` finalization. Activation is
  therefore blocked for the entire recording, and the stored provenance is
  the exact identity used by empty-frame filtering.
- `stopFrameRecording()` joins the recording thread; the thread drains the write
  queue, writes `/recording_info`, and closes the HDF5 file before the stop call
  returns.
- The collector thread hands batches to a 3-slot [[../services/Hdf5Service]]
  `HdfWriteQueue` (writer thread does `appendRecordingFrames`), so slow disk no
  longer stalls FrameStore reads. The written count advances only on a confirmed
  write; a failed write or queue overflow stops recording and fires the fatal
  save-error sink. This fixed the old silent count-and-drop-on-failure bug.
- The queue's writer thread must not block on synchronous I/O or it backs up and
  trips the fatal overflow, so [[../services/Hdf5Service]] flushes on a time
  interval (no per-append full-file copy); see `MIB_HDF5_FLUSH_INTERVAL_MS`.
- Each frame is **cropped to the preview ROI** (`getRealtimeRoi`) before
  storage, via the pure `backend::recording::clampRoiToFrame`
  (`include/backend/recording/RoiCrop.h`, tested by
  `tests/backend/roi_crop_test.cpp`) — full frame when no ROI is set.
  Recorded frame metadata width/height reflect the crop.
  The realtime ROI is set from two sources: PlaybackPanel's canvas ROI
  drawing and OverviewTab's `roiChanged` signal (connected in
  `MainWindow.cpp`). Both call `ProcessingService::setRealtimeRoi()`.
- The selected core therefore owns raw-recording empty classification too. An
  unsatisfied administrator version pin fails closed instead of recording with
  a silently different algorithm.

### Fatal save-error sink

`setFatalSaveErrorCallback(cb)` / `reportFatalSaveError(msg)` funnel both
recording **and** experiment-flush ([[../services/ProcessingService]]) write
failures to one callback. `MainWindow` marshals it to the UI thread, stops the
active operation, and shows a modal Save Error dialog — failed saves are never
silent.

## Config JSON storage

`setLastConfigJson(json)` / `getLastConfigJson()` — raw JSON captured by the
config watcher, stored as a string attribute on `/experiment_info` in HDF5
(see `Hdf5Service::writeConfigJson`).
