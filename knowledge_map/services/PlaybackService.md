# PlaybackService

> Thin UI-facing wrapper around [[../data-model/FrameStore]]. No thread of
> its own.

**Source:** `src/backend/services/PlaybackService.cpp`,
`include/backend/services/PlaybackService.h`
**Related:** [[../data-model/FrameStore]], [[../frontend/System-Utilities]]
(PlaybackPanel)

## Responsibility

- Forward reads to the shared `FrameStore` (`fetchLatest`, `fetchByIndex`,
  `queryRange`).
- Expose convenience: `totalWritten`, `capacity`,
  `getAvailableRange`, `getAvailableTimestampRange`,
  `estimateMemoryBytesForCapacity`.
- Save-to-disk helpers (TIFF export) by index range or timestamp range,
  with optional `filterFn` to skip empty frames.
- `resize(newCapacity)` — delegates to FrameStore; preserves frames if the
  new capacity is big enough.
- `play()` / `pause()` — transport-style flag, read by UI.

## Threading

Synchronous passthroughs. Thread-safety comes from FrameStore's internal
mutex.

## Gotchas

- `fetchByIndex` takes an **absolute** write index (monotonic since start),
  not a slot index. Use `queryRange` to get the current window.
- Save helpers write as TIFF — see `FrameStore::saveFramesToDisk` for the
  single-frame implementation.

Tauri buffer controls reuse index/timestamp save overloads and the same active-kernel
empty-frame filter as Qt. Buffer mutation requires stopped capture and an idle experiment;
resize below the retained count requires explicit clear confirmation. Requests preserve
u64 identifiers as decimal strings and bind selections to the ring generation. Paused
background capture currently accepts valid Mono8 frames only. Timestamp inputs use raw
source units, not inferred nanoseconds or converted wall time.
