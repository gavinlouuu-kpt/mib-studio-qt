# ReplayClipRecorder

> Silently keeps the first frames of every experiment, with everything needed
> to reprocess them, so a run can be rerun at small scale in the app (mock
> camera) or headless. Backend-only: Qt and React/Tauri get it without shell
> work because both start runs through [[../architecture/ExperimentCoordinator]].

**Source:** `src/backend/recording/ReplayClipRecorder.cpp`,
`include/backend/recording/ReplayClipRecorder.h`
**Plan:** `docs/exec-plans/active/2026-09-30-replay-clip-capture.md` (issue #463)
**Related:** [[../architecture/ExperimentCoordinator]], [[../architecture/AppBackend]],
[[../data-model/FrameStore]], [[../camera/MockCamera]], [[../diagnostics/MemoryBudget]]

## Responsibility

- **Capture rule:** the first **1000 frames or 1 s** of host time after the run
  enters Active, whichever comes first, capped by `maxBytes` (512 MB). An
  early Stop ends the clip there (`run_ended`, state `incomplete`).
- `arm(ReplayClipArm)` — called by the coordinator inside the Start
  transaction right after the run becomes Active, with the frozen
  `RunConfigurationSnapshot`, its JSON, the canonical processing config, the
  raw `config.json`, the background and `FrameStore::committedCount()` as the
  first write index. It only starts one worker thread; it never blocks or
  fails the Start. A clip still being written when the next run starts means
  the new clip is skipped (logged), never waited for.
- `notifyRunEnded(startGeneration)` — called at the start of finalization;
  closes the capture window if it is still open.
- The worker copies frames by write index (`waitForFrame` +
  `readByWriteIndex`). `Overwritten` / `Malformed` become explicit gap
  records and `contiguous: false`. Then it writes, rate-limited
  (`maxWriteBytesPerSec`, 64 MB/s; unthrottled during shutdown):

```
<dataDir>/replay-clips/<UTC start>-g<startGeneration>/
  manifest.json          state, end_reason, contiguous, config_verified, counts, run link, hashes
  frames/000000.png …    lossless Mono8, named by offset from the first write index
  frames.jsonl           one record per index in the window (written / gap / failure / abandoned)
  run_snapshot.json      runSnapshotToJson(run)
  processing_config.txt  canonical ProcessingConfig (hashes to processing_config_sha256)
  config.json            raw config.json as last applied (when present)
  background.png         the run's background (when present)
```

- `frames/` is directly usable as a mock-camera folder (sorted PNGs, Mono8).
- `config_verified` is true only when the saved config, `config.json` and
  background hash to the values frozen in the run snapshot.
- States: `complete` (window closed at frame/duration/byte limit, no gaps),
  `incomplete` (run ended early, shutdown, gaps, or some write failed),
  `skipped` (disabled, busy, insufficient free space, no frames — nothing
  written), `failed` (nothing could be written). The manifest is replaced
  atomically; one left in `writing` means the process died mid-clip.

## Threading

One worker per clip (`worker_`), joined on the next `arm()` or at
`shutdown()`. Shared state (`busy_`, window/abort flags, status) is under
`mutex_`; `idleCv_` wakes `waitIdle()`, the throttle wait and shutdown. The
worker only reads the `FrameStore` through per-slot reads (no structural
lock, no capture callback), so the capture producer is never blocked.

## Failure isolation

- Free-space preflight: clip estimate + `freeSpaceReserveBytes` (512 MB)
  must be available, otherwise `skipped` — a clip never starves HDF5.
- The experiment HDF5 file is never opened (#451 writer ownership).
- Every error ends up in the manifest/status and the log, never in the
  experiment's status.
- `shutdown()` (from `AppBackend::shutdown()` after the coordinator) closes
  the window, waits `shutdownDrainTimeout` (5 s), then abandons remaining
  frames (`abandoned` records, state `incomplete`) and joins.
- `MIB_REPLAY_CLIP=0` disables capture (admin opt-out).

## Memory

`memoryStats()` → owner `recording.replayClip` (Measured; capacity = `maxBytes`
/ `maxFrames`), included in `AppBackend::memoryBudgetSnapshot()`.

## Tests

`e2e.replay_clip` (`tests/integration/e2e_replay_clip_test.cpp`): mock camera
over ID-stamped frames — exact frame count, pixel-exact in-order frames,
config/background hashes, replay of the clip through the mock camera,
duration limit, early stop, no free space, disabled, rapid start/stop,
shutdown mid-clip.

## Gotchas

- Not yet recorded: the live per-frame results for the clip frames and a
  mock camera that replays recorded pacing/timestamps — slice 2 of the plan.
- The frame bound counts write indices spanned (copied + gaps), so gaps never
  extend the clip past the start of the run.
- The duration bound is host time since the first copied frame; it also
  closes the window when frames stall. No frame within `firstFrameTimeout`
  (2 s) → `skipped`.
- Only Mono8 is encoded; any other pixel format is recorded as
  `unsupported_pixel_format` (none exists in the pipeline today).
