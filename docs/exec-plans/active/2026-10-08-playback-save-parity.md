# PZ7035 stop, playback and save: what the PC does (parity spec for #649)

Status: active

## Goal

[#649](https://github.com/gavinlouuu-kpt/mib-studio-qt/issues/649) asks for YOFO Studio on the PZ7035 to
stop a 5 kHz run, play back the buffered frames with their overlays and cell results, and save them to the
SSD, "matching what PC MIB Studio already does". This document records exactly what the PC does, with code
references, so the PL ring (board owner) and the Studio playback UI (this session) are built against the
same reference, and says where the issue's wording does not match the PC. Read-only audit of develop at
`723b84d3` (2026-10-08). Nothing here changes code.

## Headline findings (read these first)

1. **The PC's "stop and review" buffer holds raw camera frames only.** It is the `FrameStore`, a ring of Mono8
   camera frames (default 5000, resizable). It holds **no mask and no cell results**. The experiment staging
   buffer (`ExperimentFrameBuffer`, the "Valid Buffered" counter) is not reviewable: it is flushed to HDF5.
2. **Overlays on a paused PC frame are recomputed from the raw pixels in the host**, in the Qt `PlaybackPanel`
   (`computeProcessedFrame`); the desktop (Tauri/React) buffer controls draw paused frames **raw, without
   overlay**. Per-frame mask/contours/cell results stored with the frame exist on the PC **only inside a saved
   HDF5 file**, shown by the Review tab. So "image + mask + cell results of that frame's own record" has no
   PC live equivalent: the closest PC behaviour is *Review of a saved file*; the PZ's playback needs record-based
   overlay by construction (the PL does the science, the PS has no host processing).
3. **The PC has no "save a range of the buffer to HDF5".** "Save Buffer" writes TIFF-per-frame or one AVI for a
   chosen range. The two HDF5 writers (experiment, Raw Record) can only start "now". A range save to HDF5 is new
   functionality on the PZ, not parity.
4. **The PZ already writes the experiment HDF5, with metadata rows per cell and no `images`/`masks`** (YOFO S3).
   YOFO Review opens such a file, but its frame/thumbnail/mask browser is empty (it keys on image-dataset counts).

## 1. The three PC retention structures

| # | Structure | Holds | Reviewable? | Code |
|---|---|---|---|---|
| 1 | `ExperimentFrameBuffer` (valid + invalid deques) | processed objects waiting for the HDF5 writer | No (counted as "Valid/Invalid Buffered", flushed) | `include/backend/processing/ExperimentFrameBuffer.h:34-95`, `src/backend/processing/ExperimentFrameBuffer.cpp` |
| 2 | `FrameStore` (raw camera ring) | raw Mono8 frames, write-indexed | **Yes**: all PC scrubbing, Save Buffer | `include/backend/playback/FrameStore.h:17-31`, `src/backend/playback/FrameStore.cpp` |
| 3 | Monitoring rings (`FrameRingBuffer` 1000 valid + 1000 invalid) | ROI image + mask + metrics, only while the Monitoring view is visible | Qt monitoring tab only; the desktop bridge sends metric rows without images | `ProcessingService.h:874-875`, `ProcessingService.cpp:2644-2663` |

### 1.1 FrameStore (the review buffer)
- One `Frame`: `storeGeneration`, `captureSession`, `width`, `height`, `pixelFormat`, `linePitch`, `timestamp`,
  `hostTimestampUs`, `data` bytes (`FrameStore.h:17-31`). No mask, no results.
- Capacity: **5000** (`AppBackend.cpp:546` `std::make_shared<playback::FrameStore>(5000)`); the default constructor
  argument is 512, the Aravis fallback 512, overview mode 8 (`AppBackend.cpp:2069-2071, 2110, 2360`). Memory is
  capacity x frame bytes, slots pre-reserved at capture start (`CaptureService.cpp:569`).
- Policy: ring overwrite, oldest evicted. Per-slot mutexes; reserve-then-commit; `FrameReadOutcome` =
  `Available | NotYetCommitted | Overwritten | Malformed | OutOfRange` (`FrameStore.h:36-54`).
- Resize (`save_preview_buffer action=resize`, `FrameStore.cpp:653-713`): indices renumber from 0, `storeGeneration`
  increments; frames are kept when `newCapacity >= available`, else cleared. Bridge limit 1..1,000,000 and refused
  above 75 % of RAM (`BackendFacade.cpp:3374-3381`).
- Stopping capture does **not** clear it (no `FrameStore::clear` in `CaptureService`): that is why Stop leaves
  frames reviewable.

### 1.2 Experiment staging buffer: keys and defaults
Applied at `ProcessingService::startExperiment()` (`ProcessingService.cpp:624-665`, log line at 660-664).

| Key | Where | Default | Rule |
|---|---|---|---|
| `buffer_threshold` (= flush interval) | `ConfigDocumentApply.cpp:108-109`; Qt `AppConfigWatcher.cpp:492-496` | 1000 (`resources/defaults/config.json:3`); member default 100 (`ProcessingService.h:886`) | 1..10,000,000 |
| `experiment_buffer_max_mb` | `ConfigDocumentApply.cpp:110-111`; Qt `AppConfigWatcher.cpp:497-501` | 512 MB (`ProcessingService.h:101`) | 0..1,048,576; 0 = count-only |
| max buffered frames | derived, **no key**: `defaultMaxBufferedFrames` = `max(flush, min(max(1000, 4*flush), 5000))` (`ProcessingService.cpp:59-67, 2320-2327`) | 1000 | |
| invalid sampling rate | no key; Qt `MainWindow.cpp:448-449` sets 200 (flush 200); Tauri path stays 100 | 100 | |
| `display_fps` | `ConfigDocumentApply.cpp:218` | 60 | 1..240, display only |

Eviction (`ExperimentFrameBuffer.cpp:62-111`): a valid frame evicts the oldest invalid ones first; a valid frame never
evicts another valid frame, so a full all-valid buffer **refuses** the newcomer. Any eviction or refusal raises
`"Experiment buffer capacity exceeded: recording data was dropped"` (`ProcessingService.cpp:2183-2207`), which fails the
run (`ExperimentCoordinator.cpp:1224-1234`). Flush triggers: `needsFlush` at rows >= flush interval or bytes >=
maxBytes/2 (`ProcessingService.cpp:2210-2226`), plus the coordinator's 2 s backstop (`ExperimentCoordinator.cpp:1242-1275`);
the writer queue is `HdfWriteQueue` depth 3. The buffer is emptied on each flush.

### 1.3 What one staged `ProcessedFrame` holds (`ProcessingTypes.h:162-188`)
`index`, `timestampNs`, `hostTimestampUs`; `originalImage` (full-frame gray), `processedImage` (full-size mask),
optional `seriesImages`; one `validation` (`FilterResult`: validity flags, object/track ids, bbox, centroid,
deformability, area, area ratio, ring ratio, Laplacian variance, Young's modulus, brightness quartiles/mean/variance,
contour area, pixel count, blemish count, `allContours`). **One `ProcessedFrame` is one object (cell) record**; several
records can share a source frame index. Bytes counted: `originalImage + processedImage (+ series)`, about
`2 x W x H` (`ExperimentFrameBuffer.cpp:7-21`).

## 2. Stop, scrub, step, play

### 2.1 Qt (`src/frontend`)
- Stop Camera: `CameraController.cpp:38-41, ~112`; Stop Experiment: `MainWindow.cpp:446, 1534-1566` (only
  `requestStop(false)`; the coordinator worker flushes and closes HDF5).
- `PlaybackPanel` (`src/frontend/system/PlaybackPanel.cpp`, hosted in `tabs/PreviewPage.cpp:70-78`):
  scrub slider whose range is `playback().queryRange(earliest, latest, count)` every tick (absolute write index);
  releasing it sets `followLive_=false` and pins the frame (`~588-635`); **step = keyboard only** on the slider
  (Left/Right 1, Shift = 8, Home/End, Space toggles capture; `eventFilter` 1281-1331).
- **No play/pause button, no display-fps selector, no range selection** in the panel. Display rate = `display_fps`
  from the config (`setDisplayFps` 1210-1219). `PlaybackService::play()/pause()` only toggle a flag reported in the
  playback-position event (`PlaybackService.cpp:14-22`, `BackendFacade.cpp:1142`).
- Fetch: in-process `fetchLatest` / `fetchByIndex` -> `FrameStore::getLatest/getByWriteIndex`, one frame per timer
  tick (default 60 Hz). Empty buffer: `queryRange` is false, the canvas keeps its previous image (no placeholder).
- Buttons: Overlay cycle, Set Background / Auto / Clear ROI, **Save Buffer** (`BufferSaveDialog`), **Record**,
  Fit: Window / 100 %.

### 2.2 Desktop (Tauri/React, also the YOFO Studio browser UI)
- There is **no PlaybackPanel** in `desktop/src`. The equivalent is `PreviewBufferControls`
  (`desktop/src/previewBuffer.tsx`), rendered only when `caps.frame_buffer` (`App.tsx:1916`), which is **false on the
  PZ7035** (`BackendFacade.cpp:3013`, `platformCapabilities.ts:14`, `tests/backend/pl_science_test.cpp:46`).
- Controls: "Pause preview" (pins `range.last`) / "Follow live"; scrub slider (BigInt offsets from `range.first`);
  Save range (All retained / Frame indices / Source timestamps), First/Last, "skip empty frames", format TIFF or AVI
  (+ AVI fps), Resize buffer, "Use paused frame as background". **No step buttons, no display-fps control**
  (`previewFpsLimit` from the profile, `App.tsx:240, 696-703`, default 30).
- Commands: `fetch_preview_buffer` -> `{capacity, generation, timestamps_available, timestamp_first/last, available,
  first, last, count, capture_running, recording}` (`dispatch.rs:153, 788`, `BackendFacade.cpp:3334-3348`);
  `fetch_frame_packet` (latest) and `fetch_indexed_frame_packet {frameIndex}` (`dispatch.rs:237-244`);
  `seek_latest` / `seek_index` emit a playback-position event (`BackendFacade.cpp:1130-1150`).
- Frame packet: 96-byte little-endian header v2, Mono8, payload == stride x height, limits 32 MiB / 16 Mpx / 8192
  (`bridge-contract.json:251-278`, decoder `framePacket.ts:30-69`). **One frame per request**; `FramePullScheduler`
  allows one in flight + one pending per view, max 4 views. No paging.
- Empty buffer: span "Buffer empty", controls disabled. A pinned frame that is evicted shows "Selected frame was
  evicted; select a retained frame." Generation change resets the selection.
- Run Stop on the desktop does not leave frames for scrubbing in the experiment: **Stop Experiment flushes to HDF5**
  (`ExperimentCoordinator.cpp:1296-1340`); live review is the camera `FrameStore`, post-run review is the Review tab.

## 3. Overlays and per-frame cell results

- Modes: `enum class OverlayMode { Off, Mask, Contours, Both }` (`PlaybackPanel.h:28`), cycled Off -> Mask -> Contours
  -> Both (`PlaybackPanel.cpp:638-668`). Colours: Target blue (0,120,255), Valid green (0,255,0), Invalid red (255,0,0).
- **Recomputed, not stored**: `computeProcessedOverlay` (`PlaybackPanel.cpp:1090-1203`) clones the current gray frame
  and calls `ProcessingService::computeProcessedFrame(frame, background, config, roi)` (`ProcessingService.cpp:1612-1665`)
  on every redraw (tick, scrub, background or ROI change): mask via the active core, then `filterProcessedImage` for
  validation, contours and metrics. A scrubbed frame is therefore classified by a **single-frame** run without
  cross-frame tracking or target-group state; while following live the colour comes from the latest realtime
  snapshot instead (`PlaybackPanel.cpp:1137-1166`). Mask tint: inner contours filled at alpha 90, else the whole mask
  (`maskToTintedOverlay` 1056-1087); contours via `findContours(RETR_CCOMP)`.
- The Qt panel shows **no cell results text/table**, only the classification colour.
- Desktop live overlay (`components/ProcessedPreview.tsx`, hosts only): polls `fetch_processed_preview` every 200 ms:
  primary object only, from the latest realtime snapshot, stale after Stop. **Buffer-paused frames on the desktop are
  drawn raw** (`App.tsx:371-420`).
- Saved-file review overlays (Review tab, `SavedReviewImage.tsx`): five modes None / AllContour / OuterInnerColorCoded /
  AllMask / FilteredMask (`review-contract.json:20-27`), rendered natively from the **stored** mask and classification
  (`render_review_overlay`, `ProcessingOverlay.cpp`, `ReviewCharts.cpp:36`). Metrics table columns: Index, Object Id,
  Track Id, Area, Deformability, Ring ratio, E (kPa), paged 50 rows (`App.tsx:256`).
- Monitoring rows (`fetch_monitoring_snapshot`, `BackendFacade.cpp:1242-1297`): frame index, timestamp, valid, target
  group, object/track id, centroid, area, deformability, area/ring ratio, Young's modulus, pixel-to-micron. No images.

## 4. Save paths on the PC

| Path | UI | Command | Writes | Range | Code |
|---|---|---|---|---|---|
| **Experiment run** | Start/Stop Experiment ("Save Experiment Data" prompt, default `experiment.h5`) | `experiment_start {outputPath}` / `experiment_stop` | HDF5 "experiment file" (section 5) | none: only frames after Start | `App.tsx:812-838`, `ExperimentCoordinator.cpp:889, 1287`, `ProcessingService.cpp:2176-2330` |
| **Record** (raw recording) | "Record" button, needs capture running (default `clip.h5`) | `start_recording {filePath}` / `stop_recording` | HDF5 "recording file": images + (index, ts, w, h) only | none: starts at the latest index | `App.tsx:769-790`, `AppBackend.cpp:2564-2960` |
| **Save Buffer** | Save Buffer... | `save_preview_buffer {request}` | directory `preview-buffer-N/` with `frame_%06llu.tiff` or `frames.avi` | **yes**: all / index range / timestamp range, optional skip-empty | `previewBuffer.tsx`, `BackendFacade.cpp:3350-3433`, `FrameStore.cpp:308-560` |
| Exports | Review tab | `review_export_csv`, `review_export_json` | CSV / images / charts from an existing HDF5 | n/a | `HdfExportService.cpp` |

There is no UI string "Raw Record"; the backend says "raw recording". The server also **stops and saves** when the last
client has gone for the grace time (5 s): `experiment_stop`, `stop_recording`, and on the PZ the idle safe state
(`crates/mib-bridge-server/src/lib.rs:348-390`).

### 4.1 Guards
- **Experiment start** (`ExperimentCoordinator.cpp:889-1100`, readiness `evaluateLocked` 543-880): refused when Active,
  mutex busy, not Idle/Failed, a Z-stage op is active, raw recording or an HDF5 file is open, stale readiness
  generation, any blocking gate, or Latest-Frame delivery without acknowledgement. Outcomes: Started / NotReady /
  StaleReadiness / AlreadyActive / StorageFailed / ProvenanceFailed / Busy. Storage gates: `storage.output` (writable
  probe, refuses a directory, **less than 64 MiB free**), `storage.roundtrip` (writes and reads back a small HDF5),
  `storage.persistent` (PZ only, **warn** when the target is RAM), `storage.buffer` (host only), `storage.hdf5`.
  PZ gates: `instrument.mode` (Run only), `science.pl`, `processing.profileCompile`, `telemetry.transportLoss`.
- **Overwrite**: `H5F_ACC_TRUNC` (`Hdf5Service.cpp:340`): an existing file at the chosen path is silently replaced. There
  is no existence check; the remote (`window.prompt`) dialog has no confirmation either.
- **Raw recording** (`AppBackend.cpp:2564-2640`): refused during an experiment ("Stop or reset the experiment..."), while
  already recording, with capture stopped, with the processing-core pin unsatisfied (also on the PZ), with an HDF5 file open.
  No readiness gates, **no free-space check**, no writability probe. While recording, camera/mode changes are refused.
  A write-queue overflow ("disk too slow") or write error is fatal and latched (`HdfWriteQueue.h:41-56`).
- **Save Buffer**: capture must be stopped and the experiment idle (`withIdleConfiguration`); the `generation` must match
  the ring's `storeGeneration`; the range must still be retained; the destination must be an existing directory; a new
  `preview-buffer-N` is created, never overwriting; no disk-space check. Not gated by `hostProcessingAvailable()` in the
  backend (only the UI hides it).
- Finalize order (`ExperimentCoordinator.cpp:1287-1430`): stop provider (PZ) -> flush + `finishFlush` -> `endExperiment`
  -> remainder flush -> trigger events -> metadata, accounting, provenance, config JSON -> close. Failures leave the run
  Failed with `experiment.flushFailed` / `provenanceFailed` / `saveFailed`.

## 5. HDF5 layout

Library settings (`Hdf5Service.cpp`): `H5F_ACC_TRUNC`, strong close, no file locking, **no compression**, default libver,
interval `H5Fflush` at most every `MIB_HDF5_FLUSH_INTERVAL_MS` (5000), final flush on close. Attribute names are
snake_case (`knowledge_map/data-model/HDF5-Storage.md` had them camelCase: corrected in the same change).

### 5.1 Experiment file
1. **Start**: `/valid_frames`, `/invalid_frames` groups (datasets created lazily); `/run_provenance` attributes
   `run_snapshot_schema_version` (uint64 2, `ExperimentReadiness.h:92`), `run_snapshot_json` (camera, delivery mode,
   timestamp descriptor, ROI, frame, processing core/config hashes, profile, **pixel_to_micron**, background, trigger, RF,
   output path, realtime mode, **science_placement, execution_provider, pl_core{abi, science profile, build id, weights prefix}**,
   application) and `readiness_json` (`Hdf5Service.cpp:4695`, `ExperimentCoordinator.cpp:228-286`). A failure removes the file
   (ProvenanceFailed).
2. **Frames** (`appendFrames`, `Hdf5Service.cpp:1403`): `/valid_frames/images` and `/masks` uint8 `(N,H,W)`, max
   `(unlimited,H,W)`, chunk `(1,H,W)`, no filter; `/valid_frames/metadata` compound, chunk `min(1000, first-batch rows)`,
   one `H5Dwrite` per row on later appends (`:487-676, 872-940`). Compound members in order (`:143-196`): u64 `index`,
   `timestampNs`; f64 `deformability`, `area`, `areaRatio`, `ringRatio`, `laplacianVariance`; u8 `isValid`,
   `touchesBorder`, `hasSingleInnerContour`, `inRange`; i32 `innerContourCount`; f64 `brightness_q1..q4`, `youngsModulus`;
   u8 `isTargetGroup`; f64 `brightness_mean`, `brightness_variance`, `contourArea`; i32 `pixelCount`, `blemishCount`; u8
   `degenerateContour`; i32 `objectId`, `objectCount`, `trackId`; u64 `trackFirstFrame`, `trackLastFrame`; i32
   `trackObservationCount`; f64 `bboxX/Y/Width/Height`, `centroidX/Y`. (`inChannel` is not stored.) `/invalid_frames/*`
   has the same layout, sampled. Multi-image series (host only): `series_images (N,S,H,W)`, `series_meta`,
   `series_contiguous`. `/trigger_events` (host only). A group is imageless or not for the whole run.
   **Images and masks are per object record**, so a frame with 2 cells stores its image twice.
3. **Finalize**: `/experiment_info` attributes `start_time_ns`, `end_time_ns`, `total_valid_frames`, `total_invalid_frames`
   (u64), all `processing_config_*`, `roi_x/y/w/h`, `multi_image_*`, nine `processing_core_*` provenance attributes
   (`Hdf5Service.cpp:1614-1918`); `/experiment_info/background` `(1,H,W)` if present; **run accounting** attributes
   `accounting_*` (schema version 1; frame terms, persistence terms, objects, index range, gaps, `completion_state`
   complete | intentionallyPartial | incompleteLoss | failed, `reconciled`; `Hdf5Service.cpp:4190-4264`,
   `RecordingAccounting.h:99-199`); acquisition provenance `timestamp_*` and `telemetry_*` (`:4417-4480`, **unverified for
   PZ runs**: fed by the capture service, which is stopped in Run); `config_json`.

### 5.2 Raw recording file
`/recorded_frames/images` uint8 `(N,H,W)` chunk `(1,H,W)` (each frame cropped to the ROI), `/recorded_frames/metadata`
compound `{index, timestampNs, width, height}` (all u64), `/recording_info` attributes `start_time_ns`, `end_time_ns`,
`total_recorded_frames`, `total_filtered_empty_frames`, `multi_image_*`, nine core provenance attributes, `mode =
"frame_recording"`, plus accounting and acquisition provenance. **No** `/run_provenance` (no pixel-to-micron),
`/experiment_info`, ROI, background, config JSON, masks, valid/invalid groups. The two kinds are told apart by
`/recording_info` (`Hdf5Service.cpp:3700`).

### 5.3 What YOFO Review needs
Only that `H5Fopen(RDONLY)` succeeds (`ReviewSession.cpp:272`, `Hdf5Service.cpp:2045`); no schema gate on open. Everything
else is tolerated when absent: `/experiment_info` (zeros), metadata compounds (members absent keep NaN/0), accounting
("Unknown"; a schema version other than 1 is a read error), `pixel_to_micron` from `run_snapshot_json` (fallback 0.4886,
labelled "(fallback)"). A crashed run has no `/experiment_info` (written at finalize). **For an imageless PZ file** the
metrics table and charts work (ring ratio is NaN, so its histogram is empty) but the frame/thumbnail/mask browser shows 0
frames (`ReviewPanel.tsx:136, 645-648`, `ThumbnailGrid.tsx:188`), and regenerate-masks/reanalysis fail. There is no
Review test for an imageless file. FCS export needs consistent `processing_*_contract_version` attributes (1..3).

## 6. What the PZ7035 does today

- **Experiment**: `ProcessingService::ingestProviderFrame` (`:2709-2745`) turns PL RESULT records into metadata rows; valid
  cells always, invalid sampled; **no images or masks**; an ingress-error frame is booked `storeMalformed` (declared loss),
  an empty frame `Empty`. `ringRatio` and brightness quartiles are NaN, `area` is the hull area in px²
  (`PzExecutionProviders.cpp:25-49`). Accounting counts frames for `admitted` but cell rows for `persistence_*`, so they
  differ by design. `total_valid_frames` is a row count.
- **Capabilities**: `frame_buffer`, `reanalysis`, `host_background`, `trigger`, `autofocus`, `core_updates` are false
  (`BackendFacade.cpp:3004-3036`), so there is **no FrameStore-based pause/scrub/Save Buffer** in the PZ UI.
- **Run**: the producer is stopped (`set_instrument_mode`), so there is no camera stream to scrub; the Run preview is a
  single-frame PL cell capture (`fetch_run_preview`: `PzInstrumentControl::captureCell`, S[36] arm / S[42] done, 100 ms
  timeout; packet `MIBC` v1 = 32-byte header, 512x96 gray, 1 bit/px mask, `listed x 18` u32 cell words; `runPreview.ts`).
- **Align**: preview frames come through the bridge/producer; **Record is not capability-hidden** there and would write
  ~60 fps preview frames, not PL results; it cannot start in Run (`start_capture` is refused in Run, `BackendFacade.cpp:737-745`).
- **Storage**: the target is the RAM root (`recordingTarget`, `RecordingTarget.cpp:15-52`), ~0.4 GB free; only a warning
  gate and the 64 MiB output probe exist. #615: a dense scene at 5 kHz outran the ARM writer ("Experiment buffer capacity
  exceeded" after 0.83 s). Repo evidence says the shipped PL path writes metadata only, so which path #615 measured is open.

## 7. Parity table for the #649 definition of done

| #649 item | PC reference | Parity or new? |
|---|---|---|
| Every frame enters the buffer, contiguous ids | FrameStore write index; no gaps accounted on PC live | **New on PZ** (PL ring + frame ids) |
| Stop keeps the last N frames; N = shown capacity | FrameStore capacity 5000 (config via `save_preview_buffer resize`, shown as `capacity`) | Parity for Stop/capacity display; capacity in frames and **seconds at the current fps is new** |
| Scrub, step, play at a display rate | Qt: slider + keyboard step; desktop: pause/follow + slider; **no play control, no fps selector** | The PZ UI should do at least the desktop set; step buttons and an fps selector are **new** (the issue asks for them) |
| Overlays Off/Mask/Contours/Both with that frame's own result | Qt: recomputed from pixels, single-frame; desktop: none; saved file: stored record | **New by construction**: record-based, closest to Review overlays (`render_review_overlay`) |
| Cell results for the frame | Qt panel: colour only; Review: table from the metadata compound | Parity target = the Review metrics table |
| Save buffer or range to SSD | Save Buffer = TIFF/AVI range; HDF5 writers have no range | **Range-to-HDF5 is new**; decide the format below |
| Opens in YOFO Review on a PC | needs HDF5 open + image-dataset counts for the frame browser | Needs `images`/`masks` datasets, not today's metadata-only file |
| Start resumes live; Run -> Align -> Run | PC: Start camera | Parity (existing mode switches) |
| Buffer size by configuration, capacity display follows | `buffer_threshold`, `experiment_buffer_max_mb`, resize | Parity for the idea; the ring is board memory, sized by configuration |

## 8. Per-frame content and capacity (for the PL ring)

What the PC file holds per saved object record, and what the PZ ring has per frame:

| Item | PC HDF5 | PL ring (512x96 Run) |
|---|---|---|
| image | uint8 `(N,H,W)`, 49,152 B, no compression | gray 49,152 B |
| mask | uint8 `(N,H,W)` **unpacked**, 49,152 B | 1 bit/px, 6,144 B (convert on save) |
| results | one compound row per **cell**: 36 members (section 5.1), about 220 B | RESULT record per cell, keyed by frame id (up to 16 cells x 18 u32 words in the run preview) |
| index | dataset position + `index`/`timestampNs` columns; no separate index | frame id; random access by id |
| container | HDF5, no compression, chunk `(1,H,W)` | n/a |

Capacity (an illustrative ring size, not a decision): gray + packed mask is about 55.3 KB/frame, so a 256 MB ring would hold about 4,800 frames, **about 0.96 s at
5 kHz**; with the unpacked mask (98.3 KB) about 2,700 frames, 0.54 s (records add about 220 B per cell). The PC default of 5000 frames is therefore about
1 s at 5 kHz. The capacity display should show frames and seconds (`N / fps`).

Playback reads (what Studio needs): **random access by frame id** (PC: `fetchByIndex` on the absolute write index, plus
a range query `earliest/latest/count`), one frame per request (PC frame packet, no paging), and per frame: gray, mask
(bit-packed is fine) and its RESULT records. A natural PZ packet is the existing `MIBC` run-preview layout keyed by
frame id; the range query is `first/last/count/capacity/generation` as in `fetch_preview_buffer`.

## 9. Decisions needed (not made here)

1. **Save format** (Gavin / coordinator): the experiment-file layout (images + masks + metadata compound + the attribute
   sets above, so YOFO Review gets a working frame browser) written from the ring for a chosen range, versus TIFF/AVI like
   Save Buffer, versus a raw ring dump plus an offline converter. Recommendation: the experiment layout, per frame once
   (not once per cell), unpacked masks, and the same provenance/accounting attributes with `completion_reason` saying it
   is a buffer range save.
2. **Range semantics**: the PC HDF5 writers cannot do a range. Define it for the PZ (by frame id; contiguous only).
3. **Overlay modes** on the PZ: Mask and Contours need the contour from the mask (host-side extraction from the ring mask,
   cheap at 512x96) or stored contour records. Decide where.
4. **Where is the buffer used while running?** The PC FrameStore is fed continuously; the PZ ring freezes on Stop. Confirm
   with the board owner that "ring head and frame ids" are readable while running for the contiguity check.
5. **Disk**: persistent target (SATA SSD, #651 G2) and an overwrite policy (PC silently truncates).

## 10. Unverified or contradictory

- Whether the board package used for #615 had the image-writing path.
- PZ `timestamp_*`/`telemetry_*` acquisition provenance contents in Run.
- TIFF compression: the code comment says LZW but passes value 1 (`FrameStore.cpp:~866-870`); not checked in OpenCV.
- `trackId`/track fields on the PZ rows (not set by the PL ingest path; defaults not checked).
- Whether Record in Align on a PZ behaves as described (not run).
