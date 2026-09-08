# MockCamera

> Folder-backed [[ICamera]] for development and tests. Reads PNG/TIFF/JPEG
> images from a directory and replays them at a configured interval.

**Source:** `src/camera/mock/MockCamera.cpp`,
`include/camera/mock/MockCamera.h`
**Related:** [[ICamera]], [[../build-and-run/Run-Modes]]
(mock env vars), [[../frontend/ConnectTab]]

## Options — `MockCameraOptions`

```cpp
std::filesystem::path folder;
std::chrono::microseconds frameInterval{33'000}; // ~30 fps default
bool loopFiles{true};
```

## Env vars (read in `main.cpp` / `AppBackend`)

- `MIB_CAMERA_MODE=mock` — force mock (bypasses ConnectTab selection)
- `MIB_MOCK_CAMERA_DIR=<path>` — folder source
- `MIB_MOCK_CAMERA_INTERVAL_MS=<ms>` — overrides `frameInterval`
- `MIB_MOCK_CAMERA_LOOP=true|false` — overrides `loopFiles`

## Responsibility

- `refreshFileList()` scans the folder for supported extensions.
- `preloadFrames()` reads all images into `preloadedFrames_` at start
  (so `grabFrame` is fast and deterministic).
- `grabFrame(out)` returns the next preloaded frame, sleeping as needed
  to hit `frameInterval`. Returns false when `loopFiles == false` and
  the list is exhausted.

## Timestamps (issue #368)

`Frame::timestamp` is `std::chrono::steady_clock` nanoseconds at delivery
(`timestampDescriptor()`: `hostSteadyNs @1e9 Hz, synthetic`). It is a
different unit from `Tools::getTimestamp()` microseconds, so the mock does
not claim `timestampsHostComparable`.

## Gotchas

- Mock camera **simulates** trigger output: `setTriggerOutput` flips an
  atomic line level and counts rising edges (`triggerPulseCount()`), always
  returning true, so [[../services/TriggerService]] fires real pulses (and
  [[../diagnostics/PipelineTimingRecorder]] records them) in headless
  pipeline dry-runs — see `tests/tools/mock_pipeline_timing_run.cpp` and
  `docs/howto/pipeline-latency-diagnosis.md`. No electrical output exists,
  of course.
- Timestamps are synthesized from steady-clock deltas, not device ticks —
  useful for dev, not for absolute timing.
- Delivery modes: frames are synthesized on demand (pull source), so both
  `EveryFrame` and `LatestFrame` are supported and behave identically —
  `pollAcquisitionQueueStats` reports a genuinely-zero completed queue and
  no restart is required for mode changes. The queue policies themselves
  are exercised by `tests/support/queue_camera.h`, not by MockCamera.
- See `docs/howto/mock-camera-dev-mode.md` and task
  `knowledge_map/task/mock_camera_dev_mode.md`.
- `data/mock_frames/frame_00000.tiff` is checked in as a minimal sample.
- `refreshFileList` uses the `std::error_code` overload of
  `directory_iterator` — the folder can vanish between the `exists()` check
  and iteration, and the throwing overload would propagate
  `std::filesystem_error` out of `start()`.
