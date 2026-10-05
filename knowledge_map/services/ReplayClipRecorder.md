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
- **Retention (agreed 2026-10-05):** a clip belongs to its recording. It is
  written next to the run's HDF5 file and is deleted only when the run's
  files are deleted; there is no age-based expiry and older clips are never
  deleted to make room. A reused output path gets `<stem>.replay-clip-1`, …
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
<run dir>/<stem>.replay-clip/       next to <stem>.h5
                                    (fallback for a run without an output path:
                                    <dataDir>/replay-clips/<UTC start>-g<generation>/)
  manifest.json          state, end_reason, contiguous, config_verified, counts, run link, hashes
  frames/000000.png …    lossless Mono8, named by offset from the first write index
  frames.jsonl           one record per index in the window (written / gap / failure /
                         disk_reserve / abandoned)
  run_snapshot.json      runSnapshotToJson(run)
  processing_config.txt  canonical ProcessingConfig (hashes to processing_config_sha256)
  config.json            raw config.json as last applied (when present)
  background.png         the run's background (when present)
```

- `frames/` is directly usable as a mock-camera folder (sorted PNGs, Mono8).
- `config_verified` is true only when the saved config, `config.json` and
  background hash to the values frozen in the run snapshot.
- States: `complete` (window closed at the frame or duration bound, no
  gaps), `incomplete` (byte cap reached, free space fell below the reserve
  while writing, run ended early, shutdown, gaps, or some write failed — the
  frames already written are kept), `skipped` (disabled, busy, not enough
  free space for clip + reserve, no frames — nothing written), `failed`
  (nothing could be written). The manifest is replaced atomically; one left
  in `writing` means the process died mid-clip.
- **Run provenance:** at finalization the coordinator stores
  `provenanceJson(startGeneration)` as `/run_provenance @replay_clip_json` in
  the run's HDF5 ([[../data-model/HDF5-Storage]]): state, `final`,
  `end_reason` (`byte_limit` / `disk_reserve` mark a cap or reserve stop),
  message, `clip_dir` (relative to the HDF5 file, null when skipped), counts.
  A clip still capturing/writing at finalization is recorded with
  `final: false`; its `manifest.json` is then authoritative (the HDF5 is
  closed by then and is not reopened).

## Threading

One worker per clip (`worker_`), joined on the next `arm()` or at
`shutdown()`. Shared state (`busy_`, window/abort flags, status) is under
`mutex_`; `idleCv_` wakes `waitIdle()`, the throttle wait and shutdown. The
worker only reads the `FrameStore` through per-slot reads (no structural
lock, no capture callback), so the capture producer is never blocked.

## Failure isolation

- Free-space preflight on the run's volume: clip estimate +
  `freeSpaceReserveBytes` (512 MB) must be available, otherwise `skipped`
  (`disk_reserve`). Free space is re-checked before every frame write; below
  the reserve the clip stops (`incomplete`, `disk_reserve`, remaining frames
  recorded as `disk_reserve`). A clip never starves HDF5 and never deletes
  older clips. Every cap/reserve stop is logged as a warning.
- The recorder never opens the experiment HDF5 file; only the coordinator
  writes the clip outcome into it (#451 writer ownership).
- Every error ends up in the manifest/status and the log, never in the
  experiment's status.
- `shutdown()` (from `AppBackend::shutdown()` after the coordinator) closes
  the window, waits `shutdownDrainTimeout` (5 s), then abandons remaining
  frames (`abandoned` records, state `incomplete`) and joins.
- `setFreeSpaceProbeForTests()` replaces the free-space query (fault
  injection).

## Configuration

Defaults are the agreed policy; each is overridable at startup
(`replayClipOptionsFromEnvironment()`, read by `AppBackend::initialize()`)
or at runtime (`setOptions()`). A value that is not a non-negative integer
is ignored with a warning.

| Variable | Default | Meaning |
|---|---|---|
| `MIB_REPLAY_CLIP` | `1` | `0`/`off` disables capture |
| `MIB_REPLAY_CLIP_MAX_FRAMES` | `1000` | frame bound |
| `MIB_REPLAY_CLIP_MAX_MS` | `1000` | duration bound (host time) |
| `MIB_REPLAY_CLIP_MAX_MB` | `512` | clip cap |
| `MIB_REPLAY_CLIP_RESERVE_MB` | `512` | free space that must remain on the run's volume |
| `MIB_REPLAY_CLIP_WRITE_MBPS` | `64` | write throttle (`0` = unthrottled) |

## Memory

`memoryStats()` → owner `recording.replayClip` (Measured; capacity = `maxBytes`
/ `maxFrames`), included in `AppBackend::memoryBudgetSnapshot()`.

## Tests

`e2e.replay_clip` (`tests/integration/e2e_replay_clip_test.cpp`): mock camera
over ID-stamped frames — exact frame count, pixel-exact in-order frames,
config/background hashes, replay of the clip through the mock camera,
duration limit, early stop, no free space, disabled, rapid start/stop
(busy skips in provenance), byte cap, free space falling below the reserve
mid-write (probe seam), reused output path, run provenance for every case,
shutdown mid-clip, environment overrides.

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
